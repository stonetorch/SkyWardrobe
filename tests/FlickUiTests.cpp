#include "ui/FlickUi.h"
#include <iostream>
#include <stdexcept>

namespace
{
    void Check(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }
    std::string expected, committed;
    bool change = false;
    int seenFlags = 0;
    bool Input(const char*, char* buffer, std::size_t capacity, int flags)
    {
        Check(expected == buffer, "Existing Unicode/long text was truncated");
        Check(capacity > expected.size() + 65535, "No input headroom");
        seenFlags = flags;
        if (change) strcpy_s(buffer, capacity, committed.c_str());
        return change;
    }
    double testTime = 1;
    bool checkboxPressed = false;
    std::string checkboxLabel;
    int checkLines = 0;
    float currentFontSize = 20, windowScale = 1;
    std::vector<float> fontSizes;
    int styleDepth = 0;

    void CheckControls(FUCK_Interface& host)
    {
        host.GetFrameHeight = [] { return 24.0f; };
        host.GetTextLineHeight = [] { return currentFontSize * windowScale; };
        host.GetCursorScreenPos = [](float* x, float* y) { *x = 0; *y = 0; };
        host.GetStyleVarVec = [](ImGuiStyleVar, float* x, float* y) { *x = 4; *y = 4; };
        host.GetStyleColorVec4 = [](ImGuiCol, float* r, float* g, float* b, float* a) { *r = *g = *b = *a = 1; };
        host.PushID_Str = [](const char*) {};
        host.PopID = [] {};
        host.Selectable = [](const char* label, bool, int flags, const ImVec2& size)
        {
            checkboxLabel = label;
            Check((flags & ImGuiSelectableFlags_NoAutoClosePopups) != 0, "Checkbox closes its popup");
            Check(size.x > 20 && size.y == 24, "Checkbox label is outside the hitbox");
            return checkboxPressed;
        };
        host.DrawRect = [](const ImVec2&, const ImVec2&, const ImVec4&, float, float) {};
        host.DrawRectFilled = [](const ImVec2&, const ImVec2&, const ImVec4&, float) {};
        host.DrawLine = [](const ImVec2&, const ImVec2&, const ImVec4&, float) { ++checkLines; };
        bool checked = false;
        Check(!FlickUi::Checkbox("Preview", &checked) && !checked && checkLines == 0, "Idle checkbox changed");
        const auto identity = checkboxLabel;
        checkboxPressed = true;
        Check(FlickUi::Checkbox("Preview", &checked) && checked && checkLines == 2, "Checkbox did not enable");
        Check(checkboxLabel == identity, "Checkbox ID changed with its value");
        Check(FlickUi::Checkbox("Preview", &checked) && !checked && checkLines == 2, "Checkbox did not disable");
        checkboxPressed = false;
        Check(!FlickUi::Checkbox("Preview", &checked) && !checked, "Unpressed/disabled checkbox changed");

        host.SetWindowFontScale = [](float scale) { windowScale = scale; };
        host.PushFont = [](ImFont*, float size) { fontSizes.push_back(currentFontSize); currentFontSize = size; };
        host.PopFont = [] { currentFontSize = fontSizes.back(); fontSizes.pop_back(); };
        host.PushStyleVar = [](ImGuiStyleVar, float) { ++styleDepth; };
        host.PushStyleVarVec = [](ImGuiStyleVar, const ImVec2&) { ++styleDepth; };
        host.PopStyleVar = [](int count) { styleDepth -= count; };
        host.GetStyleVar = [](ImGuiStyleVar) { return 4.0f; };
        host.BeginChild = [](const char*, float, float, bool, int) { windowScale = 1; };
        host.EndChild = [] {};
        host.GetWindowPos = [](float* x, float* y) { *x = *y = 0; };
        host.GetWindowSize = [](float* x, float* y) { *x = *y = 500; };
        for (float factor : {0.5f, 1.0f, 1.5f, 2.0f})
        {
            windowScale = 2; // 上一版可能在外层窗口留下倍率。
            {
                const FlickUi::ContentScaleScope scale(factor);
                Check(FlickUi::GetFontSize() == 20 * factor, "Outer content scale compounded");
                FlickUi::BeginChild("Browse");
                FlickUi::BeginChild("Catalog grid");
                Check(FlickUi::GetFontSize() == 20 * factor, "Nested wardrobe children lost content scale");
                FlickUi::EndChild();
                FlickUi::EndChild();
                Check(styleDepth == 8, "Content style scale incomplete");
            }
            Check(currentFontSize == 20 && styleDepth == 0 && fontSizes.empty(), "Content scale leaked to other windows");
        }
    }
}

