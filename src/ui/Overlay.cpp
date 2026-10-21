#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include "ui/Overlay.h"
#include "ui/FlickUi.h"
#include "ui/FlickFonts.h"
#include "ui/UiTabs.h"
#include "wardrobe/ActorRuntime.h"
#include "wardrobe/Panel.h"
#include "wardrobe/SafeMode.h"
#include "wardrobe/Service.h"
#include <atomic>
#include <filesystem>
#include <fstream>

namespace
{
    constexpr UINT kDefaultKey = VK_F11;
    constexpr float kMinScale = 0.75f, kMaxScale = 3.0f;
    constexpr float kMinCursorSensitivity = 0.1f, kMaxCursorSensitivity = 4.0f;
    std::atomic_bool visible = false;
    std::atomic_bool popupOpen = false;
    bool connected = false, hotkeyEnabled = false, waitingForKey = false;
    UINT toggleKey = kDefaultKey;
    float uiScale = 1.5f, cursorSensitivity = 1.0f;
    // 旧字体设置继续读取和保留，实际字体由 FLICK 配置。
    std::string fontPath;
    float fontSize = 20, smallFontSize = 16;
    bool SaveSettings();
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
                else if (key == "player_outfit_storage")
                {
                    if (value == "global") Wardrobe::playerOutfitStorage.store(Wardrobe::PlayerOutfitStorage::Global);
                    else if (value == "save_local") Wardrobe::playerOutfitStorage.store(Wardrobe::PlayerOutfitStorage::SaveLocal);
                    else SKSE::log::warn("Invalid player_outfit_storage; using global");
                }
            }
            catch (const std::exception&) { SKSE::log::warn("Invalid SkyWardrobe.ini setting"); }
        }
    }
    bool SaveSettings()
    {
        const auto path = SettingsPath();
        std::vector<std::string> preserved;
        std::ifstream input(path);
        std::string line;
        while (std::getline(input, line))
        {
            const auto equal = line.find('=');
            const auto key = line.substr(0, equal);
            if (key != "hotkey_enabled" && key != "toggle_key" && key != "ui_scale" &&
                key != "player_outfit_storage" && key != "safe_mode_on_unclean_exit") preserved.push_back(line);
        }
        input.close();
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        const auto temporary = path.wstring() + L".tmp";
        {
            std::ofstream output{std::filesystem::path(temporary)};
            output << "hotkey_enabled=" << hotkeyEnabled << '\n' << "toggle_key=" << toggleKey << '\n'
                << "ui_scale=" << uiScale << '\n'
                << "player_outfit_storage=" << (Wardrobe::playerOutfitStorage.load() == Wardrobe::PlayerOutfitStorage::Global ? "global" : "save_local") << '\n'
                << "safe_mode_on_unclean_exit=" << Wardrobe::SafeMode::Get().AutomaticRecovery() << '\n';
            for (const auto& entry : preserved) output << entry << '\n';
            output.flush(); output.close();
            if (!output) { SKSE::log::error("Cannot save SkyWardrobe settings"); return false; }
        }
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            SKSE::log::error("Cannot replace SkyWardrobe settings: {}", GetLastError());
            return false;
        }
        return true;
    }
    std::string KeyName(UINT key)
    {
        return FUCK::GetKeyName(MapVirtualKeyW(key, MAPVK_VK_TO_VSC_EX));
    }
    void SetVisible(bool show)
    {
        if (show && !visible.exchange(show)) Wardrobe::CaptureWatchedActor();
        else visible = show;
        if (!show) { waitingForKey = false; FlickUi::ResetInputState(); popupOpen = false; }
    }
    bool HandleInput(const void* raw)
    {
        const auto* events = static_cast<const RE::InputEvent* const*>(raw);
        if (!events) return false;
        const bool editing = FlickUi::WantsTextInput();
        if (editing) return false;
        for (auto* event = *events; event; event = event->next)
        {
            const auto* button = event->AsButtonEvent();
            if (!button || event->GetDevice() != RE::INPUT_DEVICE::kKeyboard || !button->IsDown()) continue;
            auto scan = button->GetIDCode();
            const auto windowsScan = (scan & 0x80) ? (0xE000 | (scan & 0x7F)) : scan;
            const UINT key = MapVirtualKeyW(windowsScan, MAPVK_VSC_TO_VK_EX);
            if (waitingForKey)
            {
                waitingForKey = false;
                if (key != VK_ESCAPE && key >= 8 && key <= 254) { toggleKey = key; SaveSettings(); }
                return true;
            }
            if (key == VK_ESCAPE && visible.load() && !popupOpen.load())
            {
                SetVisible(false);
                return true;
            }
            auto* ui = RE::UI::GetSingleton();
            if (hotkeyEnabled && key == toggleKey && !(ui && ui->IsMenuOpen(RE::BSFixedString("Console"))))
            {
                SetVisible(!visible.load());
                return true;
            }
        }
        return false;
    }
    class WardrobeWindow final : public FUCK::IWindow
    {
    public:
        const char* Id() const override { return "Wardrobe"; }
        const char* Title() const override { return "SkyWardrobe"; }
        bool IsOpen() const override { return visible.load(); }
        void SetOpen(bool open) override { SetVisible(open); }
        FUCK::WindowFlags GetFlags() const override
        {
            return FUCK::WindowFlags::kBlockVanity | FUCK::WindowFlags::kCloseOnGameMenu;
        }
        ImVec2 GetDefaultSize() const override { return {1120, 700}; }
        void Draw() override
        {
            FlickUi::BeginFrame();
            const FlickUi::ContentScaleScope contentScale(uiScale / 1.5f);
            Wardrobe::DrawPanel();
            FlickUi::EndFrame();
            popupOpen = FlickUi::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        }
    } wardrobeWindow;
    class WardrobeTool final : public FUCK::ITool
    {
    public:
        const char* Name() const override { return "SkyWardrobe"; }
        void Draw() override
        {
            FlickUi::TextWrapped("%s", Wardrobe::Tr("Open the wardrobe to manage clothing, actors and outfit rules.", "打开衣柜以管理服装、角色及换装规则。"));
            if (FUCK::Button(Wardrobe::Tr("Open wardrobe", "打开衣柜"))) SetVisible(true);
        }
        bool OnAsyncInput(const void* events) override { return HandleInput(events); }
    } wardrobeTool;
}

