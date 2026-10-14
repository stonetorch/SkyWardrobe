#include "wardrobe/ActualEquipment.h"
#include "wardrobe/ActorRuntime.h"
#include <map>
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <algorithm>
#include <mutex>

namespace Wardrobe
{
    namespace
    {
        struct TemporaryItem
        {
            RE::FormID form{};
            RE::FormID owner{};
            std::uint16_t uniqueID{};
            RE::FormID actor{};
        };
        std::mutex equipmentMutex;
        std::vector<TemporaryItem> temporaryItems;
        std::map<RE::FormID, std::vector<RE::FormID>> existingByActor;
        constexpr std::uint32_t serializationID = 0x53574445; // SWDE
        constexpr std::uint32_t temporaryRecord = 1, existingRecord = 2;

        auto Inventory(RE::Actor* player)
        {
            return player->GetInventory([](RE::TESBoundObject& object) { return object.IsArmor(); });
        }

        RE::ExtraDataList* FindTemporary(RE::InventoryEntryData* entry, const TemporaryItem& item)
        {
            if (!entry || !entry->extraLists) return nullptr;
            for (auto* list : *entry->extraLists)
            {
                const auto* id = list ? list->GetByType<RE::ExtraUniqueID>() : nullptr;
                if (id && id->baseID == item.owner && id->uniqueID == item.uniqueID) return list;
            }
            return nullptr;
        }

        RE::ExtraDataList* WornList(RE::InventoryEntryData* entry)
        {
            if (!entry || !entry->extraLists) return nullptr;
            for (auto* list : *entry->extraLists)
                if (list && (list->HasType<RE::ExtraWorn>() || list->HasType<RE::ExtraWornLeft>())) return list;
            return nullptr;
        }

        std::string RemoveTemporary(RE::Actor* player, RE::ActorEquipManager* manager,
            const std::vector<RE::FormID>& keep)
        {
            for (auto it = temporaryItems.begin(); it != temporaryItems.end();)
            {
                if (it->actor != player->GetFormID()) { ++it; continue; }
                auto* armor = RE::TESForm::LookupByID<RE::TESObjectARMO>(it->form);
                auto inventory = Inventory(player);
                const auto found = inventory.find(armor);
                auto* list = found != inventory.end() && found->second.first > 0 ? FindTemporary(found->second.second.get(), *it) : nullptr;
                if (!armor || !list)
                {
                    // 物品已被丢弃/转移或唯一标识丢失时，不按 FormID 删除其他同款物品。
                    it = temporaryItems.erase(it);
                    continue;
                }
                if (std::find(keep.begin(), keep.end(), it->form) != keep.end()) { ++it; continue; }
                if (list->HasType<RE::ExtraWorn>() || list->HasType<RE::ExtraWornLeft>())
                    manager->UnequipObject(player, armor, list, 1, nullptr, false, false, false, true);
                // 卸下可能重建 ExtraDataList，必须重新查询后再回收指定实例。
                inventory = Inventory(player);
                const auto refreshed = inventory.find(armor);
                list = refreshed != inventory.end() ? FindTemporary(refreshed->second.second.get(), *it) : nullptr;
                if (list && (list->HasType<RE::ExtraWorn>() || list->HasType<RE::ExtraWornLeft>()))
                    return "Cannot unequip temporary armor: " + std::to_string(it->form);
                if (list) player->RemoveItem(armor, 1, RE::ITEM_REMOVE_REASON::kRemove, list, nullptr);
                inventory = Inventory(player);
                const auto remaining = inventory.find(armor);
                if (remaining != inventory.end() && FindTemporary(remaining->second.second.get(), *it))
                    return "Cannot reclaim temporary armor: " + std::to_string(it->form);
                SKSE::log::info("Actual equipment reclaimed temporary armor {:08X}", it->form);
                it = temporaryItems.erase(it);
            }
            return {};
        }

