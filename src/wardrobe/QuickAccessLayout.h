#pragma once
#include <imgui.h>
#include <algorithm>
#include <cmath>

namespace Wardrobe
{
    struct QuickAccessLayout
    {
        float side, gap, padding;
        ImVec2 content, size;
    };

    inline QuickAccessLayout CalculateQuickAccessLayout(int rows, int columns, float cellSize,
        float scale, ImVec2 viewportSize, float scrollbarSize)
    {
        // ImGui 会截断窗口尺寸；先将格子、间距和留白统一到整像素，避免内容比窗口大零点几像素。
        const float side = std::ceil(cellSize * scale);
        const float gap = std::ceil(8.0f * scale), padding = std::ceil(12.0f * scale);
        const ImVec2 content(columns * side + (columns - 1) * gap, rows * side + (rows - 1) * gap);
        const ImVec2 available(std::floor(viewportSize.x), std::floor(viewportSize.y));
        const float scrollbar = std::ceil(scrollbarSize);
        bool scrollX = false, scrollY = false;
        for (int pass = 0; pass < 3; ++pass)
        {
            scrollX = content.x + padding * 2 + (scrollY ? scrollbar : 0) > available.x;
            scrollY = content.y + padding * 2 + (scrollX ? scrollbar : 0) > available.y;
        }
        return {side, gap, padding, content,
            ImVec2(std::min(content.x + padding * 2 + (scrollY ? scrollbar : 0), available.x),
                std::min(content.y + padding * 2 + (scrollX ? scrollbar : 0), available.y))};
    }
}