bool Overlay::IsVisible() { return connected && visible.load(); }
void Overlay::CloseAllMenus() { SetVisible(false); }
void Overlay::Open()
{
    if (!connected)
    {
        SKSE::log::error("SkyWardrobe UI requires FLICK API 7 or newer (FUCK.dll)");
        if (auto* console = RE::ConsoleLog::GetSingleton()) console->Print("[SkyWardrobe] UI requires FLICK API 7 or newer (FUCK.dll).");
        return;
    }
    SKSE::GetTaskInterface()->AddTask([]
    {
        if (auto* queue = RE::UIMessageQueue::GetSingleton())
            queue->AddMessage(RE::BSFixedString("Console"), RE::UI_MESSAGE_TYPE::kHide, nullptr);
        SetVisible(true);
    });
}
void Overlay::Install()
{
    LoadSettings();
    connected = FUCK::Connect("SkyWardrobe");
    if (!connected)
    {
        SKSE::log::error("FLICK unavailable: install FLICK with API 7 or newer to use the SkyWardrobe UI");
        return;
    }
    // 框架字体已合并图标；不修改宿主字体图集，也不安装本地 IME/WndProc hook。
    CAF::Ui::SetIconFontAvailable(FlickFonts::IconsAvailable());
    FUCK::RegisterTool(&wardrobeTool);
    FUCK::RegisterWindow(&wardrobeWindow);
    SKSE::log::info("SkyWardrobe UI registered with FLICK");
}
void Overlay::DrawInterfaceSettings()
{
    using Wardrobe::Tr;
    auto& service = Wardrobe::Service::Get();
    const auto previousStorage = Wardrobe::playerOutfitStorage.load();
    int storage = previousStorage == Wardrobe::PlayerOutfitStorage::Global ? 0 : 1;
    if (FlickUi::Combo(Tr("Player outfit storage", "玩家搭配存储方式"), &storage,
        Tr("Global\0Save local\0", "全局\0存档局部\0")))
    {
        service.SetPlayerStorage(storage == 0 ? Wardrobe::PlayerOutfitStorage::Global : Wardrobe::PlayerOutfitStorage::SaveLocal);
        if (!SaveSettings())
        {
            service.SetPlayerStorage(previousStorage);
            service.status = Tr("Cannot save player outfit storage setting", "无法保存玩家搭配存储方式设置");
        }
    }
    FlickUi::TextWrapped("%s", Wardrobe::playerOutfitStorage.load() == Wardrobe::PlayerOutfitStorage::Global ?
        Tr("Player outfits are saved in JSON. Loading an old game does not restore old outfits.",
            "玩家搭配保存到 JSON，加载旧游戏存档不会回滚搭配。") :
        Tr("Save the game to keep player outfit edits, including safe-mode repairs. Loading an older game restores its outfits.",
            "玩家搭配修改（包括安全模式修复）需要保存游戏。读取旧存档会恢复其中的旧搭配。"));
    auto& safety = Wardrobe::SafeMode::Get();
    bool automaticRecovery = safety.AutomaticRecovery();
    if (FlickUi::Checkbox(Tr("Recover crashes within 5s of SOSBackend calls", "SOSBackend 调用后 5 秒内崩溃时自动进入安全模式"), &automaticRecovery))
    {
        safety.SetAutomaticRecovery(automaticRecovery);
        SaveSettings();
    }
    FlickUi::TextWrapped("%s", Tr("Each backend call opens a 5-second window. Only abnormal exits within this window request safe mode on the next launch.",
        "每次后端调用重新开启 5 秒窗口，仅在窗口内异常退出才会在下次启动时自动进入安全模式。"));
    if (!safety.Active() && FlickUi::Button(Tr("Enter safe mode", "进入安全模式"))) Wardrobe::Service::Get().EnterSafeMode();
    if (FlickUi::Checkbox(Tr("Enable opening hotkey", "启用打开快捷键"), &hotkeyEnabled)) SaveSettings();
    FlickUi::Text("%s: %s", Tr("Hotkey", "快捷键"), waitingForKey ? Tr("Press a key (Esc cancels)", "按下新按键（Esc 取消）") : KeyName(toggleKey).c_str());
    if (FlickUi::Button(Tr("Change key", "修改按键"))) waitingForKey = true;
    if (FlickUi::SliderFloat(Tr("UI scale", "界面缩放"), &uiScale, kMinScale, kMaxScale, "%.2fx")) SaveSettings();

    FlickUi::TextWrapped("%s", Tr("Fonts, pointer movement and input methods are managed by FLICK. The previous pointer sensitivity is retained in SkyWardrobe.ini for compatibility; FLICK API 7 has no per-window pointer-speed setting.",
        "字体、指针移动和输入法由 FLICK 管理。旧鼠标灵敏度仍保留在 SkyWardrobe.ini；FLICK API 7 没有独立窗口的指针速度设置。"));
    static std::string singleLine, multiLine;
    FlickUi::SeparatorText(Tr("Input method test", "输入法测试"));
    FlickUi::InputTextWithHint("##ime test", Tr("Type Chinese here", "在此测试中文输入"), &singleLine);
    FlickUi::InputTextMultiline("##ime multiline", &multiLine, ImVec2(-1, FlickUi::GetTextLineHeight() * 4));
}
