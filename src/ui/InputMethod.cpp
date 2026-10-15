#include <RE/Skyrim.h>

#include "ui/InputMethod.h"
#include "ui/ime/Win32Ime.h"
#include "ui/Overlay.h"
#include "wardrobe/Panel.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <Windows.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string_view>
#include <utility>

namespace Overlay::InputMethod
{
    namespace
    {
        constexpr std::string_view kMenuName = "SkyWardrobe Text Input";
        const char* ProviderName(Provider value)
        {
            switch (value)
            {
            case Provider::kSimpleIME: return "SimpleIME";
            case Provider::kSkyrimInputMethod: return "Skyrim Input Method";
            case Provider::kInputMethodUnlocked: return "Input Method Unlocked";
            case Provider::kConflict: return "conflicting IME plugins";
            default: return "none (clipboard/game input)";
            }
        }
        Provider provider = Provider::kNone;
        Ime::Win32Ime nativeIme;
        std::atomic<Ime::Backend> requestedBackend = Ime::Backend::Automatic;
        std::atomic_uint disabledBackends = 0;
        POINT caret{};
        LONG caretHeight = 20;
        Ime::Backend EffectiveBackend()
        {
            return Ime::Resolve(requestedBackend.load(), IsExternal(provider), provider == Provider::kConflict, disabledBackends.load());
        }
        bool ExternalEnabled() { return EffectiveBackend() == Ime::Backend::External; }
        std::mutex stateMutex;
        TextSession session;
        Utf16Decoder decoder;
        bool overlayVisible = false;
        bool applicationFocused = true;
        bool resetKeys = false;
        bool textLeaseReady = false;
        bool ctrlPressed = false;
        std::atomic_bool menuOpen = false;
        std::atomic_bool syncPending = false;
        // 以下字段只由同一 UITask 队列修改，原生调用时不持有字符队列锁。
        bool menuRequested = false;
        bool ownsTextInput = false;
        std::uint32_t leaseID = 0;
        std::uint64_t reportedCharacterGeneration = 0;

        struct Event
        {
            enum class Kind { kCharacter, kKey, kPaste } kind;
            std::uint32_t code;
            bool down;
            std::uint8_t modifiers;
            std::uint64_t generation;
        };
        std::vector<Event> events;

        void PublishNativeState()
        {
            // 调用方持有 stateMutex；Win32 模块只复制请求，不在这里操作窗口资源。
            nativeIme.Publish({ EffectiveBackend(), session.Generation(), overlayVisible && applicationFocused && session.Active(), caret, caretHeight });
        }

        void ScheduleSync();

        void NativeAllowTextInput(bool allow)
        {
            if (auto* controls = RE::ControlMap::GetSingleton())
            {
                // CommonLib 同名方法只改计数；必须进入被输入法模组 hook 的游戏原生函数。
                using Function = void(RE::ControlMap*, bool);
                static REL::Relocation<Function> function{ RELOCATION_ID(67252, 68552) };
                function(controls, allow);
            }
        }

        void InvalidateInput()
        {
            session.Invalidate();
            events.clear();
            decoder.Reset();
            resetKeys = true;
            textLeaseReady = false;
            ctrlPressed = false;
            PublishNativeState();
        }

        void SyncGameState()
        {
            syncPending = false;
            bool show;
            bool want;
            std::uint32_t id;
            std::uint64_t generation;
            {
                std::lock_guard lock(stateMutex);
                show = ExternalEnabled() && overlayVisible && applicationFocused && NeedsMenuBridge(provider);
                want = ExternalEnabled() && CanAcquireTextInput(provider, overlayVisible, applicationFocused, session.Active(), menuOpen.load());
                id = session.ID();
                generation = session.Generation();
            }
            // 先释放本模组的申请，再隐藏接收菜单；切换输入框也结束上一轮组词。
            if (ownsTextInput && (!want || leaseID != id))
            {
                NativeAllowTextInput(false);
                SKSE::log::info("IME text lease released: provider={}, editor={}, count={}", ProviderName(provider), leaseID,
                    static_cast<int>(RE::ControlMap::GetSingleton()->GetRuntimeData().textEntryCount));
                spdlog::default_logger()->flush();
                ownsTextInput = false;
                leaseID = 0;
            }
            if (show != menuRequested)
            {
                if (auto* queue = RE::UIMessageQueue::GetSingleton())
                {
                    queue->AddMessage(RE::BSFixedString(kMenuName), show ? RE::UI_MESSAGE_TYPE::kShow : RE::UI_MESSAGE_TYPE::kHide, nullptr);
                    menuRequested = show;
                }
            }
            if (want && !ownsTextInput && RE::ControlMap::GetSingleton())
            {
                NativeAllowTextInput(true);
                ownsTextInput = true;
                leaseID = id;
                SKSE::log::info("IME text lease acquired: provider={}, editor={}, count={}", ProviderName(provider), id,
                    static_cast<int>(RE::ControlMap::GetSingleton()->GetRuntimeData().textEntryCount));
                spdlog::default_logger()->flush();
            }
            {
                std::lock_guard lock(stateMutex);
                if (generation == session.Generation()) textLeaseReady = ownsTextInput && want;
            }
        }

