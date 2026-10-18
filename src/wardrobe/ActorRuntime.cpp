#include "wardrobe/ActualEquipment.h"
#include "wardrobe/SosBackend.h"
#include "wardrobe/ActorRuntime.h"
#include "wardrobe/Model.h"
#include "wardrobe/PlayerOutfitStorage.h"
#include "wardrobe/Store.h"
#include <map>
#include <mutex>
#include <atomic>

namespace Wardrobe
{
    namespace
    {
        std::mutex snapshotMutex;
        std::optional<ActorSnapshot> snapshot;
        std::optional<ActorInfo> watched;
        std::map<std::string, RE::FormID> dynamicActors;
        std::map<std::string, bool> originalAuto;
        constexpr std::uint32_t bindingRecord = 3, autoRecord = 4, stateRecord = 5;
        std::atomic<std::uint64_t> sessionEpoch{};
        Json publishedState = Json::object();
        std::optional<Json> loadedState;
        std::string persistenceError;

        bool IsPerson(RE::Actor* actor)
        {
            const auto* race = actor ? actor->GetRace() : nullptr;
            return race && race->HasKeywordString("ActorTypeNPC") && !actor->IsDead();
        }

        std::string ActorKey(RE::Actor* actor)
        {
            if (actor == RE::PlayerCharacter::GetSingleton()) return "player";
            const auto id = actor->GetFormID();
            if ((id >> 24) != 0xFF)
            {
                const auto* file = actor->GetFile(0);
                if (file) return FormKey{file->fileName, id & (file->IsLight() ? 0xFFFu : 0xFFFFFFu)}.String();
            }
            for (const auto& [key, form] : dynamicActors) if (form == id) return key;
            // 动态引用使用独立 ID，co-save 负责解析读档后的引用变化。
            const auto key = "dynamic:" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ":" + std::to_string(id);
            dynamicActors[key] = id;
            return key;
        }

        ActorInfo Describe(RE::Actor* actor, std::string key, bool includeEquipment = false)
        {
            ActorInfo info{std::move(key), actor ? actor->GetName() : "", actor && actor->Is3DLoaded(), {}};
            if (!info.ready) return info;
            if (includeEquipment) info.equipment = ReadOriginalEquipment(actor);
            if (actor->IsInCombat()) info.states.push_back("combat");
            bool dungeon = false, city = false, town = false;
            auto* location = actor->GetCurrentLocation();
            for (int depth = 0; location && depth < 32; ++depth, location = location->parentLoc)
            {
                dungeon |= location->HasKeywordString("LocTypeDungeon");
                city |= location->HasKeywordString("LocTypeCity");
                town |= location->HasKeywordString("LocTypeTown") || location->HasKeywordString("LocTypeSettlement");
            }
            if (dungeon) info.states.push_back("dungeon");
            if (city) info.states.push_back("city");
            if (town) info.states.push_back("town");
            info.states.push_back("default");
            return info;
        }
    }

    RE::Actor* ResolveActor(const std::string& key)
    {
        if (key == "player") return RE::PlayerCharacter::GetSingleton();
        if (const auto it = dynamicActors.find(key); it != dynamicActors.end()) return RE::TESForm::LookupByID<RE::Actor>(it->second);
        const auto formKey = FormKey::Parse(key);
        auto* data = RE::TESDataHandler::GetSingleton();
        auto* form = formKey && data ? data->LookupForm(formKey->localFormID, formKey->plugin) : nullptr;
        return form ? form->As<RE::Actor>() : nullptr;
    }

    void CaptureWatchedActor()
    {
        auto* pick = RE::CrosshairPickData::GetSingleton();
        const auto handle = pick ? pick->target : RE::ObjectRefHandle{};
        const auto epoch = sessionEpoch.load();
        SKSE::GetTaskInterface()->AddTask([handle, epoch]
        {
            if (epoch != sessionEpoch.load()) return;
            const auto ref = handle.get();
            auto* actor = ref ? ref->As<RE::Actor>() : nullptr;
            std::optional<ActorInfo> info;
            if (IsPerson(actor)) info = Describe(actor, ActorKey(actor));
            std::lock_guard lock(snapshotMutex);
            watched = std::move(info);
        });
    }

