#include "ui/FlickUi.h"

#include <charconv>
#include <atomic>
#include <optional>
#include <unordered_map>

namespace FlickUi
{
    namespace
    {
        struct Region { ImVec2 min, max; };
        std::vector<Region> regions;
        std::vector<ImVec2> windowContent;
        ImVec2 nextContent{};
        std::optional<std::pair<ImVec2, ImVec2>> nextConstraints;
        std::string doubleClickCard;
        double lastClick = -1;
        ImVec2 lastClickPosition{};
        bool navigationDragging = false;
        bool frameEditing = false;
        std::atomic_bool editing = false;
        struct IntegerEdit { std::string text; int observed{}; };
        std::unordered_map<const int*, IntegerEdit> integerEdits;
    }

    void BeginFrame()
    {
        regions.clear();
        windowContent.clear();
        nextContent = {};
        nextConstraints.reset();
        navigationDragging = false;
        frameEditing = false;
    }
    void EndFrame() { editing = frameEditing; }
    bool WantsTextInput() { return editing.load(); }
    void ResetInputState() { editing = false; }
    void RecordTextInput() { frameEditing = frameEditing || FUCK::IsItemActive(); }
    ContentScaleScope::ContentScaleScope(float scale)
    {
        // 清除旧版本留在宿主 content child 上的倍率，避免重复相乘。
        FUCK::SetWindowFontScale(1.0f);
        FUCK::PushFont(FUCK::GetFont(FUCK::Font::kRegular), FUCK::GetTextLineHeight() * scale);
        for (const auto variable : {ImGuiStyleVar_FramePadding, ImGuiStyleVar_ItemSpacing,
            ImGuiStyleVar_ItemInnerSpacing, ImGuiStyleVar_CellPadding, ImGuiStyleVar_WindowPadding})
        {
            const auto size = FUCK::GetStyleVarVec(variable);
            FUCK::PushStyleVar(variable, ImVec2(size.x * scale, size.y * scale));
        }
        for (const auto variable : {ImGuiStyleVar_IndentSpacing, ImGuiStyleVar_ScrollbarSize, ImGuiStyleVar_GrabMinSize})
            FUCK::PushStyleVar(variable, FUCK::GetStyleVar(variable) * scale);
    }
    ContentScaleScope::~ContentScaleScope()
    {
        FUCK::PopStyleVar(8);
        FUCK::PopFont();
    }
    bool Begin(const char* name, bool* open, int flags)
    {
        const bool draw = FUCK::GetInterface()->BeginWindow(name, open, flags);
        if (nextConstraints)
        {
            auto size = FUCK::GetWindowSize();
            const auto [minimum, maximum] = *nextConstraints;
            size.x = std::clamp(size.x, minimum.x, maximum.x);
            size.y = std::clamp(size.y, minimum.y, maximum.y);
            FUCK::SetWindowSize(size);
            nextConstraints.reset();
        }
        windowContent.push_back(nextContent);
        nextContent = {};
        return draw;
    }
    void End()
    {
        if (!windowContent.empty())
        {
            const auto size = windowContent.back();
            windowContent.pop_back();
            if (size.x > 0 || size.y > 0)
            {
                const auto cursor = FUCK::GetCursorPos();
                FUCK::SetCursorPos({std::max(cursor.x, size.x), std::max(cursor.y, size.y)});
                FUCK::Dummy({0, 0});
            }
        }
        FUCK::EndWindow();
    }
    bool BeginChild(const char* id, ImVec2 size, int childFlags, int flags)
    {
        FUCK::GetInterface()->BeginChild(id, size.x, size.y, (childFlags & ImGuiChildFlags_Borders) != 0, flags);
        const auto position = FUCK::GetWindowPos(), dimensions = FUCK::GetWindowSize();
        const auto padding = FUCK::GetStyleVarVec(ImGuiStyleVar_WindowPadding);
        const ImVec2 min{position.x + padding.x, position.y + padding.y};
        ImVec2 max{position.x + dimensions.x, position.y + dimensions.y};
        if (!regions.empty())
        {
            max.x = std::min(max.x, regions.back().max.x);
            max.y = std::min(max.y, regions.back().max.y);
        }
        regions.push_back({{std::max(min.x, regions.empty() ? 0.0f : regions.back().min.x),
            std::max(min.y, regions.empty() ? 0.0f : regions.back().min.y)}, max});
        return true;
    }
    void EndChild()
    {
        if (!regions.empty()) regions.pop_back();
        FUCK::EndChild();
    }
    void SetNextWindowContentSize(ImVec2 size) { nextContent = size; }
    void SetNextWindowSizeConstraints(ImVec2 minimum, ImVec2 maximum) { nextConstraints = {{minimum, maximum}}; }
    bool BeginPopup(const char* id, int flags) { return FUCK::GetInterface()->BeginPopup(id, flags); }
    bool BeginPopupModal(const char* id, bool* open, int flags)
    {
        const auto constraints = nextConstraints;
        nextConstraints.reset();
        const bool draw = FUCK::GetInterface()->BeginPopupModal(id, open, flags);
        if (draw && constraints)
        {
            const auto [minimum, maximum] = *constraints;
            const auto size = FUCK::GetWindowSize();
            FUCK::SetWindowSize({std::clamp(size.x, minimum.x, maximum.x), std::clamp(size.y, minimum.y, maximum.y)});
        }
        return draw;
    }
    bool BeginPopupContextWindow(const char* id, int flags)
    {
        if (FUCK::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup) &&
            (!(flags & ImGuiPopupFlags_NoOpenOverItems) || !FUCK::IsAnyItemHovered()) &&
            FUCK::IsMouseReleased(flags & ImGuiPopupFlags_MouseButtonMask_)) OpenPopup(id);
        return BeginPopup(id);
    }
    bool Button(const char* label, ImVec2 size)
    {
        if (size.x == 0 && size.y == 0) return FUCK::Button(label);
        // 宿主 Button 没有尺寸参数；Selectable 保留尺寸与手柄导航，并显式使用按钮配色。
        FUCK::PushStyleColor(ImGuiCol_Header, FUCK::GetStyleColorVec4(ImGuiCol_Button));
        FUCK::PushStyleColor(ImGuiCol_HeaderHovered, FUCK::GetStyleColorVec4(ImGuiCol_ButtonHovered));
        FUCK::PushStyleColor(ImGuiCol_HeaderActive, FUCK::GetStyleColorVec4(ImGuiCol_ButtonActive));
        FUCK::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, FUCK::GetStyleVarVec(ImGuiStyleVar_ButtonTextAlign));
        const bool clicked = FUCK::Selectable(label, true, ImGuiSelectableFlags_NoAutoClosePopups, size);
        FUCK::PopStyleVar();
        FUCK::PopStyleColor(3);
        return clicked;
    }
    bool SmallButton(const char* label)
    {
        FUCK::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3, 0));
        const bool clicked = FUCK::Button(label);
        FUCK::PopStyleVar();
        return clicked;
    }
    bool Checkbox(const char* label, bool* value)
    {
        // FLICK 原生 CheckBox 使用外部图片且只响应图片区域；这里保留完整行点击与矢量勾选标记。
        FUCK::PushID(label);
        const auto position = FUCK::GetCursorScreenPos();
        const float height = FUCK::GetFrameHeight();
        const float side = std::min(height, FUCK::GetTextLineHeight());
        const float gap = FUCK::GetStyleVarVec(ImGuiStyleVar_ItemInnerSpacing).x;
        const auto* end = std::strstr(label, "##");
        if (!end) end = label + std::strlen(label);
        const float space = std::max(1.0f, FUCK::CalcTextSize(" ").x);
        const auto spaces = static_cast<std::size_t>(std::ceil((side + gap) / space));
        const std::string display = std::string(spaces, ' ') + std::string(label, end) + "###checkbox";
        const ImVec2 size(side + gap + FUCK::CalcTextSize(label, end).x, height);
        const bool changed = FUCK::Selectable(display.c_str(), false, ImGuiSelectableFlags_NoAutoClosePopups, size);
        if (changed) *value = !*value;
        const ImVec2 min(position.x, position.y + (height - side) * 0.5f);
        const ImVec2 max(min.x + side, min.y + side);
        FUCK::DrawRectFilled(min, max, FUCK::GetStyleColorVec4(ImGuiCol_FrameBg), side * 0.15f);
        FUCK::DrawRect(min, max, FUCK::GetStyleColorVec4(ImGuiCol_Border), side * 0.15f, 1.0f);
        if (*value)
        {
            const ImVec2 a(min.x + side * 0.2f, min.y + side * 0.5f);
            const ImVec2 b(min.x + side * 0.43f, min.y + side * 0.73f);
            const ImVec2 c(min.x + side * 0.82f, min.y + side * 0.25f);
            const auto color = FUCK::GetStyleColorVec4(ImGuiCol_CheckMark);
            FUCK::DrawLine(a, b, color, std::max(1.0f, side * 0.1f));
            FUCK::DrawLine(b, c, color, std::max(1.0f, side * 0.1f));
        }
        FUCK::PopID();
        return changed;
    }
    bool MenuItem(const char* label, const char*, bool selected, bool enabled)
    {
        FUCK::BeginDisabled(!enabled);
        const bool clicked = FUCK::Selectable(label, selected);
        FUCK::EndDisabled();
        return clicked;
    }
    bool BeginCombo(const char* label, const char* preview, int)
    {
        FUCK::PushID(label);
        const std::string display = std::string(preview ? preview : "") + "  v###combo";
        if (FUCK::Button(display.c_str())) OpenPopup("##choices");
        if (label[0] != '#') { FUCK::SameLine(); FUCK::TextUnformatted(label); }
        if (BeginPopup("##choices")) return true;
        FUCK::PopID();
        return false;
    }
    void EndCombo() { FUCK::EndPopup(); FUCK::PopID(); }
    bool Combo(const char* label, int* selected, const char* items)
    {
        std::vector<const char*> values;
        for (const char* item = items; *item; item += std::strlen(item) + 1) values.push_back(item);
        return FUCK::GetInterface()->Combo(label, selected, values.data(), static_cast<int>(values.size()));
    }
    bool InputText(const char* label, std::string* value, int flags)
    {
        // 官方 std::string 包装固定为 2048 字节；这里保留长备注，并为本帧输入留余量。
        std::vector<char> buffer(value->size() + 65537, '\0');
        std::memcpy(buffer.data(), value->data(), value->size());
        const bool changed = FUCK::InputText(label, buffer.data(), buffer.size(), flags);
        RecordTextInput();
        if (changed) *value = buffer.data();
        return changed;
    }
    bool InputTextMultiline(const char* label, std::string* value, ImVec2 size, int flags)
    {
        std::vector<char> buffer(value->size() + 65537, '\0');
        std::memcpy(buffer.data(), value->data(), value->size());
        const bool changed = FUCK::InputTextMultiline(label, buffer.data(), buffer.size(), size, flags);
        RecordTextInput();
        if (changed) *value = buffer.data();
        return changed;
    }
    bool InputTextWithHint(const char* label, const char* hint, std::string* value, int flags)
    {
        const bool changed = InputText(label, value, flags);
        if (FUCK::IsItemHovered() && hint && *hint) FUCK::SetTooltip(hint);
        return changed;
    }
    bool InputInt(const char* label, int* value, int step, int, int flags)
    {
        FUCK::PushID(label);
        auto [it, inserted] = integerEdits.try_emplace(value, IntegerEdit{std::to_string(*value), *value});
        auto& edit = it->second;
        if (edit.observed != *value) edit.text = std::to_string(*value);
        bool changed = InputText(label, &edit.text, flags | ImGuiInputTextFlags_CharsDecimal);
        if (changed)
        {
            int parsed{};
            const auto result = std::from_chars(edit.text.data(), edit.text.data() + edit.text.size(), parsed);
            changed = result.ec == std::errc{} && result.ptr == edit.text.data() + edit.text.size();
            if (changed) *value = parsed;
        }
        if (!FUCK::IsItemActive()) edit.text = std::to_string(*value);
        if (step > 0)
        {
            FUCK::SameLine();
            if (SmallButton("-")) { *value = static_cast<int>(std::max<long long>(INT_MIN, static_cast<long long>(*value) - step)); changed = true; }
            FUCK::SameLine();
            if (SmallButton("+")) { *value = static_cast<int>(std::min<long long>(INT_MAX, static_cast<long long>(*value) + step)); changed = true; }
        }
        edit.observed = *value;
        FUCK::PopID();
        return changed;
    }
    void NewLine() { FUCK::Dummy({0, FUCK::GetTextLineHeight()}); }
    void BulletText(const char* format, ...)
    {
        va_list arguments;
        va_start(arguments, format);
        const auto count = _vscprintf(format, arguments);
        va_end(arguments);
        if (count < 0) return;
        std::vector<char> buffer(static_cast<std::size_t>(count) + 1);
        va_start(arguments, format);
        vsnprintf(buffer.data(), buffer.size(), format, arguments);
        va_end(arguments);
        FUCK::Text("• %s", buffer.data());
    }
    void SetTooltip(const char* format, ...)
    {
        va_list arguments;
        va_start(arguments, format);
        const auto count = _vscprintf(format, arguments);
        va_end(arguments);
        if (count < 0) return;
        std::vector<char> buffer(static_cast<std::size_t>(count) + 1);
        va_start(arguments, format);
        vsnprintf(buffer.data(), buffer.size(), format, arguments);
        va_end(arguments);
        FUCK::SetTooltip(buffer.data());
    }
    ImVec2 ClipMin() { return regions.empty() ? FUCK::GetWindowPos() : regions.back().min; }
    ImVec2 ClipMax()
    {
        if (!regions.empty()) return regions.back().max;
        const auto position = FUCK::GetWindowPos(), size = FUCK::GetWindowSize();
        return {position.x + size.x, position.y + size.y};
    }
    bool IsItemVisible()
    {
        const auto min = FUCK::GetItemRectMin(), max = FUCK::GetItemRectMax();
        const auto clipMin = ClipMin(), clipMax = ClipMax();
        return max.x > clipMin.x && max.y > clipMin.y && min.x < clipMax.x && min.y < clipMax.y;
    }
    const char* FitText(const char* begin, const char* end, float width, float size)
    {
        FUCK::PushFont(GetFont(), size);
        const char* fit = begin;
        for (const char* cursor = begin; cursor < end;)
        {
            if (*cursor == '\r' || *cursor == '\n') break;
            const char* next = cursor + 1;
            while (next < end && (static_cast<unsigned char>(*next) & 0xC0) == 0x80) ++next;
            if (FUCK::CalcTextSize(begin, next).x > width) break;
            fit = cursor = next;
        }
        FUCK::PopFont();
        return fit;
    }
    void DrawTextAt(ImVec2 position, float size, ImVec4 color, const char* begin, const char* end)
    {
        const auto cursor = FUCK::GetCursorScreenPos();
        FUCK::SetCursorScreenPos(position);
        FUCK::PushFont(GetFont(), size);
        FUCK::PushStyleColor(ImGuiCol_Text, color);
        FUCK::TextUnformatted(begin, end);
        FUCK::PopStyleColor();
        FUCK::PopFont();
        FUCK::SetCursorScreenPos(cursor);
    }
    void DrawList::AddRect(ImVec2 min, ImVec2 max, ImU32 color, float rounding, int, float thickness) const
    {
        if (screen) FUCK::DrawScreenRect(min, max, color, rounding, thickness);
        else FUCK::DrawRect(min, max, Color(color), rounding, thickness);
    }
    void DrawList::AddRectFilled(ImVec2 min, ImVec2 max, ImU32 color, float rounding) const
    {
        if (screen) FUCK::DrawScreenRectFilled(min, max, color, rounding);
        else FUCK::DrawRectFilled(min, max, Color(color), rounding);
    }
    void DrawList::AddLine(ImVec2 begin, ImVec2 end, ImU32 color, float thickness) const
    {
        if (screen) FUCK::DrawScreenLine(begin, end, color, thickness);
        else FUCK::DrawLine(begin, end, Color(color), thickness);
    }
    void DrawList::AddTriangleFilled(ImVec2 a, ImVec2 b, ImVec2 c, ImU32 color) const
    {
        if (screen) FUCK::DrawScreenTriangleFilled(a, b, c, color);
        else FUCK::DrawTriangleFilled(a, b, c, Color(color));
    }
    RowRange VisibleRows(int count, float height, float cursorY, float scrollY, float availableHeight)
    {
        if (count <= 0 || height <= 0) return {};
        const int first = std::clamp(static_cast<int>(std::floor((scrollY - cursorY) / height)), 0, count);
        const int last = std::clamp(static_cast<int>(std::ceil((scrollY - cursorY + availableHeight) / height)) + 1, first, count);
        return {first, last, first * height, (count - last) * height};
    }
    void ListClipper::Begin(int rowCount, float rowHeight)
    {
        start = FUCK::GetCursorPos().y;
        height = rowHeight; count = rowCount; phase = 0;
        rows = VisibleRows(count, height, start, FUCK::GetScrollY(), ClipMax().y - ClipMin().y);
        DisplayStart = rows.first; DisplayEnd = rows.last;
    }
    bool ListClipper::Step()
    {
        if (phase++ == 0 && DisplayStart < DisplayEnd)
        {
            FUCK::SetCursorPosY(start + rows.before);
            return true;
        }
        if (phase == 2 || (phase == 1 && DisplayStart == DisplayEnd))
        {
            FUCK::SetCursorPosY(start + count * height);
            FUCK::Dummy({0, 0});
        }
        return false;
    }
    bool SetDragDropPayload(const char* type, const void* data, std::size_t size, int condition)
    {
        if (std::strcmp(type, "SW_NAV") == 0) navigationDragging = true;
        return FUCK::SetDragDropPayload(type, data, size, condition);
    }
    bool NavigationDragging() { return navigationDragging; }
    bool CardDoubleClicked(const std::string& id, bool clicked)
    {
        if (!clicked) return false;
        const double now = FUCK::GetTime();
        const auto position = FUCK::GetMousePos();
        const float dx = position.x - lastClickPosition.x, dy = position.y - lastClickPosition.y;
        const bool doubleClicked = id == doubleClickCard && now - lastClick < 0.3 && dx * dx + dy * dy < 36;
        doubleClickCard = doubleClicked ? "" : id;
        lastClick = now; lastClickPosition = position;
        return doubleClicked;
    }
}