        void ScheduleSync()
        {
            if (syncPending.exchange(true)) return;
            if (const auto* tasks = SKSE::GetTaskInterface()) tasks->AddUITask(SyncGameState);
            else syncPending = false;
        }

        void QueueCharacter(std::uint32_t code, CharacterSource source)
        {
            std::lock_guard lock(stateMutex);
            if (provider == Provider::kInputMethodUnlocked && source == CharacterSource::kGame && code >= 0x100 &&
                reportedCharacterGeneration != session.Generation())
            {
                reportedCharacterGeneration = session.Generation();
                // 每个编辑会话仅记录一次非 ASCII 事件的接收条件，不记录用户输入内容。
                SKSE::log::info("IMU character arrived: editing={}, editor={}, visible={}, focused={}, leaseReady={}",
                    session.Active(), session.ID(), overlayVisible, applicationFocused, textLeaseReady);
                spdlog::default_logger()->flush();
            }
            if (!overlayVisible || !applicationFocused || !session.Active()) return;
            if (Ime::IsNative(EffectiveBackend()) && nativeIme.Snapshot().active) return;
            if (!AcceptsSource(ExternalEnabled() ? provider : Provider::kNone, source)) return;
            if (ExternalEnabled() && !textLeaseReady) return;
            if (source == CharacterSource::kScaleform && IsPasteFallback(provider, code, ctrlPressed))
            {
                events.push_back({ Event::Kind::kPaste, 0, true, 0, session.Generation() });
                return;
            }
            // Scaleform 输入法发送 UTF-16 单元，IMU 的游戏 CharEvent 已经是 Unicode 码点。
            if (source == CharacterSource::kScaleform)
            {
                for (auto scalar : decoder.Push(code)) events.push_back({ Event::Kind::kCharacter, scalar, true, 0, session.Generation() });
            }
            else
            {
                if (code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) code = 0xFFFD;
                events.push_back({ Event::Kind::kCharacter, code, true, 0, session.Generation() });
            }
        }

