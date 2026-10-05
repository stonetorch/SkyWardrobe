#include "ui/Text.h"

namespace CAF::Ui::Text
{
#if CAF_UI_ZH_CN
    namespace
    {
        struct Translation
        {
            std::string_view source;
            std::string_view display;
        };
        constexpr Translation labels[]{
#define CAF_TEXT(name, english, chinese)
#define CAF_DIAGNOSTIC(name, english, chinese)
#define CAF_LABEL(name, english, chinese) {english, name},
#include "ui/Text.def"
#undef CAF_LABEL
#undef CAF_DIAGNOSTIC
#undef CAF_TEXT
        };
        constexpr Translation diagnostics[]{
#define CAF_TEXT(name, english, chinese)
#define CAF_LABEL(name, english, chinese)
#define CAF_DIAGNOSTIC(name, english, chinese) {english, name},
#include "ui/Text.def"
#undef CAF_DIAGNOSTIC
#undef CAF_LABEL
#undef CAF_TEXT
        };

        constexpr bool IdentifierCharacter(char value)
        {
            return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
                (value >= '0' && value <= '9') || value == '_' || value == '.' ||
                value == '/' || value == '\\' || value == '|';
        }
    }
#endif

    std::string_view DisplayName(std::string_view name)
    {
#if CAF_UI_ZH_CN
        for (const auto& translation : labels)
            if (translation.source == name) return translation.display;
#endif
        return name;
    }

    std::string Diagnostic(std::string_view message)
    {
#if CAF_UI_ZH_CN
        std::string result;
        result.reserve(message.size());
        for (std::size_t offset = 0; offset < message.size();)
        {
            // 只匹配原始输入，并优先使用最长片段；译文不再参与替换，动态 ID/数值原样保留。
            const Translation* match = nullptr;
            for (const auto& translation : diagnostics)
            {
                const auto end = offset + translation.source.size();
                // 不替换文件路径、EditorID 等标识内部恰好出现的英文状态词。
                if (IdentifierCharacter(translation.source.front()) && offset > 0 && IdentifierCharacter(message[offset - 1])) continue;
                if (IdentifierCharacter(translation.source.back()) && end < message.size() && IdentifierCharacter(message[end])) continue;
                if (message.substr(offset).starts_with(translation.source) &&
                    (!match || translation.source.size() > match->source.size())) match = &translation;
            }
            if (match)
            {
                result.append(match->display);
                offset += match->source.size();
            }
            else
            {
                result.push_back(message[offset++]);
            }
        }
        return result;
#else
        return std::string(message);
#endif
    }
}
