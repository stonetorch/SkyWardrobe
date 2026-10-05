#pragma once

#include <imgui.h>

#include <span>
#include <string>

namespace CAF::Ui
{
    // 标签导航的跨帧状态，由调用方持有。
    struct TabState
    {
        int active = 0;
    };

    // 一个标签页：图标、标签文本与内容绘制函数。
    struct Page
    {
        const char* icon; // Font Awesome 码位；图标字体缺失时自动省略
        const char* label;
        void (*draw)();
    };

    // 由 Overlay 在字体加载后设置。图标字体缺失时所有标签退回纯文字，避免渲染出豆腐块。
    void SetIconFontAvailable(bool available);
    bool IconFontAvailable();
    // 拼接「图标 + 文字」的控件标签；图标字体缺失或未给图标时只留文字。
    std::string Label(const char* icon, const char* text);

    // 左侧一列标签、右侧内容区的两栏布局，每帧只绘制当前活跃页。
    // 同一窗口内嵌套使用时，各自传入不同的 id 与 TabState。
    // stripWidth <= 0 时按字号自动取值，随 UI 缩放一起变化。
    void DrawTabs(const char* id, TabState& state, std::span<const Page> pages, float stripWidth = 0.0f);
}
