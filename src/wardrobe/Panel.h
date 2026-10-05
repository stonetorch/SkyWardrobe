#pragma once
#include <string>

namespace Wardrobe
{
    constexpr const char* Tr(const char* english, const char* chinese)
    {
#if CAF_UI_ZH_CN
        return chinese;
#else
        return english;
#endif
    }
    void DrawPanel();
    void ProcessUiRequests();
    void QueueUiCommand(std::string action, std::string argument = {});
}
