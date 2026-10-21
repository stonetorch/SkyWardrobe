#pragma once

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>
#include "flick/FUCK_API.h"

// 只通过 FLICK 的公开 ABI 绘制；禁止在客户端创建或切换宿主的 ImGui context。
namespace FlickUi
{
    using namespace FUCK;
    void RecordTextInput();

    // 显式字号作用域可跨越多层 child；窗口字号缩放只作用于当前窗口。
    class ContentScaleScope
    {
    public:
        explicit ContentScaleScope(float scale);
        ~ContentScaleScope();
        ContentScaleScope(const ContentScaleScope&) = delete;
        ContentScaleScope& operator=(const ContentScaleScope&) = delete;
    };

    struct Viewport
    {
        ImVec2 WorkPos{}, WorkSize{};
        ImVec2 GetCenter() const { return {WorkSize.x / 2, WorkSize.y / 2}; }
    };
    inline Viewport* GetMainViewport()
    {
        static Viewport viewport;
        viewport.WorkSize = FUCK::GetDisplaySize();
        return &viewport;
    }
    struct Style
    {
        ImVec2 ItemSpacing{}, CellPadding{}, WindowPadding{};
        float ScrollbarSize{}, ChildBorderSize{};
    };
    inline Style GetStyle()
    {
        return {FUCK::GetStyleVarVec(ImGuiStyleVar_ItemSpacing),
            FUCK::GetStyleVarVec(ImGuiStyleVar_CellPadding), FUCK::GetStyleVarVec(ImGuiStyleVar_WindowPadding),
            FUCK::GetStyleVar(ImGuiStyleVar_ScrollbarSize), FUCK::GetStyleVar(ImGuiStyleVar_ChildBorderSize)};
    }
    inline float GetFontSize() { return FUCK::GetTextLineHeight(); }
    inline ImFont* GetFont() { return FUCK::GetFont(FUCK::Font::kRegular); }
    inline float GetCursorPosX() { return FUCK::GetCursorPos().x; }
    inline float GetCursorPosY() { return FUCK::GetCursorPos().y; }
    bool Begin(const char* name, bool* open = nullptr, int flags = 0);
    void End();
    bool BeginChild(const char* id, ImVec2 size = {}, int childFlags = 0, int flags = 0);
    void EndChild();
    void SetNextWindowContentSize(ImVec2 size);
    void SetNextWindowSizeConstraints(ImVec2 minimum, ImVec2 maximum);
    bool BeginPopup(const char* id, int flags = 0);
    bool BeginPopupModal(const char* id, bool* open = nullptr, int flags = 0);
    inline void OpenPopup(const char* id, int flags = 0) { FUCK::GetInterface()->OpenPopup(id, flags); }
    inline bool IsPopupOpen(const char* id, int flags = 0) { return FUCK::GetInterface()->IsPopupOpen(id, flags); }
    inline bool BeginPopupContextItem(const char* id = nullptr, int flags = 1)
    { return FUCK::GetInterface()->BeginPopupContextItem(id, flags); }
    bool BeginPopupContextWindow(const char* id, int flags = 1);
    inline bool BeginTable(const char* id, int columns, int flags = 0, ImVec2 size = {}, float inner = 0)
    { return FUCK::GetInterface()->BeginTable(id, columns, flags, size, inner); }
    inline void TableSetupColumn(const char* name, int flags = 0, float width = 0, unsigned int user = 0)
    { FUCK::GetInterface()->TableSetupColumn(name, flags, width, user); }
    inline bool BeginTabItem(const char* label, bool* = nullptr, int flags = 0)
    { return FUCK::GetInterface()->BeginTabItem(label, flags); }
    inline bool BeginDragDropSource(int flags = 0) { return FUCK::GetInterface()->BeginDragDropSource(flags); }
    inline const ImGuiPayload* AcceptDragDropPayload(const char* type, int flags = 0)
    { return FUCK::GetInterface()->AcceptDragDropPayload(type, flags); }
    bool SetDragDropPayload(const char* type, const void* data, std::size_t size, int condition = 0);
    inline bool InvisibleButton(const char* id, ImVec2 size, int flags = 0)
    { return FUCK::GetInterface()->InvisibleButton(id, size, flags); }
    bool Button(const char* label, ImVec2 size = {});
    bool Checkbox(const char* label, bool* value);
    bool SmallButton(const char* label);
    bool MenuItem(const char* label, const char* shortcut = nullptr, bool selected = false, bool enabled = true);
    bool BeginCombo(const char* label, const char* preview, int flags = 0);
    void EndCombo();
    bool Combo(const char* label, int* selected, const char* items);
    inline bool Combo(const char* label, int* selected, const char* const* items, int count)
    { return FUCK::GetInterface()->Combo(label, selected, items, count); }
    bool InputInt(const char* label, int* value, int step = 1, int fastStep = 100, int flags = 0);
    inline bool SliderInt(const char* label, int* value, int minimum, int maximum, const char* format = "%d", int = 0)
    {
        const bool changed = FUCK::SliderInt(label, value, minimum, maximum, format);
        RecordTextInput();
        if (changed) *value = std::clamp(*value, minimum, maximum);
        return changed;
    }
    inline bool SliderFloat(const char* label, float* value, float minimum, float maximum, const char* format = "%.3f", int = 0)
    {
        const bool changed = FUCK::SliderFloat(label, value, minimum, maximum, format);
        RecordTextInput();
        if (changed) *value = std::clamp(*value, minimum, maximum);
        return changed;
    }
    bool InputText(const char* label, std::string* value, int flags = 0);
    bool InputTextMultiline(const char* label, std::string* value, ImVec2 size = {}, int flags = 0);
    bool InputTextWithHint(const char* label, const char* hint, std::string* value, int flags = 0);
    void NewLine();
    void BulletText(const char* format, ...);
    void SetTooltip(const char* format, ...);
    inline ImVec4 Color(ImU32 color)
    {
        return {((color >> IM_COL32_R_SHIFT) & 255) / 255.0f, ((color >> IM_COL32_G_SHIFT) & 255) / 255.0f,
            ((color >> IM_COL32_B_SHIFT) & 255) / 255.0f, ((color >> IM_COL32_A_SHIFT) & 255) / 255.0f};
    }
    inline ImU32 GetColorU32(ImVec4 color)
    {
        const auto byte = [](float value) { return static_cast<unsigned int>(std::clamp(value, 0.0f, 1.0f) * 255 + 0.5f); };
        return IM_COL32(byte(color.x), byte(color.y), byte(color.z), byte(color.w));
    }
    inline ImU32 GetColorU32(ImGuiCol color) { return GetColorU32(FUCK::GetStyleColorVec4(color)); }
    bool IsItemVisible();
    ImVec2 ClipMin();
    ImVec2 ClipMax();
    void DrawTextAt(ImVec2 position, float size, ImVec4 color, const char* begin, const char* end = nullptr);
    const char* FitText(const char* begin, const char* end, float width, float size);

