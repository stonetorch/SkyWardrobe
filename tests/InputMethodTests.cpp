#include "ui/InputMethodState.h"

#include <iostream>
#include <stdexcept>

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }
}

int main()
{
    using namespace Overlay::InputMethod;
    try
    {
        Require(SelectProvider(false, false, false) == Provider::kNone, "No plugin keeps game input");
        Require(SelectProvider(true, false, false) == Provider::kSimpleIME, "Detect SimpleIME");
        Require(SelectProvider(false, true, false) == Provider::kSkyrimInputMethod, "Detect SIM");
        Require(SelectProvider(false, false, true) == Provider::kInputMethodUnlocked, "Detect IMU");
        Require(SelectProvider(true, true, false) == Provider::kConflict && !IsExternal(Provider::kConflict), "Do not acquire competing IMEs");
        for (auto provider : { Provider::kSimpleIME, Provider::kSkyrimInputMethod })
        {
            Require(AcceptsSource(provider, CharacterSource::kScaleform), "Menu providers accept committed Scaleform text");
            Require(!AcceptsSource(provider, CharacterSource::kGame), "Raw keyboard text must not duplicate menu IME commits");
        }
        Require(AcceptsSource(Provider::kInputMethodUnlocked, CharacterSource::kGame), "IMU uses game Unicode code points");
        Require(!AcceptsSource(Provider::kInputMethodUnlocked, CharacterSource::kScaleform), "Ignore the same game chars translated by MenuControls");
        Require(UsesProviderPaste(Provider::kSimpleIME) && UsesProviderPaste(Provider::kSkyrimInputMethod), "Menu IMEs own Unicode paste");
        Require(!UsesProviderPaste(Provider::kInputMethodUnlocked), "IMU keeps ImGui clipboard shortcut");
        Require(IsPasteFallback(Provider::kSimpleIME, 'v', true), "SimpleIME disabled paste falls back from Ctrl+V character");
        Require(!IsPasteFallback(Provider::kSimpleIME, 'v', false), "Regular v must remain text");
        Require(IsPasteFallback(Provider::kSkyrimInputMethod, 0x16, true), "SIM disabled paste falls back from control character");
        Require(!IsPasteFallback(Provider::kSkyrimInputMethod, 'v', true), "SIM clipboard contents may include v while Ctrl is held");

        TextSession session;
        Require(!session.Accepts(session.Generation()), "Unfocused editor rejects text");
        Require(session.SetFocus(true, 101), "Enter text input");
        const auto first = session.Generation();
        Require(session.Accepts(first), "Current editor accepts its queued text");
        Require(!session.SetFocus(true, 101) && session.Generation() == first, "Repeated frames must not reacquire text input");
        Require(session.SetFocus(true, 202) && !session.Accepts(first), "Switching editors drops old queued commits");
        const auto second = session.Generation();
        session.SetFocus(false, 202);
        Require(!session.Accepts(second) && session.ID() == 0, "Close or focus loss releases editor identity");
        session.SetFocus(true, 202);
        Require(!session.Accepts(second), "Reopening the same editor cannot revive stale commits");

        Utf16Decoder decoder;
        Require(decoder.Push(0x88D9) == std::vector<std::uint32_t>{0x88D9}, "Chinese BMP character");
        Require(decoder.Push(0x88D9) == std::vector<std::uint32_t>{0x88D9}, "Repeated Chinese character is preserved");
        Require(decoder.Push(0xD840).empty(), "Hold high surrogate across frames");
        Require(decoder.Push(0xDC00) == std::vector<std::uint32_t>{0x20000}, "Decode supplementary CJK");
        decoder.Push(0xD840);
        decoder.Reset();
        Require(decoder.Push(0xDC00) == std::vector<std::uint32_t>{0xFFFD}, "New editor must not inherit old surrogate");
        decoder.Push(0xD840);
        Require(decoder.Push('A') == std::vector<std::uint32_t>{0xFFFD, 'A'}, "Malformed pair does not swallow following ASCII");
        Require(decoder.Push(0x110000) == std::vector<std::uint32_t>{0xFFFD}, "Reject invalid scalar");
        std::cout << "Input method routing/session/Unicode tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