        void Save(SKSE::SerializationInterface* serialization)
        {
            std::lock_guard lock(equipmentMutex);
            SaveActorBindings(serialization);
            for (const auto& item : temporaryItems)
            {
                const std::uint32_t data[] = {item.form, item.owner, item.uniqueID, item.actor};
                if (!serialization->WriteRecord(temporaryRecord, 2, data)) SKSE::log::error("Cannot save temporary equipment record");
            }
            for (const auto& [actor, forms] : existingByActor)
                for (const auto form : forms)
                {
                    const std::uint32_t data[] = {form, actor};
                    if (!serialization->WriteRecord(existingRecord, 2, data)) SKSE::log::error("Cannot save actual equipment record");
                }
        }

        void Revert(SKSE::SerializationInterface*)
        {
            // 读档只丢弃旧存档的记账，不对即将卸载的世界执行穿脱或删除。
            std::lock_guard lock(equipmentMutex);
            temporaryItems.clear();
            existingByActor.clear();
            ResetActorBindings();
        }

        void Load(SKSE::SerializationInterface* serialization)
        {
            std::lock_guard lock(equipmentMutex);
            temporaryItems.clear();
            existingByActor.clear();
            ResetActorBindings();
            std::uint32_t type{}, version{}, length{};
            while (serialization->GetNextRecordInfo(type, version, length))
            {
                if (type >= 3 && type <= 5) { LoadActorBinding(serialization, type, version, length); continue; }
                if (version != 1 && version != 2) continue;
                std::uint32_t data[4]{};
                const auto expected = type == temporaryRecord ? (version == 1 ? 12u : 16u) : (version == 1 ? 4u : 8u);
                if ((type != temporaryRecord && type != existingRecord) || length != expected) continue;
                if (serialization->ReadRecordData(data, length) != length) continue;
                RE::FormID resolved{}, actor = 0x14;
                if (!serialization->ResolveFormID(data[0], resolved)) continue;
                if (version == 2 && !serialization->ResolveFormID(data[type == temporaryRecord ? 3 : 1], actor)) continue;
                if (type == temporaryRecord && data[2] <= 0xFFFF)
                {
                    RE::FormID owner{};
                    if (data[1] == 0 || serialization->ResolveFormID(data[1], owner))
                        temporaryItems.push_back({resolved, owner, static_cast<std::uint16_t>(data[2]), actor});
                }
                else if (type == existingRecord) existingByActor[actor].push_back(resolved);
            }
        }
    }

    void RegisterActualEquipmentSerialization()
    {
        if (const auto* serialization = SKSE::GetSerializationInterface())
        {
            serialization->SetUniqueID(serializationID);
            serialization->SetSaveCallback(Save);
            serialization->SetLoadCallback(Load);
            serialization->SetRevertCallback(Revert);
        }
    }

    std::string ClearTemporaryEquipment(RE::Actor* actor)
    {
        std::lock_guard lock(equipmentMutex);
        auto* player = actor ? actor : RE::PlayerCharacter::GetSingleton();
        auto* manager = RE::ActorEquipManager::GetSingleton();
        if (!player || !player->Is3DLoaded() || !manager) return "Actor is not ready for temporary equipment cleanup";
        // 玩家原有物品保留当前穿戴；仅回收衣柜自动添加的指定实例，不恢复先前装备。
        existingByActor.erase(player->GetFormID());
        return RemoveTemporary(player, manager, {});
    }

