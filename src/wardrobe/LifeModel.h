#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <random>
#include <string>
#include <vector>
#include <stdexcept>
#include <format>
#include <string_view>

namespace Wardrobe
{
    using LifeJson = nlohmann::json;
    inline constexpr std::array<const char*, 5> OutfitStates{"combat", "dungeon", "city", "town", "default"};

    inline std::string RuntimeOutfitName(std::string_view actor)
    {
        // 使用稳定引用键，不用会随加载顺序变化的运行时 FormID。
        std::uint64_t hash = 14695981039346656037ull;
        for (const unsigned char value : actor) { hash ^= value; hash *= 1099511628211ull; }
        return std::format("__SW_{:016X}", hash);
    }

    inline LifeJson NewActorProfile(std::string name)
    {
        return {{"name", std::move(name)}, {"managed", true}, {"workingItems", LifeJson::array()},
            {"activePreset", ""}, {"automatic", false}, {"manual", false}, {"actualEquipmentMode", "off"},
            {"runtime", LifeJson::object()}};
    }

    inline void ValidateOutfitRuntime(const LifeJson& runtime, int depth = 0)
    {
        if (!runtime.is_object() || depth > 4) throw std::runtime_error("Invalid outfit runtime");
        for (const auto* field : {"state", "group", "signature", "error"})
            if (runtime.contains(field) && !runtime[field].is_string()) throw std::runtime_error("Invalid runtime string");
        if (runtime.contains("items"))
        {
            if (!runtime["items"].is_array()) throw std::runtime_error("Invalid runtime items");
            for (const auto& key : runtime["items"]) if (!key.is_string()) throw std::runtime_error("Invalid runtime item");
        }
        if (runtime.contains("progress"))
        {
            if (!runtime["progress"].is_object()) throw std::runtime_error("Invalid runtime progress");
            for (const auto& progress : runtime["progress"])
                if (!progress.is_object() || (progress.contains("last") && !progress["last"].is_string())) throw std::runtime_error("Invalid progress entry");
        }
        if (runtime.contains("resume")) ValidateOutfitRuntime(runtime["resume"], depth + 1);
    }

