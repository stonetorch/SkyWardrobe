#include "scene/ScenePlaybackPlan.h"
#include <stdexcept>
#include <algorithm>
using namespace CAF::Scene;
void Check(bool value) { if (!value) throw std::runtime_error("Logic playback plan regression failed"); }
int main()
{
    Context facialContext;
    std::string facialError;
    const auto facialDocument = Document::Parse("1 PLAY actor.hkx\nAT 0 EXPRESSION 1 MoodHappy 80\nAT 1 MODIFIER 1 BrowUpLeft 50\nAT 2 PHONEME 1 Oh 40\nAT EXIT EXPRESSION 1 RESTORE\nAT 3 MODIFIER UNK RESET");
    auto facialPlan = Adapt(Prepare(facialDocument, facialContext, {}), facialError);
    Check(facialPlan && facialPlan->events.size() == 4 && facialError.empty());
    Check(facialPlan->events[0].kind == EventKind::Facial && facialPlan->events[0].source == 2 &&
        facialPlan->events[0].facial.index == 10 && facialPlan->events[0].facial.strength == 80);
    Check(facialPlan->events[1].facial.category == CAF::Facial::Category::Modifier &&
        facialPlan->events[2].facial.category == CAF::Facial::Category::Phoneme &&
        facialPlan->events[3].facial.operation == CAF::Facial::Operation::Restore && facialPlan->events[3].trigger == Trigger::Exit);
    Check(!Adapt(Prepare(Document::Parse("1 PLAY actor.hkx\nAT 0 EXPRESSION 2 10 80"), facialContext, {}), facialError));
    Check(!Prepare(Document::Parse("STATE logic LOGIC\nAT 0 EXPRESSION 1 10 80"), facialContext, {}).diagnostics.empty());
    Context pairedContext;
    std::string pairedError;
    auto paired = Adapt(Prepare(Document::Parse("1 PLAY paired.hkx ROOT NPC\n2 PLAY paired.hkx ROOT 2_"), pairedContext, {}), pairedError);
    Check(paired && paired->settings[2].rootBone == "NPC" && paired->settings[3].rootBone == "2_");
    // 同一 parser/adapter 路径验证零 PLAY 入口和稍后才进入的普通段。
    auto document = Document::Parse("STATE entry LOGIC\nAT 0 USER_OPTION hidden \"Player only\" TO action\nAT 0 USER_OPTION start \"Start\" TO action FORCE\nAT 5 END_SCENE\nAT EXIT CAMERA RESTORE\nSTATE action\nONCE\n1 PLAY action.hkx\n");
    Context context;
    auto entry = Prepare(document, context, {}, {}, false);
    std::string error;
    auto logic = Adapt(entry, error);
    Check(entry.diagnostics.empty() && entry.logic && logic.has_value() && error.empty());
    Check(std::none_of(logic->active.begin(), logic->active.end(), [](bool active) { return active; }));
    Check(logic->events.size() == 4 && !logic->events[0].option.force && logic->events[1].option.force);
    auto action = Adapt(Prepare(document, context, {}, "action", false), error);
    Check(action.has_value() && action->active[2] && action->settings[2].path == "action.hkx");
    // Logic 相机由 Session 校验接管，adapter 不再要求本段 PLAY。
    auto camera = Prepare(Document::Parse("STATE held LOGIC\nAT 0 CAMERA FOLLOW 2 Root 0 0 0"), context, {}, {}, false);
    auto target = Adapt(camera, error);
    Check(target.has_value() && target->events.size() == 1 && target->events[0].source == 3 && !target->active[3]);
    auto normalCamera = Prepare(Document::Parse("STATE normal\n1 PLAY action.hkx\nAT 0 CAMERA FOLLOW 2 Root 0 0 0"), context, {}, {}, false);
    Check(!Adapt(normalCamera, error).has_value());
    return 0;
}
