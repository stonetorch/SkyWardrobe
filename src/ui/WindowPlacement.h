#pragma once
#include <imgui.h>
#include <algorithm>

namespace CAF::Ui
{
    // 按实际窗口尺寸限制四条边；小屏幕先缩小窗口，再将鼠标附近的起点夹到可用区域。
    inline void SetNextWindowNearPoint(ImVec2 anchor, ImVec2 preferredSize,
        ImGuiCond condition = ImGuiCond_Appearing)
    {
        const auto* viewport = ImGui::GetMainViewport();
        const ImVec2 size(std::clamp(preferredSize.x, 1.0f, std::max(1.0f, viewport->WorkSize.x)),
            std::clamp(preferredSize.y, 1.0f, std::max(1.0f, viewport->WorkSize.y)));
        const ImVec2 position(std::clamp(anchor.x + 16.0f, viewport->WorkPos.x,
            viewport->WorkPos.x + std::max(0.0f, viewport->WorkSize.x - size.x)),
            std::clamp(anchor.y + 16.0f, viewport->WorkPos.y,
            viewport->WorkPos.y + std::max(0.0f, viewport->WorkSize.y - size.y)));
        ImGui::SetNextWindowSize(size, condition);
        ImGui::SetNextWindowPos(position, condition);
    }
}
