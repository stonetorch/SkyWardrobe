#pragma once

#include <nlohmann/json.hpp>
#include "wardrobe/PinyinSearch.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <format>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace Wardrobe
{
    using Json = nlohmann::json;

    struct FormKey
    {
        std::string plugin;
        std::uint32_t localFormID{};
        std::string String() const { return std::format("{}|{:06X}", plugin, localFormID); }
        static std::optional<FormKey> Parse(std::string_view text)
        {
            const auto separator = text.rfind('|');
            if (separator == std::string_view::npos || separator == 0) return {};
            const auto hex = text.substr(separator + 1);
            std::uint32_t id{};
            const auto result = std::from_chars(hex.data(), hex.data() + hex.size(), id, 16);
            if (hex.empty() || result.ec != std::errc{} || result.ptr != hex.data() + hex.size() || id > 0xFFFFFF) return {};
            return FormKey{ std::string(text.substr(0, separator)), id };
        }
    };

    inline constexpr const char* PartIDs[] = {
        "unknown", "body", "top", "bottom", "underwear", "bra", "panties", "shoes", "stockings",
        "gloves", "head", "hair", "neck", "cloak", "accessory",
        "custom1", "custom2", "custom3", "custom4", "custom5", "custom6", "custom7", "custom8"
    };

    inline std::string Normalize(std::string_view value)
    {
        std::string result;
        for (unsigned char c : value)
        {
            if (c < 128 && std::isspace(c)) continue;
            result += c < 128 ? static_cast<char>(std::tolower(c)) : static_cast<char>(c);
        }
        return result;
    }

    inline int SearchScore(std::string_view query, std::string_view value, bool pinyin = false)
    {
        const auto needle = Normalize(query), haystack = Normalize(value);
        if (needle.empty()) return 1;
        if (haystack == needle) return 100;
        if (haystack.starts_with(needle)) return 90;
        if (haystack.find(needle) != std::string::npos) return 70;
        return PinyinSearchScore(needle, value, pinyin);
    }

    inline int SearchMetadata(std::string_view query, const Json& metadata)
    {
        int score = 0;
        for (const char* field : { "name", "alias", "note" })
            if (metadata.contains(field) && metadata[field].is_string())
                score = std::max(score, SearchScore(query, metadata[field].get<std::string>()));
        for (const char* field : { "search", "aliases" })
            if (metadata.contains(field) && metadata[field].is_array())
                for (const auto& key : metadata[field]) if (key.is_string())
                    score = std::max(score, SearchScore(query, key.get<std::string>(), true));
        if (metadata.contains("pinyin") && metadata["pinyin"].is_string())
            score = std::max(score, SearchScore(query, metadata["pinyin"].get<std::string>(), true));
        return score;
    }

    struct PartGroup
    {
        int part{};
        std::vector<std::size_t> items;
        int lanes = 1;
    };

    inline void AllocateLanes(std::vector<PartGroup>& groups, int columns, bool automatic)
    {
        for (auto& group : groups) group.lanes = 1;
        if (!automatic || groups.empty()) return;
        int allocated = static_cast<int>(groups.size());
        // 部件多于设置列数时保持每部件独立列；UI 用横向滚动展示。
        while (allocated < columns)
        {
            auto best = groups.end();
            for (auto it = groups.begin(); it != groups.end(); ++it)
                if (it->lanes < static_cast<int>(it->items.size()) &&
                    (best == groups.end() || static_cast<float>(it->items.size()) / it->lanes > static_cast<float>(best->items.size()) / best->lanes)) best = it;
            if (best == groups.end()) break;
            ++best->lanes;
            ++allocated;
        }
    }

    inline Json EmptyDatabase()
    {
        return Json{ {"schemaVersion", 1}, {"items", Json::object()}, {"categories", Json::object()},
            {"groups", Json::object()}, {"pluginCategories", Json::object()}, {"presets", Json::object()}, {"workingItems", Json::array()}, {"activePreset", ""} };
    }

    inline void ValidateDatabase(Json& database)
    {
        if (!database.is_object() || database.value("schemaVersion", 1) != 1) throw std::runtime_error("Unsupported wardrobe schema");
        for (const char* field : {"items", "categories", "pluginCategories", "presets", "groups"})
        {
            if (!database.contains(field)) database[field] = Json::object();
            if (!database[field].is_object()) throw std::runtime_error(std::string(field) + " must be an object");
            for (const auto& entry : database[field])
            {
                if (!entry.is_object()) throw std::runtime_error(std::string(field) + " entry must be an object");
                for (const char* key : {"alias", "name", "note", "pinyin", "part", "group"})
                    if (entry.contains(key) && !entry[key].is_string()) throw std::runtime_error(std::string(key) + " must be a string");
                for (const char* key : {"search", "aliases", "categories", "items", "references"})
                    if (entry.contains(key))
                    {
                        if (!entry[key].is_array()) throw std::runtime_error(std::string(key) + " must be an array");
                        for (const auto& value : entry[key]) if (!value.is_string()) throw std::runtime_error(std::string(key) + " must contain strings");
                    }
                for (const char* key : {"favorite", "enabled"})
                    if (entry.contains(key) && !entry[key].is_boolean()) throw std::runtime_error(std::string(key) + " must be boolean");
            }
        }
        if (!database.contains("workingItems")) database["workingItems"] = Json::array();
        if (!database["workingItems"].is_array()) throw std::runtime_error("workingItems must be an array");
        for (const auto& key : database["workingItems"]) if (!key.is_string()) throw std::runtime_error("workingItems must contain strings");
        if (!database.contains("activePreset")) database["activePreset"] = "";
        if (!database["activePreset"].is_string()) throw std::runtime_error("activePreset must be a string");
        database["schemaVersion"] = 1;
        Json canonicalItems = Json::object();
        for (const auto& [text, metadata] : database["items"].items())
        {
            const auto key = FormKey::Parse(text);
            if (!key) throw std::runtime_error("Invalid stable armor key: " + text);
            if (canonicalItems.contains(key->String())) throw std::runtime_error("Duplicate armor key: " + text);
            canonicalItems[key->String()] = metadata;
        }
        database["items"] = std::move(canonicalItems);
        const auto canonicalList = [](Json& values)
        {
            std::vector<std::string> keys;
            for (const auto& text : values)
            {
                const auto key = FormKey::Parse(text.get<std::string>());
                if (!key) throw std::runtime_error("Invalid stable preset key: " + text.get<std::string>());
                if (std::find(keys.begin(), keys.end(), key->String()) == keys.end()) keys.push_back(key->String());
            }
            values = keys;
        };
        canonicalList(database["workingItems"]);
        for (auto& preset : database["presets"])
        {
            if (!preset.contains("items")) preset["items"] = Json::array();
            canonicalList(preset["items"]);
        }
        for (auto& group : database["groups"])
        {
            if (!group.contains("items")) group["items"] = Json::array();
            canonicalList(group["items"]);
        }
        // 旧预设只迁移成员集合，物品元数据始终保留在全局 items 中。
        for (auto& [id, preset] : database["presets"].items())
        {
            if (!preset.contains("group"))
            {
                auto groupID = "preset-" + id;
                while (database["groups"].contains(groupID)) groupID += "-1";
                database["groups"][groupID] = preset;
                database["groups"][groupID].erase("group");
                preset["group"] = "groups:" + groupID;
            }
        }
        if (database.contains("partNames"))
        {
            if (!database["partNames"].is_object()) throw std::runtime_error("partNames must be an object");
            for (const auto& name : database["partNames"]) if (!name.is_string()) throw std::runtime_error("Part names must be strings");
        }
        if (database.contains("viewOptions"))
        {
            const auto& options = database["viewOptions"];
            if (!options.is_object()) throw std::runtime_error("viewOptions must be an object");
            for (const char* field : {"byPart", "automatic"})
                if (options.contains(field) && !options[field].is_boolean()) throw std::runtime_error(std::string(field) + " must be boolean");
            if (options.contains("columns") && (!options["columns"].is_number_integer() || options["columns"] < 1 || options["columns"] > 12))
                throw std::runtime_error("columns must be between 1 and 12");
        }
    }
    inline std::pair<std::string, std::string> SplitGroupID(std::string_view key)
    {
        const auto split = key.find(':');
        if (split == std::string_view::npos) return {};
        auto collection = std::string(key.substr(0, split));
        if (collection != "groups" && collection != "categories" && collection != "pluginCategories") return {};
        return {collection, std::string(key.substr(split + 1))};
    }

    template<class Generator>
    std::vector<std::string> RandomOutfit(std::vector<std::pair<std::string, std::uint32_t>> candidates, Generator& generator)
    {
        // 随机顺序贪心选择，真实 biped 槽位互斥；零槽位物品仍可加入。
        std::shuffle(candidates.begin(), candidates.end(), generator);
        std::uint32_t occupied = 0;
        std::vector<std::string> result;
        for (const auto& [key, slots] : candidates)
            if (!(occupied & slots) && std::find(result.begin(), result.end(), key) == result.end())
            {
                occupied |= slots;
                result.push_back(key);
            }
        return result;
    }

}