    void RequestActorSnapshot(std::vector<std::string> keys)
    {
        const auto epoch = sessionEpoch.load();
        SKSE::GetTaskInterface()->AddTask([keys = std::move(keys), epoch]
        {
            if (epoch != sessionEpoch.load()) return;
            ActorSnapshot next;
            auto* ui = RE::UI::GetSingleton();
            if (!ui || ui->IsMenuOpen(RE::BSFixedString("Main Menu")) || ui->GameIsPaused()) return;
            for (const auto& key : keys)
            {
                auto info = Describe(ResolveActor(key), key, true);
                if (info.ready) SosBackend::Get().RefreshLayers(key, info.equipment);
                next.managed.push_back(std::move(info));
            }
            if (auto* processes = RE::ProcessLists::GetSingleton())
                processes->ForEachHighActor([&](RE::Actor* actor)
                {
                    if (IsPerson(actor) && actor != RE::PlayerCharacter::GetSingleton())
                        next.nearby.push_back(Describe(actor, ActorKey(actor)));
                    return RE::BSContainer::ForEachResult::kContinue;
                });
            std::sort(next.nearby.begin(), next.nearby.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
            std::lock_guard lock(snapshotMutex);
            next.watched = watched;
            snapshot = std::move(next);
        });
    }

    std::optional<ActorSnapshot> TakeActorSnapshot()
    {
        std::lock_guard lock(snapshotMutex);
        auto result = std::move(snapshot); snapshot.reset(); return result;
    }

    std::optional<bool> OriginalAutoSwitch(const std::string& key)
    {
        const auto it = originalAuto.find(key);
        return it == originalAuto.end() ? std::nullopt : std::optional(it->second);
    }
    void RememberAutoSwitch(const std::string& key, std::optional<bool> value)
    {
        if (value) originalAuto[key] = *value;
        else originalAuto.erase(key);
    }
    void SaveActorBindings(SKSE::SerializationInterface* serialization)
    {
        const auto write = [&](std::uint32_t type, const std::string& key, std::uint32_t value)
        {
            if (!serialization->OpenRecord(type, 1)) return;
            const auto length = static_cast<std::uint32_t>(key.size());
            serialization->WriteRecordData(value); serialization->WriteRecordData(length);
            serialization->WriteRecordData(key.data(), length);
        };
        for (const auto& [key, form] : dynamicActors) write(bindingRecord, key, form);
        for (const auto& [key, value] : originalAuto) write(autoRecord, key, value ? 1u : 0u);
        std::lock_guard lock(snapshotMutex);
        const auto mode = playerOutfitStorage.load();
        const auto state = ActorStateForSave(publishedState, mode).dump();
        if (mode == PlayerOutfitStorage::SaveLocal)
        {
            persistenceError = MetadataStore("Data/SKSE/Plugins/SkyWardrobe").CleanPlayerOutfit();
            if (!persistenceError.empty()) SKSE::log::error("Player outfit JSON cleanup failed: {}", persistenceError);
        }
        if (!serialization->WriteRecord(stateRecord, 1, state.data(), static_cast<std::uint32_t>(state.size())))
            SKSE::log::error("Cannot save actor outfit state");
    }
    void LoadActorBinding(SKSE::SerializationInterface* serialization, std::uint32_t type, std::uint32_t version, std::uint32_t length)
    {
        if (type == stateRecord && version == 1 && length > 0 && length < 4 * 1024 * 1024)
        {
            std::string text(length, '\0');
            if (serialization->ReadRecordData(text.data(), length) != length) return;
            try
            {
                auto value = Json::parse(text);
                if (!value.is_object()) return;
                auto actors = ActorStateForLoad(std::move(value), playerOutfitStorage.load());
                std::lock_guard lock(snapshotMutex); loadedState = std::move(actors);
            }
            catch (const std::exception& error) { SKSE::log::warn("Invalid actor state in co-save: {}", error.what()); }
            return;
        }
        if ((type != bindingRecord && type != autoRecord) || version != 1 || length < 8 || length > 4096) return;
        std::uint32_t value{}, size{};
        if (!serialization->ReadRecordData(value) || !serialization->ReadRecordData(size) || size != length - 8) return;
        std::string key(size, '\0');
        if (serialization->ReadRecordData(key.data(), size) != size) return;
        if (type == bindingRecord)
        {
            RE::FormID resolved{};
            if (serialization->ResolveFormID(value, resolved)) dynamicActors[key] = resolved;
        }
        else if (value <= 1) originalAuto[key] = value != 0;
    }
    void ResetActorBindings()
    {
        ++sessionEpoch;
        dynamicActors.clear(); originalAuto.clear();
        std::lock_guard lock(snapshotMutex); snapshot.reset(); watched.reset(); loadedState.reset(); publishedState = Json::object();
    }
    std::string TakePlayerPersistenceError()
    {
        std::lock_guard lock(snapshotMutex);
        auto result = std::move(persistenceError); persistenceError.clear(); return result;
    }
    void PublishActorState(Json state)
    {
        std::lock_guard lock(snapshotMutex); publishedState = std::move(state);
    }
    std::optional<Json> TakeLoadedActorState()
    {
        std::lock_guard lock(snapshotMutex);
        auto result = std::move(loadedState); loadedState.reset(); return result;
    }
}
