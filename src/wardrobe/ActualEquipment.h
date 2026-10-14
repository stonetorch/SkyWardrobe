#pragma once
#include "wardrobe/ActualEquipmentPlan.h"
#include <string>
#include <vector>

namespace RE { class TESObjectARMO; class Actor; }

namespace Wardrobe
{
    void RegisterActualEquipmentSerialization();
    // 仅在 SKSE 游戏线程任务中调用；空字符串表示请求完成。
    std::string ApplyActualEquipment(const std::vector<RE::TESObjectARMO*>& armors, ActualEquipmentMode mode, RE::Actor* actor = nullptr);
    std::string ClearTemporaryEquipment(RE::Actor* actor = nullptr);
}
