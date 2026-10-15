#include <RE/Skyrim.h>

#include "wardrobe/ActorRuntime.h"
#include "ui/Overlay.h"
#include "ui/IconsFontAwesome6.h"
#include "ui/UiTabs.h"
#include "ui/InputMethod.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <MinHook.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <format>
#include <mutex>
#include <string>
#include <vector>

#include "wardrobe/Panel.h"
namespace
{
    constexpr UINT kDefaultKey = VK_F11;
    constexpr float kDefaultScale = 1.5f;
    constexpr float kMinScale = 0.75f;
    constexpr float kMaxScale = 3.0f;
    constexpr float kDefaultCursorSensitivity = 1.0f;
    constexpr float kMinCursorSensitivity = 0.1f;
    constexpr float kMaxCursorSensitivity = 4.0f;

    // 图标字体随 SkyWardrobe 分发。
    constexpr const char* kIconFontPath = "Data/SKSE/Plugins/SkyWardrobe/Fonts/fa-solid-900.ttf";

    using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

    PresentFn originalPresent = nullptr;
    WNDPROC originalWndProc = nullptr;
    HWND window = nullptr;
    bool unicodeWindow = false;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    bool initialized = false;
    bool visible = false;
    std::atomic_bool inputOverlayVisible = false;
    bool hotkeyEnabled = false;
    std::atomic_int visibilityRequest = -1;
    std::atomic_bool closeAllMenusRequested = false;
    bool waitingForKey = false;
    bool previousLookInputEnabled = true;
    bool lookInputSuspended = false;
    bool cursorShownByOverlay = false;
    UINT toggleKey = kDefaultKey;
    void SaveSettings();
    std::string fontPath = "C:/Windows/Fonts/msyh.ttc";
    float fontSize = 20.0f, smallFontSize = 16.0f;
    ImFont* noteFont = nullptr;
    bool leftCtrl = false, rightCtrl = false;
    void CaptureKey(UINT code)
    {
        waitingForKey = false;
        if (code == VK_ESCAPE) return;
        toggleKey = code;
        SaveSettings();
    }
    float uiScale = kDefaultScale;
    float cursorSensitivity = kDefaultCursorSensitivity;
    ImVec2 cursorPosition{};
    bool cursorPositionInitialized = false;
    ImGuiStyle baseStyle;
    std::chrono::steady_clock::time_point lastFrameTime{};

    struct InputRecord
    {
        enum class Kind { kMouseMove, kMouseButton, kMouseWheel, kKey } kind;
        UINT code;
        bool down;
        std::int32_t deltaX = 0;
        std::int32_t deltaY = 0;
        bool initialPress = false;
    };
    std::mutex inputMutex;
    std::vector<InputRecord> inputRecords;

    void SaveSettings();

    UINT ScanCodeToVirtualKey(std::uint32_t scanCode)
    {
        const auto windowsScanCode = (scanCode & 0x80) ? (0xE000 | (scanCode & 0x7F)) : scanCode;
        return MapVirtualKeyW(windowsScanCode, MAPVK_VSC_TO_VK_EX);
    }

    ImGuiKey VirtualKeyToImGuiKey(UINT key)
    {
        if (key >= '0' && key <= '9') return static_cast<ImGuiKey>(ImGuiKey_0 + key - '0');
        if (key >= 'A' && key <= 'Z') return static_cast<ImGuiKey>(ImGuiKey_A + key - 'A');
        if (key >= VK_F1 && key <= VK_F12) return static_cast<ImGuiKey>(ImGuiKey_F1 + key - VK_F1);
        switch (key)
        {
        case VK_TAB: return ImGuiKey_Tab;
        case VK_LEFT: return ImGuiKey_LeftArrow;
        case VK_RIGHT: return ImGuiKey_RightArrow;
        case VK_UP: return ImGuiKey_UpArrow;
        case VK_DOWN: return ImGuiKey_DownArrow;
        case VK_PRIOR: return ImGuiKey_PageUp;
        case VK_NEXT: return ImGuiKey_PageDown;
        case VK_HOME: return ImGuiKey_Home;
        case VK_END: return ImGuiKey_End;
        case VK_INSERT: return ImGuiKey_Insert;
        case VK_DELETE: return ImGuiKey_Delete;
        case VK_BACK: return ImGuiKey_Backspace;
        case VK_SPACE: return ImGuiKey_Space;
        case VK_RETURN: return ImGuiKey_Enter;
        case VK_ESCAPE: return ImGuiKey_Escape;
        case VK_LCONTROL: return ImGuiKey_LeftCtrl;
        case VK_RCONTROL: return ImGuiKey_RightCtrl;
        case VK_LSHIFT: return ImGuiKey_LeftShift;
        case VK_RSHIFT: return ImGuiKey_RightShift;
        case VK_LMENU: return ImGuiKey_LeftAlt;
        case VK_RMENU: return ImGuiKey_RightAlt;
        default: return ImGuiKey_None;
        }
    }