    // 手册箭头及拖动提示只调用宿主图元接口，不跨 DLL 访问 ImDrawList。
    struct DrawList
    {
        bool screen{};
        ImVec2 GetClipRectMin() const { return ClipMin(); }
        ImVec2 GetClipRectMax() const { return ClipMax(); }
        void AddRect(ImVec2 min, ImVec2 max, ImU32 color, float rounding = 0, int = 0, float thickness = 1) const;
        void AddRectFilled(ImVec2 min, ImVec2 max, ImU32 color, float rounding = 0) const;
        void AddLine(ImVec2 begin, ImVec2 end, ImU32 color, float thickness = 1) const;
        void AddTriangleFilled(ImVec2 a, ImVec2 b, ImVec2 c, ImU32 color) const;
    };
    inline DrawList* GetWindowDrawList() { static DrawList list; return &list; }
    inline DrawList* GetForegroundDrawList() { static DrawList list{true}; return &list; }

    // 固定行高列表在客户端计算可见区，始终保留完整的滚动高度。
    struct RowRange { int first{}, last{}; float before{}, after{}; };
    RowRange VisibleRows(int count, float height, float cursorY, float scrollY, float availableHeight);
    class ListClipper
    {
    public:
        int DisplayStart{}, DisplayEnd{};
        void Begin(int count, float height);
        bool Step();
    private:
        RowRange rows;
        float start{}, height{};
        int count{}, phase{};
    };
    bool NavigationDragging();
    void BeginFrame();
    void EndFrame();
    bool WantsTextInput();
    void ResetInputState();
    bool CardDoubleClicked(const std::string& id, bool clicked);
}
