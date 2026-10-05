#include "scene/LogicStatePolicy.h"
#include "scene/UserOptions.h"
#include "scene/SessionLifecycle.h"
#include <array>
using namespace CAF::Scene;

struct Event { float time; int action; bool enabled = true; };
static_assert(!RequiresAnimationActors(true, false));
static_assert(!RequiresAnimationActors(false, true));
static_assert(RequiresAnimationActors(false, false));
// 真实派发器的纯批次核心：GOTO 后的同刻修改执行，更晚事件不消费。
static_assert([]
{
    constexpr std::array events{Event{0, 1}, Event{0, 2}, Event{0, 1}, Event{1, 1}};
    std::array<bool, 4> fired{};
    int modifications = 0;
    bool requested = false;
    const auto dispatch = [&](const Event& event)
    {
        if (event.action == 1) ++modifications; else requested = true;
        return requested;
    };
    DispatchCrossedBatch(events, fired, -0.001f, 2, false, [](const auto&) { return true; }, dispatch);
    if (modifications != 2 || !requested || fired[3]) return false;
    // 相同更新再次停在 0 不得重复 AT 0。
    DispatchCrossedBatch(events, fired, 0, 0, false, [](const auto&) { return true; }, dispatch);
    return modifications == 2;
}());
static_assert([]
{
    constexpr std::array events{Event{1, 1, false}, Event{1, 2}, Event{1, 3}, Event{1, 1}, Event{2, 1}};
    std::array<bool, 5> fired{};
    int modifications = 0;
    bool go = false, end = false;
    DispatchCrossedBatch(events, fired, 0, 3, false, [](const auto&) { return true; }, [&](const Event& event)
    {
        if (event.enabled)
        {
            if (event.action == 1) ++modifications;
            if (event.action == 2) go = true;
            if (event.action == 3) end = true;
        }
        return go || end;
    });
    return modifications == 1 && go && end && !fired[4];
}());
// 零播放器不妨碍 Session 活动；FORCE 可选，普通选项不显示。
static_assert([]
{
    SessionLifecycle lifecycle;
    if (!lifecycle.Begin()) return false;
    lifecycle.PrepareFragment(); lifecycle.MarkPlaying();
    UserOptions options;
    options.Upsert({"hidden", "Requires player", "play", false, false});
    options.Upsert({"start", "Start", "play", true, false});
    options.Move(0, false); options.selected = options.hovered;
    if (!lifecycle.IsActive() || options.Visible(false).size() != 1 || options.Transition(false, false)) return false;
    // Logic 给 Transition 的 atEnd/确认策略为 true，无须 IMMEDIATE。
    if (options.Transition(false, true) != "play") return false;
    lifecycle.BeginFinish(); lifecycle.Finish();
    return !lifecycle.IsActive();
}());
static_assert(NextSlotPlayback(true, false, false, true, false) == SlotPlaybackAction::Hold);
static_assert(NextSlotPlayback(false, true, false, true, false) == SlotPlaybackAction::None);
static_assert(NextSlotPlayback(false, true, false, false, false) == SlotPlaybackAction::None);
static_assert(NextSlotPlayback(false, true, true, false, false) == SlotPlaybackAction::Play);
static_assert(NextSlotPlayback(true, false, false, false, false) == SlotPlaybackAction::Stop);
static_assert([]
{
    std::size_t count = 0;
    for (int i = 0; i < 128; ++i) if (!ConsumeImmediateTransition(count)) return false;
    return !ConsumeImmediateTransition(count) && count == 128;
}());
int main() { return 0; }