        ImGuiKey MapKey(std::uint32_t code)
        {
            if (code >= RE::GFxKey::kA && code <= RE::GFxKey::kZ) return static_cast<ImGuiKey>(ImGuiKey_A + code - RE::GFxKey::kA);
            if (code >= RE::GFxKey::kNum0 && code <= RE::GFxKey::kNum9) return static_cast<ImGuiKey>(ImGuiKey_0 + code - RE::GFxKey::kNum0);
            if (code >= RE::GFxKey::kF1 && code <= RE::GFxKey::kF15) return static_cast<ImGuiKey>(ImGuiKey_F1 + code - RE::GFxKey::kF1);
            if (code >= RE::GFxKey::kKP_0 && code <= RE::GFxKey::kKP_9) return static_cast<ImGuiKey>(ImGuiKey_Keypad0 + code - RE::GFxKey::kKP_0);
            switch (code)
            {
            case RE::GFxKey::kBackspace: return ImGuiKey_Backspace;
            case RE::GFxKey::kTab: return ImGuiKey_Tab;
            case RE::GFxKey::kReturn: return ImGuiKey_Enter;
            case RE::GFxKey::kKP_Enter: return ImGuiKey_KeypadEnter;
            case RE::GFxKey::kEscape: return ImGuiKey_Escape;
            case RE::GFxKey::kSpace: return ImGuiKey_Space;
            case RE::GFxKey::kPageUp: return ImGuiKey_PageUp;
            case RE::GFxKey::kPageDown: return ImGuiKey_PageDown;
            case RE::GFxKey::kEnd: return ImGuiKey_End;
            case RE::GFxKey::kHome: return ImGuiKey_Home;
            case RE::GFxKey::kLeft: return ImGuiKey_LeftArrow;
            case RE::GFxKey::kUp: return ImGuiKey_UpArrow;
            case RE::GFxKey::kRight: return ImGuiKey_RightArrow;
            case RE::GFxKey::kDown: return ImGuiKey_DownArrow;
            case RE::GFxKey::kInsert: return ImGuiKey_Insert;
            case RE::GFxKey::kDelete: return ImGuiKey_Delete;
            case RE::GFxKey::kShift: return ImGuiKey_LeftShift;
            case RE::GFxKey::kControl: return ImGuiKey_LeftCtrl;
            case RE::GFxKey::kAlt: return ImGuiKey_LeftAlt;
            case RE::GFxKey::kSemicolon: return ImGuiKey_Semicolon;
            case RE::GFxKey::kEqual: return ImGuiKey_Equal;
            case RE::GFxKey::kComma: return ImGuiKey_Comma;
            case RE::GFxKey::kMinus: return ImGuiKey_Minus;
            case RE::GFxKey::kPeriod: return ImGuiKey_Period;
            case RE::GFxKey::kSlash: return ImGuiKey_Slash;
            case RE::GFxKey::kBar: return ImGuiKey_GraveAccent;
            case RE::GFxKey::kBracketLeft: return ImGuiKey_LeftBracket;
            case RE::GFxKey::kBackslash: return ImGuiKey_Backslash;
            case RE::GFxKey::kBracketRight: return ImGuiKey_RightBracket;
            case RE::GFxKey::kQuote: return ImGuiKey_Apostrophe;
            case RE::GFxKey::kKP_Multiply: return ImGuiKey_KeypadMultiply;
            case RE::GFxKey::kKP_Add: return ImGuiKey_KeypadAdd;
            case RE::GFxKey::kKP_Subtract: return ImGuiKey_KeypadSubtract;
            case RE::GFxKey::kKP_Decimal: return ImGuiKey_KeypadDecimal;
            case RE::GFxKey::kKP_Divide: return ImGuiKey_KeypadDivide;
            default: return ImGuiKey_None;
            }
        }

        class TextInputMenu final : public RE::IMenu
        {
        public:
            TextInputMenu()
            {
                // 无 SWF、不暂停、不接管指针；位于 SimpleIME 的 depth 13 下方接收放行事件。
                depthPriority = 10;
                menuFlags.set(RE::UI_MENU_FLAGS::kCustomRendering, RE::UI_MENU_FLAGS::kUsesMenuContext,
                    RE::UI_MENU_FLAGS::kAllowSaving, RE::UI_MENU_FLAGS::kDisablePauseMenu);
                inputContext = RE::UserEvents::INPUT_CONTEXT_ID::kMenuMode;
            }

            static RE::IMenu* Create() { return new TextInputMenu(); }

