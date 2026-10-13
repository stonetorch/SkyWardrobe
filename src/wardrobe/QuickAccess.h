#pragma once
#include <imgui.h>
#include <string>

namespace Wardrobe
{
    struct QuickAccessRequest
    {
        std::string action, target;
    };
    QuickAccessRequest DrawQuickAccess(ImVec2 panelPosition, ImVec2 panelSize);
    void DrawQuickAccessSettings();
}
