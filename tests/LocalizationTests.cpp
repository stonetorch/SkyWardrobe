#include "ui/Text.h"

#include <array>
#include <cstdlib>
#include <string_view>

namespace
{
    struct Tokens
    {
        std::array<char, 256> text{};
        std::size_t size = 0;
        int nulls = 0;
        constexpr bool operator==(const Tokens&) const = default;
    };

    // 翻译必须保留 printf / std::format 占位符顺序与类型，以及组合框的 NUL 分隔。
    consteval Tokens FormatTokens(std::string_view text)
    {
        Tokens result;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == '\0') ++result.nulls;
            if (text[i] == '{')
            {
                do { result.text.at(result.size++) = text[i]; }
                while (text[i++] != '}' && i < text.size());
                --i;
            }
            else if (text[i] == '%')
            {
                const auto start = i;
                auto end = i + 1;
                while (end < text.size() && std::string_view("-+ #0.123456789hljztL").find(text[end]) != std::string_view::npos) ++end;
                if (end < text.size() && std::string_view("diuoxXfFeEgGaAcspn%").find(text[end]) != std::string_view::npos)
                {
                    for (auto at = start; at <= end; ++at) result.text.at(result.size++) = text[at];
                    i = end;
                }
            }
        }
        return result;
    }
#define CAF_TEXT(name, english, chinese) \
    static_assert(FormatTokens(std::string_view(english, sizeof(english) - 1)) == \
        FormatTokens(std::string_view(chinese, sizeof(chinese) - 1)), #name " format / combo mismatch");
#define CAF_LABEL(name, english, chinese) CAF_TEXT(name, english, chinese)
#define CAF_DIAGNOSTIC(name, english, chinese) CAF_TEXT(name, english, chinese)
#include "ui/Text.def"
#undef CAF_DIAGNOSTIC
#undef CAF_LABEL
#undef CAF_TEXT

    void Require(bool value)
    {
        if (!value) std::abort();
    }
}

int main()
{
    using namespace CAF::Ui::Text;
    Require(DisplayName("UnknownCustomNode") == "UnknownCustomNode");
    Require(Diagnostic("CustomPlugin.esm|123ABC / NPC Root [Root]") == "CustomPlugin.esm|123ABC / NPC Root [Root]");
    Require(Diagnostic("Data/Playing.hkx MyPausedEditorID") == "Data/Playing.hkx MyPausedEditorID");
#if CAF_UI_ZH_CN
    Require(std::string_view(kCancel) == "取消");
    Require(DisplayName("THROAT_SLIT") == "THROAT_SLIT");
    Require(DisplayName("Throat slit") == "割喉");
    Require(Diagnostic("Actor 00000014: scene loading - Loading HKX") == "角色 00000014: 场景加载 - 正在加载 HKX");
    Require(Diagnostic("Scene: playback startup total") == "场景：播放启动总耗时");
#else
    Require(std::string_view(kCancel) == "Cancel");
    Require(DisplayName("Throat slit") == "Throat slit");
    Require(Diagnostic("Actor 00000014: scene loading - Loading HKX") == "Actor 00000014: scene loading - Loading HKX");
#endif
}