            RE::UI_MESSAGE_RESULTS ProcessMessage(RE::UIMessage& message) override
            {
                if (message.type == RE::UI_MESSAGE_TYPE::kShow || message.type == RE::UI_MESSAGE_TYPE::kReshow)
                {
                    menuOpen = true;
                    ScheduleSync();
                    return RE::UI_MESSAGE_RESULTS::kHandled;
                }
                if (message.type == RE::UI_MESSAGE_TYPE::kHide || message.type == RE::UI_MESSAGE_TYPE::kForceHide)
                {
                    menuOpen = false;
                    bool close;
                    {
                        std::lock_guard lock(stateMutex);
                        close = overlayVisible && ExternalEnabled();
                        if (close)
                        {
                            overlayVisible = false;
                            session.SetFocus(false, 0);
                            InvalidateInput();
                        }
                    }
                    if (close) Overlay::CloseAllMenus();
                    ScheduleSync();
                    return RE::UI_MESSAGE_RESULTS::kHandled;
                }
                if (message.type != RE::UI_MESSAGE_TYPE::kScaleformEvent) return RE::UI_MESSAGE_RESULTS::kPassOn;
                const auto* data = static_cast<RE::BSUIScaleformData*>(message.data);
                if (!data || !data->scaleformEvent) return RE::UI_MESSAGE_RESULTS::kPassOn;
                const auto* event = data->scaleformEvent;
                if (event->type == RE::GFxEvent::EventType::kCharEvent)
                {
                    // 两个模组的公开 GFxCharEvent 布局均在偏移 4 放置 wcharCode；不保存事件指针。
                    static_assert(sizeof(RE::GFxEvent) == 4);
                    std::uint32_t code = 0;
                    std::memcpy(&code, reinterpret_cast<const std::byte*>(event) + sizeof(RE::GFxEvent), sizeof(code));
                    QueueCharacter(code, CharacterSource::kScaleform);
                }
                else if (event->type == RE::GFxEvent::EventType::kKeyDown || event->type == RE::GFxEvent::EventType::kKeyUp)
                {
                    const auto* key = static_cast<const RE::GFxKeyEvent*>(event);
                    const bool down = event->type == RE::GFxEvent::EventType::kKeyDown;
                    bool close = false;
                    {
                        std::lock_guard lock(stateMutex);
                        if (ExternalEnabled() && overlayVisible && applicationFocused)
                        {
                            close = down && key->keyCode == RE::GFxKey::kEscape && !session.Active();
                            ctrlPressed = (key->specialKeyState.states & 2) != 0;
                            if (key->keyCode == RE::GFxKey::kControl) ctrlPressed = down;
                            // 菜单型输入法自己投递 Unicode 粘贴；避免 ImGui 的 Ctrl+V 再粘贴一次。
                            const bool providerPaste = session.Active() && UsesProviderPaste(provider) &&
                                key->keyCode == RE::GFxKey::kV && ctrlPressed;
                            if (!providerPaste)
                                events.push_back({ Event::Kind::kKey, key->keyCode, down, key->specialKeyState.states, session.Generation() });
                        }
                    }
                    if (close) Overlay::CloseAllMenus();
                }
                // 只由衣柜消费放行的菜单输入，避免点击或字符泄漏给下面的原生菜单。
                return RE::UI_MESSAGE_RESULTS::kHandled;
            }
        };

        class MenuObserver final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
            {
                if (!event || !event->opening || event->menuName == kMenuName || event->menuName == "SimpleImeMenu")
                    return RE::BSEventNotifyControl::kContinue;
                if (auto* ui = RE::UI::GetSingleton())
                {
                    const auto menu = ui->GetMenu(event->menuName.c_str());
                    if (menu && (menu->UsesMenuContext() || menu->PausesGame()))
                    {
                        SetOverlayState(false, true);
                        Overlay::CloseAllMenus();
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        } menuObserver;
    }

    void Install()
    {
        provider = SelectProvider(GetModuleHandleW(L"SimpleIME.dll") != nullptr,
            GetModuleHandleW(L"SkyrimInputMethod.dll") != nullptr, GetModuleHandleW(L"InputMethodUnlocked.dll") != nullptr);
        SKSE::log::info("External IME provider: {}", ProviderName(provider));
        spdlog::default_logger()->flush();
        if (auto* ui = RE::UI::GetSingleton())
        {
            if (NeedsMenuBridge(provider)) ui->Register(kMenuName, TextInputMenu::Create);
            ui->AddEventSink<RE::MenuOpenCloseEvent>(&menuObserver);
        }
        if (provider == Provider::kConflict)
            SKSE::log::warn("Multiple external IME plugins loaded; SkyWardrobe will not acquire text input");
    }

    void SetOverlayState(bool visible, bool focused)
    {
        bool changed;
        {
            std::lock_guard lock(stateMutex);
            changed = overlayVisible != visible || applicationFocused != focused;
            overlayVisible = visible;
            applicationFocused = focused;
            if (!visible || !focused) session.SetFocus(false, 0);
            if (changed) InvalidateInput();
        }
        if (changed) ScheduleSync();
    }

    void PublishTextFocus(bool wanted, std::uint32_t inputID)
    {
        bool changed;
        {
            std::lock_guard lock(stateMutex);
            changed = session.SetFocus(wanted && overlayVisible && applicationFocused, inputID);
            if (changed) InvalidateInput();
        }
        if (changed) ScheduleSync();
    }

