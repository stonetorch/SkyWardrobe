#pragma once

#include "ui/InputMethodState.h"
#include "ui/ime/BackendPolicy.h"
#include <Windows.h>
#include <optional>

namespace Overlay::InputMethod
{
    void Install();
    void SetOverlayState(bool visible, bool focused);
    void PublishTextFocus(bool wanted, std::uint32_t inputID);
    bool UsesMenuInput();
    bool UsesGameInputMethod();
    bool WantsTextInput();
    void QueueGameCharacter(std::uint32_t codePoint);
    void Flush();
    void AttachWindow(HWND window);
    std::optional<LRESULT> HandleWindowMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void PublishCaret(float x, float y, float lineHeight);
    bool FilterNativeKey(unsigned key, bool down);
    void EndFrame();
    void Configure(Ime::Backend backend, unsigned disabled);
    Ime::Backend RequestedBackend();
    unsigned DisabledBackends();
    bool DrawStatus();
}
