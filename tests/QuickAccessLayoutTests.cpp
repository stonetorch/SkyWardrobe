#include "wardrobe/QuickAccessLayout.h"
#include <imgui_internal.h>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
    void Check(int rows, int columns, float cellSize, float scale, ImVec2 viewport)
    {
        auto& io = ImGui::GetIO();
        io.DisplaySize = viewport;
        const auto layout = Wardrobe::CalculateQuickAccessLayout(rows, columns, cellSize, scale,
            viewport, ImGui::GetStyle().ScrollbarSize);
        const bool fits = layout.content.x + layout.padding * 2 <= viewport.x &&
            layout.content.y + layout.padding * 2 <= viewport.y;
        // 连续改变同一个窗口，覆盖 ImGui 的前帧内容尺寸与滚动条稳定机制。
        for (int frame = 0; frame < 3; ++frame)
        {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(layout.size);
            ImGui::SetNextWindowContentSize(layout.content);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(layout.padding, layout.padding));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(layout.gap, layout.gap));
            ImGui::Begin("Grid", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_HorizontalScrollbar);
            const auto origin = ImGui::GetCursorPos();
            for (int row = 0; row < rows; ++row)
                for (int column = 0; column < columns; ++column)
                {
                    ImGui::SetCursorPos(ImVec2(origin.x + column * (layout.side + layout.gap),
                        origin.y + row * (layout.side + layout.gap)));
                    ImGui::PushID(row * columns + column);
                    ImGui::Button("+", ImVec2(layout.side, layout.side));
                    ImGui::PopID();
                }
            const auto* window = ImGui::GetCurrentWindow();
            if (fits && (window->ScrollbarX || window->ScrollbarY || window->ScrollMax.x > 0 || window->ScrollMax.y > 0))
                throw std::runtime_error("Unnecessary scrollbar: " + std::to_string(rows) + "x" + std::to_string(columns) +
                    " scale=" + std::to_string(scale) + " frame=" + std::to_string(frame));
            if (!fits && frame == 2 && !window->ScrollbarX && !window->ScrollbarY)
                throw std::runtime_error("Oversized grid cannot scroll");
            ImGui::End();
            ImGui::PopStyleVar(2);
            ImGui::Render();
        }
    }
}

int main()
{
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.DeltaTime = 1.0f / 60.0f;
    io.Fonts->AddFontDefault(); io.Fonts->Build();
    try
    {
        for (float scale : {0.85f, 1.0f, 1.125f, 1.3f, 1.5f, 1.75f})
            for (float cellSize : {32.0f, 64.0f, 73.0f, 128.0f})
                for (int rows = 1; rows <= 12; ++rows)
                    for (int columns = 1; columns <= 12; ++columns)
                        Check(rows, columns, cellSize, scale, ImVec2(1920, 1080));
        Check(12, 12, 128, 1.3f, ImVec2(640, 480));
        Check(3, 3, 64, 1.3f, ImVec2(1920, 1080));
        ImGui::DestroyContext();
        std::cout << "Quick access ImGui layout checks passed\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