    class OverlayInputSink final : public RE::BSTEventSink<RE::InputEvent*>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* events, RE::BSTEventSource<RE::InputEvent*>* source) override
        {
            if (!inputOverlayVisible.load() || !events) return RE::BSEventNotifyControl::kContinue;
            const bool menuInput = Overlay::InputMethod::UsesMenuInput();
            {
                std::lock_guard lock(inputMutex);
                for (auto* event = *events; event; event = event->next)
                {
                    if (auto* button = event->AsButtonEvent())
                    {
                        const auto code = button->GetIDCode();
                        const auto down = button->IsPressed();
                        if (event->GetDevice() == RE::INPUT_DEVICE::kMouse)
                        {
                            if (code < 5) inputRecords.push_back({ InputRecord::Kind::kMouseButton, code, down });
                            else if (code == 8 || code == 9) inputRecords.push_back({ InputRecord::Kind::kMouseWheel, code, down });
                        }
                        else if (event->GetDevice() == RE::INPUT_DEVICE::kKeyboard)
                        {
                            const auto virtualKey = ScanCodeToVirtualKey(code);
                            if (virtualKey && !menuInput) inputRecords.push_back({ InputRecord::Kind::kKey, virtualKey, down, 0, 0, button->IsDown() });
                        }
                    }
                    else if (auto* character = event->AsCharEvent())
                    {
                        Overlay::InputMethod::QueueGameCharacter(character->keyCode);
                    }
                    else if (auto* movement = event->AsMouseMoveEvent())
                    {
                        inputRecords.push_back({ InputRecord::Kind::kMouseMove, 0, false,
                            movement->mouseInputX, movement->mouseInputY });
                    }
                }
            }
            // 显式交给菜单输入链，让输入法先过滤组词按键；仍截断 PlayerControls，避免游戏动作。
            // 不在 inputMutex 内调用引擎，菜单回调可能同步排队新的 UI 输入。
            if (menuInput)
                if (auto* menus = RE::MenuControls::GetSingleton()) menus->ProcessEvent(events, source);
            return RE::BSEventNotifyControl::kStop;
        }
    } inputSink;

    void FlushInputRecords()
    {
        std::vector<InputRecord> records;
        {
            std::lock_guard lock(inputMutex);
            records.swap(inputRecords);
        }
        auto& io = ImGui::GetIO();
        if (!cursorPositionInitialized)
        {
            // 首次打开界面时从屏幕中心开始，之后只累计游戏提供的相对位移。
            cursorPosition = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
            cursorPositionInitialized = true;
        }

        const auto clampCursorPosition = [&io]()
        {
            cursorPosition.x = std::clamp(cursorPosition.x, 0.0f, std::max(0.0f, io.DisplaySize.x - 1.0f));
            cursorPosition.y = std::clamp(cursorPosition.y, 0.0f, std::max(0.0f, io.DisplaySize.y - 1.0f));
        };

        for (const auto& record : records)
        {
            switch (record.kind)
            {
            case InputRecord::Kind::kMouseMove:
                cursorPosition.x += static_cast<float>(record.deltaX) * cursorSensitivity;
                cursorPosition.y += static_cast<float>(record.deltaY) * cursorSensitivity;
                clampCursorPosition();
                // 保持与游戏事件相同的顺序，随后到达的左键会以这里的位置命中。
                io.AddMousePosEvent(cursorPosition.x, cursorPosition.y);
                break;
            case InputRecord::Kind::kMouseButton:
                io.AddMouseButtonEvent(static_cast<int>(record.code), record.down);
                break;
            case InputRecord::Kind::kMouseWheel:
                if (record.down) io.AddMouseWheelEvent(0.0f, record.code == 8 ? 1.0f : -1.0f);
                break;
            case InputRecord::Kind::kKey:
                if (Overlay::InputMethod::FilterNativeKey(record.code, record.down)) break;
                if (waitingForKey && record.down)
                {
                    CaptureKey(record.code);
                }
                else if (record.code == VK_ESCAPE && record.initialPress && Overlay::InputMethod::UsesGameInputMethod() &&
                    !Overlay::InputMethod::WantsTextInput())
                {
                    // IMU 放行的 Esc 才能关闭菜单，组词取消键不会绕过它的过滤。
                    Overlay::CloseAllMenus();
                }
                else if (const auto key = VirtualKeyToImGuiKey(record.code); key != ImGuiKey_None)
                {
                    io.AddKeyEvent(key, record.down);
                    // ImGui 的组合键状态独立于左右按键，Ctrl+点击数值框需要显式提交。
                    if (key == ImGuiKey_LeftCtrl || key == ImGuiKey_RightCtrl)
                    {
                        (key == ImGuiKey_LeftCtrl ? leftCtrl : rightCtrl) = record.down;
                        io.AddKeyEvent(ImGuiMod_Ctrl, leftCtrl || rightCtrl);
                    }
                }
                break;
            }
        }
        // 没有移动事件时也提交当前位置，确保打开界面后的第一帧有有效指针。
        clampCursorPosition();
        io.AddMousePosEvent(cursorPosition.x, cursorPosition.y);
    }

    void PrepareGameInputFrame()
    {
        RECT clientRect{};
        GetClientRect(window, &clientRect);

        auto& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(
            static_cast<float>(clientRect.right - clientRect.left),
            static_cast<float>(clientRect.bottom - clientRect.top));

        const auto now = std::chrono::steady_clock::now();
        // 不使用 Win32 后端的 NewFrame，因此在这里提供 ImGui 必需的帧间隔。
        io.DeltaTime = lastFrameTime == std::chrono::steady_clock::time_point{} ?
            1.0f / 60.0f : std::chrono::duration<float>(now - lastFrameTime).count();
        lastFrameTime = now;
    }

    std::filesystem::path SettingsPath() { return "Data/SKSE/Plugins/SkyWardrobe.ini"; }
    void LoadSettings()
    {
        std::ifstream input(SettingsPath());
        std::string line;
        while (std::getline(input, line))
        {
            const auto equal = line.find('=');
            if (equal == std::string::npos) continue;
            try
            {
                const auto key = line.substr(0, equal), value = line.substr(equal + 1);
                if (key == "font_path") fontPath = value;
                else if (key == "font_size") { const auto number = std::stof(value); if (std::isfinite(number)) fontSize = std::clamp(number, 12.0f, 40.0f); }
                else if (key == "small_font_size") { const auto number = std::stof(value); if (std::isfinite(number)) smallFontSize = std::clamp(number, 10.0f, 32.0f); }
                else if (key == "hotkey_enabled") hotkeyEnabled = std::stoi(value) == 1;
                else if (key == "toggle_key") { const auto code = std::stoul(value); if (code >= 8 && code <= 254 && code != VK_ESCAPE) toggleKey = code; }
                else if (key == "ui_scale") { const auto number = std::stof(value); if (std::isfinite(number)) uiScale = std::clamp(number, kMinScale, kMaxScale); }
                else if (key == "cursor_sensitivity") { const auto number = std::stof(value); if (std::isfinite(number)) cursorSensitivity = std::clamp(number, kMinCursorSensitivity, kMaxCursorSensitivity); }
                else if (key == "ime_backend") Overlay::InputMethod::Configure(static_cast<Ime::Backend>(std::stoi(value)), Overlay::InputMethod::DisabledBackends());
                else if (key == "ime_disabled") Overlay::InputMethod::Configure(Overlay::InputMethod::RequestedBackend(), std::stoul(value));
            }
            catch (const std::exception&) { SKSE::log::warn("Invalid SkyWardrobe.ini setting"); }
        }
    }
    void SaveSettings()
    {
        const auto path = SettingsPath();
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        const auto temporary = path.wstring() + L".tmp";
        {
            std::ofstream output{ std::filesystem::path(temporary) };
            output << "hotkey_enabled=" << hotkeyEnabled << '\n' << "toggle_key=" << toggleKey << '\n'
                << "font_path=" << fontPath << '\n' << "font_size=" << fontSize << '\n' << "small_font_size=" << smallFontSize << '\n'
                << "ui_scale=" << uiScale << '\n' << "cursor_sensitivity=" << cursorSensitivity << '\n'
                << "ime_backend=" << static_cast<int>(Overlay::InputMethod::RequestedBackend()) << '\n'
                << "ime_disabled=" << Overlay::InputMethod::DisabledBackends() << '\n';
            output.flush();
            output.close();
            if (!output) { SKSE::log::error("Cannot save overlay settings"); return; }
        }
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            SKSE::log::error("Cannot replace overlay settings: {}", GetLastError());
    }
    std::string KeyName(UINT key)
    {
        const auto scan = MapVirtualKeyW(key, MAPVK_VK_TO_VSC);
        wchar_t name[64]{};
        if (scan && GetKeyNameTextW(static_cast<LONG>(scan << 16), name, 64))
        {
            const auto length = WideCharToMultiByte(CP_UTF8, 0, name, -1, nullptr, 0, nullptr, nullptr);
            std::string result(static_cast<std::size_t>(length), '\0');
            WideCharToMultiByte(CP_UTF8, 0, name, -1, result.data(), length, nullptr, nullptr);
            result.pop_back();
            return result;
        }
        return "VK " + std::to_string(key);
    }

    void SetOverlayVisible(bool show)
    {
        if (show && !visible) Wardrobe::CaptureWatchedActor();
        inputOverlayVisible = show;
        visible = show;
        Overlay::InputMethod::SetOverlayState(show, true);
        if (auto* controls = RE::PlayerControls::GetSingleton(); controls && controls->lookHandler)
        {
            if (show && !lookInputSuspended)
            {
                previousLookInputEnabled = controls->lookHandler->IsInputEventHandlingEnabled();
                controls->lookHandler->SetInputEventHandlingEnabled(false);
                lookInputSuspended = true;
            }
            else if (!show && lookInputSuspended)
            {
                controls->lookHandler->SetInputEventHandlingEnabled(previousLookInputEnabled);
                lookInputSuspended = false;
            }
        }

        if (show)
        {
            cursorPositionInitialized = false;
            ReleaseCapture();
            ClipCursor(nullptr);
            if (!cursorShownByOverlay)
            {
                ShowCursor(TRUE);
                cursorShownByOverlay = true;
            }
        }
        else
        {
            waitingForKey = false;
            leftCtrl = false; rightCtrl = false;
            if (ImGui::GetCurrentContext())
            {
                // 隐藏面板同时结束编辑，重新打开时不能自动恢复上次的输入法焦点。
                ImGui::ClearActiveID();
                ImGui::GetIO().ClearInputKeys();
                ImGui::GetIO().ClearInputMouse();
            }
            {
                std::lock_guard lock(inputMutex);
                inputRecords.clear();
            }
            if (cursorShownByOverlay)
            {
                ShowCursor(FALSE);
                cursorShownByOverlay = false;
            }
        }
    }

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_ACTIVATEAPP && !wParam)
            Overlay::InputMethod::SetOverlayState(inputOverlayVisible.load(), false);
        if (const auto handled = Overlay::InputMethod::HandleWindowMessage(hwnd, message, wParam, lParam)) return *handled;
        if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN)
        {
            // 等待菜单型输入法或 IMU 过滤后放行的 Esc，原始窗口按键不能抢先关闭衣柜。
            const bool externalInput = Overlay::InputMethod::UsesMenuInput() || Overlay::InputMethod::UsesGameInputMethod() || Overlay::InputMethod::WantsTextInput();
            if (visible && !waitingForKey && !externalInput && wParam == VK_ESCAPE) { Overlay::CloseAllMenus(); return 0; }
            if (waitingForKey)
            {
                if ((lParam & (1LL << 30)) == 0)
                {
                    CaptureKey(static_cast<UINT>(wParam));
                }
                return 0;
            }
            if (hotkeyEnabled && wParam == toggleKey && !Overlay::InputMethod::WantsTextInput() && !(RE::UI::GetSingleton() && RE::UI::GetSingleton()->IsMenuOpen(RE::BSFixedString("Console"))))
            {
                if ((lParam & (1LL << 30)) == 0)
                {
                    if (visible) Overlay::CloseAllMenus();
                    else SetOverlayVisible(true);
                }
                return 0;
            }
        }

        // 打开界面时不再把 Win32 的绝对坐标或按键喂给 ImGui。
        // Skyrim 输入事件流是唯一来源，避免绘制指针与控件命中位置不同步。
        return unicodeWindow ? CallWindowProcW(originalWndProc, hwnd, message, wParam, lParam) :
            CallWindowProcA(originalWndProc, hwnd, message, wParam, lParam);
    }

    // 加载主字体，并把 Font Awesome 图标合并进去。
    // 1.92 起字形按需光栅化，且 DX11 后端已声明 RendererHasTextures，因此不必再传 GlyphRanges。
    // 合并时按 Sources 添加顺序查找字形，主字体在前，所以图标只补上主字体没有的码位。
    void LoadFonts()
    {
        auto* fonts = ImGui::GetIO().Fonts;
        smallFontSize = std::min(smallFontSize, fontSize);
        ImFont* base = nullptr;
        std::error_code fontError;
        if (std::filesystem::exists(std::filesystem::u8path(fontPath), fontError))
        {
            base = fonts->AddFontFromFileTTF(fontPath.c_str(), fontSize);
            noteFont = fonts->AddFontFromFileTTF(fontPath.c_str(), smallFontSize);
        }
        if (!base)
        {
            base = fonts->AddFontDefaultVector();
            SKSE::log::warn("CJK font unavailable: {}; configure font_path in SkyWardrobe.ini", fontPath);
        }
        if (!noteFont) noteFont = base;

        const std::filesystem::path path(kIconFontPath);
        std::error_code error;
        if (!std::filesystem::exists(path, error))
        {
            SKSE::log::error("Icon font is missing, panel labels fall back to plain text: {}", path.string());
            return;
        }

        ImFontConfig config;
        config.MergeMode = true;  // 并入主字体，而不是替换它
        config.DstFont = base;
        config.PixelSnapH = true; // 图标落在整像素上，与主字体对齐
        // 让每个图标至少占一个字宽，各按钮的文字起始位置才能对齐。
        config.GlyphMinAdvanceX = base->LegacySize;
        if (!fonts->AddFontFromFileTTF(path.string().c_str(), base->LegacySize, &config))
        {
            SKSE::log::error("Cannot load icon font: {}", path.string());
            return;
        }
        CAF::Ui::SetIconFontAvailable(true);
    }

    bool Initialize(IDXGISwapChain* swapChain)
    {
        DXGI_SWAP_CHAIN_DESC description{};
        if (FAILED(swapChain->GetDesc(&description)) ||
            FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))))
        {
            return false;
        }
        window = description.OutputWindow;
        device->GetImmediateContext(&context);
        if (!window || !context)
        {
            return false;
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        baseStyle = ImGui::GetStyle();
        ImGui::GetIO().IniFilename = "Data/SKSE/Plugins/SkyWardrobe/ImGui.ini";
        LoadFonts();
        if (!ImGui_ImplWin32_Init(window) || !ImGui_ImplDX11_Init(device, context))
        {
            SKSE::log::error("Dear ImGui backend initialization failed");
            return false;
        }
        // 原生模式发布插入点，外部模式不触碰其上下文；坐标转换隔离在宿主适配层。
        ImGui::GetPlatformIO().Platform_SetImeDataFn = [](ImGuiContext*, ImGuiViewport* viewport, ImGuiPlatformImeData* data)
        {
            Overlay::InputMethod::PublishTextFocus(data->WantTextInput, ImGui::GetActiveID());
            Overlay::InputMethod::PublishCaret(data->InputPos.x - viewport->Pos.x, data->InputPos.y - viewport->Pos.y, data->InputLineHeight);
        };
        SetLastError(0);
        unicodeWindow = IsWindowUnicode(window) != FALSE;
        // 保持窗口原来的 A/W 消息语义，避免改变 IMU 的 ANSI 字符转换管线。
        originalWndProc = reinterpret_cast<WNDPROC>(unicodeWindow ?
            SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WindowProc)) :
            SetWindowLongPtrA(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WindowProc)));
        if (!originalWndProc)
        {
            SKSE::log::error("Cannot attach overlay window input: {}", GetLastError());
            return false;
        }
        Overlay::InputMethod::AttachWindow(window);
        initialized = true;
        return true;
    }

    void DrawSettings()
    {
        const auto* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowSize(ImVec2(std::min(1120.0f * uiScale, viewport->WorkSize.x * 0.95f),
            std::min(700.0f * uiScale, viewport->WorkSize.y * 0.9f)), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        // 右侧公共操作、三个层标题及紧凑列表需要随字体缩放保留可操作空间。
        const float row = ImGui::GetFrameHeightWithSpacing();
        ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(viewport->WorkSize.x, ImGui::GetFontSize() * 40),
            std::min(viewport->WorkSize.y, row * 19)), ImVec2(FLT_MAX, FLT_MAX));
        if (ImGui::Begin("SkyWardrobe", &visible)) Wardrobe::DrawPanel();
        ImGui::End();
        if (!visible) SetOverlayVisible(false);
    }
    HRESULT STDMETHODCALLTYPE Present(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
    {
        if (!initialized && !Initialize(swapChain))
        {
            return originalPresent(swapChain, syncInterval, flags);
        }
        // 统一在 UI 线程释放鼠标和视角输入，游戏事件线程仅提交关闭请求。
        if (closeAllMenusRequested.exchange(false)) SetOverlayVisible(false);
        const auto request = visibilityRequest.exchange(-1);
        if (request >= 0) SetOverlayVisible(request == 1);
        Wardrobe::ProcessUiRequests();
        if (visible)
        {
            DWORD foregroundProcess = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcess);
            // SimpleIME 会把焦点移到同进程的 IME 窗口，不能误当作 Alt+Tab。
            Overlay::InputMethod::SetOverlayState(true, foregroundProcess == GetCurrentProcessId());
            ImGui_ImplDX11_NewFrame();
            // 不调用 Win32 后端 NewFrame：它会覆盖游戏事件流驱动的虚拟指针位置。
            PrepareGameInputFrame();
            Overlay::InputMethod::Flush();
            if (visible) FlushInputRecords();
            ImGui::NewFrame();
            ImGui::GetStyle() = baseStyle;
            ImGui::GetStyle().ScaleAllSizes(uiScale);
            ImGui::GetStyle().FontScaleMain = uiScale;
            ImGui::GetIO().MouseDrawCursor = visible;
            if (visible) DrawSettings();
            if (closeAllMenusRequested.exchange(false)) SetOverlayVisible(false);
            ImGui::Render();
            // 回调不保证在输入框 ID 改变但位置不变时执行，帧尾再同步一次编辑会话。
            Overlay::InputMethod::PublishTextFocus(visible && !waitingForKey && ImGui::GetCurrentContext()->PlatformImeData.WantTextInput, ImGui::GetActiveID());
            Overlay::InputMethod::EndFrame();

            ID3D11Texture2D* backBuffer = nullptr;
            ID3D11RenderTargetView* target = nullptr;
            if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))))
            {
                device->CreateRenderTargetView(backBuffer, nullptr, &target);
                backBuffer->Release();
            }
            if (target)
            {
                ID3D11RenderTargetView* previous = nullptr;
                ID3D11DepthStencilView* previousDepth = nullptr;
                context->OMGetRenderTargets(1, &previous, &previousDepth);
                context->OMSetRenderTargets(1, &target, nullptr);
                ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
                context->OMSetRenderTargets(1, &previous, previousDepth);
                if (previous) previous->Release();
                if (previousDepth) previousDepth->Release();
                target->Release();
            }
        }
        return originalPresent(swapChain, syncInterval, flags);
    }
}