    bool UsesMenuInput()
    {
        std::lock_guard lock(stateMutex);
        return ExternalEnabled() && NeedsMenuBridge(provider) && overlayVisible && applicationFocused && menuOpen.load();
    }

    bool UsesGameInputMethod()
    {
        std::lock_guard lock(stateMutex);
        return ExternalEnabled() && provider == Provider::kInputMethodUnlocked && overlayVisible && applicationFocused;
    }

    bool WantsTextInput()
    {
        std::lock_guard lock(stateMutex);
        return session.Active();
    }

    void QueueGameCharacter(std::uint32_t codePoint) { QueueCharacter(codePoint, CharacterSource::kGame); }

    void Flush()
    {
        const auto nativeCommits = nativeIme.Drain();
        std::vector<Event> pending;
        bool clear;
        std::uint64_t generation;
        bool active;
        {
            std::lock_guard lock(stateMutex);
            if (session.Active() && Ime::IsNative(EffectiveBackend()))
            {
                for (const auto& commit : nativeCommits)
                {
                    if (!session.Accepts(commit.generation)) continue;
                    for (auto unit : commit.text)
                        for (auto scalar : decoder.Push(unit))
                            if (scalar >= 32 && scalar != 127)
                                events.push_back({ Event::Kind::kCharacter, scalar, true, 0, commit.generation });
                }
            }
            pending.swap(events);
            clear = std::exchange(resetKeys, false);
            generation = session.Generation();
            active = session.Active();
        }
        auto& io = ImGui::GetIO();
        if (clear || nativeIme.Snapshot().suppressKeys) io.ClearInputKeys();
        // 本模块只在绘制线程写 ImGui；跨线程提交按编辑会话校验，重复文字照常保留。
        for (const auto& event : pending)
        {
            if (event.generation != generation) continue;
            if (event.kind == Event::Kind::kCharacter)
            {
                if (active && event.code != 0) io.AddInputCharacter(event.code);
            }
            else if (event.kind == Event::Kind::kPaste)
            {
                if (!active) continue;
                // 外部模组禁用其 Unicode 粘贴时，让 ImGui 使用现有剪贴板实现。
                io.AddKeyEvent(ImGuiMod_Ctrl, true);
                io.AddKeyEvent(ImGuiKey_V, true);
                io.AddKeyEvent(ImGuiKey_V, false);
            }
            else
            {
                if (const auto key = MapKey(event.code); key != ImGuiKey_None) io.AddKeyEvent(key, event.down);
                io.AddKeyEvent(ImGuiMod_Shift, (event.modifiers & 1) != 0);
                io.AddKeyEvent(ImGuiMod_Ctrl, (event.modifiers & 2) != 0);
                io.AddKeyEvent(ImGuiMod_Alt, (event.modifiers & 4) != 0);
            }
        }
    }

    void AttachWindow(HWND window) { nativeIme.Attach(window); }

    std::optional<LRESULT> HandleWindowMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_KILLFOCUS && Ime::IsNative(EffectiveBackend()))
        {
            std::lock_guard lock(stateMutex);
            session.SetFocus(false, 0);
            InvalidateInput();
        }
        return nativeIme.Handle(window, message, wParam, lParam);
    }

    void PublishCaret(float x, float y, float lineHeight)
    {
        std::lock_guard lock(stateMutex);
        caret = { static_cast<LONG>(x), static_cast<LONG>(y) };
        caretHeight = static_cast<LONG>(lineHeight);
        PublishNativeState();
    }

    bool FilterNativeKey(unsigned key, bool down) { return nativeIme.FilterKey(key, down); }
    void EndFrame() { nativeIme.EndFrame(); }
    Ime::Backend RequestedBackend() { return requestedBackend.load(); }
    unsigned DisabledBackends() { return disabledBackends.load(); }

    void Configure(Ime::Backend backend, unsigned disabled)
    {
        if (backend < Ime::Backend::Automatic || backend > Ime::Backend::Disabled) backend = Ime::Backend::Automatic;
        requestedBackend = backend;
        disabledBackends = disabled & 14u;
        {
            std::lock_guard lock(stateMutex);
            session.SetFocus(false, 0);
            InvalidateInput();
        }
        ScheduleSync();
        SKSE::log::info("IME backend requested={}, effective={}, disabled={}", static_cast<int>(backend), static_cast<int>(EffectiveBackend()), disabledBackends.load());
    }

