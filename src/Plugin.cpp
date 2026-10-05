#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <string>
#include <utility>

#include "ui/Overlay.h"
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
        spdlog::set_default_logger(std::make_shared<spdlog::logger>("global", std::move(sink)));
        SKSE::log::info("SkyWardrobe loaded");
        SKSE::log::info("Skyrim runtime {}", REL::Module::get().version().string());
    }

    const auto* messaging = SKSE::GetMessagingInterface();
    return messaging && messaging->RegisterListener([](SKSE::MessagingInterface::Message* message) {
        Wardrobe::OnGameMessage(message->type);
        if (message->type == SKSE::MessagingInterface::kDataLoaded) {
            OnDataLoaded();
        }
    });
}
