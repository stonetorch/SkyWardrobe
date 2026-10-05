#include "animation/PairedPose.h"
#include "animation/NamedPoseMapping.h"

namespace
{
    using namespace CAF::Playback;
    constexpr bool SameRotation(PoseQuaternion left, PoseQuaternion right)
    {
        bool same = true, opposite = true;
        for (int i = 0; i < 4; ++i)
        {
            same &= PoseAbsolute(left[i]-right[i]) < 0.00001f;
            opposite &= PoseAbsolute(left[i]+right[i]) < 0.00001f;
        }
        return same || opposite;
    }
    // 非交换旋转验证 merger 的逆操作次序，防止把右乘误写为左乘。
    constexpr LocalPoseTransform originalRoot{{12, -7, 3, 0}, {0, 0, 0.6f, 0.8f}, {1.2f, 0.9f, 1.1f, 0}};
    constexpr LocalPoseTransform originalCom{{4, 5, 6, 0}, {0.8f, 0, 0, 0.6f}, {1.3f, 1.1f, 0.8f, 0}};
    constexpr LocalPoseTransform compensationRoot{{0, 0, 0, 0}, InverseRotation(originalRoot.rotation), {1, 1, 1, 0}};
    constexpr LocalPoseTransform pairedCom{originalCom.translation,
        MultiplyRotation(originalCom.rotation, originalRoot.rotation), originalCom.scale};
    constexpr auto restored = DepairSample(originalRoot, compensationRoot, pairedCom);
    static_assert(restored.restored);
    static_assert(restored.root.translation == originalRoot.translation && restored.root.scale == originalRoot.scale);
    static_assert(SameRotation(restored.root.rotation, originalRoot.rotation));
    static_assert(SameRotation(restored.com.rotation, originalCom.rotation));
    static_assert(restored.com.translation == originalCom.translation && restored.com.scale == originalCom.scale);
    static_assert(!SameRotation(MultiplyRotation(InverseRotation(originalRoot.rotation), pairedCom.rotation), originalCom.rotation));
    // q 与 -q 是同一旋转；identity carrier 及量化误差不应破坏还原。
    constexpr LocalPoseTransform oppositeCompensation{{0, 0, 0, 0}, {0, 0, 0.6f, -0.8f}, {1, 1, 1, 0}};
    static_assert(DepairSample(originalRoot, oppositeCompensation, pairedCom).restored);
    constexpr LocalPoseTransform identity;
    static_assert(SameRotation(DepairSample(identity, identity, originalCom).com.rotation, originalCom.rotation));
    constexpr LocalPoseTransform compressedCompensation{{0.0001f, 0, 0, 0}, {0, 0, -0.60001f, 0.8f}, {1, 1, 1, 0}};
    static_assert(DepairSample(originalRoot, compressedCompensation, pairedCom).restored);
    // 非零根偏移、不匹配的旋转、非单位缩放及无效四元数不能误当补偿 Root。
    constexpr LocalPoseTransform nativeRoot{{2, 0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1, 0}};
    static_assert(!DepairSample(originalRoot, nativeRoot, pairedCom).restored);
    static_assert(!DepairSample(originalRoot, identity, pairedCom).restored);
    constexpr LocalPoseTransform scaledRoot{{0, 0, 0, 0}, InverseRotation(originalRoot.rotation), {2, 1, 1, 0}};
    static_assert(!DepairSample(originalRoot, scaledRoot, pairedCom).restored);
    constexpr LocalPoseTransform invalidRoot{{0, 0, 0, 0}, {0, 0, 0, 0}, {1, 1, 1, 0}};
    static_assert(!DepairSample(invalidRoot, compensationRoot, pairedCom).restored);

    // 实机 MammoryMigraine 的 2_ 恒为 y=100、绕 z 旋转 180 度；初始摆位不能再次移动骨架。
    constexpr LocalPoseTransform mammoryCarrier{{0, 100, 0, 0}, {0, 0, 1, 0}, {1, 1, 1, 0}};
    constexpr auto stationaryCarrier = RelativeCarrier(mammoryCarrier, mammoryCarrier);
    static_assert(stationaryCarrier.translation == identity.translation);
    static_assert(SameRotation(stationaryCarrier.rotation, identity.rotation));
    static_assert(stationaryCarrier.scale == identity.scale);
    // 保留运动并转换到初始朝向，不能只减世界平移或在 seek 时重置基准。
    constexpr LocalPoseTransform movedCarrier{{12, 107, 3, 0}, {0, 0, 0.6f, 0.8f}, {2, 1, 1, 0}};
    constexpr auto motion = RelativeCarrier(mammoryCarrier, movedCarrier);
    static_assert(motion.translation == std::array<float, 4>{-12, -7, 3, 0});
    static_assert(SameRotation(motion.rotation, PoseQuaternion{0, 0, -0.8f, 0.6f}));
    static_assert(motion.scale == std::array<float, 4>{2, 1, 1, 0});
    constexpr auto initialRoot = RelativeCarrier(originalRoot, originalRoot);
    static_assert(initialRoot.translation == identity.translation);
    static_assert(SameRotation(initialRoot.rotation, identity.rotation));
    static_assert(initialRoot.scale == identity.scale);

    constexpr std::string_view tracks[]{"2_NPC COM [COM ]", "NPC", "NPC COM [COM ]", "2_",
        "NPC Root [Root]", "2_NPC Root [Root]"};
    constexpr std::string_view bones[]{"NPC Root [Root]", "NPC COM [COM ]"};
    constexpr std::int16_t parents[]{-1, 0};
    static_assert([] {
        const auto mapping = ResolveNamedPoseMapping(tracks, bones, parents, "2_");
        return mapping.error.empty() && mapping.participantRootTrack == 3 && mapping.skeletonRootTrack == 5 && mapping.comTrack == 0;
    }());
    // COM 子树也解析其父级依赖，但不会把被排除的 Root 写入输出。
    static_assert([] {
        const auto mapping = ResolveNamedPoseMapping(tracks, bones, parents, "2_NPC COM [COM ]");
        return mapping.error.empty() && mapping.participantRootTrack == 3 && mapping.skeletonRootTrack == 5 &&
            mapping.comTrack == 0 && mapping.bones[0] == 1 && mapping.bones[5] == -1;
    }());
    static_assert([] {
        const auto mapping = ResolveNamedPoseMapping(tracks, bones, parents, "NPC");
        return mapping.error.empty() && mapping.participantRootTrack == 1 && mapping.skeletonRootTrack == 4 && mapping.comTrack == 2;
    }());
}

int main() {}
