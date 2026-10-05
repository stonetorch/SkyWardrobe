#pragma once
#include <algorithm>

namespace CAF::SceneEditor
{
    constexpr float TimelineTime(float x, float left, float width, float start, float end)
    {
        return width > 0 ? start + std::clamp((x-left)/width, 0.0f, 1.0f)*(end-start) : start;
    }
    constexpr float StepTime(float time, float step, float duration)
    {
        return std::clamp(time+step, 0.0f, std::max(0.0f, duration));
    }
}
