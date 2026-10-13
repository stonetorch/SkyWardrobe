#pragma once
#include <nlohmann/json.hpp>
#include <cmath>
#include <stdexcept>
#include <string_view>

namespace Wardrobe
{
    inline bool IsQuickAccessAction(std::string_view action)
    {
        return action == "none" || action == "category" || action == "group" ||
            action == "preset" || action == "applyPreset" || action == "useGroup";
    }

    inline nlohmann::json DefaultQuickAccess()
    {
        return {{"enabled", true}, {"attached", true}, {"rows", 3}, {"columns", 3},
            {"cellSize", 64}, {"cells", nlohmann::json::array()}};
    }

    inline void ValidateQuickAccess(nlohmann::json& options)
    {
        if (!options.is_object()) throw std::runtime_error("quickAccess must be an object");
        const auto defaults = DefaultQuickAccess();
        for (const auto& [key, value] : defaults.items())
            if (!options.contains(key)) options[key] = value;
        for (const char* key : {"enabled", "attached"})
            if (!options[key].is_boolean()) throw std::runtime_error("Invalid quickAccess toggle");
        for (const char* key : {"rows", "columns"})
            if (!options[key].is_number_integer() || options[key] < 1 || options[key] > 12)
                throw std::runtime_error("quickAccess rows and columns must be between 1 and 12");
        if (!options["cellSize"].is_number() || !std::isfinite(options["cellSize"].get<double>()) ||
            options["cellSize"] < 32 || options["cellSize"] > 128)
            throw std::runtime_error("quickAccess cellSize must be between 32 and 128");
        auto& cells = options["cells"];
        if (!cells.is_array() || cells.size() > 144) throw std::runtime_error("quickAccess cells must be an array of at most 144 entries");
        for (auto& cell : cells)
        {
            if (!cell.is_object()) throw std::runtime_error("quickAccess cell must be an object");
            for (const char* key : {"action", "target", "icon", "label"})
                if (cell.contains(key) && !cell[key].is_string()) throw std::runtime_error("quickAccess cell fields must be strings");
            if (!IsQuickAccessAction(cell.value("action", "none"))) throw std::runtime_error("Invalid quickAccess action");
            // 目标被删除时仍保留稳定 ID，由界面标记失效，避免整个衣柜无法加载。
        }
    }
}
