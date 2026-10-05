#include "animation/PlaybackMath.h"
#include "animation/PoseMapping.h"
#include "animation/NamedPoseMapping.h"
#include "animation/AnnotationPolicy.h"

using namespace CAF::Playback;
// 大 dt 不得跨过采样目标，抵达后继续 Update 仍保持精确目标，不循环或提前释放。
static_assert(AdvanceToSample(1.0, 0.5, 6.7, 1.25).to == 1.25);
static_assert(AdvanceToSample(1.25, 0.5, 6.7, 1.25).to == 1.25);
static_assert(!AdvanceToSample(1.0, 0.5, 6.7, 1.25).ended);
static_assert(AdvanceToSample(1.0, 0.5, 1.2, 1.5).to == 1.2);
static_assert(AdvanceToSample(0, 241, 6.7, 1.25).to == 0);
// 保留完整 payload，且仅开放原版脚步、声音和攻击输出事件。
static_assert(SplitAnnotation("SoundPlay.NPCHumanCombatShieldBash").tag == "SoundPlay");
static_assert(SplitAnnotation("SoundPlay.a.b").payload == "a.b");
static_assert(ClassifyAnnotation("SoundPlay.NPCHumanCombatShieldBash") == NativeAnnotation::Sound);
static_assert(ClassifyAnnotation("SoundPlay.") == NativeAnnotation::None);
static_assert(ClassifyAnnotation("FootSprintLeft") == NativeAnnotation::Footstep);
static_assert(IsFootstepTag("FootLeft") && IsFootstepTag("FootScuffRight"));
static_assert(!IsFootstepTag("FootLeft.payload") && !IsFootstepTag("FootLeft.") && !IsFootstepTag("HitFrame"));
static_assert(ClassifyAnnotation("HitFrame") == NativeAnnotation::Attack);
static_assert(ClassifyAnnotation("weaponLeftSwing") == NativeAnnotation::Attack);
static_assert(ClassifyAnnotation("attackStart") == NativeAnnotation::None);
static_assert(ClassifyAnnotation("Death") == NativeAnnotation::None);
static_assert(ClassifyAnnotation("CombatGrunt") == NativeAnnotation::None);
// 回归：booty_vic 有 156 条轨道且映射为空，较小骨架只使用自身范围内的轨道。
static_assert(BoneForTrack(98, {}, 99) == 98);
static_assert(BoneForTrack(155, {}, 99) == -1);
constexpr std::int16_t remap[]{ 2, -1, 156 };
static_assert(BoneForTrack(0, remap, 99) == 2);
static_assert(BoneForTrack(1, remap, 99) == -1);
static_assert(BoneForTrack(2, remap, 99) == -1);
static_assert(BoneForTrack(3, remap, 99) == -1);
// 编译期检查端点、跨多圈、Seek 后的开区间和混合退出，无须启动 Skyrim。
constexpr auto finish = Advance(0.75, 0.5, 1.0, false);
static_assert(finish.ended && finish.to == 1.0 && finish.loops == 0);
constexpr auto wrap = Advance(0.75, 2.5, 1.0, true);
static_assert(wrap.loops == 3 && wrap.to == 0.25 && !wrap.ended);
static_assert(Crossings(0.8, wrap, 1.0, false) == 3);
static_assert(Crossings(0.1, wrap, 1.0, false) == 3);
static_assert(Crossings(0.5, wrap, 1.0, false) == 2);
constexpr auto start = Advance(0.0, 0.25, 1.0, false);
static_assert(Crossings(0, start, 1, true) == 1);
static_assert(Crossings(0, start, 1, false) == 0);
constexpr auto exactWrap = Advance(0.75, 0.25, 1.0, true);
static_assert(exactWrap.to == 0 && exactWrap.loops == 1);
static_assert(Crossings(0, exactWrap, 1, false) == 1);
static_assert(Crossings(1, exactWrap, 1, false) == 1);
constexpr auto afterSeek = Advance(0.5, 0.25, 1.0, false);
static_assert(Crossings(0.25, afterSeek, 1, false) == 0);
static_assert(Crossings(0.5, afterSeek, 1, false) == 0);
static_assert(Crossings(0.75, afterSeek, 1, false) == 1);
static_assert(Advance(0.5, 0, 1, false).to == 0.5);
static_assert(Advance(1, 0, 1, true).to == 1 && Advance(1, 0, 1, true).loops == 0);
static_assert(Advance(0, 1, 0, true).loops == 0);
static_assert(Advance(0, -1, 1, true).to == 0);
static_assert(Advance(0, 241, 1, true).loops == 0);
static_assert(Crossings(-1, wrap, 1, false) == 0);
static_assert(Crossings(2, wrap, 1, false) == 0);
static_assert(Approach(1, 0, 0.25f, 0.5f) == 0.5f);
static_assert(Approach(0.5f, 0, 0.5f, 0.5f) == 0);
static_assert(Approach(0, 1, 0, 0) == 1);
int main() { return 0; }

