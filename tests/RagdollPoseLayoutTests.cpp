#include "animation/ragdoll/PoseLayout.h"
#include <array>

namespace
{
    constexpr bool ReducedOutput()
    {
        // 实机回归：126 根骨架的有效输出只有 98 根，保留动画前缀并补齐 reference 尾部。
        std::array<int, 126> reference{}, complete{};
        std::array<int, 98> animated{}, driverOutput{};
        for (int i = 0; i < 126; ++i) reference[i] = i;
        for (int i = 0; i < 98; ++i) animated[i] = 1000 + i;
        if (!CAF::Ragdoll::CompletePose<int>(animated, reference, complete)) return false;
        for (int i = 0; i < 126; ++i)
            if (complete[i] != (i < 98 ? 1000 + i : i)) return false;
        if (!CAF::Ragdoll::WritePosePrefix<int>(complete, driverOutput) || driverOutput != animated) return false;
        // 原版输出恢复完整 count 时仍可提交同一快照。
        std::array<int, 126> fullOutput{};
        return CAF::Ragdoll::WritePosePrefix<int>(complete, fullOutput) && fullOutput == complete;
    }
    constexpr bool InvalidLayout()
    {
        // 无效布局必须拒绝且不修改目标数组，避免失败交接污染原版 pose。
        std::array<int, 2> reference{1, 2}, complete{7, 8};
        std::array<int, 3> oversized{3, 4, 5};
        std::array<int, 1> wrongDestination{};
        const auto original = complete;
        return !CAF::Ragdoll::CompletePose<int>(oversized, reference, complete) && complete == original &&
            !CAF::Ragdoll::CompletePose<int>({}, reference, complete) && complete == original &&
            !CAF::Ragdoll::CompletePose<int>(reference, reference, wrongDestination) &&
            !CAF::Ragdoll::WritePosePrefix<int>(reference, oversized) && oversized[0] == 3;
    }
    constexpr bool FullOutput()
    {
        // 完整输出全部保留动画值，reference pose 不能覆盖已经输出的骨骼。
        std::array<int, 2> reference{1, 2}, output{3, 4}, complete{};
        return CAF::Ragdoll::CompletePose<int>(output, reference, complete) && complete == output;
    }
    static_assert(ReducedOutput());
    static_assert(InvalidLayout());
    static_assert(FullOutput());
}

int main() { return 0; }
