#include "ui/InputMethodState.h"
#include "ui/ime/BackendPolicy.h"

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
        using Ime::Backend;
        Require(Ime::Resolve(Backend::Automatic, false, false, 0) == Backend::ImmShared, "Standalone defaults to shared IMM");
        Require(Ime::Resolve(Backend::Automatic, false, false, 2) == Backend::ImmPrivate, "Disabled shared falls back to private");
        Require(Ime::Resolve(Backend::Automatic, false, false, 6) == Backend::Disabled, "Disabling both native paths leaves clipboard/game input");
        Require(Ime::Resolve(Backend::Automatic, true, false, 0) == Backend::External, "External IME owns its window context");
        Require(Ime::Resolve(Backend::Automatic, true, false, 8) == Backend::Disabled, "Disabling bridge does not take the loaded mod's context");
        for (auto backend : { Backend::ImmShared, Backend::ImmPrivate })
        {
            Require(Ime::Resolve(backend, true, false, 0) == Backend::Disabled, "Explicit native mode cannot race an external mod");
            Require(Ime::Resolve(backend, false, false, 1u << static_cast<int>(backend)) == Backend::Disabled, "Explicit disabled mode does not silently substitute another backend");
        }
        Require(Ime::Resolve(Backend::External, false, false, 0) == Backend::Disabled, "No missing external dependency activation");
        Require(Ime::Resolve(Backend::Automatic, true, true, 0) == Backend::Disabled, "Conflicting external mods never trigger native takeover");
        Ime::CompositionKeys keys;
        Require(!keys.Filter(13, true), "Normal Enter remains available outside composition");
        keys.Begin();
        Require(keys.Filter(13, true) && keys.Filter(27, true), "Candidate confirm/cancel keys are blocked");
        keys.End();
        keys.EndFrame();
        Require(keys.Filter(32, true), "Late game poll from composition boundary is blocked");
        keys.EndFrame();
        Require(!keys.Suppresses(), "Boundary guard ends");
        Require(keys.Filter(27, true), "Held cancel key cannot close the overlay after composition ends");
        Require(keys.Filter(27, false) && !keys.Filter(27, true), "Blocked release completes pair; a fresh Escape is available");
        Require(keys.Filter(13, false) && !keys.Filter(13, true), "Confirm key cannot submit twice; new Enter works");
        Require(!keys.Filter(256, true), "Unknown virtual keys do not index the gate");
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
        Require(!NeedsMenuBridge(Provider::kInputMethodUnlocked), "IMU filtered keys stay on the game input path");
        Require(NeedsMenuBridge(Provider::kSimpleIME) && NeedsMenuBridge(Provider::kSkyrimInputMethod), "Scaleform IMEs require the bridge");
        Require(CanAcquireTextInput(Provider::kInputMethodUnlocked, true, true, true, false), "IMU editing does not wait for a blank menu");
        Require(!CanAcquireTextInput(Provider::kSimpleIME, true, true, true, false), "Menu provider waits for its receiver");
        Require(!CanAcquireTextInput(Provider::kInputMethodUnlocked, true, true, false, false), "Visible overlay alone cannot enable IMU");
        Require(!CanAcquireTextInput(Provider::kInputMethodUnlocked, false, true, true, false), "Closing overlay releases IMU");
        Require(!CanAcquireTextInput(Provider::kInputMethodUnlocked, true, false, true, false), "Application focus loss releases IMU");
        Require(UsesProviderPaste(Provider::kSimpleIME) && UsesProviderPaste(Provider::kSkyrimInputMethod), "Menu IMEs own Unicode paste");
        Require(!UsesProviderPaste(Provider::kInputMethodUnlocked), "IMU keeps ImGui clipboard shortcut");
        Require(IsPasteFallback(Provider::kSimpleIME, 'v', true), "SimpleIME disabled paste falls back from Ctrl+V character");
        Require(!IsPasteFallback(Provider::kSimpleIME, 'v', false), "Regular v must remain text");
        Require(IsPasteFallback(Provider::kSkyrimInputMethod, 0x16, true), "SIM disabled paste falls back from control character");
        Require(!IsPasteFallback(Provider::kSkyrimInputMethod, 'v', true), "SIM clipboard contents may include v while Ctrl is held");

        TextSession session;
        Require(!session.Accepts(session.Generation()), "Unfocused editor rejects text");
        Require(!session.SetFocus(true, 0) && !session.Active(), "A stale IME visibility flag without an active editor must not enable input");
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
        const auto reopened = session.Generation();
        Require(session.SetFocus(true, 0) && !session.Active() && !session.Accepts(reopened), "Losing the active ID releases an existing editor even when the IME flag is stale");

        Utf16Decoder decoder;
        for (auto scalar : { 0x3042u, 0x30A2u, 0xD55Cu, 0x0627u, 0x05D0u, 0x0E01u, 0x0301u })
            Require(decoder.Push(scalar) == std::vector<std::uint32_t>{ scalar }, "Language-neutral Unicode including combining marks");
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
