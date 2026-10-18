#pragma once
#include "wardrobe/Model.h"
#include <atomic>

namespace Wardrobe
{
    enum class PlayerOutfitStorage { Global, SaveLocal };
    inline std::atomic<PlayerOutfitStorage> playerOutfitStorage{PlayerOutfitStorage::Global};

    // 整组搭配状态共用一个来源；物品记账和动态引用不属于这些字段。
    inline constexpr const char* PlayerOutfitFields[] = {
        "workingItems", "backupItems", "backupEnabled", "backupGroup", "originalEnabled", "hiddenSlots",
        "activePreset", "baseline", "automatic", "actualEquipmentMode", "runtime", "presetRuntime",
        "manual", "manualState", "releasePending", "takeoverPending", "appearanceActive"
    };

    inline void ResetPlayerOutfit(LifeJson& profile)
    {
        for (const auto* field : PlayerOutfitFields) profile.erase(field);
        const auto defaults = NewActorProfile("Player");
        for (const auto* field : PlayerOutfitFields)
            if (defaults.contains(field)) profile[field] = defaults[field];
    }

    inline bool StripPlayerOutfit(LifeJson& database)
    {
        bool changed = false;
        if (database.contains("actors") && database["actors"].is_object() &&
            database["actors"].contains("player") && database["actors"]["player"].is_object())
            for (const auto* field : PlayerOutfitFields)
                changed = database["actors"]["player"].erase(field) != 0 || changed;
        // 同时去除根级兼容镜像，防止旧迁移路径再次带回玩家搭配。
        for (const auto* field : PlayerOutfitFields) changed = database.erase(field) != 0 || changed;
        return changed;
    }

    inline LifeJson ActorStateForSave(LifeJson actors, PlayerOutfitStorage mode)
    {
        if (mode == PlayerOutfitStorage::Global) actors.erase("player");
        return actors;
    }

    inline LifeJson ActorStateForLoad(LifeJson actors, PlayerOutfitStorage mode)
    {
        if (!actors.is_object()) throw std::runtime_error("Invalid actor state");
        if (mode == PlayerOutfitStorage::Global) actors.erase("player");
        const bool hasPlayer = actors.contains("player");
        auto check = EmptyDatabase();
        check["actors"] = std::move(actors);
        // 快照校验不应用全局预设策略，避免局部存档的活动预设被默认值改写。
        check["multiplePresets"] = true;
        ValidateDatabase(check);
        if (!hasPlayer) check["actors"].erase("player");
        return std::move(check["actors"]);
    }

    inline void RestorePlayerOutfit(LifeJson& profile, const LifeJson& actors)
    {
        ResetPlayerOutfit(profile);
        if (!actors.contains("player")) return;
        for (const auto* field : PlayerOutfitFields)
            if (actors["player"].contains(field)) profile[field] = actors["player"][field];
    }
}