    bool DrawStatus()
    {
        using Wardrobe::Tr;
        const char* name = "Ctrl+V";
        switch (provider)
        {
        case Provider::kSimpleIME: name = "SimpleIME"; break;
        case Provider::kSkyrimInputMethod: name = "Skyrim Input Method"; break;
        case Provider::kInputMethodUnlocked: name = "Input Method Unlocked"; break;
        case Provider::kConflict: name = Tr("Multiple IME plugins loaded", "同时加载了多个输入法模组"); break;
        default: break;
        }
        ImGui::Text("%s: %s", Tr("Detected external IME", "检测到的外部 IME"), name);
        if (provider == Provider::kSimpleIME)
            ImGui::TextWrapped("%s", Tr("In SimpleIME, set IME window positioning to Follow Cursor or None.", "请在 SimpleIME 中将候选窗位置设为 Follow Cursor 或 None。"));
        if (provider == Provider::kConflict)
            ImGui::TextWrapped("%s", Tr("Enable one IME plugin and restart the game. Clipboard paste remains available.", "请只启用一个输入法模组并重启游戏。仍可使用剪贴板粘贴。"));
        const char* names[] = { Tr("Automatic", "自动"), "IMM shared [1]", "IMM private [2]",
            Tr("External mod bridge [3]", "外部模组桥接 [3]"), Tr("Disabled / clipboard", "禁用 / 剪贴板") };
        int selection = static_cast<int>(RequestedBackend());
        bool changed = ImGui::Combo(Tr("IME backend", "IME 实现"), &selection, names, 5);
        auto disabled = DisabledBackends();
        for (int index = 1; index <= 3; ++index)
        {
            bool enabled = !(disabled & (1u << index));
            ImGui::PushID(index);
            if (ImGui::Checkbox(names[index], &enabled))
            {
                disabled = enabled ? disabled & ~(1u << index) : disabled | (1u << index);
                changed = true;
            }
            ImGui::PopID();
            if (index < 3) ImGui::SameLine();
        }
        if (changed)
        {
            ImGui::ClearActiveID();
            Configure(static_cast<Ime::Backend>(selection), disabled);
        }
        ImGui::Text("%s: %s", Tr("Effective backend", "实际实现"), names[static_cast<int>(EffectiveBackend())]);
        ImGui::TextWrapped("%s", Tr("Uncheck a backend to disable it persistently. Native IMM is blocked while known external IME mods are loaded. Shared borrows the existing context; private restores the original context after editing.",
            "取消勾选可持久禁用对应实现。已加载已知外部输入法模组时，原生 IMM 不接管窗口。Shared 复用现有上下文；Private 在编辑结束后恢复原关联。"));
        const auto status = nativeIme.Snapshot();
        ImGui::Text("IMM: active=%d, context=%d, composing=%d, commits=%u, failures=%u", status.active, status.contextAvailable, status.composing, status.commits, status.failures);
        if (status.associationChanged)
            ImGui::TextWrapped("%s", Tr("Another component changed the IME context; this session was released.", "其他组件改变了 IME 上下文，当前接入已释放。"));
        if (Ime::IsNative(EffectiveBackend()) && !status.contextAvailable)
            ImGui::TextWrapped("%s", Tr("Focus the test field. If shared has no context, try private [2].", "请聚焦测试框；若 Shared 仍无上下文，切换 Private [2] 测试。"));
        static char testText[4096]{};
        static char secondText[4096]{};
        ImGui::InputText(Tr("IME test A", "IME 测试 A"), testText, sizeof(testText));
        ImGui::InputText(Tr("IME test B", "IME 测试 B"), secondText, sizeof(secondText));
        if (ImGui::Button(Tr("Clear test text", "清空测试文字"))) testText[0] = secondText[0] = '\0';
        ImGui::TextWrapped("%s", Tr("Test your language, repeated text, long commits, Backspace, candidate selection, Esc, switching A/B and Alt+Tab. Test text is not saved.",
            "测试你的语言、重复文字、长句、退格、候选选择、Esc、A/B 切换与 Alt+Tab。测试文字不保存。"));
        return changed;
    }
}