    inline void ValidateLifeData(LifeJson& db)
    {
        // 新字段渐进迁移；旧预设和工作搭配完整保留为玩家配置。
        for (const auto* field : {"directories", "navigation", "actors"})
        {
            if (!db.contains(field)) db[field] = LifeJson::object();
            if (!db[field].is_object()) throw std::runtime_error(std::string(field) + " must be an object");
        }
        if (!db["actors"].contains("player"))
        {
            db["actors"]["player"] = NewActorProfile("Player");
            for (const auto* field : {"workingItems", "activePreset", "actualEquipmentMode"})
                db["actors"]["player"][field] = db[field];
        }
        if (!db.contains("livePreview")) db["livePreview"] = true;
        if (!db["livePreview"].is_boolean()) throw std::runtime_error("livePreview must be boolean");
        auto& nav = db["navigation"];
        for (const auto* field : {"directoryOrder", "categoryOrder", "groupOrder"})
        {
            if (!nav.contains(field)) nav[field] = LifeJson::array();
            if (!nav[field].is_array()) throw std::runtime_error("navigation order must be array");
            for (const auto& id : nav[field]) if (!id.is_string()) throw std::runtime_error("navigation order must contain strings");
        }
        for (const auto* field : {"placements", "collapsed"})
        {
            if (!nav.contains(field)) nav[field] = LifeJson::object();
            if (!nav[field].is_object()) throw std::runtime_error("navigation map must be object");
        }
        for (const auto& value : nav["placements"]) if (!value.is_string()) throw std::runtime_error("directory placement must be string");
        for (const auto& value : nav["collapsed"]) if (!value.is_boolean()) throw std::runtime_error("collapsed must be boolean");
        for (const auto& directory : db["directories"])
            if (!directory.is_object() || !directory.contains("name") || !directory["name"].is_string())
                throw std::runtime_error("directory must have a name");
        for (auto& actor : db["actors"])
        {
            if (!actor.is_object()) throw std::runtime_error("actor must be object");
            const auto defaults = NewActorProfile("");
            for (const auto& [field, value] : defaults.items())
            {
                if (!actor.contains(field)) actor[field] = value;
                if (actor[field].type() != value.type()) throw std::runtime_error("Invalid actor field: " + field);
            }
            for (const auto& key : actor["workingItems"]) if (!key.is_string()) throw std::runtime_error("actor working items must be strings");
            for (const auto* field : {"releasePending", "takeoverPending"})
                if (actor.contains(field) && !actor[field].is_boolean()) throw std::runtime_error("Invalid actor boolean");
            if (actor.contains("manualState") && !actor["manualState"].is_string()) throw std::runtime_error("Invalid manual state");
            if (actor.contains("baseline"))
            {
                if (!actor["baseline"].is_array()) throw std::runtime_error("Invalid actor baseline");
                for (const auto& key : actor["baseline"]) if (!key.is_string()) throw std::runtime_error("Invalid baseline item");
            }
            ValidateOutfitRuntime(actor["runtime"]);
            if (actor.contains("presetRuntime"))
            {
                if (!actor["presetRuntime"].is_object()) throw std::runtime_error("Invalid preset runtime map");
                for (const auto& runtime : actor["presetRuntime"]) ValidateOutfitRuntime(runtime);
            }
            const auto mode = actor["actualEquipmentMode"].get<std::string>();
            if (mode != "off" && mode != "shoes" && mode != "all") throw std::runtime_error("Invalid actor equipment mode");
        }
        db["actors"]["player"]["managed"] = true;
        for (auto& preset : db["presets"])
        {
            if (!preset.contains("actor")) preset["actor"] = "player";
            if (!preset["actor"].is_string()) throw std::runtime_error("preset actor must be string");
            if (!preset.contains("rules")) preset["rules"] = {{"default", {{"enabled", true}, {"mode", "single"},
                {"groups", LifeJson::array({preset.value("group", "")})}}}};
            if (!preset["rules"].is_object()) throw std::runtime_error("preset rules must be object");
            for (const auto& [state, rule] : preset["rules"].items())
            {
                if (std::find(OutfitStates.begin(), OutfitStates.end(), state) == OutfitStates.end()) throw std::runtime_error("Invalid outfit state");
                if (!rule.is_object() || !rule.value("enabled", LifeJson(true)).is_boolean() ||
                    !rule.contains("mode") || !rule["mode"].is_string() || !rule.contains("groups") || !rule["groups"].is_array())
                    throw std::runtime_error("Invalid outfit rule");
                const auto mode = rule["mode"].get<std::string>();
                if (mode != "single" && mode != "random" && mode != "sequence") throw std::runtime_error("Invalid selection mode");
                for (const auto& group : rule["groups"]) if (!group.is_string()) throw std::runtime_error("rule groups must be strings");
            }
        }
    }

    inline std::vector<std::string> OrderedIDs(const LifeJson& order, const std::vector<std::string>& available)
    {
        std::vector<std::string> result;
        for (const auto& id : order)
            if (id.is_string() && std::find(available.begin(), available.end(), id.get<std::string>()) != available.end() &&
                std::find(result.begin(), result.end(), id.get<std::string>()) == result.end()) result.push_back(id.get<std::string>());
        for (const auto& id : available) if (std::find(result.begin(), result.end(), id) == result.end()) result.push_back(id);
        return result;
    }

    inline void MoveOrdered(std::vector<std::string>& values, const std::string& id, std::size_t position)
    {
        std::erase(values, id);
        values.insert(values.begin() + std::min(position, values.size()), id);
    }

    inline LifeJson RestoreCombatOutfit(const LifeJson& runtime)
    {
        auto restored = runtime.at("resume");
        // 恢复外观不能回退战斗条件的轮换进度。
        restored["progress"] = runtime.value("progress", LifeJson::object());
        return restored;
    }

    template<class Generator>
    std::string ChooseRuleGroup(const LifeJson& rule, const std::vector<std::string>& valid,
        const LifeJson& progress, Generator& generator)
    {
        if (valid.empty()) return {};
        const auto mode = rule.value("mode", "single");
        if (mode == "sequence")
        {
            const auto last = progress.value("last", "");
            const auto it = std::find(valid.begin(), valid.end(), last);
            return it == valid.end() || it + 1 == valid.end() ? valid.front() : *(it + 1);
        }
        if (mode == "random")
        {
            auto candidates = valid;
            if (candidates.size() > 1) std::erase(candidates, progress.value("last", ""));
            return candidates[std::uniform_int_distribution<std::size_t>(0, candidates.size() - 1)(generator)];
        }
        return valid.front();
    }
}
