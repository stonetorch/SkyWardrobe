#pragma once
struct ImFont;
namespace Overlay
{
    ImFont* NoteFont();
    void Install();
    bool IsVisible();
    void CloseAllMenus();
    void Open();
    void DrawInterfaceSettings();
}
