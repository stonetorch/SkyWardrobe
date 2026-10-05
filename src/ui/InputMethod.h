#pragma once

#include "ui/InputMethodState.h"

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
    void DrawStatus();
}
