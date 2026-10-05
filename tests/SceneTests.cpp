#include "scene/SceneMath.h"
#include "scene/AlignmentCache.h"
#include "ui/TimelineMath.h"
#include <initializer_list>
using namespace CAF::SceneMath;
// 对齐缓存键覆盖采样时刻、骨骼和条件；位移/旋转拖动复用几何采样。
struct CacheEvent
{
    int kind = 5, trigger = 0, source = 1, target = 0, line = 1;
    std::string_view condition, sourceBone = "hand", targetBone = "head", resource;
    float time = 1, predictionTime = 2;
    struct Point { float x = 0, y = 0, z = 0; } offset, rotation;
};
static_assert([]
{
    const CacheEvent original;
    auto edited = original;
    edited.line = 8; edited.resource = "sound";
    if (!CAF::SceneCache::SameAlignment(original, edited)) return false;
    edited.predictionTime = 3;
    if (CAF::SceneCache::SameAlignment(original, edited)) return false;
    edited = original; edited.offset.x = 1;
    if (!CAF::SceneCache::SameAlignment(original, edited)) return false;
    edited = original; edited.rotation.z = 1;
    if (!CAF::SceneCache::SameAlignment(original, edited)) return false;
    edited = original; edited.sourceBone = "foot";
    if (CAF::SceneCache::SameAlignment(original, edited)) return false;
    edited = original; edited.condition = "flag";
    return !CAF::SceneCache::SameAlignment(original, edited);
}());
static_assert(CAF::SceneEditor::TimelineTime(150, 100, 100, 2, 4) == 3);
static_assert(CAF::SceneEditor::TimelineTime(50, 100, 100, 2, 4) == 2);
static_assert(CAF::SceneEditor::TimelineTime(250, 100, 100, 2, 4) == 4);
static_assert(CAF::SceneEditor::TimelineTime(150, 100, 0, 2, 4) == 2);
static_assert(CAF::SceneEditor::StepTime(9, 2, 10) == 10);
static_assert(CAF::SceneEditor::StepTime(0, -0.01f, 10) == 0);
static_assert(Crossed(0, -0.001f, 0));
static_assert(Crossed(1, 0, 1));
static_assert(!Crossed(1, 1, 1));
static_assert(!Crossed(0.5f, 1, 0.5f));
static_assert(!Crossed(2, 0, 1));
static_assert(ScreenY(0) == 1 && ScreenY(1) == 0 && ScreenY(0.5f) == 0.5f);
constexpr auto key = ParseFormKey("Skyrim.esm|1234AB");
static_assert(key && key->plugin == "Skyrim.esm" && key->localID == 0x1234AB);
static_assert(!ParseFormKey(""));
static_assert(!ParseFormKey("|12"));
static_assert(!ParseFormKey("Skyrim.esm|"));
static_assert(!ParseFormKey("Skyrim.esm|0"));
static_assert(!ParseFormKey("Skyrim.esm|12zz"));
static_assert(!ParseFormKey("Skyrim.esm|1000000"));
// 替代、解除、到期和回跳：同一个编号在不同角色上互不影响。
static_assert([]
{
    bool fired = false;
    if (!ConsumeCrossing(fired, 1, 0, 1)) return false;
    if (ConsumeCrossing(fired, 1, 1, 1)) return false;
    if (ConsumeCrossing(fired, 1, 1, 0)) return false;
    if (ConsumeCrossing(fired, 1, 0, 1)) return false;
    fired = false; // 只有新一轮播放重新允许事件执行。
    return ConsumeCrossing(fired, 1, 0, 1);
}());
constexpr BindingEvent history[]{
    { BindingAction::Bind, 0, 0, 1, 0 },
    { BindingAction::Bind, 1, 1, 1, 0 },
    { BindingAction::Bind, 2, 0, 1, 1 },
    { BindingAction::Unbind, 4, 1, 1, 0 },
    { BindingAction::Bind, 5, 0, 1, 0 },
    { BindingAction::Unbind, 5, 0, 1, 0 }
};
static_assert(ActiveBinding(history, 0, 0));
static_assert(ActiveBinding(history, 0, 1));
static_assert(!ActiveBinding(history, 0, 2));
static_assert(ActiveBinding(history, 1, 3));
static_assert(ActiveBinding(history, 2, 2));
static_assert(!ActiveBinding(history, 2, 3));
static_assert(!ActiveBinding(history, 0, 3));
static_assert(!ActiveBinding(history, 1, 4));
static_assert(!ActiveBinding(history, 4, 5));
static_assert(!ActiveBinding(history, 3, 4));
static_assert(!ActiveBinding(history, 99, 4));
// 覆盖端点、同刻稳定排序、提前死亡以及两个独立的预测区间。
static_assert(PredictionConflict(1, 3, 0, 2, 1, false));
static_assert(PredictionConflict(1, 3, 0, 3, 1, false));
static_assert(!PredictionConflict(1, 3, 0, 4, 1, false));
static_assert(!PredictionConflict(1, 3, 0, 0, 1, false));
static_assert(PredictionConflict(1, 3, 0, 1, 1, false));
static_assert(!PredictionConflict(1, 3, 1, 1, 0, false));
static_assert(!PredictionConflict(1, 3, 0, 1, 0, false));
static_assert(PredictionConflict(1, 3, 1, 0, 0, true));
static_assert(PredictionConflict(1, 3, 1, 3, 0, true));
static_assert(!PredictionConflict(1, 3, 1, 4, 0, true));
int main() { return 0; }
