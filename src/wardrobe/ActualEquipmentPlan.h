#pragma once
#include <cstdint>
#include <string_view>

namespace Wardrobe
{
    enum class ActualEquipmentMode { Off, Shoes, All };

    inline ActualEquipmentMode ParseActualEquipmentMode(std::string_view value)
    {
        if (value == "shoes") return ActualEquipmentMode::Shoes;
        if (value == "all") return ActualEquipmentMode::All;
        return ActualEquipmentMode::Off;
    }

    inline const char* ActualEquipmentModeID(ActualEquipmentMode mode)
    {
        switch (mode)
        {
        case ActualEquipmentMode::Shoes: return "shoes";
        case ActualEquipmentMode::All: return "all";
        default: return "off";
        }
    }

    inline bool ShouldActuallyEquip(ActualEquipmentMode mode, std::uint32_t slots)
    {
        // 鞋子按原版脚部槽位 37 判定，语义部件只用于显示，不改变装备行为。
        constexpr std::uint32_t feetMask = 1u << (37 - 30);
        return mode == ActualEquipmentMode::All ||
            (mode == ActualEquipmentMode::Shoes && (slots & feetMask) != 0);
    }
}