    std::string ApplyActualEquipment(const std::vector<RE::TESObjectARMO*>& armors, ActualEquipmentMode mode, RE::Actor* actor)
    {
        if (mode == ActualEquipmentMode::Off) return ClearTemporaryEquipment(actor);
        std::lock_guard lock(equipmentMutex);
        auto* player = actor ? actor : RE::PlayerCharacter::GetSingleton();
        auto* manager = RE::ActorEquipManager::GetSingleton();
        if (!player || !player->Is3DLoaded() || !manager) return "Actor is not ready for actual equipment";
        auto& equippedExistingItems = existingByActor[player->GetFormID()];
        std::vector<RE::TESObjectARMO*> targets;
        std::vector<RE::FormID> keep;
        std::uint32_t occupied{};
        for (auto* armor : armors)
        {
            if (!armor || !ShouldActuallyEquip(mode, armor->GetSlotMask().underlying())) continue;
            if (std::find(keep.begin(), keep.end(), armor->GetFormID()) != keep.end()) continue;
            const auto slots = armor->GetSlotMask().underlying();
            if ((occupied & slots) != 0) return "Actual equipment has conflicting armor slots";
            occupied |= slots;
            targets.push_back(armor);
            keep.push_back(armor->GetFormID());
        }
        if (const auto error = RemoveTemporary(player, manager, keep); !error.empty()) return error;
        for (auto it = equippedExistingItems.begin(); it != equippedExistingItems.end();)
        {
            if (std::find(keep.begin(), keep.end(), *it) != keep.end()) { ++it; continue; }
            auto* armor = RE::TESForm::LookupByID<RE::TESObjectARMO>(*it);
            auto inventory = Inventory(player);
            const auto found = inventory.find(armor);
            if (found != inventory.end() && found->second.second && found->second.second->IsWorn())
            {
                manager->UnequipObject(player, armor, WornList(found->second.second.get()), 1, nullptr, false, false, false, true);
                inventory = Inventory(player);
                const auto refreshed = inventory.find(armor);
                if (refreshed != inventory.end() && refreshed->second.second && refreshed->second.second->IsWorn())
                    return "Cannot unequip previous actual armor: " + std::to_string(*it);
            }
            it = equippedExistingItems.erase(it);
        }
        for (auto* armor : targets)
        {
            auto inventory = Inventory(player);
            auto found = inventory.find(armor);
            RE::ExtraDataList* equipList{};
            const auto added = found == inventory.end() || found->second.first <= 0;
            if (added)
            {
                // 让原版添加/装备流程创建 ExtraDataList，避免自行构造 SE/AE 布局不同的列表。
                player->AddObjectToContainer(armor, nullptr, 1, nullptr);
                inventory = Inventory(player);
                found = inventory.find(armor);
                if (found == inventory.end() || found->second.first <= 0) return "Cannot add actual armor: " + std::to_string(armor->GetFormID());
            }
            else
            {
                if (found->second.second && found->second.second->IsWorn()) continue;
                // 已有物品优先使用原实例的 ExtraDataList，保留强化、附魔等数据。
                if (found->second.second && found->second.second->extraLists)
                    for (auto* list : *found->second.second->extraLists) if (list) { equipList = list; break; }
            }
            const auto isTemporary = added || std::any_of(temporaryItems.begin(), temporaryItems.end(), [armor, player](const auto& item) { return item.actor == player->GetFormID() && item.form == armor->GetFormID(); });
            if (!isTemporary && std::find(equippedExistingItems.begin(), equippedExistingItems.end(), armor->GetFormID()) == equippedExistingItems.end())
                equippedExistingItems.push_back(armor->GetFormID());
            // 不排队、不强制锁定，走原版 EquipObject 的即时路径，让装备效果及其他模组收到正常事件。
            manager->EquipObject(player, armor, equipList, 1, nullptr, false, false, false, true);
            inventory = Inventory(player);
            const auto equipped = inventory.find(armor);
            if (added && equipped != inventory.end())
            {
                auto* list = WornList(equipped->second.second.get());
                if (!list && equipped->second.second && equipped->second.second->extraLists)
                    for (auto* candidate : *equipped->second.second->extraLists) if (candidate) { list = candidate; break; }
                auto* changes = player->GetInventoryChanges();
                if (list && changes)
                {
                    auto* id = list->GetByType<RE::ExtraUniqueID>();
                    if (!id)
                    {
                        id = new RE::ExtraUniqueID(player->GetFormID(), changes->GetNextUniqueID());
                        list->Add(id);
                    }
                    // 原版为玩家物品创建的唯一标识随存档保留，用它精确回收本次添加的实例。
                    temporaryItems.push_back({armor->GetFormID(), id->baseID, id->uniqueID, player->GetFormID()});
                }
                else
                {
                    // 原版拒绝装备且没有产生实例列表时，刚添加的唯一一件物品可立即回收。
                    if (equipped->second.first == 1) player->RemoveItem(armor, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                    return "Temporary armor instance could not be identified";
                }
            }
            if (equipped == inventory.end() || !equipped->second.second || !equipped->second.second->IsWorn())
                return "Actual armor was not equipped: " + std::to_string(armor->GetFormID());
            SKSE::log::info("Actual equipment applied armor {:08X} temporary={}", armor->GetFormID(), isTemporary);
        }
        return {};
    }
}