// 短名解析在编译期验证，避免 TIME 掩盖动画尚未加载的状态。
#include "animation/ClipPath.h"
using namespace CAF::Animation::ClipPath;
constexpr std::string_view catalog[]{"animations\\CAF\\test_vic.hkx", "animations\\CAF\\sub\\test_atk.hkx"};
static_assert(Resolve("test_vic.hkx",catalog).path == catalog[0]);
static_assert(Resolve("TEST_ATK.HKX",catalog).path == catalog[1]);
static_assert(Resolve("sub/test_atk.hkx",catalog).path == catalog[1]);
static_assert(Resolve("animations/CAF/test_vic.hkx",catalog).path == catalog[0]);
static_assert(Resolve("missing.hkx",catalog).status == Status::Missing);
static_assert(Resolve("animations\\idle.hkx",catalog).status == Status::Explicit);
constexpr std::string_view duplicates[]{"animations\\CAF\\one\\a.hkx", "animations\\CAF\\two\\a.hkx"};
static_assert(Resolve("a.hkx",duplicates).status == Status::Ambiguous);
static_assert(Resolve("one/a.hkx",duplicates).path == duplicates[0]);

// paired 轨道顺序可打乱，扩展骨骼忽略，两个角色不能互相覆盖。
constexpr std::string_view pairedTracks[]{ "PairedRoot", "2_", "2_NPC Hand", "NPC", "NPC Root [Root]", "2_NPC Root [Root]", "NPC Hand", "2_Extra" };
constexpr std::string_view pairedBones[]{ "NPC Root [Root]", "NPC Hand" };
constexpr std::int16_t pairedParents[]{ -1, 0 };
static_assert([] {
    auto mapping = ResolveNamedPoseMapping(pairedTracks, pairedBones, pairedParents, "2_");
    return mapping.error.empty() && mapping.syntheticRoot && mapping.rootTrack == 1 &&
        mapping.bones[2] == 1 && mapping.bones[5] == 0 && mapping.bones[4] == -1 && mapping.bones[7] == -1;
}());
static_assert([] {
    auto mapping = ResolveNamedPoseMapping(pairedTracks, pairedBones, pairedParents, "NPC");
    return mapping.error.empty() && mapping.bones[4] == 0 && mapping.bones[6] == 1 && mapping.bones[5] == -1;
}());
static_assert([] {
    auto mapping = ResolveNamedPoseMapping(pairedTracks, pairedBones, pairedParents, "2_NPC Hand");
    return mapping.error.empty() && !mapping.syntheticRoot && mapping.bones[2] == 1 && mapping.bones[5] == -1;
}());
static_assert(!ResolveNamedPoseMapping(pairedTracks, pairedBones, pairedParents, "missing").error.empty());
static_assert(!ResolveNamedPoseMapping(pairedTracks, pairedBones, pairedParents, "PairedRoot").error.empty());
constexpr std::string_view duplicateTracks[]{ "NPC", "NPC Hand", "NPC Hand" };
static_assert(!ResolveNamedPoseMapping(duplicateTracks, pairedBones, pairedParents, "NPC").error.empty());

static_assert([] {
    const auto roots = ParticipantRoots(pairedTracks);
    return roots.size() == 2 && roots[0] == "2_" && roots[1] == "NPC";
}());
constexpr std::string_view fakeRoots[]{ "NPC", "2_", "PairedRoot" };
static_assert(ParticipantRoots(fakeRoots).empty());
