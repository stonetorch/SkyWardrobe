#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <string>
#include <utility>

#include "ui/Overlay.h"
#include "ui/InputMethod.h"
#include "wardrobe/Service.h"
#include "wardrobe/ActualEquipment.h"
#include "wardrobe/SosBackend.h"
#include "wardrobe/SafeMode.h"
#include <Windows.h>
#include <MinHook.h>
#include <cstdlib>

namespace
{
    using ExitProcessFunction = void(WINAPI*)(UINT);
    ExitProcessFunction originalExitProcess = nullptr;

    void RecordNormalExit()
    {
        // 只在引擎确认退出时清标记，主菜单和读档均不算正常结束会话。
        const auto* main = RE::Main::GetSingleton();
        if (main && main->GetRuntimeData().quitGame) Wardrobe::SafeMode::Get().NormalExit();
    }

    void WINAPI OnExitProcess(UINT exitCode)
    {
        if (exitCode == 0) RecordNormalExit();
        originalExitProcess(exitCode);
    }

    void InstallExitDetection()
    {
        std::atexit(RecordNormalExit);
        const auto initialized = MH_Initialize();
        if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED)
        {
            SKSE::log::warn("Normal exit hook initialization failed; unclean exit detection may be conservative");
            return;
        }
        auto* address = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ExitProcess"));
        if (!address || MH_CreateHook(address, &OnExitProcess, reinterpret_cast<void**>(&originalExitProcess)) != MH_OK ||
            MH_EnableHook(address) != MH_OK)
            SKSE::log::warn("Normal exit hook unavailable; using CRT exit callback");
    }

    void OnDataLoaded()
    {
        Wardrobe::SosBackend::Get().DetectInstallation();
        Wardrobe::Service::Get().Initialize();
        Wardrobe::InstallConsole();
        Overlay::Install();
        SKSE::log::info("Game data loaded");
        if (auto* console = RE::ConsoleLog::GetSingleton()) {
            console->Print("[SkyWardrobe] Loaded. Type wardrobe to open.");
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse);

    if (auto logDirectory = SKSE::log::log_directory()) {
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            (*logDirectory / "SkyWardrobe.log").string(), true);
        auto logger = std::make_shared<spdlog::logger>("global", std::move(sink));
        logger->set_level(spdlog::level::info);
        // 普通日志使用文件缓冲，不配置逐条或定时刷新。
        spdlog::set_default_logger(std::move(logger));
        SKSE::log::info("SkyWardrobe loaded");
        SKSE::log::info("Skyrim runtime {}", REL::Module::get().version().string());
        // 启动记录只显式刷新一次，立即确认本次加载已写入日志。
        spdlog::default_logger()->flush();
    }

    Wardrobe::SafeMode::Get().Initialize();
    InstallExitDetection();
    if (Wardrobe::SafeMode::Get().Active()) SKSE::log::warn("Safe mode: {}", Wardrobe::SafeMode::Get().Reason());
    if (!Wardrobe::SafeMode::Get().Error().empty()) SKSE::log::error("{}", Wardrobe::SafeMode::Get().Error());
    Wardrobe::RegisterActualEquipmentSerialization();
    const auto* messaging = SKSE::GetMessagingInterface();
    return messaging && messaging->RegisterListener([](SKSE::MessagingInterface::Message* message) {
        Wardrobe::OnGameMessage(message->type);
        if (message->type == SKSE::MessagingInterface::kPreLoadGame || message->type == SKSE::MessagingInterface::kNewGame)
        {
            Overlay::InputMethod::SetOverlayState(false, true);
            Overlay::CloseAllMenus();
        }
        if (message->type == SKSE::MessagingInterface::kDataLoaded) {
            OnDataLoaded();
        }
    });
}
