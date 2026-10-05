#include "animation/ragdoll/Velocity.h"
#include <limits>
#include <iostream>

namespace
{
    using namespace CAF::Ragdoll;
    static_assert(!ValidStep(0) && !ValidStep(-0.01) && !ValidStep(0.5));
    static_assert(ValidStep(0.001) && ValidStep(0.1));
    static_assert(Cross({0, 0, 2}, {3, 0, 0}) == Vector{0, 6, 0});
    bool Near(float a, float b) { return std::abs(a-b) < 0.0001f; }
}
int main()
{
    int failures = 0;
    const auto check = [&](bool success, const char* name)
    {
        if (!success) { ++failures; std::cerr << name << '\n'; }
    };
    const Quaternion identity{0, 0, 0, 1};
    auto translation = Difference({0, 0, 0}, identity, {0.2f, -0.1f, 0}, identity, 0.02);
    check(translation.valid && Near(translation.linear[0], 10) && Near(translation.linear[1], -5), "engine seconds translation");
    auto signFlip = Difference({0, 0, 0}, identity, {0, 0, 0}, {0, 0, 0, -1}, 0.02);
    check(signFlip.valid && Near(Length(signFlip.angular), 0), "quaternion double cover");
    const float angle = 0.1f;
    auto rotation = Difference({0, 0, 0}, identity, {0, 0, 0}, {0, 0, std::sin(angle/2), std::cos(angle/2)}, 0.02);
    check(rotation.valid && Near(rotation.angular[2], 5), "angular velocity radians");
    // 非交换旋转：世界 X 旋转乘在初始 Z 旋转左侧，结果必须是世界 X 角速度。
    const float s = std::sin(angle/2), c = std::cos(angle/2), h = std::sqrt(0.5f);
    auto worldRotation = Difference({0, 0, 0}, {0, 0, h, h}, {0, 0, 0}, {s*h, -s*h, c*h, c*h}, 0.02);
    check(worldRotation.valid && Near(worldRotation.angular[0], 5) && Near(worldRotation.angular[1], 0), "world angular basis");
    auto teleport = Difference({0, 0, 0}, identity, {100, 0, 0}, identity, 0.02);
    check(!teleport.valid, "teleport rejected");
    auto paused = Difference({0, 0, 0}, identity, {0, 0, 0}, identity, 0);
    check(!paused.valid, "duplicate generate dt rejected");
    auto invalid = Difference({0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0}, identity, 0.02);
    check(!invalid.valid, "invalid quaternion rejected");
    auto nan = Difference({std::numeric_limits<float>::quiet_NaN(), 0, 0}, identity, {0, 0, 0}, identity, 0.02);
    check(!nan.valid, "non finite pose rejected");
    // 跨越 +-pi 使用小的最短弧，而不是近一整圈。
    auto wrap = Difference({0, 0, 0}, {0, 0, std::cos(0.01f), std::sin(0.01f)},
        {0, 0, 0}, {0, 0, -std::cos(0.01f), std::sin(0.01f)}, 0.02);
    check(wrap.valid && Near(wrap.angular[2], 2), "shortest arc across pi");
    return failures ? 1 : 0;
}
