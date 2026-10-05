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

namespace
{
    void OnDataLoaded()
    {
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