int main()
{
    try
    {
        FUCK_Interface host{};
        host.InputText = Input;
        host.InputTextMultiline = [](const char* label, char* buffer, std::size_t capacity, const ImVec2& size, int flags)
        {
            Check(size.x == 320 && size.y == 160, "Multiline size lost");
            return Input(label, buffer, capacity, flags);
        };
        host.IsItemActive = [] { return true; };
        host.GetTime = [] { return testTime; };
        host.GetMousePos = [](float* x, float* y) { *x = 100; *y = 100; };
        host.GetFont = [](FUCK::Font) -> ImFont* { return nullptr; };
        host.PushFont = [](ImFont*, float) {};
        host.PopFont = [] {};
        host.CalcTextSize = [](const char* begin, const char* end, bool, float, float* x, float* y)
        {
            *x = static_cast<float>(end ? end - begin : std::strlen(begin)); *y = 1;
        };
        FUCK::GetInterface() = &host;
        expected = std::string(6000, 'a') + "中文备注与拼音搜索🙂";
        std::string value = expected;
        FlickUi::BeginFrame();
        Check(!FlickUi::InputText("search", &value), "Unchanged input reported as changed");
        Check(value == expected, "Unchanged text mutated");
        change = true;
        committed = expected + "继续输入汉字";
        Check(FlickUi::InputText("alias", &value, ImGuiInputTextFlags_AutoSelectAll), "Commit not reported");
        Check(value == committed, "UTF-8 commit corrupted");
        Check(seenFlags == ImGuiInputTextFlags_AutoSelectAll, "Input flags not forwarded");
        expected = value;
        committed += "\n第二行中文";
        Check(FlickUi::InputTextMultiline("note", &value, {320, 160}), "Multiline commit not reported");
        Check(value == committed, "Multiline Unicode corrupted");
        FlickUi::EndFrame();
        Check(FlickUi::WantsTextInput(), "Text focus was not published");
        FlickUi::ResetInputState();
        Check(!FlickUi::WantsTextInput(), "Closing UI retained text focus");
        FlickUi::BeginFrame();
        FlickUi::EndFrame();
        Check(!FlickUi::WantsTextInput(), "Empty frame restored stale text focus");
        const std::string cardText = "中a文🙂\n不能穿过卡片";
        Check(FlickUi::FitText(cardText.data(), cardText.data() + cardText.size(), 4, 20) == cardText.data() + 4,
            "Text fitting split a Unicode character");
        Check(FlickUi::FitText(cardText.data(), cardText.data() + cardText.size(), 1000, 20) == cardText.data() + 11,
            "Card note crossed a newline");

        for (int count : {0, 1, 10, 10000})
            for (float scroll : {0.0f, 10.0f, 1000.0f, 1000000.0f})
            {
                const auto rows = FlickUi::VisibleRows(count, 80, 8, scroll, 600);
                Check(rows.first >= 0 && rows.first <= rows.last && rows.last <= count, "Visible row range invalid");
                Check(rows.before + (rows.last - rows.first) * 80 + rows.after == count * 80, "Scroll extent changed");
                Check(rows.last - rows.first <= 10, "Clipping submits too many rows");
            }
        Check(!FlickUi::CardDoubleClicked("A", true), "First click applied outfit");
        testTime += 0.1;
        Check(!FlickUi::CardDoubleClicked("B", true), "Clicks on different cards combined");
        testTime += 0.1;
        Check(FlickUi::CardDoubleClicked("B", true), "Double click not recognized");
        testTime += 0.1;
        Check(!FlickUi::CardDoubleClicked("B", true), "Third click repeated double click");
        CheckControls(host);
        FUCK::GetInterface() = nullptr;
        std::cout << "FLICK UI Unicode, clipping, card, checkbox and nested scaling checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
