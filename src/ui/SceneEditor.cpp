#include "ui/Text.h"
#include "ui/SceneEditor.h"
#include "ui/SceneTimeline.h"
#include "ui/WindowPlacement.h"
#include "animation/HkxMetadata.h"
#include "scene/SceneDocument.h"
#include "scene/SceneFile.h"
#include "scene/SceneMath.h"
#include "runtime/Runtime.h"
#include "features/MCGore.h"
#include "features/GTSGore.h"
#include "facial/FacialControl.h"
#include "ui/Overlay.h"
#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <format>

namespace CAF::SceneEditor
{
    namespace
    {
        struct Editor
        {
            Scene::Document document = Scene::Document::Parse("STATE default\nLOOP\n1 PLAY test_vic.hkx\n2 PLAY test_atk.hkx\n; Select actors, then edit and preview.\n");
            Snapshot snapshot;
            Scene::Statement details;
            Scene::Context context;
            int selected = -1, actorSlot = 0, pickSlot = 0, pickSide = -1;
            char name[65] = "default", form[16]{}, variable[128]{};
            double value = 0;
            bool open = false, dirty = false, numericInput = false, promptCameraRestore = false;
            bool previewCameraAnchor = false;
            std::string pressedBone;
            std::string activeBone;
            bool pickDragged = false;
            ImVec2 pickPress{};
            ImVec2 cameraPromptAnchor{};
            char selectedFile[65] = "default", newFile[65]{};
            std::string fileStatus;
            std::string status;
            std::string segmentName = "default", segmentDraft, rawDraft;
            bool segmentOpen = false;
            int newSegmentType = 0;
            bool promptDeleteSegment = false;
            std::size_t deleteSegmentBegin = 0;
            std::string deleteSegmentName;
            std::vector<std::string> paths;
            std::string inspectedHkx;
            Animation::HkxMetadata hkxMetadata;
        } editor;
        void Select(int index)
        {
            editor.activeBone.clear();
            editor.selected = index; editor.pickSide = -1; editor.pressedBone.clear();
            if (index >= 0 && index < static_cast<int>(editor.document.lines.size()))
            {
                editor.details = editor.document.lines[index].statement;
                editor.rawDraft = editor.document.lines[index].raw;
            }
        }
        void Send(Action action, float value = 0)
        {
            Command c; c.action = action; c.value = value; Enqueue(std::move(c));
        }
        void Start(bool preview)
        {
            editor.pickSide = -1; editor.pressedBone.clear();
            Command c; c.action = Action::StartScriptScene; c.text = editor.document.Source(); c.context = editor.context; c.value = preview ? 1.0f : 0.0f;
            if (preview)
            {
                const auto segments = Scene::Segments(editor.document);
                if (!std::ranges::any_of(segments, [&](const auto& segment) { return segment.name == editor.segmentName; }))
                    editor.segmentName = segments.front().name;
                c.entrySegment = editor.segmentName;
                if (std::ranges::any_of(segments, [&](const auto& segment) { return segment.name == c.entrySegment && segment.logic; })) return;
            }
            Enqueue(std::move(c));
            if (preview && editor.snapshot.scriptScene && editor.snapshot.scenePreview && editor.snapshot.scenePlaying && editor.snapshot.fragmentState == editor.segmentName)
                Send(Action::SeekScene, editor.snapshot.sceneTime);
        }
        void RefreshAlignmentPreview()
        {
            if (!editor.snapshot.scriptScene || !editor.snapshot.scenePlaying || !editor.snapshot.scenePreview ||
                !editor.document.diagnostics.empty() || editor.selected < 0 || editor.snapshot.fragmentState != editor.segmentName) return;
            const auto kind = editor.document.lines[editor.selected].statement.kind;
            if (kind != Scene::Kind::Align && kind != Scene::Kind::Predict && kind != Scene::Kind::Facial) return;
            Command command; command.action = kind == Scene::Kind::Facial ? Action::UpdatePreviewFacial : Action::UpdatePreviewAlignment;
            command.slot = editor.selected; command.text = editor.document.Source();
            Enqueue(std::move(command));
        }
        bool Text(const char* label, std::string& value)
        {
            std::vector<char> buffer(std::max<std::size_t>(4096, value.size()+1024), '\0');
            std::copy(value.begin(), value.end(), buffer.begin());
            if (!ImGui::InputText(label, buffer.data(), buffer.size())) return false;
            value = buffer.data(); return true;
        }
        bool Number(const char* label, double& value)
        {
            return ImGui::InputDouble(label, &value, 0.01, 1, "%.4f");
        }
        bool Vector(const char* label, std::array<float, 3>& value, float speed)
        {
            // 两种控件共享同一参数，键盘输入与拖动都走已有的预览刷新路径。
            return editor.numericInput ? ImGui::InputFloat3(label, value.data(), "%.4f") :
                ImGui::DragFloat3(label, value.data(), speed, 0, 0, "%.4f");
        }
        std::string_view GoreDisplayName(std::string_view code)
        {
            // 显示配方名称时使用本地化标签，脚本仍保存原始效果代码。
            for (const auto& choice : MCGore::choices)
                if (choice.code == code) return CAF::Ui::Text::DisplayName(choice.label);
            return code;
        }
        std::string Description(const Scene::Line& line, int index)
        {
            if (std::ranges::any_of(editor.document.diagnostics, [index](const auto& diagnostic) { return diagnostic.line == index+1; }))
                return CAF::Ui::Text::kRawLine + line.raw;
            if (editor.document.IsDisabled(index)) return CAF::Ui::Text::kDisabled2 + line.raw.substr(line.raw.find(";@disabled ") + 11);
            const auto& s = line.statement;
            const auto actor = [](int slot) { return slot ? std::format(CAF::Ui::Text::kActor, slot) : std::string(CAF::Ui::Text::kUnassignedActor); };
            std::string description;
            // 列表使用动作摘要；脚本原文仍只用于保存与运行，不作为编辑入口。
            switch (s.kind)
            {
            case Scene::Kind::Empty:
            {
                const auto start = line.raw.find(';');
                return start == std::string::npos ? CAF::Ui::Text::kEmptyOperation : CAF::Ui::Text::kNote + line.raw.substr(start+1);
            }
            case Scene::Kind::State: description = CAF::Ui::Text::kSegment + s.text; break;
            case Scene::Kind::Goto: description = CAF::Ui::Text::kGoToSegment + s.text; break;
            case Scene::Kind::Raw: description = CAF::Ui::Text::kRawLine + line.raw; break;
            case Scene::Kind::Loop: description = CAF::Ui::Text::kRepeatPlayback; break;
            case Scene::Kind::Once: description = CAF::Ui::Text::kPlayOnce; break;
            case Scene::Kind::Time: description = std::format(CAF::Ui::Text::kSceneDurationFSeconds, s.duration); break;
            case Scene::Kind::Play: description = std::format(CAF::Ui::Text::kPlayOnStartAtFS, s.text, actor(s.actor), s.offset); if (!s.rootBone.empty()) description += CAF::Ui::Text::kFrom + s.rootBone; break;
            case Scene::Kind::If: description = CAF::Ui::Text::kWhen + s.expression; break;
            case Scene::Kind::Align: case Scene::Kind::Predict:
                description = std::format(CAF::Ui::Text::kAlignTo, actor(s.actor), s.bone, actor(s.target), s.targetBone);
                if (s.kind == Scene::Kind::Predict) description += std::format(CAF::Ui::Text::kUsingThePoseAtFS, s.prediction);
                break;
            case Scene::Kind::GTSGore: description = std::format(CAF::Ui::Text::kOn2, GTSGore::Enabled() ? "" : CAF::Ui::Text::kGTSGoreDisabled, CAF::Ui::Text::DisplayName(s.text), actor(s.actor)); break;
            case Scene::Kind::Death: description = CAF::Ui::Text::kKill + actor(s.actor); break;
            case Scene::Kind::Gore: description = std::format(CAF::Ui::Text::kApplyTo, MCGore::Enabled() ? "" : CAF::Ui::Text::kMCGoreDisabled, GoreDisplayName(s.text), actor(s.actor)); break;
            case Scene::Kind::ClearGore: description = std::string(MCGore::Enabled() ? "" : CAF::Ui::Text::kMCGoreDisabled) + CAF::Ui::Text::kClearCAFMCGoreFrom + actor(s.actor); break;
            case Scene::Kind::GorePrimitive:
                if (const auto* primitive = MCGore::ParsePrimitive(s.text))
                    description = std::format(CAF::Ui::Text::kOn2, MCGore::Enabled() ? "" : CAF::Ui::Text::kMCGoreDisabled, CAF::Ui::Text::DisplayName(primitive->label), actor(s.actor));
                break;
            case Scene::Kind::Facial:
                description = std::format(CAF::Ui::Text::kOn3, Facial::Enabled() ? "" : CAF::Ui::Text::kFacialDisabled,
                    CAF::Ui::Text::DisplayName(Facial::Keyword(s.facial.category)), s.facial.operation == Facial::Operation::Reset ? CAF::Ui::Text::kLabelRESET :
                    (s.facial.operation == Facial::Operation::Restore ? CAF::Ui::Text::kLabelRESTORE :
                    std::format("{} {:.1f}%", CAF::Ui::Text::DisplayName(Facial::Names(s.facial.category)[s.facial.index]), s.facial.strength)), actor(s.actor));
                break;
            case Scene::Kind::Sound: description = std::format(CAF::Ui::Text::kPlaySoundOn, s.text, s.actor ? actor(s.actor) : CAF::Ui::Text::kTheFirstParticipant); break;
            case Scene::Kind::Footstep: description = std::format(CAF::Ui::Text::kFootstepOn, CAF::Ui::Text::DisplayName(s.text), actor(s.actor)); break;
            case Scene::Kind::AnimationSound: description = std::format(CAF::Ui::Text::kSoundPlayOn, s.text, actor(s.actor)); break;
            case Scene::Kind::Hit: description = std::format("HIT {} -> {} ({})", actor(s.actor), actor(s.target), s.mode == 1 ? "SOUND" : s.mode == 2 ? "IMPACT" : "SOUND IMPACT"); break;
            case Scene::Kind::Modify: description = std::format(CAF::Ui::Text::kSetTo, s.text, s.expression); break;
            case Scene::Kind::EndScene: description = CAF::Ui::Text::kEndTheScene; break;
            case Scene::Kind::Bind: description = std::format(CAF::Ui::Text::kAttachEffectToBinding, s.text, actor(s.actor), s.bone, s.binding); break;
            case Scene::Kind::Unbind: description = std::format(CAF::Ui::Text::kRemoveEffectBindingFrom, s.binding, actor(s.actor)); break;
            case Scene::Kind::UserOption:
                description = std::format(CAF::Ui::Text::kOption, s.option.id, s.option.text, s.option.nextState,
                    s.option.immediate ? CAF::Ui::Text::kImmediate : CAF::Ui::Text::kAtFragmentEnd, s.option.force ? " [FORCE]" : "");
                break;
            case Scene::Kind::CameraShake: description = std::format(CAF::Ui::Text::kStrengthFDurationFS, s.text, s.strength, s.duration); break;
            case Scene::Kind::TimeScale:
                description = s.text == "RESTORE" ? CAF::Ui::Text::kRestoreOriginalTimeScale : std::format(CAF::Ui::Text::kSlowGameTimeToFX, s.strength);
                if (s.text != "RESTORE" && s.duration > 0) description += std::format(CAF::Ui::Text::kForFSAtOriginalSpeed, s.duration);
                break;
            case Scene::Kind::ScreenBlood: description = std::format(CAF::Ui::Text::kAddScreenBloodDroplets, s.count); break;
            case Scene::Kind::Camera:
                description = s.text == "FIRST" ? CAF::Ui::Text::kSwitchToFirstPersonCamera : s.text == "THIRD" ? CAF::Ui::Text::kSwitchToThirdPersonCamera :
                    s.text == "FREE" ? CAF::Ui::Text::kEnterFreecam : s.text == "EXIT_FREE" ? CAF::Ui::Text::kExitFreecam :
                    s.text == "RELEASE" ? CAF::Ui::Text::kReleaseCameraControl : s.text == "FOLLOW" ?
                    std::format(CAF::Ui::Text::kFollowPositionPreserveOrientation, actor(s.actor), s.bone) : s.text == "POSITION" ?
                    std::format(CAF::Ui::Text::kMoveCameraOnceToPreserveOrientation, actor(s.actor), s.bone) : s.text == "FOCUS" ?
                    std::format(CAF::Ui::Text::kOrbit, actor(s.actor), s.bone, s.focusOnce ? CAF::Ui::Text::kFixedWorldCenter : CAF::Ui::Text::kFollowCenter) : CAF::Ui::Text::kRestoreTheCamera;
                break;
            }
            switch (s.trigger)
            {
            case Scene::Trigger::Time: description = std::format(CAF::Ui::Text::kAtFS, s.time, description); break;
            case Scene::Trigger::End: description = CAF::Ui::Text::kAtPlaybackEnd + description; break;
            case Scene::Trigger::Exit: description = CAF::Ui::Text::kOnSegmentExit + description; break;
            case Scene::Trigger::Immediate: break;
            }
            return s.continuation ? CAF::Ui::Text::kUnderTheSameCondition + description : description;
        }
        void Changed()
        {
            editor.dirty = true;
            editor.document = Scene::Document::Parse(editor.document.Source());
            // 草稿只在显式应用时替换计划；正在播放的 HKX 不受逐行编辑影响。
        }
        void Insert(std::string code, bool below)
        {
            const auto candidate = Scene::Document::Parse(code);
            if (candidate.diagnostics.empty() && !candidate.lines.empty())
            {
                auto s = candidate.lines.front().statement;
                // 插入新行自动分配身份；作者仍可显式复用 ID 来编排原位更新。
                if (s.kind == Scene::Kind::UserOption)
                {
                    int id = 1;
                    while (std::ranges::any_of(editor.document.lines, [&](const auto& line)
                        { return line.statement.kind == Scene::Kind::UserOption && line.statement.option.id == "choice" + std::to_string(id); })) ++id;
                    s.option.id = "choice" + std::to_string(id);
                    for (const auto& line : editor.document.lines)
                        if (line.statement.kind == Scene::Kind::State) { s.option.nextState = line.statement.text; break; }
                    code = Scene::Code(s);
                }
                if (s.kind == Scene::Kind::State)
                {
                    int id = 1;
                    while (std::ranges::any_of(editor.document.lines, [&](const auto& line)
                        { return line.statement.kind == Scene::Kind::State && line.statement.text == "state" + std::to_string(id); })) ++id;
                    if (std::ranges::any_of(editor.document.lines, [](const auto& line) { return line.statement.kind == Scene::Kind::State; }))
                    { s.text = "state" + std::to_string(id); code = Scene::Code(s); }
                }
                if (s.trigger == Scene::Trigger::Time && editor.snapshot.scriptScene && editor.snapshot.scenePlaying &&
                    !std::ranges::any_of(Scene::Segments(editor.document), [&](const auto& segment) { return segment.name == editor.segmentName && segment.logic; }))
                {
                    s.time = editor.snapshot.sceneTime;
                    if (s.kind == Scene::Kind::Predict)
                    { s.prediction = std::max(0.001, s.time); s.time = std::max(0.0,s.prediction-1.0); }
                    code = Scene::Code(s);
                }
            }
            const auto segments = Scene::Segments(editor.document);
            auto end = editor.document.lines.size();
            for (const auto& segment : segments) if (segment.name == editor.segmentName) end = segment.end;
            auto at = editor.selected < 0 ? end : static_cast<std::size_t>(editor.selected + (below ? 1 : 0));
            editor.document.lines.insert(editor.document.lines.begin()+at, {std::move(code), {}});
            Changed(); Select(static_cast<int>(at));
            if (editor.details.kind == Scene::Kind::Camera && editor.details.text != "RESTORE")
            {
                editor.promptCameraRestore = true;
                editor.cameraPromptAnchor = ImGui::GetMousePos();
            }
        }
        Scene::Segment CurrentSegment()
        {
            const auto segments = Scene::Segments(editor.document);
            for (const auto& segment : segments) if (segment.name == editor.segmentName) return segment;
            editor.segmentName = segments.front().name;
            return segments.front();
        }
        void OpenSegment(const Scene::Segment& segment)
        {
            editor.segmentName = segment.name; editor.segmentDraft = segment.name;
            editor.segmentOpen = true; Select(-1);
        }
        void RequestSegmentDeletion(const Scene::Segment& segment)
        {
            editor.deleteSegmentBegin = segment.begin;
            editor.deleteSegmentName = segment.name;
            editor.promptDeleteSegment = true;
        }
        void DeleteSegmentPopup()
        {
            if (editor.promptDeleteSegment)
            { ImGui::OpenPopup(CAF::Ui::Text::kDeleteSegment); editor.promptDeleteSegment = false; }
            if (!ImGui::BeginPopupModal(CAF::Ui::Text::kDeleteSegment, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
            ImGui::Text(CAF::Ui::Text::kDeleteSegmentSAndAllItsOperations, editor.deleteSegmentName.c_str());
            ImGui::TextWrapped(CAF::Ui::Text::kGOTOReferencesFromOtherSegmentsAreKeptAndWill);
            if (ImGui::Button(CAF::Ui::Text::kDeleteSegment2))
            {
                if (editor.document.EraseSegment(editor.deleteSegmentBegin))
                {
                    editor.dirty = true; editor.segmentOpen = false; Select(-1);
                    editor.segmentName = Scene::Segments(editor.document).front().name;
                    editor.segmentDraft.clear();
                    editor.status = CAF::Ui::Text::kSegmentDeletedReviewAnyMissingGOTOTargetsInThe;
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine(); if (ImGui::Button(CAF::Ui::Text::kCancel)) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        void SegmentOverview()
        {
            ImGui::TextWrapped(CAF::Ui::Text::kSTATEStartsASegmentIFGroupsAreEvaluatedOnce);
            Text(CAF::Ui::Text::kNewSegment, editor.segmentDraft);
            ImGui::Combo(CAF::Ui::Text::kStateType, &editor.newSegmentType, CAF::Ui::Text::kNormalLogic);
            ImGui::SameLine();
            if (ImGui::Button(CAF::Ui::Text::kAddSegmentSTATE))
            {
                const auto candidate = Scene::Document::Parse("STATE " + editor.segmentDraft);
                const auto segments = Scene::Segments(editor.document);
                if (candidate.lines.size() != 1 || !candidate.diagnostics.empty() || candidate.lines.front().statement.kind != Scene::Kind::State ||
                    candidate.lines.front().statement.text != editor.segmentDraft)
                    editor.status = CAF::Ui::Text::kEnterASingleSegmentNameWithoutSpaces;
                else if (!editor.document.lines.empty() && std::ranges::any_of(segments, [&](const auto& segment) { return segment.name == editor.segmentDraft; }))
                    editor.status = CAF::Ui::Text::kASegmentWithThisNameAlreadyExists;
                else
                {
                    editor.document.lines.push_back({"STATE " + editor.segmentDraft + (editor.newSegmentType ? " LOGIC" : ""), {}});
                    if (!editor.newSegmentType) editor.document.lines.push_back({"ONCE", {}});
                    Changed();
                    editor.status = CAF::Ui::Text::kSegmentAddedClickItsNodeToEdit;
                }
            }
            const auto segments = Scene::Segments(editor.document);
            if (editor.document.lines.empty())
            { ImGui::TextDisabled(CAF::Ui::Text::kNoSegmentsAddASTATEToBeginEditing); return; }
            const auto transitions = Scene::Transitions(editor.document);
            // 节点和边从原文派生，包含尚未冻结的条件分支；不会执行游戏宏。
            ImGui::BeginChild(CAF::Ui::Text::kSegmentGraph, ImVec2(0, 330), true, ImGuiWindowFlags_HorizontalScrollbar);
            const auto origin = ImGui::GetCursorScreenPos();
            const float available = std::max(240.0f, ImGui::GetContentRegionAvail().x);
            const int columns = std::max(1, static_cast<int>(available / 240));
            std::vector<ImVec2> points;
            for (std::size_t i = 0; i < segments.size(); ++i)
                points.push_back({origin.x + 35 + (i % columns) * 240.0f, origin.y + 40 + (i / columns) * 120.0f});
            auto* draw = ImGui::GetWindowDrawList();
            for (const auto& edge : transitions)
            {
                int from = -1, to = -1;
                for (int i = 0; i < static_cast<int>(segments.size()); ++i)
                { if (segments[i].name == edge.source) from = i; if (segments[i].name == edge.target) to = i; }
                if (from < 0 || to < 0) continue;
                ImVec2 start(points[from].x + 170, points[from].y + 22), end(points[to].x, points[to].y + 22);
                const auto color = edge.condition.empty() ? IM_COL32(95,205,240,230) : IM_COL32(245,190,85,230);
                const float bend = from == to ? 65.0f : 35.0f;
                const ImVec2 controlA(start.x + bend, start.y - bend), controlB(end.x - bend, end.y - bend);
                draw->AddBezierCubic(start, controlA, controlB, end, color, 2);
                const float length = std::hypot(end.x-controlB.x, end.y-controlB.y);
                const float dx = (end.x-controlB.x)/length, dy = (end.y-controlB.y)/length;
                draw->AddTriangleFilled(end, {end.x-dx*10-dy*5,end.y-dy*10+dx*5}, {end.x-dx*10+dy*5,end.y-dy*10-dx*5}, color);
            }
            for (int i = 0; i < static_cast<int>(segments.size()); ++i)
            {
                ImGui::SetCursorScreenPos(points[i]); ImGui::PushID(i);
                const auto label = segments[i].name + (segments[i].logic ? " [LOGIC]" : "") + (i == 0 ? CAF::Ui::Text::kEntry : "");
                if (ImGui::Button(label.c_str(), ImVec2(170,44))) OpenSegment(segments[i]);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(CAF::Ui::Text::kLinesZuZuClickToEditRightClickFor, segments[i].begin+1, segments[i].end);
                if (ImGui::BeginPopupContextItem(CAF::Ui::Text::kSegmentActions))
                {
                    if (ImGui::MenuItem(CAF::Ui::Text::kDeleteSegment3)) RequestSegmentDeletion(segments[i]);
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            ImGui::SetCursorScreenPos({origin.x, origin.y + 95 + ((segments.size()-1)/columns)*120.0f});
            ImGui::Dummy(ImVec2(available,1)); ImGui::EndChild();
            ImGui::TextDisabled(CAF::Ui::Text::kBlueUnconditionalGOTOGoldIFBranchFixedAtSegment);
            for (const auto& edge : transitions)
            {
                const bool exists = std::ranges::any_of(segments, [&](const auto& segment) { return segment.name == edge.target; });
                const auto trigger = edge.trigger == Scene::Trigger::End ? std::string(CAF::Ui::Text::kAtEndLabel) : edge.trigger == Scene::Trigger::Time ? std::format(CAF::Ui::Text::kAtTimeLabel, edge.time) : std::string(CAF::Ui::Text::kImmediate2);
                const auto label = std::format(CAF::Ui::Text::kTransitionSummary, edge.source, edge.target, trigger,
                    edge.condition.empty() ? CAF::Ui::Text::kAlways : "IF " + edge.condition, exists ? "" : CAF::Ui::Text::kMissingTarget, edge.line);
                if (ImGui::Selectable(label.c_str()))
                    for (const auto& segment : segments) if (segment.name == edge.source) { OpenSegment(segment); Select(static_cast<int>(edge.line-1)); break; }
            }
        }
        bool Slot(const char* label, int& slot)
        {
            return ImGui::Combo(label, &slot, CAF::Ui::Text::kUNKSlotSlotSlotSlotSlotSlotSlotSlot);
        }
        bool Bone(const char* label, int slot, std::string& name)
        {
            bool changed = Text(label, name);
            const bool inputHovered = ImGui::IsItemHovered();
            if (ImGui::IsItemActivated() || (inputHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)))
            {
                editor.activeBone = label;
                if (slot >= 1 && slot <= 8)
                {
                    Command c; c.action = Action::ReadSkeleton; c.sceneActor = true; c.slot = slot-1;
                    Enqueue(std::move(c));
                }
            }
            if (editor.activeBone == label)
            {
                // 树内操作维持选择状态；点击外部或 Escape 自动收起，文本仍可直接输入。
                const auto start = ImGui::GetCursorScreenPos();
                const float width = ImGui::GetContentRegionAvail().x;
                const bool inside = ImGui::IsMouseHoveringRect(start, ImVec2(start.x+width, start.y+240));
                const std::string id = std::string(CAF::Ui::Text::kHierarchy) + label;
                ImGui::BeginChild(id.c_str(), ImVec2(0,240), true);
                if (ImGui::Selectable("UNK", name == "UNK")) { name = "UNK"; changed = true; editor.activeBone.clear(); }
                if (slot >= 1 && slot <= 8)
                {
                    const auto& bones = editor.snapshot.sceneActors[slot-1].skeleton;
                    if (ImGui::Button(CAF::Ui::Text::kRefreshSkeleton))
                    {
                        Command c; c.action = Action::ReadSkeleton; c.sceneActor = true; c.slot = slot-1;
                        Enqueue(std::move(c));
                    }
                    if (bones.empty()) ImGui::TextDisabled(CAF::Ui::Text::kReadTheSelectedActorSSkeletonToChooseA);
                    // 快照按深度优先排列；关闭父节点时跳过其整段子树，选择文本与展开箭头独立。
                    const auto drawNode = [&](auto&& self, int& index) -> void
                    {
                        const int node = index++;
                        const auto& bone = bones[node];
                        const bool hasChildren = index < static_cast<int>(bones.size()) && bones[index].depth > bone.depth;
                        auto flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
                        if (name == bone.name) flags |= ImGuiTreeNodeFlags_Selected;
                        if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
                        ImGui::PushID(node);
                        const bool expanded = ImGui::TreeNodeEx("node", flags, "%s", bone.name.c_str());
                        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
                        {
                            name = bone.name; changed = true; editor.activeBone.clear();
                        }
                        if (hasChildren)
                        {
                            while (index < static_cast<int>(bones.size()) && bones[index].depth > bone.depth)
                            {
                                if (expanded) self(self, index); else ++index;
                            }
                            if (expanded) ImGui::TreePop();
                        }
                        ImGui::PopID();
                    };
                    int index = 0;
                    while (index < static_cast<int>(bones.size())) drawNode(drawNode, index);
                }
                ImGui::EndChild();
                if (ImGui::IsKeyPressed(ImGuiKey_Escape) || (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !inside && !inputHovered))
                    editor.activeBone.clear();
            }
            return changed;
        }
        void PickButton(const char* label, int slot, int side)
        {
            const bool available = editor.snapshot.scriptScene && editor.snapshot.scenePlaying && editor.snapshot.scenePreview &&
                !editor.snapshot.sceneSeeking && slot >= 1 && slot <= 8 && editor.snapshot.sceneActors[slot-1].loaded;
            ImGui::BeginDisabled(!available);
            if (ImGui::Button(label))
            {
                editor.pickSide = side; editor.pickSlot = slot;
                editor.pressedBone.clear(); editor.pickDragged = false;
            }
            ImGui::EndDisabled();
        }
        void Details()
        {
            if (editor.selected < 0 || editor.selected >= static_cast<int>(editor.document.lines.size())) return;
            auto s = editor.details;
            if (editor.document.lines[editor.selected].statement.kind == Scene::Kind::Raw) s.kind = Scene::Kind::Raw;
            ImGui::TextWrapped("%s", Description(editor.document.lines[editor.selected], editor.selected).c_str());
            const bool disabled = editor.document.IsDisabled(editor.selected);
            if (ImGui::Button(disabled ? CAF::Ui::Text::kEnableLine : CAF::Ui::Text::kTemporarilyDisableLine))
            {
                editor.document.ToggleDisabled(editor.selected); editor.dirty = true;
                Select(editor.selected); return;
            }
            if (CurrentSegment().logic && !Scene::LogicOperationAllowed(s.kind) && s.kind != Scene::Kind::Raw)
            {
                ImGui::TextWrapped(CAF::Ui::Text::kThisOperationIsUnsupportedInLOGICDisableOrDelete);
                Text(CAF::Ui::Text::kReplacementLine, editor.rawDraft);
                if (ImGui::Button(CAF::Ui::Text::kReplaceReparse))
                { editor.document.ReplaceRaw(editor.selected, editor.rawDraft); editor.dirty = true; Select(editor.selected); }
                return;
            }
            // 注释和未知语句直接编辑原文，不尝试恢复注释中的结构化语句。
            if (disabled || s.kind == Scene::Kind::Raw || s.kind == Scene::Kind::Empty)
            {
                Text(CAF::Ui::Text::kRawLine2, editor.rawDraft);
                if (ImGui::Button(CAF::Ui::Text::kApplyRawLineReparse))
                {
                    editor.document.ReplaceRaw(editor.selected, editor.rawDraft); editor.dirty = true; Select(editor.selected);
                }
                return;
            }
            if (s.kind == Scene::Kind::Align || s.kind == Scene::Kind::Predict || s.kind == Scene::Kind::Bind)
            {
                ImGui::Checkbox(CAF::Ui::Text::kTypePositionRotationValues, &editor.numericInput);
                ImGui::TextDisabled(CAF::Ui::Text::kDragValuesOrDoubleClickCtrlClickToType);
            }
            bool changed = false;
            if (s.kind != Scene::Kind::Empty && s.kind != Scene::Kind::If)
            {
                changed |= ImGui::Checkbox(CAF::Ui::Text::kANDSameIFGroup, &s.continuation);
                if (s.kind != Scene::Kind::State && s.kind != Scene::Kind::Loop && s.kind != Scene::Kind::Once && s.kind != Scene::Kind::Time && s.kind != Scene::Kind::Play)
                {
                    if (s.kind != Scene::Kind::UserOption)
                    {
                    int trigger = static_cast<int>(s.trigger);
                    if (CurrentSegment().logic)
                    {
                        // EXIT 在枚举中是 3；显示列表去掉 END 后必须显式映射。
                        const bool timedOnly = s.kind == Scene::Kind::Camera || s.kind == Scene::Kind::EndScene;
                        int selected = timedOnly ? (trigger == 3 ? 1 : 0) : (trigger == 3 ? 2 : trigger);
                        const char* choices = timedOnly ? CAF::Ui::Text::kATTimeATEXIT : s.kind == Scene::Kind::Goto ? CAF::Ui::Text::kImmediateATTime : CAF::Ui::Text::kImmediateATTimeATEXIT;
                        if (ImGui::Combo(CAF::Ui::Text::kTrigger, &selected, choices))
                        { s.trigger = timedOnly ? (selected ? Scene::Trigger::Exit : Scene::Trigger::Time) : selected == 2 ? Scene::Trigger::Exit : static_cast<Scene::Trigger>(selected); changed = true; }
                    }
                    else
                    {
                        const char* triggers = s.kind == Scene::Kind::Goto ? CAF::Ui::Text::kImmediateATTimeATEND : CAF::Ui::Text::kImmediateATTimeATENDATEXIT;
                        changed |= ImGui::Combo(CAF::Ui::Text::kTrigger, &trigger, triggers);
                        s.trigger = static_cast<Scene::Trigger>(trigger);
                    }
                    }
                    if (s.trigger == Scene::Trigger::Time) changed |= Number(CAF::Ui::Text::kAtSeconds, s.time);
                }
            }
            switch (s.kind)
            {
            case Scene::Kind::UserOption:
                changed |= Text(CAF::Ui::Text::kOptionID, s.option.id);
                changed |= Text(CAF::Ui::Text::kOptionText, s.option.text);
                if (ImGui::BeginCombo(CAF::Ui::Text::kNextState, s.option.nextState.c_str()))
                {
                    for (const auto& segment : Scene::Segments(editor.document))
                        if (ImGui::Selectable(segment.name.c_str(), segment.name == s.option.nextState))
                        { s.option.nextState = segment.name; changed = true; }
                    ImGui::EndCombo();
                }
                changed |= Text(CAF::Ui::Text::kNextStateName, s.option.nextState);
                changed |= ImGui::Checkbox(CAF::Ui::Text::kFORCEShowWithoutPlayer, &s.option.force);
                if (CurrentSegment().logic) ImGui::TextWrapped(CAF::Ui::Text::kEConfirmsAndImmediatelyEntersTheNextStateLOGIC);
                else
                {
                    changed |= ImGui::Checkbox(CAF::Ui::Text::kImmediateTransition, &s.option.immediate);
                    ImGui::TextWrapped(CAF::Ui::Text::kTheSameIDUpdatesInPlaceIncludingOnLOOP);
                }
                break;
            case Scene::Kind::State: changed |= Text(CAF::Ui::Text::kSegmentName, s.text); break;
            case Scene::Kind::Goto:
                if (ImGui::BeginCombo(CAF::Ui::Text::kTargetSegment, s.text.c_str()))
                {
                    for (const auto& segment : Scene::Segments(editor.document))
                        if (ImGui::Selectable(segment.name.c_str(), s.text == segment.name)) { s.text = segment.name; changed = true; }
                    ImGui::EndCombo();
                }
                break;
            case Scene::Kind::Loop: case Scene::Kind::Once:
            { bool loop = s.kind == Scene::Kind::Loop; if (ImGui::Checkbox(CAF::Ui::Text::kLoopPlayback, &loop)) { s.kind = loop ? Scene::Kind::Loop : Scene::Kind::Once; changed = true; } break; }
            case Scene::Kind::Time: changed |= Number(CAF::Ui::Text::kTotalDuration, s.duration); break;
            case Scene::Kind::Play:
                changed |= Slot(CAF::Ui::Text::kActor2, s.actor); changed |= Text(CAF::Ui::Text::kHKXPath, s.text);
                if (ImGui::BeginCombo(CAF::Ui::Text::kHKXCatalog, s.text.c_str()))
                {
                    for (const auto& path : editor.paths) if (ImGui::Selectable(path.c_str(), s.text == path)) { s.text = path; changed = true; }
                    ImGui::EndCombo();
                }
                // 仅路径改变或主动刷新时读文件，普通 UI 帧沿用结果，不启动任何播放器。
                if (editor.inspectedHkx != s.text)
                {
                    editor.inspectedHkx = s.text;
                    editor.hkxMetadata = Animation::InspectHkx(s.text);
                }
                if (ImGui::SmallButton(CAF::Ui::Text::kRefreshHKXInfo)) editor.hkxMetadata = Animation::InspectHkx(s.text);
                if (!editor.hkxMetadata.valid)
                    ImGui::TextWrapped(CAF::Ui::Text::kHKXInspectionUnavailableS, editor.hkxMetadata.error.c_str());
                else if (editor.hkxMetadata.paired)
                {
                    ImGui::Text(CAF::Ui::Text::kPairedAnimationDTracks, editor.hkxMetadata.transformTracks);
                    if (ImGui::BeginCombo(CAF::Ui::Text::kParticipantRoot, s.rootBone.empty() ? CAF::Ui::Text::kSelectAParticipant : s.rootBone.c_str()))
                    {
                        for (const auto& root : editor.hkxMetadata.roots)
                            if (ImGui::Selectable(root.c_str(), s.rootBone == root)) { s.rootBone = root; changed = true; }
                        ImGui::EndCombo();
                    }
                    if (editor.hkxMetadata.roots.empty()) ImGui::TextWrapped(CAF::Ui::Text::kNoNamedParticipantRootsFoundEnterROOTManually);
                    else if (std::find(editor.hkxMetadata.roots.begin(), editor.hkxMetadata.roots.end(), s.rootBone) == editor.hkxMetadata.roots.end())
                        ImGui::TextWrapped(CAF::Ui::Text::kChooseAParticipantRootForFullPairedPlaybackOr);
                }
                else ImGui::Text(CAF::Ui::Text::kSingleAnimationDTracks, editor.hkxMetadata.transformTracks);
                changed |= Text(CAF::Ui::Text::kSamplingRootBone, s.rootBone);
                ImGui::TextDisabled(CAF::Ui::Text::kOptionalForSingleHKXNamedBonesSelectASubtree);
                changed |= Number(CAF::Ui::Text::kClipStartOffset, s.offset); break;
            case Scene::Kind::If: changed |= Text(CAF::Ui::Text::kExpression, s.expression); break;
            case Scene::Kind::Modify: changed |= Text(CAF::Ui::Text::kContextVariable, s.text); changed |= Text(CAF::Ui::Text::kValueExpression, s.expression); break;
            case Scene::Kind::GTSGore:
                changed |= Slot(CAF::Ui::Text::kActor2, s.actor);
                if (!GTSGore::Enabled()) ImGui::TextDisabled(CAF::Ui::Text::kGTSGoreIsDisabledThisEventWillBeSkipped);
                if (ImGui::BeginCombo(CAF::Ui::Text::kGTSEvent, CAF::Ui::Text::DisplayName(s.text).data()))
                {
                    for (auto name : GTSGore::Events)
                        if (ImGui::Selectable(CAF::Ui::Text::DisplayName(name).data(), s.text == name)) { s.text = name; changed = true; }
                    ImGui::EndCombo();
                }
                changed |= Text(CAF::Ui::Text::kNode, s.goreOptions.node);
                if (s.text == "Crush" || s.text == "CrushFX" || s.text == "CrushSound")
                {
                    changed |= ImGui::Checkbox(CAF::Ui::Text::kStrongSound, &s.goreOptions.strong);
                    changed |= ImGui::Checkbox(CAF::Ui::Text::kScaleBasedPitch, &s.goreOptions.scaleBasedPitch);
                    ImGui::BeginDisabled(s.goreOptions.scaleBasedPitch);
                    changed |= ImGui::SliderFloat(CAF::Ui::Text::kPitch, &s.goreOptions.pitch, 0.25f, 4);
                    ImGui::EndDisabled();
                    changed |= ImGui::SliderInt(CAF::Ui::Text::kSoundTargetCount, &s.goreOptions.count, 1, 64);
                }
                if (s.text != "CrushSound" && s.text != "BloodSpray")
                    changed |= ImGui::SliderFloat(CAF::Ui::Text::kIntensity, &s.goreOptions.intensity, 0.1f, 4);
                ImGui::TextWrapped(CAF::Ui::Text::kRequiresGTSEspAndAssetsCrushKillsAndHides);
                break;
            case Scene::Kind::Death: changed |= Slot(CAF::Ui::Text::kActor2, s.actor); break;
            case Scene::Kind::Gore:
                changed |= Slot(CAF::Ui::Text::kActor2, s.actor);
                if (!MCGore::Enabled()) ImGui::TextDisabled(CAF::Ui::Text::kMCGoreIsDisabledThisEffectEventWillBe);
                if (ImGui::BeginCombo(CAF::Ui::Text::kGoreEffect, GoreDisplayName(s.text).data()))
                {
                    for (const auto& choice : MCGore::choices)
                        if (ImGui::Selectable(CAF::Ui::Text::DisplayName(choice.label).data(), s.text == choice.code)) { s.text = choice.code; changed = true; }
                    ImGui::EndCombo();
                }
                ImGui::TextWrapped(CAF::Ui::Text::kRequiresMaximumCarnageEspAndItsAssetsEffectsRemainUntil);
                break;
            case Scene::Kind::ClearGore: changed |= Slot(CAF::Ui::Text::kActor2, s.actor); break;
            case Scene::Kind::GorePrimitive:
            {
                changed |= Slot(CAF::Ui::Text::kActor2, s.actor);
                if (!MCGore::Enabled()) ImGui::TextDisabled(CAF::Ui::Text::kMCGoreIsDisabledThisEffectEventWillBe);
                const auto* current = MCGore::ParsePrimitive(s.text);
                if (current && !current->argument.empty() && ImGui::BeginCombo(CAF::Ui::Text::kGoreVariant, CAF::Ui::Text::DisplayName(current->label).data()))
                {
                    for (const auto& primitive : MCGore::primitives)
                        if (primitive.command == current->command && ImGui::Selectable(CAF::Ui::Text::DisplayName(primitive.label).data(), s.text == primitive.code))
                        { s.text = primitive.code; changed = true; }
                    ImGui::EndCombo();
                }
                ImGui::TextWrapped(CAF::Ui::Text::kRunsOnlyThisOperationAddSeparateSprayEquipmentDecal);
                break;
            }
            case Scene::Kind::Facial:
            {
                changed |= Slot(CAF::Ui::Text::kActor2, s.actor);
                int category = static_cast<int>(s.facial.category);
                if (ImGui::Combo(CAF::Ui::Text::kFacialCategory, &category, CAF::Ui::Text::kExpressionModifierPhoneme))
                { s.facial.category = static_cast<Facial::Category>(category); s.facial.index = 0; changed = true; }
                int operation = static_cast<int>(s.facial.operation);
                if (ImGui::Combo(CAF::Ui::Text::kOperation, &operation, CAF::Ui::Text::kSetResetCAFOverrideRestoreSavedState))
                { s.facial.operation = static_cast<Facial::Operation>(operation); changed = true; }
                if (s.facial.operation == Facial::Operation::Set)
                {
                    const auto names = Facial::Names(s.facial.category);
                    if (ImGui::BeginCombo(CAF::Ui::Text::kPresetChannel, CAF::Ui::Text::DisplayName(names[s.facial.index]).data()))
                    {
                        for (std::size_t i = 0; i < names.size(); ++i)
                            if (ImGui::Selectable(std::format("{}: {}", i, CAF::Ui::Text::DisplayName(names[i])).c_str(), s.facial.index == static_cast<int>(i)))
                            { s.facial.index = static_cast<int>(i); changed = true; }
                        ImGui::EndCombo();
                    }
                    float strength = static_cast<float>(s.facial.strength);
                    if (ImGui::SliderFloat(CAF::Ui::Text::kStrength, &strength, 0, 100, "%.1f%%"))
                    { s.facial.strength = strength; changed = true; }
                }
                ImGui::Text(CAF::Ui::Text::kFacialBackendS, CAF::Ui::Text::DisplayName(Facial::Backend()).data());
                ImGui::TextWrapped(CAF::Ui::Text::kRESETReleasesCAFOverridesInThisCategoryRESTOREReturns);
                if (s.facial.category != Facial::Category::Expression)
                    ImGui::TextWrapped(CAF::Ui::Text::kStrengthReleasesTheInputLayerItDoesNotForce);
                break;
            }
            case Scene::Kind::Sound: changed |= Text(CAF::Ui::Text::kSNDRPluginEsmLocalHexIDOrEditorID, s.text); changed |= Slot(CAF::Ui::Text::kSoundActorUNKFirstParticipant,s.actor); break;
            case Scene::Kind::Footstep:
            {
                changed |= Slot(CAF::Ui::Text::kActor2, s.actor);
                constexpr const char* tags[]{ "FootLeft", "FootRight", "FootLeft2", "FootRight2", "FootLeft3", "FootRight3", "FootFront", "FootBack", "FootSprintLeft", "FootSprintRight", "FootScuffLeft", "FootScuffRight" };
                if (ImGui::BeginCombo(CAF::Ui::Text::kFootstep, CAF::Ui::Text::DisplayName(s.text).data()))
                {
                    for (const auto* tag : tags) if (ImGui::Selectable(CAF::Ui::Text::DisplayName(tag).data(), s.text == tag)) { s.text = tag; changed = true; }
                    ImGui::EndCombo();
                }
                break;
            }
            case Scene::Kind::AnimationSound:
                changed |= Slot(CAF::Ui::Text::kActor2, s.actor); changed |= Text(CAF::Ui::Text::kSOUNEditorIDSoundPlay, s.text); break;
            case Scene::Kind::Hit:
            {
                changed |= Slot(CAF::Ui::Text::kAttackerEquippedWeapon, s.actor); changed |= Slot(CAF::Ui::Text::kImpactTarget, s.target);
                constexpr const char* modes[]{ CAF::Ui::Text::kLabelSOUND, CAF::Ui::Text::kLabelIMPACT, CAF::Ui::Text::kLabelSOUNDIMPACT };
                int mode = std::clamp(s.mode - 1, 0, 2);
                if (ImGui::Combo(CAF::Ui::Text::kHitEffects, &mode, modes, 3)) { s.mode = mode + 1; changed = true; }
                changed |= ImGui::Checkbox(CAF::Ui::Text::kUseLeftHandWeapon, &s.leftHand);
                changed |= Text(CAF::Ui::Text::kTargetBone, s.bone);
                changed |= Text(CAF::Ui::Text::kSoundMaterialSkinMeatMetalWoodStoneMATT, s.text);
                changed |= ImGui::InputFloat3(CAF::Ui::Text::kWorldRayDirection, s.position.data());
                changed |= Number(CAF::Ui::Text::kRayLength, s.duration);
                ImGui::TextWrapped(CAF::Ui::Text::kSOUNDUsesExplicitMaterialIMPACTAndSOUNDIMPACTShare);
                break;
            }
            case Scene::Kind::TimeScale:
            {
                bool restore = s.text == "RESTORE";
                if (ImGui::Checkbox(CAF::Ui::Text::kRestoreOriginalTimeScale, &restore))
                { s.text = restore ? "RESTORE" : ""; if (restore) s.duration = 0; changed = true; }
                if (!restore)
                {
                    changed |= Number(CAF::Ui::Text::kTimeMultiplier, s.strength);
                    bool timed = s.duration > 0;
                    if (ImGui::Checkbox(CAF::Ui::Text::kRestoreAfterDuration, &timed))
                    { s.duration = timed ? 1 : 0; changed = true; }
                    if (timed) changed |= Number(CAF::Ui::Text::kDurationAtOriginalSpeedSeconds, s.duration);
                    ImGui::TextWrapped(CAF::Ui::Text::kDurationCountsSlowedGameTimeSecondAtXLasts);
                }
                ImGui::TextWrapped(CAF::Ui::Text::kGlobalGameTimeIncludingScenePlaybackPersistsAcrossFragments);
                break;
            }
            case Scene::Kind::ScreenBlood:
                changed |= ImGui::InputInt(CAF::Ui::Text::kScreenBloodDroplets, &s.count);
                ImGui::TextWrapped(CAF::Ui::Text::kCountIsTheNumberOfDropletsAddedByThe);
                break;
            case Scene::Kind::CameraShake:
            {
                const char* modes[]{"SHAKE_CAMERA", "SHAKE_CAMERA_AT_ACTOR", "SHAKE_CAMERA_AT_NODE"};
                int mode = s.text == modes[1] ? 1 : s.text == modes[2] ? 2 : 0;
                if (ImGui::Combo(CAF::Ui::Text::kShakeSource, &mode, CAF::Ui::Text::kPlayerCameraActorCOMBoneSelectedBone))
                { s.text = modes[mode]; changed = true; if (s.bone.empty()) s.bone = "UNK"; }
                if (mode != 0) changed |= Slot(CAF::Ui::Text::kShakeSourceActor, s.actor);
                if (mode == 2)
                {
                    changed |= Bone(CAF::Ui::Text::kShakeSourceBone, s.actor, s.bone);
                    PickButton(CAF::Ui::Text::kPickShakeSourceBoneInWorld, s.actor, 0);
                }
                changed |= Number(mode == 0 ? CAF::Ui::Text::kShakeStrength : CAF::Ui::Text::kShakeStrength2, s.strength);
                changed |= Number(CAF::Ui::Text::kShakeDurationSeconds, s.duration);
                break;
            }
            case Scene::Kind::Camera:
            {
                const char* modes[]{"FIRST", "THIRD", "FREE", "RESTORE", "RELEASE", "EXIT_FREE", "FOLLOW", "POSITION", "FOCUS"};
                int mode = 3;
                for (int i = 0; i < 9; ++i) if (s.text == modes[i]) mode = i;
                if (ImGui::Combo(CAF::Ui::Text::kCamera, &mode, CAF::Ui::Text::kFirstPersonThirdPersonEnterFreecamRestoreCameraRelease))
                {
                    s.text = modes[mode]; s.focusOnce = false; changed = true;
                    if (s.bone.empty()) s.bone = "UNK";
                    if (s.text != "RESTORE") editor.promptCameraRestore = true;
                }
                if (s.text == "FOLLOW" || s.text == "POSITION" || s.text == "FOCUS")
                {
                    if (!CurrentSegment().logic) ImGui::Checkbox(CAF::Ui::Text::kPreviewCameraAnchor, &editor.previewCameraAnchor);
                    if (!CurrentSegment().logic && editor.previewCameraAnchor)
                    {
                        ImGui::TextWrapped(CAF::Ui::Text::kAnchorSamplesTheCurrentTimelinePoseIncludingPOSITIONONCE);
                        if (editor.snapshot.cameraAnchorValid)
                        {
                            const auto& anchor = editor.snapshot.cameraAnchorPosition;
                            ImGui::Text("%s: %.1f, %.1f, %.1f", s.text == "FOCUS" ? CAF::Ui::Text::kFocus : CAF::Ui::Text::kPosition, anchor.x, anchor.y, anchor.z);
                            if (!editor.snapshot.cameraAnchorOnScreen) ImGui::TextUnformatted(CAF::Ui::Text::kAnchorIsOutsideTheCameraView);
                        }
                        else ImGui::TextUnformatted(CAF::Ui::Text::kWaitingForALoadedTargetActorAndBone);
                    }
                    changed |= Slot(CAF::Ui::Text::kCameraTargetActor, s.actor);
                    changed |= Bone(CAF::Ui::Text::kCameraTargetBone, s.actor, s.bone);
                    if (!CurrentSegment().logic) PickButton(CAF::Ui::Text::kPickCameraTargetBoneInWorld, s.actor, 0);
                    ImGui::Checkbox(CAF::Ui::Text::kTypePositionValues, &editor.numericInput);
                    changed |= Vector(CAF::Ui::Text::kBoneLocalCameraOffset, s.position, 0.1f);
                    if (s.text == "FOCUS")
                    {
                        changed |= ImGui::Checkbox(CAF::Ui::Text::kOnceFreezeTheCenterInWorldSpace, &s.focusOnce);
                        ImGui::TextWrapped(CAF::Ui::Text::kMouseLookOrbitsTheCenterMouseWheelUsesNative);
                    }
                    else ImGui::TextWrapped(CAF::Ui::Text::kChangesCameraPositionOnlyFOLLOWTracksTheBoneEach);
                }
                break;
            }
            case Scene::Kind::Align: case Scene::Kind::Predict:
                if (s.kind == Scene::Kind::Predict) changed |= Number(CAF::Ui::Text::kPredictedContactSeconds, s.prediction);
                changed |= Slot(CAF::Ui::Text::kMovingActor, s.actor); changed |= Slot(CAF::Ui::Text::kTargetActor, s.target);
                changed |= Bone(CAF::Ui::Text::kMovingBone, s.actor, s.bone);
                PickButton(CAF::Ui::Text::kPickMovingBoneInWorld, s.actor, 0);
                changed |= Bone(CAF::Ui::Text::kTargetBone, s.target, s.targetBone);
                PickButton(CAF::Ui::Text::kPickTargetBoneInWorld, s.target, 1);
                changed |= Vector(CAF::Ui::Text::kPosTargetRightForwardUp, s.position, 0.1f);
                changed |= Vector(CAF::Ui::Text::kRotPitchRollYawRadians, s.rotation, 0.005f);
                if (ImGui::SmallButton(CAF::Ui::Text::kFaceEachOther)) { s.rotation = { 0, 0, 3.141592654f }; changed = true; }
                ImGui::SameLine();
                if (ImGui::SmallButton(CAF::Ui::Text::kSameDirection)) { s.rotation = {}; changed = true; }
                ImGui::TextWrapped(CAF::Ui::Text::kPosUsesTargetActorAxesAndGameUnitsRot);
                break;
            case Scene::Kind::Bind: case Scene::Kind::Unbind:
                changed |= Slot(CAF::Ui::Text::kActor2, s.actor); changed |= ImGui::InputInt(CAF::Ui::Text::kBindingID, &s.binding);
                if (s.kind == Scene::Kind::Bind)
                {
                    changed |= Text(CAF::Ui::Text::kARTOPluginEsmLocalHexID2, s.text); changed |= Bone(CAF::Ui::Text::kAttachBone, s.actor, s.bone);
                    PickButton(CAF::Ui::Text::kPickAttachmentBoneInWorld, s.actor, 0);
                    changed |= Number(CAF::Ui::Text::kDurationUntilRemoved, s.duration);
                    changed |= ImGui::Combo(CAF::Ui::Text::kBindingBackend, &s.mode, CAF::Ui::Text::kNativeArtObjectNIFClone);
                    changed |= Vector(CAF::Ui::Text::kBoneLocalOffset, s.position, 0.1f);
                    changed |= Vector(CAF::Ui::Text::kRotationXYZRadians, s.rotation, 0.005f);
                }
                break;
            case Scene::Kind::Empty:
            {
                const auto start = editor.document.lines[editor.selected].raw.find(';');
                std::string note = start == std::string::npos ? "" : editor.document.lines[editor.selected].raw.substr(start+1);
                if (Text(CAF::Ui::Text::kNote2, note))
                {
                    editor.document.lines[editor.selected].raw = ";" + note; Changed();
                }
                break;
            }
            default: break;
            }
            if (changed)
            {
                editor.pickSide = -1; editor.pressedBone.clear();
                editor.document.Replace(editor.selected, Scene::Code(s)); editor.dirty = true;
                editor.details = s;
                editor.rawDraft = editor.document.lines[editor.selected].raw;
                RefreshAlignmentPreview();
            }
            if (!CurrentSegment().logic && s.trigger == Scene::Trigger::Time && editor.snapshot.scenePreview && editor.snapshot.fragmentState == editor.segmentName)
            {
                if (ImGui::Button(CAF::Ui::Text::kJumpEditEvent)) Send(Action::SeekScene, static_cast<float>(s.kind == Scene::Kind::Predict ? s.prediction : s.time));
            }
            if (!CurrentSegment().logic) ImGui::TextWrapped(CAF::Ui::Text::kOffsetAndRotationEditsUpdateImmediatelyFromCachedContact);
        }
        void Actors()
        {
            if (!ImGui::CollapsingHeader(CAF::Ui::Text::kPreviewActorsIndependentOfDebugAB)) return;
            int displayedSlot = editor.actorSlot+1;
            ImGui::SliderInt(CAF::Ui::Text::kSceneSlot, &displayedSlot, 1, 8, "%d", ImGuiSliderFlags_AlwaysClamp); editor.actorSlot = displayedSlot-1;
            ImGui::Text(CAF::Ui::Text::kEditingSceneSlotD, editor.actorSlot+1);
            auto select = [&](Action action)
            { Command c; c.action = action; c.sceneActor = true; c.slot = editor.actorSlot; c.form = static_cast<RE::FormID>(std::strtoul(editor.form,nullptr,16)); Enqueue(std::move(c)); };
            ImGui::BeginDisabled(editor.snapshot.scenePlaying);
            if (ImGui::Button(CAF::Ui::Text::kUsePlayer)) select(Action::SelectPlayer); ImGui::SameLine();
            if (ImGui::Button(CAF::Ui::Text::kUseConsoleSelection)) select(Action::SelectConsole);
            ImGui::InputText(CAF::Ui::Text::kActorReferenceFormID, editor.form, sizeof(editor.form), ImGuiInputTextFlags_CharsHexadecimal);
            if (ImGui::Button(CAF::Ui::Text::kResolveActor)) select(Action::SelectForm);
            ImGui::EndDisabled();
            for (std::size_t i = 0; i < 8; ++i)
            {
                const auto& actor = editor.snapshot.sceneActors[i];
                ImGui::Text("%zu: %s [%08X] %s", i+1, actor.name.c_str(), actor.id, actor.loaded ? CAF::Ui::Text::kLoaded : CAF::Ui::Text::kUnassigned);
            }
        }
        void Context()
        {
            if (!ImGui::CollapsingHeader(CAF::Ui::Text::kContextVariables)) return;
            if (editor.snapshot.scriptScene && editor.snapshot.scenePlaying)
            {
                for (const auto& [name, value] : editor.snapshot.sceneContext) ImGui::Text("%s = %.6g", name.c_str(), value);
                ImGui::TextDisabled(CAF::Ui::Text::kStopPlaybackToEditTheContextUsedByThe); return;
            }
            for (auto it = editor.context.begin(); it != editor.context.end(); )
            {
                ImGui::PushID(it->first.c_str()); Number(it->first.c_str(), it->second); ImGui::SameLine();
                bool erase = ImGui::SmallButton(CAF::Ui::Text::kRemove); ImGui::PopID();
                if (erase) it = editor.context.erase(it); else ++it;
            }
            ImGui::InputText(CAF::Ui::Text::kName, editor.variable, sizeof(editor.variable)); Number(CAF::Ui::Text::kInitialValue, editor.value);
            if (ImGui::Button(CAF::Ui::Text::kAddSetContextVariable) && editor.variable[0] && std::isfinite(editor.value)) editor.context[editor.variable] = editor.value;
            if (ImGui::Button(CAF::Ui::Text::kUseLastRuntimeContext)) editor.context = editor.snapshot.sceneContext;
        }
        void InsertMenu()
        {
            ImGui::SetNextWindowSize(ImVec2(std::min(760.0f, ImGui::GetMainViewport()->WorkSize.x), 350), ImGuiCond_Appearing);
            if (ImGui::BeginPopup(CAF::Ui::Text::kInsertOperation))
            {
                struct Operation { const char* label; const char* code; const char* category; };
                // 标签与脚本模板绑定，功能模块的命令在自己的分类中维护。
                static constexpr Operation operations[]{
                    {CAF::Ui::Text::kShowUserOption, "AT 0 USER_OPTION choice \"Continue\" TO default", CAF::Ui::Text::kLogic},
                    {CAF::Ui::Text::kGoToAnotherSegment, "AT END GOTO default", CAF::Ui::Text::kLogic},
                    {CAF::Ui::Text::kAddARawLine, "; edit this raw line", CAF::Ui::Text::kBasic},
                    {CAF::Ui::Text::kRepeatPlayback, "LOOP", CAF::Ui::Text::kBasic},
                    {CAF::Ui::Text::kPlayOnce, "ONCE", CAF::Ui::Text::kBasic},
                    {CAF::Ui::Text::kSetSceneDuration, "TIME 10", CAF::Ui::Text::kBasic},
                    {CAF::Ui::Text::kPlayAnAnimation, "1 PLAY test.hkx", CAF::Ui::Text::kBasic},
                    {CAF::Ui::Text::kEndTheScene, "AT END END_SCENE", CAF::Ui::Text::kBasic},
                    {CAF::Ui::Text::kAddANote, "; comment", CAF::Ui::Text::kBasic},
                    {CAF::Ui::Text::kAlignActors, "AT 0 ALIGN UNK UNK TO UNK UNK 0 0 0 0 0 0", CAF::Ui::Text::kAlignment},
                    {CAF::Ui::Text::kAlignUsingAFuturePose, "AT 0 PREDICT_ALIGN 1 UNK UNK TO UNK UNK 0 0 0 0 0 0", CAF::Ui::Text::kAlignment},
                    {CAF::Ui::Text::kSetExpression, "AT 0 EXPRESSION 1 MoodHappy 80", CAF::Ui::Text::kFacial},
                    {CAF::Ui::Text::kResetExpression, "AT 0 EXPRESSION 1 RESET", CAF::Ui::Text::kFacial},
                    {CAF::Ui::Text::kRestoreExpression, "AT 0 EXPRESSION 1 RESTORE", CAF::Ui::Text::kFacial},
                    {CAF::Ui::Text::kSetModifier, "AT 0 MODIFIER 1 BrowUpLeft 50", CAF::Ui::Text::kFacial},
                    {CAF::Ui::Text::kResetModifiers, "AT 0 MODIFIER 1 RESET", CAF::Ui::Text::kFacial},
                    {CAF::Ui::Text::kRestoreModifiers, "AT 0 MODIFIER 1 RESTORE", CAF::Ui::Text::kFacial},
                    {CAF::Ui::Text::kSetPhoneme, "AT 0 PHONEME 1 Oh 40", CAF::Ui::Text::kFacial},
                    {CAF::Ui::Text::kResetPhonemes, "AT 0 PHONEME 1 RESET", CAF::Ui::Text::kFacial},
                    {CAF::Ui::Text::kRestorePhonemes, "AT 0 PHONEME 1 RESTORE", CAF::Ui::Text::kFacial},
                    {CAF::Ui::Text::kAddAFootstep, "AT 0 FOOTSTEP 1 FootLeft", CAF::Ui::Text::kAudioImpact},
                    {CAF::Ui::Text::kAddHITSoundImpact, "AT 0 HIT 1 TO 2 SOUND IMPACT RIGHT \"NPC Spine2 [Spn2]\" \"Skin\" 0 0 -1 100", CAF::Ui::Text::kAudioImpact},
                    {CAF::Ui::Text::kPlayAnAnnotationSound, "AT 0 SOUND_PLAY 1 \"WPNSwingBladeMedium\"", CAF::Ui::Text::kAudioImpact},
                    {CAF::Ui::Text::kPlayASound, "AT 0 PLAY_SOUND \"soundname\"", CAF::Ui::Text::kAudioImpact},
                    {CAF::Ui::Text::kShakePlayerCamera, "AT 0 SHAKE_CAMERA 0.5 0.3", CAF::Ui::Text::kCamera},
                    {CAF::Ui::Text::kShakeAtActorCOM, "AT 0 SHAKE_CAMERA_AT_ACTOR 1 0.5 0.3", CAF::Ui::Text::kCamera},
                    {CAF::Ui::Text::kShakeAtABone, "AT 0 SHAKE_CAMERA_AT_NODE UNK UNK 0.5 0.3", CAF::Ui::Text::kCamera},
                    {CAF::Ui::Text::kSlowGameTime, "AT 0 TIME_SCALE 0.2", CAF::Ui::Text::kEffects},
                    {CAF::Ui::Text::kRestoreGameTime, "AT 0 TIME_SCALE RESTORE", CAF::Ui::Text::kEffects},
                    {CAF::Ui::Text::kAddScreenBlood, "AT 0 SCREEN_BLOOD 50", CAF::Ui::Text::kCamera},
                    {CAF::Ui::Text::kRestoreCamera, "AT EXIT CAMERA RESTORE", CAF::Ui::Text::kCamera},
                    {CAF::Ui::Text::kReleaseCameraControl, "AT 0 CAMERA RELEASE", CAF::Ui::Text::kCamera},
                    {CAF::Ui::Text::kEnterFreecam, "AT 0 CAMERA FREE", CAF::Ui::Text::kCamera},
                    {CAF::Ui::Text::kExitFreecam, "AT 0 CAMERA EXIT_FREE", CAF::Ui::Text::kCamera},
                    {CAF::Ui::Text::kFollowBonePosition, "AT 0 CAMERA FOLLOW UNK UNK 0 0 0", CAF::Ui::Text::kCamera},
                    {CAF::Ui::Text::kMoveCameraToBoneOnce, "AT 0 CAMERA POSITION UNK UNK 0 0 0", CAF::Ui::Text::kCamera},
                    {CAF::Ui::Text::kFocusOrbitBone, "AT 0 CAMERA FOCUS UNK UNK 0 0 0", CAF::Ui::Text::kCamera},
                    {CAF::Ui::Text::kFocusOrbitFixedPoint, "AT 0 CAMERA FOCUS ONCE UNK UNK 0 0 0", CAF::Ui::Text::kCamera},
                    {CAF::Ui::Text::kAttachAnEffect, "AT 0 BIND_ART 1 1 \"Skyrim.esm|123ABC\" \"NPC Root\" 0 1 0 0 0 0 0 0", CAF::Ui::Text::kEffects},
                    {CAF::Ui::Text::kRemoveAnEffect, "AT 0 UNBIND_ART 1 1", CAF::Ui::Text::kEffects},
                    {CAF::Ui::Text::kKillAnActor, "AT 0 DEATH 1", CAF::Ui::Text::kLogic},
                    {CAF::Ui::Text::kAddACondition, "IF !ShouldKill", CAF::Ui::Text::kLogic},
                    {CAF::Ui::Text::kSetAContextVariable, "MODIFY_VALUE custom111 custom111+3", CAF::Ui::Text::kLogic},
                    {CAF::Ui::Text::kApplyMCGore, "AT 0 APPLY_GORE 1 THROAT_SLIT", CAF::Ui::Text::kMCGore},
                    {CAF::Ui::Text::kClearMCGore, "AT 0 CLEAR_GORE 1", CAF::Ui::Text::kMCGore},
                    {CAF::Ui::Text::kBloodSpray, "AT 0 GORE_BLOOD 1 NECK", CAF::Ui::Text::kMCGore},
                    {CAF::Ui::Text::kWoundEquipment, "AT 0 GORE_EQUIP 1 NECK", CAF::Ui::Text::kMCGore},
                    {CAF::Ui::Text::kBloodDecal, "AT 0 GORE_DECAL 1 NORMAL", CAF::Ui::Text::kMCGore},
                    {CAF::Ui::Text::kDebrisBurst, "AT 0 GORE_DEBRIS 1 GENERIC", CAF::Ui::Text::kMCGore},
                    {CAF::Ui::Text::kHideTorso, "AT 0 GORE_HIDE_TORSO 1", CAF::Ui::Text::kMCGore},
                    {CAF::Ui::Text::kUncoverHead, "AT 0 GORE_UNCOVER_HEAD 1", CAF::Ui::Text::kMCGore}
                };
                static bool below = false;
                const bool logic = CurrentSegment().logic;
                if (!logic && GTSGore::Enabled())
                    for (auto name : GTSGore::Events)
                        if (ImGui::Selectable((std::string(CAF::Ui::Text::kGTSGore) + std::string(CAF::Ui::Text::DisplayName(name))).c_str()))
                            Insert(std::string("AT 0 ") + std::string(name) + " 1", below);
                ImGui::Checkbox(CAF::Ui::Text::kInsertBelowSelectedLine, &below);
                ImGui::Separator();
                if (ImGui::BeginTabBar("Operation categories", ImGuiTabBarFlags_FittingPolicyScroll))
                {
                    for (const char* category : {CAF::Ui::Text::kBasic, CAF::Ui::Text::kAlignment, CAF::Ui::Text::kAudioImpact, CAF::Ui::Text::kCamera, CAF::Ui::Text::kEffects, CAF::Ui::Text::kFacial, CAF::Ui::Text::kLogic, CAF::Ui::Text::kMCGore})
                    {
                        if (logic && std::string_view(category) != CAF::Ui::Text::kBasic && std::string_view(category) != CAF::Ui::Text::kCamera && std::string_view(category) != CAF::Ui::Text::kEffects && std::string_view(category) != CAF::Ui::Text::kLogic) continue;
                        if (!ImGui::BeginTabItem(category)) continue;
                        ImGui::BeginChild("Operation choices", ImVec2(0, 240));
                        const bool disabled = (std::string_view(category) == CAF::Ui::Text::kMCGore && !MCGore::Enabled()) || (std::string_view(category) == CAF::Ui::Text::kFacial && !Facial::Enabled());
                        if (disabled) ImGui::TextWrapped(CAF::Ui::Text::kEnableThisFeatureInInterfaceSettingsToInsertThese);
                        ImGui::BeginDisabled(disabled);
                        for (const auto& operation : operations)
                        {
                            if (std::string_view(operation.category) != category) continue;
                            std::string code = operation.code;
                            if (logic)
                            {
                                if (code.starts_with("AT END ")) code.replace(0, 6, "AT 0");
                                const auto candidate = Scene::Document::Parse(code);
                                if (candidate.lines.empty() || !Scene::LogicOperationAllowed(candidate.lines.front().statement.kind)) continue;
                            }
                            if (ImGui::Selectable(operation.label)) { Insert(code, below); ImGui::CloseCurrentPopup(); }
                        }
                        ImGui::EndDisabled();
                        ImGui::EndChild();
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
                ImGui::EndPopup();
            }
        }
        bool ProjectBones()
        {
            if (editor.pickSide < 0 || editor.pickSlot < 1 || editor.pickSlot > 8) return false;
            const auto& bones = editor.snapshot.sceneActors[editor.pickSlot-1].skeleton;
            auto* draw = ImGui::GetBackgroundDrawList();
            const auto& io = ImGui::GetIO();
            const auto origin = ImGui::GetMainViewport()->Pos;
            const bool overUI = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
            float best = 144;
            int hovered = -1;
            const auto point = [&](const ActorInfo::BoneInfo& b)
            { return ImVec2(origin.x+b.screenX*io.DisplaySize.x, origin.y+SceneMath::ScreenY(b.screenY)*io.DisplaySize.y); };
            // 点选只显示当前目标演员，连线与节点都在 UI 窗口下面绘制。
            for (int i = 0; i < static_cast<int>(bones.size()); ++i)
            {
                const auto& bone = bones[i];
                if (!bone.onScreen) continue;
                const auto at = point(bone);
                if (bone.parent >= 0 && bone.parent < static_cast<int>(bones.size()) && bones[bone.parent].onScreen)
                    draw->AddLine(at, point(bones[bone.parent]), IM_COL32(80,200,255,180));
                draw->AddCircleFilled(at,3.5f,IM_COL32(80,200,255,220));
                const float distance = (at.x-io.MousePos.x)*(at.x-io.MousePos.x)+(at.y-io.MousePos.y)*(at.y-io.MousePos.y);
                if (!overUI && (distance < best || (distance == best && hovered >= 0 &&
                    bone.cameraDistanceSquared < bones[hovered].cameraDistanceSquared)))
                { best = distance; hovered = i; }
            }
            if (hovered >= 0)
            {
                draw->AddCircle(point(bones[hovered]),8,IM_COL32(255,255,255,255),16,2);
                ImGui::SetTooltip(CAF::Ui::Text::kSlotDS, editor.pickSlot,bones[hovered].name.c_str());
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                editor.pressedBone = hovered >= 0 ? bones[hovered].name : std::string{};
                editor.pickPress = io.MousePos; editor.pickDragged = false;
            }
            if (!editor.pressedBone.empty())
            {
                const auto delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0);
                const float dx = io.MousePos.x-editor.pickPress.x, dy = io.MousePos.y-editor.pickPress.y;
                const float threshold = io.MouseDragThreshold*io.MouseDragThreshold;
                editor.pickDragged |= ImGui::IsMouseDragging(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
                    dx*dx+dy*dy >= threshold || delta.x*delta.x+delta.y*delta.y >= threshold;
            }
            if (!ImGui::IsMouseReleased(ImGuiMouseButton_Left)) return false;
            // 松开且未拖动才提交节点；从节点起步的相机拖动也不会误选。
            const bool selected = !editor.pickDragged && !editor.pressedBone.empty() && hovered >= 0 &&
                editor.pressedBone == bones[hovered].name;
            editor.pressedBone.clear();
            if (!selected) return false;
            auto statement = editor.details;
            if (editor.pickSide == 0) statement.bone = bones[hovered].name; else statement.targetBone = bones[hovered].name;
            editor.document.Replace(editor.selected,Scene::Code(statement)); editor.dirty = true;
            Select(editor.selected);
            RefreshAlignmentPreview();
            return true;
        }
        void PreviewCameraAnchor()
        {
            const auto& s = editor.details;
            const bool enabled = editor.open && !CurrentSegment().logic && editor.previewCameraAnchor && editor.selected >= 0 &&
                s.kind == Scene::Kind::Camera && (s.text == "FOLLOW" || s.text == "POSITION" || s.text == "FOCUS");
            Command command; command.action = Action::SetCameraAnchorPreview;
            command.value = enabled ? 1.0f : 0.0f; command.slot = s.actor; command.text = s.bone;
            command.position = {s.position[0], s.position[1], s.position[2]};
            // 游戏线程负责骨骼采样和投影；绘制线程只读取快照，不拦截相机拖动。
            Enqueue(std::move(command));
            if (!enabled || !editor.snapshot.cameraAnchorValid || !editor.snapshot.cameraAnchorOnScreen) return;
            const auto& io = ImGui::GetIO();
            const auto origin = ImGui::GetMainViewport()->Pos;
            const ImVec2 at{origin.x + editor.snapshot.cameraAnchorScreenX * io.DisplaySize.x,
                origin.y + SceneMath::ScreenY(editor.snapshot.cameraAnchorScreenY) * io.DisplaySize.y};
            auto* draw = ImGui::GetBackgroundDrawList();
            const auto color = IM_COL32(255, 210, 70, 255);
            draw->AddCircle(at, 9, color, 24, 2);
            draw->AddLine({at.x - 15, at.y}, {at.x + 15, at.y}, color, 2);
            draw->AddLine({at.x, at.y - 15}, {at.x, at.y + 15}, color, 2);
            const auto& p = editor.snapshot.cameraAnchorPosition;
            const auto label = std::format("{} ({:.1f}, {:.1f}, {:.1f})",
                s.text == "FOCUS" ? CAF::Ui::Text::kCameraFocus : CAF::Ui::Text::kCameraPosition, p.x, p.y, p.z);
            draw->AddText({at.x + 18, at.y - 8}, color, label.c_str());
        }
        void UpdateProjection()
        {
            static bool requested = false;
            const bool wanted = editor.open && editor.pickSide >= 0;
            if (requested == wanted) return;
            Command c; c.action = Action::SetBoneProjection; c.value = wanted ? 1.0f : 0.0f; c.sceneActor = true;
            Enqueue(std::move(c)); requested = wanted;
        }

    }
    void Draw()
    {
        ImGui::TextWrapped(CAF::Ui::Text::kSelectOrCreateASceneFileThenOpenIt);
        const auto files = Scene::Files();
        if (ImGui::BeginCombo(CAF::Ui::Text::kSceneFile, editor.selectedFile))
        {
            for (const auto& name : files)
                if (ImGui::Selectable(name.c_str(), name == editor.selectedFile))
                    std::snprintf(editor.selectedFile, sizeof(editor.selectedFile), "%s", name.c_str());
            ImGui::EndCombo();
        }
        const auto openSelected = []
        {
            if (auto document = Scene::Load(editor.selectedFile, editor.fileStatus))
            {
                editor.document = std::move(*document);
                std::snprintf(editor.name, sizeof(editor.name), "%s", editor.selectedFile);
                editor.dirty = false; editor.open = true; editor.status = CAF::Ui::Text::kSceneLoaded;
                editor.promptCameraRestore = false; editor.segmentOpen = false; editor.segmentName = Scene::Segments(editor.document).front().name; Select(-1);
            }
        };
        if (ImGui::Button(CAF::Ui::Text::kOpenSceneEditor))
        {
            if (editor.dirty) ImGui::OpenPopup(CAF::Ui::Text::kDiscardUnsavedScene);
            else openSelected();
        }
        if (ImGui::BeginPopup(CAF::Ui::Text::kDiscardUnsavedScene))
        {
            ImGui::TextWrapped(CAF::Ui::Text::kOpeningAFileReplacesTheUnsavedDraftForS, editor.name);
            if (ImGui::Button(CAF::Ui::Text::kDiscardAndOpen)) { openSelected(); ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
            if (ImGui::Button(CAF::Ui::Text::kCancel)) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::Separator();
        ImGui::InputText(CAF::Ui::Text::kNewSceneName, editor.newFile, sizeof(editor.newFile));
        if (ImGui::Button(CAF::Ui::Text::kCreateSceneFile))
        {
            const auto path = Scene::FilePath(editor.newFile);
            std::error_code error;
            if (path.empty()) editor.fileStatus = CAF::Ui::Text::kUseLettersNumbersOrForTheFileName;
            else if (std::filesystem::exists(path, error) || error)
                editor.fileStatus = error ? error.message() : CAF::Ui::Text::kASceneWithThisNameAlreadyExists;
            else if (Scene::Save(editor.newFile, Scene::Document::Parse("STATE default\nLOOP\n"), editor.fileStatus))
            {
                std::snprintf(editor.selectedFile, sizeof(editor.selectedFile), "%s", editor.newFile);
                editor.fileStatus = CAF::Ui::Text::kSceneCreatedOpenItToBeginEditing;
            }
        }
        ImGui::TextWrapped("%s", CAF::Ui::Text::Diagnostic(editor.fileStatus).c_str());
    }
    bool DrawWindows()
    {
        editor.snapshot = ReadSnapshot();
        if (!editor.open || !editor.snapshot.scriptScene || !editor.snapshot.scenePlaying || !editor.snapshot.scenePreview ||
            editor.selected < 0 || editor.selected >= static_cast<int>(editor.document.lines.size()))
        { editor.pickSide = -1; editor.pressedBone.clear(); }
        UpdateProjection();
        if (!editor.open) return false;
        editor.paths = Animation::Paths();
        const auto* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowSize(ImVec2(std::min(1440.0f, viewport->WorkSize.x * 0.94f),
            std::min(900.0f, viewport->WorkSize.y * 0.84f)), ImGuiCond_FirstUseEver);
        const bool expanded = ImGui::Begin(CAF::Ui::Text::kCAFSceneEditor, &editor.open);
        bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
        if (expanded)
        {
            ImGui::Text(CAF::Ui::Text::kSceneSSceneS, editor.name, editor.dirty ? " *" : "");
            if (ImGui::Button(CAF::Ui::Text::kSave))
                if (Scene::Save(editor.name, editor.document, editor.status)) { editor.status = CAF::Ui::Text::kSceneSaved2; editor.dirty = false; }
            ImGui::SameLine();
            if (ImGui::Button(CAF::Ui::Text::kLoadFromDisk))
            {
                if (editor.dirty) ImGui::OpenPopup(CAF::Ui::Text::kReloadSceneFromDisk);
                else if (auto document = Scene::Load(editor.name, editor.status))
                { editor.document = std::move(*document); editor.segmentOpen = false; Select(-1); editor.status = CAF::Ui::Text::kSceneReloaded; }
            }
            if (ImGui::BeginPopup(CAF::Ui::Text::kReloadSceneFromDisk))
            {
                ImGui::TextUnformatted(CAF::Ui::Text::kReloadingDiscardsTheUnsavedChangesToThisFile);
                if (ImGui::Button(CAF::Ui::Text::kDiscardAndReload))
                {
                    if (auto document = Scene::Load(editor.name, editor.status))
                    { editor.document = std::move(*document); editor.segmentOpen = false; editor.dirty = false; Select(-1); editor.status = CAF::Ui::Text::kSceneReloaded; }
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine(); if (ImGui::Button(CAF::Ui::Text::kCancel)) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            ImGui::SameLine(); ImGui::TextUnformatted(editor.dirty ? CAF::Ui::Text::kUnsavedChanges : CAF::Ui::Text::kSaved);
            ImGui::TextWrapped("%s", CAF::Ui::Text::Diagnostic(editor.status).c_str());
            const bool logicPage = editor.segmentOpen && CurrentSegment().logic;
            if (!logicPage) Actors();
            Context();
            if (!logicPage)
            {
                ImGui::BeginDisabled(editor.document.lines.empty() || !editor.document.diagnostics.empty());
                if (ImGui::Button(CAF::Ui::Text::kPlay)) Start(false); ImGui::SameLine();
                if (ImGui::Button(CAF::Ui::Text::kPreviewApplyDraft)) Start(true);
                ImGui::EndDisabled(); ImGui::SameLine();
            }
            if (ImGui::Button(CAF::Ui::Text::kStop)) Send(Action::StopScene);
            if (editor.snapshot.scriptScene && editor.snapshot.scenePlaying)
            {
                ImGui::Text(CAF::Ui::Text::kActiveSegmentS, editor.snapshot.fragmentState.c_str());
                const bool durationKnown = editor.snapshot.sceneDuration > 0;
                if (!editor.snapshot.scenePreview)
                {
                    ImGui::SameLine();
                    if (ImGui::Button(editor.snapshot.scenePaused ? CAF::Ui::Text::kResume : CAF::Ui::Text::kPause))
                        Send(Action::SetScenePaused,editor.snapshot.scenePaused ? 0 : 1);
                    if (!editor.snapshot.sceneLogic) ImGui::TextDisabled(CAF::Ui::Text::kEnterPreviewToEditTheTimeline);
                }
                if (editor.snapshot.sceneLogic) ImGui::Text(CAF::Ui::Text::kLogicTimeFSNoEND, editor.snapshot.sceneTime);
                else if (!durationKnown) ImGui::TextWrapped(CAF::Ui::Text::kWaitingForHKXDurationTIMESetsTheTimelineRange);
            }
            // 场景语法错误属于编辑反馈；全局运行日志与姿态诊断留在调试页。
            for (const auto& diagnostic : editor.document.diagnostics) ImGui::TextWrapped(CAF::Ui::Text::kLineZuS,diagnostic.line,CAF::Ui::Text::Diagnostic(diagnostic.message).c_str());
            if (!editor.segmentOpen) SegmentOverview();
            if (editor.segmentOpen)
            {
                const auto segment = CurrentSegment();
                if (ImGui::Button(CAF::Ui::Text::kSegmentsTransitionGraph)) { editor.segmentOpen = false; Select(-1); }
                ImGui::SameLine(); ImGui::Text(CAF::Ui::Text::kEditingSegmentS, segment.name.c_str());
                ImGui::SameLine();
                if (ImGui::Button(CAF::Ui::Text::kDeleteSegment3)) RequestSegmentDeletion(segment);
                if (segment.explicitState)
                {
                    Text(CAF::Ui::Text::kRenameSegment, editor.segmentDraft); ImGui::SameLine();
                    if (ImGui::Button(CAF::Ui::Text::kApplyName))
                    {
                        const auto candidate = Scene::Document::Parse("STATE " + editor.segmentDraft);
                        const auto all = Scene::Segments(editor.document);
                        if (candidate.lines.size() == 1 && candidate.diagnostics.empty() && candidate.lines.front().statement.text == editor.segmentDraft &&
                            !std::ranges::any_of(all, [&](const auto& other) { return other.name == editor.segmentDraft && other.begin != segment.begin; }))
                        {
                            for (std::size_t i = 0; i < editor.document.lines.size(); ++i)
                            {
                                auto statement = editor.document.lines[i].statement;
                                // 重命名同时更新两种跳转的目标，避免用户选项留下旧名称。
                                if (statement.kind == Scene::Kind::UserOption && statement.option.nextState == segment.name)
                                { statement.option.nextState = editor.segmentDraft; editor.document.Replace(i, Scene::Code(statement)); }
                                if ((statement.kind == Scene::Kind::Goto && statement.text == segment.name) || i == segment.stateLine)
                                { statement.text = editor.segmentDraft; editor.document.Replace(i, Scene::Code(statement)); }
                            }
                            editor.segmentName = editor.segmentDraft; Changed(); Select(-1);
                        }
                        else editor.status = CAF::Ui::Text::kInvalidOrDuplicateSegmentName;
                    }
                }
                if (ImGui::Button(CAF::Ui::Text::kInsertOperation)) ImGui::OpenPopup(CAF::Ui::Text::kInsertOperation);
                InsertMenu();
                // 插入菜单关闭后再打开模态框，避免弹窗层级随菜单一起消失。
                if (editor.promptCameraRestore)
                { ImGui::OpenPopup(CAF::Ui::Text::kRestoreCameraOnExit); editor.promptCameraRestore = false; }
                Ui::SetNextWindowNearPoint(editor.cameraPromptAnchor, ImVec2(560, 210), ImGuiCond_Appearing);
                if (ImGui::BeginPopupModal(CAF::Ui::Text::kRestoreCameraOnExit, nullptr, ImGuiWindowFlags_NoResize))
                {
                    auto context = editor.context;
                    const auto plan = Scene::Prepare(editor.document, context, {}, editor.segmentName, false);
                    const bool exists = std::ranges::any_of(plan.events, [](const auto& event)
                    {
                        return event.statement.kind == Scene::Kind::Camera && event.statement.text == "RESTORE" &&
                            event.statement.trigger == Scene::Trigger::Exit && (event.condition.empty() || event.condition == "1");
                    });
                    ImGui::TextWrapped(CAF::Ui::Text::kAppendATEXITCAMERARESTOREAtTheEndTo);
                    if (exists) ImGui::TextWrapped(CAF::Ui::Text::kAnUnconditionalExitRestoreAlreadyExistsNoDuplicateWill);
                    if (ImGui::Button(CAF::Ui::Text::kYesRestoreOnExit))
                    {
                        if (!exists)
                        {
                            // 尾部孤立 IF 不能把恢复操作变成有条件执行。
                            const auto current = CurrentSegment();
                            auto at = current.end;
                            if (at > current.begin && editor.document.lines[at-1].statement.kind == Scene::Kind::If)
                            { editor.document.lines.insert(editor.document.lines.begin()+at, {"IF 1", {}}); ++at; }
                            editor.document.lines.insert(editor.document.lines.begin()+at, {"AT EXIT CAMERA RESTORE", {}}); Changed();
                        }
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(CAF::Ui::Text::kNoKeepCurrentEvents)) ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
                ImGui::SameLine();
                const char* inserted = segment.logic ? "AT 0 MODIFY_VALUE value value+1" : "AT 0 ALIGN UNK UNK TO UNK UNK 0 0 0 0 0 0";
                if (ImGui::Button(CAF::Ui::Text::kInsertAbove)) Insert(inserted,false);
                ImGui::SameLine(); if (ImGui::Button(CAF::Ui::Text::kInsertBelow)) Insert(inserted,true);
                ImGui::SameLine(); if (ImGui::Button(CAF::Ui::Text::kDeleteSelected) && editor.selected >= 0) { editor.document.lines.erase(editor.document.lines.begin()+editor.selected); Changed(); Select(-1); }
                ImGui::BeginChild("Rows",ImVec2(ImGui::GetContentRegionAvail().x*0.48f,0),true);
                int moveFrom = -1, moveTo = -1;
                const auto rowSegment = CurrentSegment();
                for (int i = static_cast<int>(rowSegment.begin); i < static_cast<int>(rowSegment.end); ++i)
                {
                    const auto& line = editor.document.lines[i];
                    if (line.statement.kind == Scene::Kind::State) continue;
                    if (line.raw.find_first_not_of(" \t\r") == std::string::npos) continue;
                    ImGui::PushID(i);
                    const auto description = Description(line, i);
                    const auto label = std::to_string(i+1)+"  "+description+"###operation";
                    if (ImGui::Selectable(label.c_str(),editor.selected==i,0,ImVec2(0,ImGui::GetTextLineHeightWithSpacing()*1.5f))) Select(i);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", description.c_str());
                    if (ImGui::BeginDragDropSource()) { ImGui::SetDragDropPayload("CAF_SCENE_LINE",&i,sizeof(i)); ImGui::TextUnformatted(description.c_str()); ImGui::EndDragDropSource(); }
                    if (ImGui::BeginDragDropTarget()) { if (const auto* payload = ImGui::AcceptDragDropPayload("CAF_SCENE_LINE")) { moveFrom = *static_cast<const int*>(payload->Data); moveTo = i; } ImGui::EndDragDropTarget(); }
                    ImGui::PopID();
                }
                ImGui::EndChild();
                if (moveFrom >= static_cast<int>(rowSegment.begin) && moveFrom < static_cast<int>(rowSegment.end) && moveFrom != moveTo)
                {
                    auto line = std::move(editor.document.lines[moveFrom]); editor.document.lines.erase(editor.document.lines.begin()+moveFrom);
                    editor.document.lines.insert(editor.document.lines.begin()+moveTo,std::move(line)); Changed(); Select(moveTo);
                }
                ImGui::SameLine(); ImGui::BeginChild("Details",ImVec2(0,0),true); Details(); ImGui::EndChild();
                hovered |= ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
            }
            DeleteSegmentPopup();
        }
        ImGui::End();
        if (editor.open && editor.segmentOpen && !CurrentSegment().logic && editor.snapshot.scriptScene && editor.snapshot.scenePlaying && editor.snapshot.scenePreview && editor.snapshot.fragmentState == editor.segmentName)
        {
            // 独立窗口沿用时间线交互与返回意图，鼠标悬停仍阻止游戏相机拖动。
            ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                viewport->WorkPos.y + viewport->WorkSize.y - 20), ImGuiCond_FirstUseEver, ImVec2(0.5f, 1));
            ImGui::SetNextWindowSize(ImVec2(std::min(1200.0f, viewport->WorkSize.x * 0.9f),
                std::min(320.0f, viewport->WorkSize.y * 0.45f)), ImGuiCond_FirstUseEver);
            const bool timelineExpanded = ImGui::Begin(CAF::Ui::Text::kCAFSceneEditorTimeline);
            hovered |= ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
            if (timelineExpanded)
            {
                ImGui::Text("%s.scene | %.3f / %.3f s %s", editor.name, editor.snapshot.sceneTime,
                    editor.snapshot.sceneDuration, editor.snapshot.sceneSeeking ? CAF::Ui::Text::kSampling : "");
                ImGui::BeginDisabled(editor.snapshot.sceneDuration <= 0);
                const auto current = CurrentSegment();
                const auto timeline = DrawTimeline(editor.document, editor.snapshot.sceneTime,
                    std::max(0.001f, editor.snapshot.sceneDuration), editor.snapshot.previewPlaying, current.begin, current.end);
                if (timeline.selectedLine >= 0) Select(timeline.selectedLine);
                if (timeline.seek) Send(Action::SeekScene, timeline.time);
                if (timeline.togglePlayback) Send(Action::SetPreviewPlaying, editor.snapshot.previewPlaying ? 0 : 1);
                ImGui::EndDisabled();
            }
            ImGui::End();
        }
        if (!editor.open) { editor.pickSide = -1; editor.pressedBone.clear(); editor.activeBone.clear(); }
        PreviewCameraAnchor();
        const bool pickedNode = editor.open && ProjectBones();
        UpdateProjection();
        return hovered || pickedNode;
    }
}
