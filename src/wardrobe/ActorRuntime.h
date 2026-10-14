#pragma once
#include <RE/Skyrim.h>
#include "wardrobe/MultiLayer.h"
#include <SKSE/SKSE.h>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace Wardrobe
{
    struct ActorInfo
    {
        std::string key, name;
        bool ready{};
        std::vector<std::string> states;
        std::vector<LayerItem> equipment;
    };
    struct ActorSnapshot
    {
        std::vector<ActorInfo> managed, nearby;
        std::optional<ActorInfo> watched;
    };
    // 引擎对象只在游戏线程解析；界面消费字符串快照。
    RE::Actor* ResolveActor(const std::string& key);
    void CaptureWatchedActor();
    void RequestActorSnapshot(std::vector<std::string> keys);
    std::optional<ActorSnapshot> TakeActorSnapshot();
    void SaveActorBindings(SKSE::SerializationInterface* serialization);
    void LoadActorBinding(SKSE::SerializationInterface* serialization, std::uint32_t type, std::uint32_t version, std::uint32_t length);
    void ResetActorBindings();
    std::optional<bool> OriginalAutoSwitch(const std::string& key);
    void RememberAutoSwitch(const std::string& key, std::optional<bool> value);
    void PublishActorState(nlohmann::json state);
    std::optional<nlohmann::json> TakeLoadedActorState();
}
