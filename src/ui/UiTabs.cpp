#include "ui/FlickUi.h"
#include "ui/UiTabs.h"

#include <algorithm>

namespace CAF::Ui
{
    namespace
    {
        // 图标字体是否加载成功；由 Overlay 在 ImGui 初始化时设置。
        bool iconFontAvailable = false;
    }

    void SetIconFontAvailable(bool available)
    {
        iconFontAvailable = available;
    }

    bool IconFontAvailable()
    {
        return iconFontAvailable;
    }

    std::string Label(const char* icon, const char* text)
    {
        if (!iconFontAvailable || icon == nullptr || icon[0] == '\0')
        {
            return std::string(text);
        }
        return std::string(icon) + " " + text;
    }

    void DrawTabs(const char* id, TabState& state, std::span<const Page> pages, float stripWidth)
    {
        if (pages.empty())
        {
            return;
        }

        state.active = std::clamp(state.active, 0, static_cast<int>(pages.size()) - 1);
        if (stripWidth <= 0.0f)
        {
            // 以字号为单位取宽度，UI 缩放时标签列同步变化。
            stripWidth = 8.0f * FlickUi::GetFontSize();
        }

        FlickUi::PushID(id);
        FlickUi::BeginChild("##tab strip", ImVec2(stripWidth, 0.0f), ImGuiChildFlags_Borders);
        const auto itemWidth = FlickUi::GetContentRegionAvail().x;
        for (int i = 0; i < static_cast<int>(pages.size()); ++i)
        {
            // 显式给出宽度，让整列空白处都能点击切换。
            if (FlickUi::Selectable(Label(pages[i].icon, pages[i].label).c_str(), state.active == i, ImGuiSelectableFlags_None, ImVec2(itemWidth, 0.0f)))
            {
                state.active = i;
            }
        }
        FlickUi::EndChild();

        FlickUi::SameLine();
        // 每个页面用独立的 child ID，滚动位置按页各自保留。
        FlickUi::PushID(state.active);
        FlickUi::BeginChild("##tab page", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
        pages[state.active].draw();
        FlickUi::EndChild();
        FlickUi::PopID();  // state.active
        FlickUi::PopID();  // id
    }
}
