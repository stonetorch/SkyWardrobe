#include "scene/SceneDocument.h"
#include "features/MCGorePlan.h"
#include <stdexcept>
#include <iostream>
using namespace CAF::Scene;
void Check(bool condition) { if (!condition) throw std::runtime_error("Scene document regression failed"); }
int main()
{
    // 时间倍率检查覆盖边界、生命周期、序列化及逻辑片段的 IF 冻结。
    for (auto trigger : {"0", "END", "EXIT"})
        for (auto value : {"0.01", "0.2", "1", "RESTORE"})
        {
            auto doc = Document::Parse(std::string("AT ") + trigger + " TIME_SCALE " + value);
            Check(doc.diagnostics.empty() && doc.lines.front().statement.kind == Kind::TimeScale);
            auto saved = Document::Parse(Code(doc.lines.front().statement));
            Check(saved.diagnostics.empty() && saved.lines.front().statement.strength == doc.lines.front().statement.strength &&
                saved.lines.front().statement.text == doc.lines.front().statement.text);
        }
    for (auto invalid : {"AT 0 TIME_SCALE", "TIME_SCALE 0.2", "AT 0 TIME_SCALE 0", "AT 0 TIME_SCALE -1",
        "AT 0 TIME_SCALE 1.01", "AT 0 TIME_SCALE nan", "AT 0 TIME_SCALE RESTORE extra", "AT 0 TIME_SCALE RESET"})
        Check(!Document::Parse(invalid).diagnostics.empty());
    {
        Context values;
        const auto doc = Document::Parse("STATE slow LOGIC\nIF 0\nAT 0 TIME_SCALE 0.2\nAT EXIT TIME_SCALE RESTORE");
        auto plan = Prepare(doc, values, {});
        Check(plan.diagnostics.empty() && plan.events.size() == 2 && plan.events[0].condition == "0");
    }
    for (auto trigger : {"0", "END", "EXIT"})
        for (auto duration : {"0.01", "1", "3600"})
        {
            auto doc = Document::Parse(std::string("AT ") + trigger + " TIME_SCALE 0.2 DURATION " + duration);
            Check(doc.diagnostics.empty());
            const auto& event = doc.lines.front().statement;
            auto saved = Document::Parse(Code(event));
            Check(saved.diagnostics.empty() && saved.lines.front().statement.duration == event.duration);
            Context values;
            auto plan = Prepare(doc, values, {});
            Check(plan.diagnostics.empty() && plan.events.size() == 1 && plan.events.front().statement.duration == event.duration);
        }
    for (auto invalid : {"AT 0 TIME_SCALE 0.2 DURATION", "AT 0 TIME_SCALE 0.2 DURATION 0",
        "AT 0 TIME_SCALE 0.2 DURATION -1", "AT 0 TIME_SCALE 0.2 DURATION 3601",
        "AT 0 TIME_SCALE 0.2 DURATION nan", "AT 0 TIME_SCALE 0.2 DURATION 1 extra",
        "AT 0 TIME_SCALE RESTORE DURATION 1", "AT 0 TIME_SCALE 0.2 1"})
        Check(!Document::Parse(invalid).diagnostics.empty());
    // 表情三类通道共享强度规则；名字/数字、生命周期触发与 IF 冻结都走真实 parser。
    for (auto category : {CAF::Facial::Category::Expression, CAF::Facial::Category::Modifier, CAF::Facial::Category::Phoneme})
    {
        const auto keyword = std::string(CAF::Facial::Keyword(category));
        const auto names = CAF::Facial::Names(category);
        for (std::size_t i = 0; i < names.size(); ++i)
        {
            for (auto preset : {std::string(names[i]), std::to_string(i)})
            {
                const auto doc = Document::Parse("AT 0.25 " + keyword + " 8 " + preset + " 73.5");
                Check(doc.diagnostics.empty());
                const auto& s = doc.lines.front().statement;
                Check(s.kind == Kind::Facial && s.actor == 8 && s.facial.category == category &&
                    s.facial.index == static_cast<int>(i) && s.facial.strength == 73.5);
                const auto saved = Document::Parse(Code(s));
                Check(saved.diagnostics.empty() && saved.lines.front().statement.facial.index == s.facial.index);
            }
        }
        for (auto action : {"RESET", "RESTORE"})
            for (auto trigger : {"0", "END", "EXIT"})
            {
                const auto doc = Document::Parse(std::string("AT ") + trigger + " " + keyword + " 1 " + action);
                Check(doc.diagnostics.empty());
                const auto saved = Document::Parse(Code(doc.lines.front().statement));
                Check(saved.diagnostics.empty() && saved.lines.front().statement.facial.operation ==
                    (std::string_view(action) == "RESET" ? CAF::Facial::Operation::Reset : CAF::Facial::Operation::Restore));
            }
        for (auto operands : {"1 0 -1", "1 0 101", "1 0 nan", "1 0 inf", "1 0", "1 -1 10",
            "1 17 10", "1 Missing 10", "0 0 10", "9 0 10", "1 RESET extra", "1 RESTORE 10", "1 0 10 extra"})
            Check(!Document::Parse("AT 0 " + keyword + " " + operands).diagnostics.empty());
        Check(!Document::Parse(keyword + " 1 0 100").diagnostics.empty());
        Context facialContext{{"enabled", 1}};
        const auto conditional = Document::Parse("IF enabled\nAT 0 " + keyword + " 1 0 100\nAND AT 1 " + keyword + " 1 RESET");
        const auto enabledPlan = Prepare(conditional, facialContext, {});
        Check(enabledPlan.diagnostics.empty() && enabledPlan.events.size() == 2 && enabledPlan.events.front().condition == "1");
        facialContext["enabled"] = 0;
        Check(enabledPlan.events.front().condition == "1");
        const auto disabledPlan = Prepare(conditional, facialContext, {});
        Check(disabledPlan.diagnostics.empty() && disabledPlan.events.front().condition == "0");
    }
    Check(!Document::Parse("AT 0 PHONEME 1 16 100").diagnostics.empty());
    for (auto strength : {"0", "100"})
        Check(Document::Parse(std::string("AT 0 EXPRESSION 1 10 ") + strength).diagnostics.empty());
    // 新选项与旧 OFFSET 兼容，往返保存、IF 冻结后仍携带角色根。
    Context pairedContext;
    auto paired = Document::Parse("1 PLAY paired.hkx ROOT \"NPC\" OFFSET 0.25\n2 PLAY paired.hkx OFFSET 0.25 ROOT \"2_\"\n");
    Check(paired.diagnostics.empty());
    auto pairedPlan = Prepare(paired, pairedContext, {});
    Check(pairedPlan.clips[0].rootBone == "NPC" && pairedPlan.clips[1].rootBone == "2_" && pairedPlan.clips[1].offset == 0.25);
    auto pairedRoundTrip = Document::Parse(Code(paired.lines[1].statement));
    Check(pairedRoundTrip.diagnostics.empty() && pairedRoundTrip.lines[0].statement.rootBone == "2_");
    for (auto invalid : {"1 PLAY a.hkx ROOT", "1 PLAY a.hkx ROOT \"\"", "1 PLAY a.hkx ROOT NPC ROOT 2_", "1 PLAY a.hkx OFFSET 0 OFFSET 1"})
        Check(!Document::Parse(invalid).diagnostics.empty());
    for (const auto* code : {"AT 0 CAMERA FIRST", "AT EXIT CAMERA RESTORE",
        "AT 1 CAMERA FOLLOW 8 \"NPC Head [Head]\" 0 -100 20", "AT 0 SCREEN_BLOOD 50",
        "AT 0 SHAKE_CAMERA 0.5 0.3", "AT END SHAKE_CAMERA_AT_ACTOR 8 1.2 1",
        "AT 2 SHAKE_CAMERA_AT_NODE 2 \"NPC R Foot [Rft ]\" 0.8 0.25"})
    {
        const auto doc = Document::Parse(code);
        Check(doc.diagnostics.empty());
        const auto roundtrip = Document::Parse(Code(doc.lines[0].statement));
        Check(roundtrip.diagnostics.empty());
        const auto& before = doc.lines[0].statement;
        const auto& after = roundtrip.lines[0].statement;
        Check(after.kind == before.kind && after.actor == before.actor &&
            after.bone == before.bone && after.strength == before.strength && after.duration == before.duration && after.count == before.count);
        Context context; const auto plan = Prepare(roundtrip, context, {});
        Check(plan.events.size() == 1);
    }
    for (const auto* code : {"SCREEN_BLOOD 50", "SHAKE_CAMERA 1 1", "AT 0 SCREEN_BLOOD -1", "AT 0 SCREEN_BLOOD 1.5",
        "AT 0 SHAKE_CAMERA 2 1", "AT 0 SHAKE_CAMERA 0.5 0", "AT 0 SHAKE_CAMERA nan 1",
        "AT 0 SHAKE_CAMERA_AT_NODE 1 \"\" 1 1", "AT 0 CAMERA FIRST extra", "AT 0 SCREEN_BLOOD", "AT 0 SHAKE_CAMERA 0.5"})
        Check(!Document::Parse(code).diagnostics.empty());
    for (auto name : CAF::GTSGore::Events)
    {
        auto gore = Document::Parse(std::string("AT 0.8 ") + std::string(name) + " 2 node \"NPC Root [Root]\" strong false pitch 1.5 intensity 2 count 3");
        Check(gore.diagnostics.empty());
        const auto& event = gore.lines.front().statement;
        Check(event.kind == Kind::GTSGore && event.actor == 2 && !event.goreOptions.strong && !event.goreOptions.scaleBasedPitch);
        auto roundTrip = Document::Parse(Code(event));
        Check(roundTrip.diagnostics.empty() && roundTrip.lines.front().statement.goreOptions.pitch == 1.5f && roundTrip.lines.front().statement.goreOptions.count == 3);
    }
    for (auto code : {"Crush 1", "AT 0 CrushFX 9", "AT 0 CrushSound 1 pitch nan", "AT 0 CrushParticles 1 intensity 0", "AT 0 BloodSpray 1 node \"\"", "AT 0 CrushSound 1 strong yes", "AT 0 CrushSound 1 count 0", "AT 0 CrushFX 1 unknown 3", "AT 0 CrushSound 1 pitch"})
        Check(!Document::Parse(code).diagnostics.empty());
    for (auto options : {"scaleBasedPitch true pitch 1.5", "pitch 1.5 scaleBasedPitch true"})
    {
        auto gore = Document::Parse(std::string("AT 0 CrushSound 1 ") + options);
        Check(gore.diagnostics.empty() && gore.lines.front().statement.goreOptions.scaleBasedPitch);
    }
    Check(CAF::GTSGore::ScalePitch(0.2f) == 2 && CAF::GTSGore::ScalePitch(2) == 1);
    Check(CAF::GTSGore::ParticlePairs(1) == 2 && CAF::GTSGore::ParticlePairs(4) == 8);
    // STATE 分割计划，GOTO 不混合片段的 HKX；IF/AND 在片段进入时仅求值一次。
    Context segmentContext{{"flag", 1}};
    int macroCalls = 0;
    Macro onceMacro = [&](std::string_view name, const std::vector<double>&) -> std::optional<double>
    { if (name == "Choose") { ++macroCalls; return 1; } return {}; };
    auto segmentsDoc = Document::Parse("STATE intro\nONCE\n1 PLAY intro.hkx\nIF Choose()\nAT END GOTO finish\nAND AT 1 MODIFY_VALUE flag 0\nSTATE finish\nTIME 2\n1 PLAY finish.hkx\nIF flag\nAT END GOTO intro\n");
    auto multi = Prepare(segmentsDoc, segmentContext, onceMacro);
    Check(multi.diagnostics.empty() && multi.segments.size() == 1 && multi.state == "intro" && macroCalls == 1);
    Check(multi.clips[0].path == "intro.hkx" && ResolveSegment(multi, "finish") == nullptr);
    Check(multi.segments[0].events[0].condition == "1" && multi.segments[0].events[1].condition == "1");
    segmentContext["flag"] = 0;
    Check(multi.events[0].condition == "1" && macroCalls == 1);
    auto enteredFinish = Prepare(segmentsDoc, segmentContext, onceMacro, "finish");
    Check(enteredFinish.diagnostics.empty() && enteredFinish.state == "finish" && enteredFinish.clips[0].path == "finish.hkx");
    Check(enteredFinish.events[0].condition == "0" && enteredFinish.duration == 2 && macroCalls == 1);
    segmentContext["flag"] = 1;
    auto enteredAgain = Prepare(segmentsDoc, segmentContext, onceMacro, "finish");
    Check(enteredAgain.events[0].condition == "1" && enteredFinish.events[0].condition == "0");
    Prepare(segmentsDoc, segmentContext, onceMacro, "intro"); Check(macroCalls == 2);
    // 未进入片段的变量操作和宏不得执行，跳转后才以最新 context 准备。
    auto deferredDoc = Document::Parse("STATE first\n1 PLAY first.hkx\nSTATE later\nIF Choose()\nMODIFY_VALUE flag flag+1\nAND 1 PLAY later.hkx\n");
    auto deferred = Prepare(deferredDoc, segmentContext, onceMacro);
    Check(deferred.diagnostics.empty() && macroCalls == 2 && segmentContext["flag"] == 1);
    auto nowEntered = Prepare(deferredDoc, segmentContext, onceMacro, "later");
    Check(nowEntered.diagnostics.empty() && macroCalls == 3 && segmentContext["flag"] == 2 && nowEntered.clips[0].path == "later.hkx");
    const auto edges = Transitions(segmentsDoc);
    Check(edges.size() == 2 && edges[0].condition == "Choose()" && edges[0].trigger == Trigger::End && edges[0].target == "finish");
    Check(Document::Parse(Code(segmentsDoc.lines[4].statement)).lines[0].statement.kind == Kind::Goto);
    auto direct = Prepare(Document::Parse("STATE entry\nIF 1\nGOTO finish\n1 PLAY skipped.hkx\nSTATE finish\n1 PLAY finish.hkx\n"), segmentContext, {});
    Check(direct.diagnostics.empty() && direct.state == "finish" && direct.clips[0].path == "finish.hkx" && direct.segments[0].clips[0].path == "skipped.hkx");
    // 运行时单段准备不能跳过中间状态；GOTO 后的同入口修改仍执行。
    auto logicDocument = Document::Parse("STATE entry LOGIC\nMODIFY_VALUE flag 2\nGOTO waiting\nMODIFY_VALUE flag flag+3\nAT 0 MODIFY_VALUE entered 1\nAT EXIT MODIFY_VALUE exited 1\nSTATE waiting LOGIC\nIF flag==5\nAT 0 USER_OPTION proceed \"Continue\" TO action FORCE\nAND AT EXIT CAMERA RESTORE\nSTATE action\nONCE\n1 PLAY action.hkx\n");
    Context logicContext{{"flag", 0}};
    auto logicEntry = Prepare(logicDocument, logicContext, {}, {}, false);
    Check(logicEntry.diagnostics.empty() && logicEntry.logic && !logicEntry.loop && logicEntry.duration == 0);
    Check(logicEntry.state == "entry" && logicEntry.immediateTarget == "waiting" && logicContext["flag"] == 5);
    Check(logicEntry.events.size() == 2 && !logicContext.contains("entered") && !logicContext.contains("exited"));
    Check(Segments(logicDocument)[0].logic && Segments(logicDocument)[1].logic && !Segments(logicDocument)[2].logic);
    Check(Document::Parse(Code(logicDocument.lines[0].statement)).lines[0].statement.logic);
    auto logicWait = Prepare(logicDocument, logicContext, {}, "waiting", false);
    Check(logicWait.logic && logicWait.events[0].condition == "1" && logicWait.events[0].statement.option.force);
    logicContext["flag"] = 0;
    Check(logicWait.events[0].condition == "1");
    auto logicAction = Prepare(logicDocument, logicContext, {}, "action", false);
    Check(!logicAction.logic && logicAction.clips[0].path == "action.hkx");
    for (const auto* operation : {"1 PLAY a.hkx", "TIME 1", "LOOP", "ONCE", "AT END END_SCENE",
        "AT 0 ALIGN 1 Root TO 2 Root 0 0 0 0 0 0", "AT 0 PREDICT_ALIGN 1 1 Root TO 2 Root 0 0 0 0 0 0",
        "AT 0 PLAY_SOUND sound", "AT 0 DEATH 1", "AT 0 FOOTSTEP 1 FootLeft", "AT 0 APPLY_GORE 1 THROAT_SLIT"})
    {
        auto invalidLogic = Document::Parse(std::string("STATE invalid LOGIC\nIF 0\n") + operation);
        Check(!invalidLogic.diagnostics.empty() && !Prepare(invalidLogic, logicContext, {}).diagnostics.empty());
    }
    for (const auto* mode : {"FIRST", "THIRD", "FREE", "EXIT_FREE", "RELEASE", "RESTORE",
        "FOLLOW 1 Root 0 0 0", "POSITION 1 Root 0 0 0", "FOCUS 1 Root 0 0 0", "FOCUS ONCE 1 Root 0 0 0"})
        Check(Prepare(Document::Parse(std::string("STATE camera LOGIC\nAT 0 CAMERA ") + mode), logicContext, {}).diagnostics.empty());
    Check(Prepare(Document::Parse("STATE empty LOGIC\nAT 5 END_SCENE\nAT EXIT MODIFY_VALUE flag 1"), logicContext, {}, {}, false).diagnostics.empty());
    Check(!Prepare(Document::Parse("STATE cycle LOGIC\nGOTO cycle"), logicContext, {}, {}, false).diagnostics.empty());
    Check(!Document::Parse("STATE x LOGIC\nFORK y").diagnostics.empty());
    auto frozenGroup = Prepare(Document::Parse("IF flag\nMODIFY_VALUE flag 0\nAND 1 PLAY kept.hkx\n"), segmentContext = {{"flag", 1}}, {});
    Check(frozenGroup.clips[0].path == "kept.hkx" && segmentContext["flag"] == 0);
    for (auto code : {"STATE x\nSTATE x", "STATE x\nAT END GOTO absent", "STATE x\nGOTO x", "STATE x\nIF 1\nSTATE y"})
        Check(!Prepare(Document::Parse(code), segmentContext, {}).diagnostics.empty());
    for (auto code : {"GOTO", "GOTO x extra", "AND STATE x", "AT EXIT GOTO x", "AT 1 STATE x"})
        Check(!Document::Parse(code).diagnostics.empty());
    auto disabled = Document::Parse("  FUTURE_COMMAND anything ; retained\nSTATE next\n1 PLAY good.hkx\n");
    Check(disabled.lines[0].statement.kind == Kind::Raw && !disabled.diagnostics.empty());
    const auto rawSource = disabled.Source();
    disabled.ToggleDisabled(0);
    Check(disabled.IsDisabled(0) && disabled.diagnostics.empty() && disabled.lines[0].statement.kind == Kind::Empty);
    disabled.ToggleDisabled(0);
    Check(disabled.Source() == rawSource && disabled.lines[0].statement.kind == Kind::Raw);
    disabled.ReplaceRaw(0, "AT END GOTO next ; corrected");
    Check(disabled.diagnostics.empty() && disabled.lines[0].statement.kind == Kind::Goto);
    disabled.ToggleDisabled(1);
    Check(Segments(disabled).size() == 1 && disabled.diagnostics.empty());
    disabled.ToggleDisabled(1);
    Check(Segments(disabled).size() == 2);
    auto blank = Document::Parse("   ");
    const auto blankSource = blank.Source(); blank.ToggleDisabled(0);
    Check(blank.IsDisabled(0)); blank.ToggleDisabled(0); Check(blank.Source() == blankSource);
    auto notes = Document::Parse("; file note\nSTATE entry\n1 PLAY x.hkx");
    Check(Segments(notes).front().begin == 0 && Segments(notes).front().stateLine == 1);
    auto cross = Prepare(Document::Parse("STATE a\nIF 1\nAT END GOTO b\nSTATE b\nAND 1 PLAY b.hkx"), segmentContext, {});
    Check(!cross.diagnostics.empty());
    // 删除中间片段不会吞掉下一边界；外部跳转保留为可修复的缺失目标。
    auto deletion = Document::Parse("STATE a\n1 PLAY a.hkx\nAT END GOTO b\nSTATE b\n1 PLAY b.hkx\n; b note\nSTATE c\n1 PLAY c.hkx\n");
    Check(deletion.EraseSegment(Segments(deletion)[1].begin));
    Check(Segments(deletion).size() == 2 && Segments(deletion)[1].name == "c" && Transitions(deletion)[0].target == "b");
    Check(!Prepare(deletion, segmentContext, {}).diagnostics.empty());
    Check(!deletion.EraseSegment(999));
    Check(deletion.EraseSegment(Segments(deletion)[0].begin));
    Check(Segments(deletion).size() == 1 && Segments(deletion)[0].name == "c");
    Check(deletion.EraseSegment(0) && deletion.lines.empty());
    Context context{{"ShouldKill",0}, {"custom111",2}};
    Macro macros = [](std::string_view name,const std::vector<double>& args)->std::optional<double>
    { if(name=="GetGender" && args.size()==1 && args[0]==8) return 1; if(name=="IsPlay" && args.empty()) return 1; return {}; };
    std::string error;
    Check(Evaluate("GetGender(8)==1 && (custom111+3>=5 || ShouldKill) && !ShouldKill",context,macros,error)==1);
    Check(Evaluate("0==2<3",context,macros,error)==0);
    Check(Evaluate("1 || Missing",context,macros,error)==1);
    Check(!Evaluate("!Missing",context,macros,error));
    Check(!Evaluate("Unknown()",context,macros,error));
    Check(!Evaluate("1/0",context,macros,error));
    Check(!Evaluate("1 &&",context,macros,error));
    auto doc = Document::Parse("STATE default\n; comment\nIF !ShouldKill\n8 PLAY \"test;v.hkx\" OFFSET 3.1 ; retained\nAND MODIFY_VALUE custom111 custom111+3\nIF custom111>=5\nAT 4 DEATH 8\nAND AT END MODIFY_VALUE custom111 custom111+1\nAT EXIT CAMERA RESTORE\nAT 3 ALIGN UNK UNK TO UNK UNK 0 0 0 0 0 0\n");
    Check(doc.diagnostics.empty());
    const auto original = doc.Source();
    auto plan = Prepare(doc,context,macros);
    Check(plan.diagnostics.empty() && plan.clips[7].path=="test;v.hkx" && plan.clips[7].offset==3.1 && context["custom111"]==5 && plan.loop && plan.duration==0);
    Check(plan.events.size()==4 && plan.events[0].condition=="1" && plan.events[1].statement.trigger==Trigger::End && plan.events[2].condition.empty());
    Check(doc.Source()==original);
    doc.Replace(3,"8 PLAY replacement.hkx");
    Check(doc.lines[3].raw=="8 PLAY replacement.hkx ; retained" && doc.lines[1].raw=="; comment");
    Check(Document::Parse("9 PLAY bad.hkx\n").diagnostics.size()==1);
    Check(!Document::Parse("AT 1 1 PLAY bad.hkx").diagnostics.empty());
    Check(!Document::Parse("IF a &&\n1 PLAY good.hkx").diagnostics.empty());
    Check(!Document::Parse("AT 0 PREDICT_ALIGN 0 1 Root TO 2 Root 0 0 0 0 0 0").diagnostics.empty());
    auto missing = Prepare(Document::Parse("IF Missing\n1 PLAY bad.hkx\n2 PLAY good.hkx\n"),context,macros);
    Check(missing.diagnostics.empty() && missing.warnings.size()==1 && missing.clips[0].path.empty() && missing.clips[1].path=="good.hkx");
    auto broken = Prepare(Document::Parse("AND 1 PLAY bad.hkx\n"),context,macros); Check(!broken.diagnostics.empty());
    auto fixed = Prepare(Document::Parse("ONCE\nTIME 10\n1 PLAY a.hkx\n1 PLAY b.hkx\n"),context,macros);
    Check(!fixed.loop && fixed.duration==10 && fixed.clips[0].path=="b.hkx");
    context["custom111"] = 100; Check(fixed.clips[0].path=="b.hkx");
    for (const auto& line : doc.lines) if (line.statement.kind!=Kind::Empty) Check(Document::Parse(Code(line.statement)).diagnostics.empty());
    // 相机事件沿用触发条件，定位只保存骨骼局部偏移，不保存某次播放的世界坐标。
    auto cameras = Document::Parse("AT 0 CAMERA RELEASE\nAT 1 CAMERA FREE\nAT 2 CAMERA EXIT_FREE\nIF ShouldKill\nAT 3 CAMERA FOLLOW 8 \"NPC Head [Head]\" 1.5 -2 3\nAT 4 CAMERA POSITION 1 \"NPC Root\" 0 0 20\nAT EXIT CAMERA RESTORE\n");
    auto cameraPlan = Prepare(cameras, context, macros);
    Check(cameras.diagnostics.empty() && cameraPlan.diagnostics.empty() && cameraPlan.events.size() == 6);
    Check(cameraPlan.events[3].condition == "0" && cameraPlan.events[4].condition.empty());
    const auto& follow = cameraPlan.events[3].statement;
    Check(follow.actor == 8 && follow.bone == "NPC Head [Head]" && follow.position == std::array<float, 3>{1.5f, -2, 3});
    for (const auto& line : cameras.lines)
    {
        auto roundtrip = Document::Parse(Code(line.statement));
        Check(roundtrip.diagnostics.empty() && roundtrip.lines.size() == 1);
        Check(roundtrip.lines[0].statement.text == line.statement.text && roundtrip.lines[0].statement.position == line.statement.position);
    }
    for (const auto* invalid : {"CAMERA FREE", "AT 0 CAMERA UNKNOWN", "AT 0 CAMERA FOLLOW 9 Root 0 0 0",
        "AT 0 CAMERA POSITION 1 Root 0 0", "AT 0 CAMERA POSITION 1 Root 0 0 inf", "AT 0 CAMERA RELEASE extra"})
        Check(!Document::Parse(invalid).diagnostics.empty());
    // ONCE 只修饰聚焦中心采样，不能改变场景循环，也不能在序列化时丢失。
    auto focus = Document::Parse("IF ShouldKill\nAT 1 CAMERA FOCUS 8 \"NPC Head [Head]\" 1 -2 3\nAND AT 2 CAMERA FOCUS ONCE 1 Root 0 0 10 ; fixed center\nAT EXIT CAMERA RELEASE\n");
    auto focusPlan = Prepare(focus, context, macros);
    Check(focus.diagnostics.empty() && focusPlan.diagnostics.empty() && focusPlan.loop && focusPlan.events.size() == 3);
    Check(!focusPlan.events[0].statement.focusOnce && focusPlan.events[1].statement.focusOnce);
    Check(focusPlan.events[0].condition == "0" && focusPlan.events[1].condition == "0");
    Check(focusPlan.events[1].statement.actor == 1 && focusPlan.events[1].statement.position == std::array<float, 3>{0, 0, 10});
    for (const auto& event : focusPlan.events)
    {
        const auto parsed = Document::Parse(Code(event.statement));
        Check(parsed.diagnostics.empty());
        const auto& again = parsed.lines.front().statement;
        Check(again.text == event.statement.text && again.focusOnce == event.statement.focusOnce &&
            again.actor == event.statement.actor && again.bone == event.statement.bone && again.position == event.statement.position);
    }
    Check(Document::Parse("AT 0 CAMERA FOCUS ONCE UNK UNK 0 0 0").diagnostics.empty());
    for (const auto* invalid : {"CAMERA FOCUS 1 Root 0 0 0", "AT 0 CAMERA FOCUS", "AT 0 CAMERA FOCUS ONCE",
        "AT 0 CAMERA FOCUS ONCE ONCE 1 Root 0 0 0", "AT 0 CAMERA FOCUS OFTEN 1 Root 0 0 0",
        "AT 0 CAMERA FOCUS 9 Root 0 0 0", "AT 0 CAMERA FOCUS ONCE 1 Root 0 0",
        "AT 0 CAMERA FOCUS 1 Root 0 0 nan", "AT 0 CAMERA FOCUS 1 Root 0 0 0 ONCE",
        "AT 0 CAMERA FOLLOW ONCE 1 Root 0 0 0"})
        Check(!Document::Parse(invalid).diagnostics.empty());
    // Gore 保留触发条件和指定演员，序列化不能丢失效果类型或清除事件。
    auto gore = Document::Parse("IF ShouldKill\nAT 1 APPLY_GORE 8 BISECTION_VERTICAL ; wound\nAND AT EXIT CLEAR_GORE 8\n");
    auto gorePlan = Prepare(gore, context, macros);
    Check(gore.diagnostics.empty() && gorePlan.events.size() == 2);
    Check(gorePlan.events[0].statement.actor == 8 && gorePlan.events[0].statement.kind == Kind::Gore &&
        gorePlan.events[0].statement.text == "BISECTION_VERTICAL" && gorePlan.events[0].condition == "0");
    Check(gorePlan.events[1].statement.kind == Kind::ClearGore && gorePlan.events[1].statement.trigger == Trigger::Exit);
    for (const auto& event : gorePlan.events)
    {
        const auto roundtrip = Document::Parse(Code(event.statement));
        Check(roundtrip.diagnostics.empty() && roundtrip.lines[0].statement.kind == event.statement.kind &&
            roundtrip.lines[0].statement.text == event.statement.text && roundtrip.lines[0].statement.actor == 8);
    }
    for (const auto* invalid : {"APPLY_GORE 1 THROAT_SLIT", "AT 0 APPLY_GORE 1 UNKNOWN", "AT 0 APPLY_GORE 9 FACE_LEFT",
        "AT 0 APPLY_GORE 1", "CLEAR_GORE 1", "AT 0 CLEAR_GORE 1 extra"})
        Check(!Document::Parse(invalid).diagnostics.empty());
    for (const auto& primitive : CAF::MCGore::primitives)
    {
        std::string code = "AT 1.25 " + std::string(primitive.command) + " 8";
        if (!primitive.argument.empty()) code += " " + std::string(primitive.argument);
        const auto atomic = Document::Parse(code);
        Check(atomic.diagnostics.empty() && atomic.lines[0].statement.kind == Kind::GorePrimitive &&
            atomic.lines[0].statement.text == primitive.code);
        const auto roundtrip = Document::Parse(Code(atomic.lines[0].statement));
        Check(roundtrip.diagnostics.empty() && roundtrip.lines[0].statement.text == primitive.code);
        const auto planned = Prepare(atomic, context, macros);
        Check(planned.events.size() == 1 && planned.events[0].statement.actor == 8);
    }
    for (const auto* invalid : {"GORE_BLOOD 1 NECK", "AT 0 GORE_BLOOD 1", "AT 0 GORE_BLOOD 1 UNKNOWN",
        "AT 0 GORE_EQUIP 9 NECK", "AT 0 GORE_DECAL 1 FACE", "AT 0 GORE_DEBRIS 1 GENERIC extra",
        "AT 0 GORE_HIDE_TORSO 1 extra", "AT 0 GORE_UNCOVER_HEAD"})
        Check(!Document::Parse(invalid).diagnostics.empty());
    // 新效果必须保留演员、模式、手、材质与射线参数，错误操作不能静默变成其他模式。
    auto native = Document::Parse("IF ShouldKill\nAT 0.2 FOOTSTEP 8 FootLeft\nAND AT 0.3 SOUND_PLAY 1 \"WPNSwingBladeMedium\"\nAT 0.4 HIT 1 TO 8 SOUND LEFT \"NPC Spine2 [Spn2]\" \"Metal\" 0 0 -1 100\nAT 0.5 HIT 8 TO 1 IMPACT RIGHT Root Skin 1 2 3 55\nAT EXIT HIT 1 TO 8 SOUND IMPACT RIGHT Root Skin 0 0 -1 100\n");
    Check(native.diagnostics.empty());
    auto nativePlan = Prepare(native, context, macros);
    Check(nativePlan.events.size() == 5 && nativePlan.events[0].condition == "0" && nativePlan.events[1].condition == "0");
    Check(nativePlan.events[2].statement.mode == 1 && nativePlan.events[2].statement.leftHand && nativePlan.events[2].statement.target == 8);
    for (const auto& event : nativePlan.events)
    {
        const auto parsed = Document::Parse(Code(event.statement));
        Check(parsed.diagnostics.empty());
        const auto& again = parsed.lines[0].statement;
        Check(again.kind == event.statement.kind && again.mode == event.statement.mode && again.leftHand == event.statement.leftHand &&
            again.actor == event.statement.actor && again.target == event.statement.target && again.text == event.statement.text &&
            again.bone == event.statement.bone && again.position == event.statement.position && again.duration == event.statement.duration);
    }
    for (const auto* invalid : { "FOOTSTEP 1 FootLeft", "AT 0 FOOTSTEP 9 FootLeft", "AT 0 FOOTSTEP 1 Death", "AT 0 SOUND_PLAY 1 \"\"",
        "AT 0 HIT 1 TO 2 DAMAGE RIGHT Root Skin 0 0 -1 100", "AT 0 HIT 1 TO 2 SOUND BOTH Root Skin 0 0 -1 100",
        "AT 0 HIT 1 TO 2 IMPACT RIGHT Root Skin 0 0 0 100", "AT 0 HIT 1 TO 2 SOUND RIGHT Root Skin 0 0 -1 -1",
        "AT 0 HIT 1 TO 2 SOUND RIGHT Root Skin 0 0 -1", "AT 0 HIT 1 TO 2 SOUND RIGHT Root Skin 0 0 -1 100 extra" })
        Check(!Document::Parse(invalid).diagnostics.empty());
    // 前导语句构成独立 default；选项切换只准备目标片段。
    auto states = Document::Parse("MODIFY_VALUE count count+10\nSTATE start\nLOOP\n1 PLAY a.hkx\nIF count>0\nAT 1 USER_OPTION proceed \"继续;下一步\" TO finish FORCE IMMEDIATE\nAND AT 2 USER_OPTION later \"稍后\" TO finish\nSTATE finish\nONCE\nTIME 4\n2 PLAY b.hkx\nMODIFY_VALUE count count+1\nAT EXIT CAMERA RESTORE\n");
    Context stateContext{{"count", 0}};
    auto defaultState = Prepare(states, stateContext, {});
    Check(defaultState.diagnostics.empty() && defaultState.state == "default" && stateContext["count"] == 10);
    auto firstState = Prepare(states, stateContext, {}, "start");
    Check(firstState.diagnostics.empty() && firstState.state == "start" && firstState.loop && firstState.events.size() == 2);
    Check(stateContext["count"] == 10 && firstState.clips[0].path == "a.hkx" && firstState.clips[1].path.empty());
    Check(firstState.events[0].condition == "1" && firstState.events[1].condition == "1");
    const auto& option = firstState.events[0].statement;
    auto optionRoundTrip = Document::Parse(Code(option));
    Check(optionRoundTrip.diagnostics.empty() && optionRoundTrip.lines[0].statement.option.text == "继续;下一步" &&
        optionRoundTrip.lines[0].statement.option.force && optionRoundTrip.lines[0].statement.option.immediate);
    auto finishState = Prepare(states, stateContext, {}, "finish");
    Check(finishState.diagnostics.empty() && finishState.state == "finish" && !finishState.loop && finishState.duration == 4);
    Check(stateContext["count"] == 11 && finishState.clips[0].path.empty() && finishState.clips[1].path == "b.hkx" && finishState.events.size() == 1);
    auto returned = Prepare(states, stateContext, {}, "start");
    Check(returned.diagnostics.empty() && stateContext["count"] == 11);
    Check(!Prepare(states, stateContext, {}, "missing").diagnostics.empty());
    for (const auto* invalid : {"USER_OPTION x \"text\" TO default", "AT END USER_OPTION x \"text\" TO default",
        "AT EXIT USER_OPTION x \"text\" TO default", "AT 0 USER_OPTION x \"\" TO default", "AT 0 USER_OPTION x text default",
        "AT 0 USER_OPTION x text TO default FAST", "AT 0 USER_OPTION x text TO default FORCE FORCE"})
        Check(!Document::Parse(invalid).diagnostics.empty());
    for (const auto* invalid : {"STATE a\nSTATE a", "STATE a\nAT 0 USER_OPTION x text TO missing",
        "IF 1\nSTATE a", "STATE a\nAND STATE b"})
    {
        Context empty;
        Check(!Prepare(Document::Parse(invalid), empty, {}).diagnostics.empty());
    }
    std::cout << "Scene document, expression, context and frozen plan checks passed\n";
}