ImFont* Overlay::NoteFont() { return noteFont; }
bool Overlay::IsVisible() { return inputOverlayVisible.load(); }
void Overlay::CloseAllMenus() { closeAllMenusRequested = true; }
void Overlay::Open()
{
    // 命令执行完后先关闭控制台，再接管衣柜输入，避免控制台与衣柜同时捕获按键。
    SKSE::GetTaskInterface()->AddTask([]
    {
        if (auto* queue = RE::UIMessageQueue::GetSingleton())
            queue->AddMessage(RE::BSFixedString("Console"), RE::UI_MESSAGE_TYPE::kHide, nullptr);
        visibilityRequest = 1;
    });
}
void Overlay::DrawInterfaceSettings()
{
    using Wardrobe::Tr;
    if (ImGui::Checkbox(Tr("Enable opening hotkey", "启用打开快捷键"), &hotkeyEnabled)) SaveSettings();
    ImGui::Text("%s: %s", Tr("Hotkey", "快捷键"), waitingForKey ? Tr("Press a key (Esc cancels)", "按下新按键（Esc 取消）") : KeyName(toggleKey).c_str());
    if (ImGui::Button(Tr("Change key", "修改按键"))) waitingForKey = true;
    if (ImGui::SliderFloat(Tr("UI scale", "界面缩放"), &uiScale, kMinScale, kMaxScale, "%.2fx")) SaveSettings();
    if (ImGui::SliderFloat(Tr("Pointer sensitivity", "鼠标灵敏度"), &cursorSensitivity, kMinCursorSensitivity, kMaxCursorSensitivity, "%.2fx")) SaveSettings();
    ImGui::TextWrapped("%s", Tr("Drag window borders to resize. Ctrl+V pastes text. IME selection and tests are below.", "拖动窗口边缘调整大小。Ctrl+V 粘贴文本。下方可切换和测试 IME 实现。"));
    if (Overlay::InputMethod::DrawStatus()) SaveSettings();
}
void Overlay::Install()
{
    LoadSettings();
    Overlay::InputMethod::Install();
    if (auto* inputManager = RE::BSInputDeviceManager::GetSingleton())
    {
        inputManager->PrependEventSink(&inputSink);
    }
    else
    {
        SKSE::log::error("Overlay cannot find Skyrim input manager");
    }
    auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
    auto* swapChain = renderer ? reinterpret_cast<IDXGISwapChain*>(renderer->GetRuntimeData().renderWindows[0].swapChain) : nullptr;
    if (!swapChain)
    {
        SKSE::log::error("Overlay cannot find the Skyrim swap chain");
        return;
    }
    const auto presentAddress = (*reinterpret_cast<void***>(swapChain))[8];
    const auto hookStatus = MH_Initialize();
    if (hookStatus != MH_OK && hookStatus != MH_ERROR_ALREADY_INITIALIZED)
    {
        SKSE::log::error("Overlay hook initialization failed");
        return;
    }
    if (MH_CreateHook(presentAddress, &Present, reinterpret_cast<void**>(&originalPresent)) != MH_OK ||
        MH_EnableHook(presentAddress) != MH_OK)
    {
        SKSE::log::error("Overlay Present hook installation failed");
    }
}
