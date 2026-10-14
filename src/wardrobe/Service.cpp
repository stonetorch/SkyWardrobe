#include "wardrobe/Service.h"
#include "wardrobe/SosBackend.h"
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

namespace Wardrobe
{
    Service& Service::Get() { static Service service; return service; }

    Service::~Service() { FlushSave(); }

    void Service::SaveWorking()
    {
        if (batching_) return;
        StoreActorContext();
        store.database["workingItems"] = store.database["actors"]["player"]["workingItems"];
        store.database["activePreset"] = store.database["actors"]["player"]["activePreset"];
        ++revision;
        if (!store.writable) { status = store.error; return; }
        // 穿脱不改变搜索元数据；连续操作只保留最新待写状态，避免全目录重建。
        saveQueued_ = true;
        status = "Saving";
        PollSave();
    }

    void Service::PollSave()
    {
        if (pendingSave_.valid())
        {
            if (pendingSave_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
            try { store.error = pendingSave_.get(); }
            catch (const std::exception& e) { store.error = e.what(); }
            status = store.error.empty() ? "Saved" : store.error;
        }
        if (!saveQueued_) return;
        try
        {
            // 同一时刻只有一个写入者，旧快照完成后才提交最新快照。
            pendingSave_ = std::async(std::launch::async, [this, snapshot = store.database]() mutable
            {
                return store.SaveSnapshot(std::move(snapshot));
            });
            saveQueued_ = false;
            status = "Saving";
        }
        catch (const std::exception& e)
        {
            saveQueued_ = false;
            store.error = e.what();
            status = store.error;
        }
    }

    void Service::FlushSave()
    {
        // 重载和退出必须先排空后台写入，防止旧快照覆盖新数据。
        while (pendingSave_.valid() || saveQueued_)
        {
            if (pendingSave_.valid()) pendingSave_.wait();
            PollSave();
        }
    }

    void Service::Initialize()
    {
        if (!SosBackend::Get().IsInstalled()) return;
        if (InitializePinyinSearch("Data/SKSE/Plugins/SkyWardrobe/dict"))
            SKSE::log::info("Automatic pinyin search initialized");
        else
            SKSE::log::warn("Pinyin dictionary unavailable; using original text and manual search keys");
        catalog.clear(); plugins.clear();
        auto* data = RE::TESDataHandler::GetSingleton();
        if (!data) return;
        for (auto* form : data->GetFormArray(RE::FormType::Armor))
        {
            auto* armor = form ? form->As<RE::TESObjectARMO>() : nullptr;
            const auto* origin = armor ? armor->GetFile(0) : nullptr;
            if (!origin) continue;
            // 普通 ESP 与 ESL 分别保留 24 / 12 位本地 ID，与加载顺序索引完全分离。
            FormKey key{ origin->fileName, armor->GetFormID() & (origin->IsLight() ? 0xFFFu : 0xFFFFFFu) };
            catalog.push_back({ key.String(), key.plugin, armor->GetName(), armor->GetSlotMask().underlying() });
            ++plugins[key.plugin];
        }
        std::sort(catalog.begin(), catalog.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
        Reload();
        SKSE::log::info("Catalog: {} armors from {} plugins", catalog.size(), plugins.size());
    }

    void Service::Reload()
    {
        FlushSave();
        SosBackend::Get().Reset();
        for (auto& [key, generation] : actorGeneration_) ++generation;
        autoBusy_.clear();
        { std::lock_guard lock(autoMutex_); autoCompletions_.clear(); }
        if (store.Load())
        {
            ClearPinyinSearchCache();
            indexedCategories_ = nullptr;
            indexedPlugins_ = nullptr;
            livePreview = store.database.value("livePreview", true);
            currentActor = "player";
            working = ActorProfile()["workingItems"].get<std::vector<std::string>>();
            activePreset = ActorProfile().value("activePreset", "");
            presetBaseline_ = working;
            undo_ = nullptr;
            status = "JSON loaded";
            RebuildIndex();
            SosBackend::Get().SetEquipmentMode(EquipmentMode());
            for (const auto& [key, profile] : store.database["actors"].items()) unavailableActors_.insert(key);
            PublishActorState(store.database["actors"]);
        }
        else { status = store.error; RebuildIndex(); }
    }

    bool Service::GenerateAliasTemplate(const std::string& plugin, bool overwrite)
    {
        if (!plugins.contains(plugin)) { status = "Plugin not found: " + plugin; return false; }
        Json items = Json::object();
        // 名称取游戏原名，部件沿用当前识别结果（包含已有元数据修正）。
        for (const auto& entry : catalog)
            if (entry.plugin == plugin)
            {
                const auto key = FormKey::Parse(entry.key);
                if (key) items[std::format("{:06X}", key->localFormID)] =
                    {{"name", entry.originalName}, {"part", PartIDs[entry.part]}};
            }
        if (!store.WriteAliasTemplate(plugin, items, overwrite)) { status = store.error; return false; }
        status = "Alias template ready: 00_Generated." + plugin + ".json";
        return true;
    }

    bool Service::Save()
    {
        if (!store.writable) { status = store.error; return false; }
        if (batching_) return true;
        StoreActorContext();
        ValidateLifeData(store.database);
        activePreset = ActorProfile().value("activePreset", "");
        RebuildIndex();
        // 元数据修改与穿脱共用串行后台写入；返回成功表示已排队，写盘失败由 PollSave 提示。
        SaveWorking();
        return saveQueued_ || pendingSave_.valid();
    }

    const ClothingEntry* Service::Find(std::string_view key) const
    {
        const auto it = std::lower_bound(catalog.begin(), catalog.end(), key, [](const auto& entry, std::string_view id) { return entry.key < id; });
        return it != catalog.end() && it->key == key ? &*it : nullptr;
    }

    int Service::Part(const ClothingEntry& entry) const
    {
        return entry.part;
    }

    void Service::RebuildIndex()
    {
        // 拼音缓存按文本建立，普通编辑保留缓存；只有重载时才清理旧文本。
        const bool sharedMetadataChanged = indexedCategories_ != store.database["categories"] ||
            indexedPlugins_ != store.database["pluginCategories"];
        ++revision;
        for (auto& entry : catalog)
        {
            auto metadata = store.Effective(entry.key);
            // 组归属等修改不会影响大部分服装，避免重复构建全目录搜索词。
            if (!sharedMetadataChanged && !entry.searchKeys.empty() && entry.metadata == metadata) continue;
            entry.metadata = std::move(metadata);
            const auto alias = entry.metadata.value("alias", "");
            entry.displayName = !alias.empty() ? alias : !entry.originalName.empty() ? entry.originalName : entry.key;
            entry.part = 0;
            // 槽位仅作初始建议，用户标记（包括 unknown）覆盖建议。
            if (entry.slots & (1u << 2)) entry.part = 1;
            else if (entry.slots & (1u << 7)) entry.part = 7;
            else if (entry.slots & (1u << 3)) entry.part = 9;
            else if (entry.slots & ((1u << 0) | (1u << 1))) entry.part = 10;
            else if (entry.slots & (1u << 5)) entry.part = 12;
            else if (entry.slots & (1u << 6)) entry.part = 14;
            if (entry.metadata.contains("part"))
                for (int i = 0; i < static_cast<int>(std::size(PartIDs)); ++i)
                    if (entry.metadata["part"] == PartIDs[i]) { entry.part = i; break; }
            entry.searchKeys = {{entry.originalName, false}, {entry.key, false}, {entry.plugin, false}};
            const auto addMetadata = [&](const Json& metadata)
            {
                for (const char* field : {"name", "alias", "note", "pinyin"})
                    if (metadata.contains(field)) entry.searchKeys.emplace_back(metadata[field].get<std::string>(), std::string_view(field) == "pinyin");
                for (const char* field : {"search", "aliases"})
                    if (metadata.contains(field)) for (const auto& key : metadata[field]) entry.searchKeys.emplace_back(key.get<std::string>(), true);
            };
            addMetadata(entry.metadata);
            if (store.database["pluginCategories"].contains(entry.plugin)) addMetadata(store.database["pluginCategories"][entry.plugin]);
            for (const auto& category : entry.metadata.value("categories", std::vector<std::string>{}))
            {
                entry.searchKeys.emplace_back(category, false);
                if (store.database["categories"].contains(category)) addMetadata(store.database["categories"][category]);
            }
            // 重建目录时预先转换中文，查询和面板重绘只读取缓存。
            for (const auto& [key, pinyin] : entry.searchKeys) CachePinyinSearch(key, pinyin);
        }
        indexedCategories_ = store.database["categories"];
        indexedPlugins_ = store.database["pluginCategories"];
        // 侧栏与预设也提前建立索引，未被服装引用的分类同样支持自动拼音。
        for (const char* collection : {"categories", "pluginCategories", "presets", "groups"})
            for (const auto& [id, metadata] : store.database[collection].items())
            {
                CachePinyinSearch(id);
                for (const char* field : {"name", "alias", "note", "pinyin"})
                    if (metadata.contains(field) && metadata[field].is_string())
                        CachePinyinSearch(metadata[field].get<std::string>(), std::string_view(field) == "pinyin");
                for (const char* field : {"search", "aliases"})
                    if (metadata.contains(field) && metadata[field].is_array())
                        for (const auto& key : metadata[field]) if (key.is_string()) CachePinyinSearch(key.get<std::string>(), true);
            }
    }

    std::string Service::Name(const ClothingEntry& entry) const
    {
        return entry.displayName;
    }

    int Service::Score(const ClothingEntry& entry, std::string_view query) const
    {
        if (query.empty()) return 1;
        int score = 0;
        for (const auto& [key, pinyin] : entry.searchKeys) score = std::max(score, SearchScore(query, key, pinyin));
        return score;
    }

    bool Service::InWorking(std::string_view key) const { return std::find(working.begin(), working.end(), key) != working.end(); }
    bool Service::Dirty() const
    {
        return !activePreset.empty() && store.database["presets"].contains(activePreset) &&
            presetBaseline_ != working;
    }

    void Service::Add(std::string key)
    {
        const auto* entry = Find(key);
        if (!entry || InWorking(key)) return;
        // 外观覆盖遵从真实 biped 冲突；替换冲突项时只修改工作搭配，不改已保存预设。
        std::erase_if(working, [&](const auto& item) { const auto* other = Find(item); return other && (entry->slots & other->slots) != 0; });
        working.push_back(std::move(key));
        MarkManual();
        SaveWorking();
        if (livePreview && !batching_) Apply(false, false);
    }

    void Service::Remove(std::string_view key)
    {
        if (std::erase(working, std::string(key)) == 0) return;
        MarkManual();
        SaveWorking();
        if (livePreview && !batching_) Apply(false, false);
    }

    LayerSettings Service::LayersFor(const std::string& actor) const
    {
        const auto& profile = store.database["actors"][actor];
        return {profile.value("backupItems", std::vector<std::string>{}), profile.value("backupEnabled", true),
            profile.value("originalEnabled", false), profile.value("hiddenSlots", 0u)};
    }
    std::vector<LayerResult> Service::LayerPreview() const
    {
        const auto settings = LayersFor(currentActor);
        const auto describe = [&](const std::vector<std::string>& keys)
        {
            std::vector<LayerItem> result;
            for (const auto& key : keys)
                if (const auto* item = Find(key)) result.push_back({key, Name(*item), item->slots});
                else result.push_back({key, "Missing: " + key, 0});
            return result;
        };
        std::vector<LayerItem> original;
        for (const auto& actor : actorSnapshot.managed) if (actor.key == currentActor) original = actor.equipment;
        const auto& profile = store.database["actors"][currentActor];
        auto top = working;
        if (profile.value("automatic", false) && !profile.value("manual", false) && profile["runtime"].contains("items"))
            top = profile["runtime"]["items"].get<std::vector<std::string>>();
        return ComposeLayers(describe(top), describe(settings.backup), original, settings);
    }
    void Service::AddBackup(const std::string& key)
    {
        if (!store.writable) return;
        const auto* item = Find(key);
        if (!item) return;
        auto items = LayersFor(currentActor).backup;
        std::erase_if(items, [&](const auto& other) { const auto* entry = Find(other); return other == key || (entry && (entry->slots & item->slots)); });
        items.push_back(key); ActorProfile()["backupItems"] = items;
        MarkManual(); SaveWorking(); if (livePreview && !batching_) Apply(false, false);
    }
    void Service::RemoveBackup(const std::string& key)
    {
        if (!store.writable) return;
        auto items = LayersFor(currentActor).backup;
        if (!std::erase(items, key)) return;
        ActorProfile()["backupItems"] = items;
        MarkManual(); SaveWorking(); if (livePreview && !batching_) Apply(false, false);
    }
    void Service::MoveLayer(const std::string& key, bool toBackup)
    {
        if (!store.writable || !Find(key)) return;
        BeginBatch();
        if (toBackup) { AddBackup(key); Remove(key); }
        else { Add(key); RemoveBackup(key); }
        EndBatch(true);
    }
    void Service::UseBackupGroup(const std::string& id)
    {
        if (!store.writable || !Group(id)) return;
        ActorProfile()["backupItems"] = GenerateGroup(id);
        ActorProfile()["backupGroup"] = id;
        MarkManual(); SaveWorking(); if (livePreview) Apply(false, false);
    }
    void Service::SetLayerOptions(bool backupEnabled, bool originalEnabled, std::uint32_t hiddenSlots)
    {
        if (!store.writable) return;
        auto& profile = ActorProfile();
        profile["backupEnabled"] = backupEnabled; profile["originalEnabled"] = originalEnabled; profile["hiddenSlots"] = hiddenSlots;
        MarkManual(); SaveWorking(); if (livePreview) Apply(false, false);
    }

    const Json* Service::Group(const std::string& id) const
    {
        const auto [collection, key] = SplitGroupID(id);
        if (collection.empty() || !store.database[collection].contains(key)) return nullptr;
        return &store.database[collection][key];
    }

    bool Service::InGroup(const ClothingEntry& entry, const std::string& id) const
    {
        const auto* group = Group(id);
        if (!group) return false;
        const auto [collection, key] = SplitGroupID(id);
        if (collection == "pluginCategories") return Normalize(entry.plugin) == Normalize(key);
        const auto members = collection == "categories"
            ? entry.metadata.value("categories", std::vector<std::string>{})
            : group->value("items", std::vector<std::string>{});
        return std::find(members.begin(), members.end(), collection == "categories" ? key : entry.key) != members.end();
    }

    void Service::SetGroupItem(const std::string& id, const ClothingEntry& entry, bool included)
    {
        if (!store.writable || !Group(id)) return;
        const auto [collection, key] = SplitGroupID(id);
        if (collection == "pluginCategories") return;
        auto values = collection == "categories" ? entry.metadata.value("categories", std::vector<std::string>{})
            : store.database[collection][key].value("items", std::vector<std::string>{});
        const auto member = collection == "categories" ? key : entry.key;
        std::erase(values, member);
        if (included) values.push_back(member);
        if (collection == "categories")
        {
            auto& item = store.database["items"][entry.key];
            if (!item.is_object()) item = Json::object();
            item["categories"] = values;
        }
        else store.database[collection][key]["items"] = values;
        Save();
    }

    void Service::UseGroup(const std::string& id, bool apply)
    {
        if (!Group(id)) { status = "Group not found: " + id; return; }
        working = GenerateGroup(id);
        MarkManual();
        if (MultiplePresets() && !ActorProfile().value("automatic", false)) activePreset.clear();
        ++revision;
        SaveWorking();
        if (apply && !batching_) Apply(false, false);
    }

    std::string Service::CreateSubgroup(const std::string& parent)
    {
        const auto* group = Group(parent);
        if (!store.writable || !group) return {};
        const auto [collection, key] = SplitGroupID(parent);
        const auto alias = group->value("alias", "");
        const auto name = alias.empty() ? group->value("name", key) : alias;
        std::string id;
        for (std::uint64_t number = 1;; ++number)
        {
            id = name + "-" + std::to_string(number);
            bool exists = store.database["groups"].contains(id);
            for (const char* field : {"groups", "categories", "pluginCategories"})
                for (const auto& [otherID, metadata] : store.database[field].items())
                    exists |= metadata.value("name", otherID) == id || metadata.value("alias", "") == id;
            if (!exists) break;
        }
        store.database["groups"][id] = {{"name", id}, {"items", Json::array()}, {"references", {parent}}};
        auto refs = store.database[collection][key].value("references", std::vector<std::string>{});
        refs.push_back("groups:" + id);
        store.database[collection][key]["references"] = refs;
        Save();
        return "groups:" + id;
    }

    void Service::SelectPreset(const std::string& id)
    {
        if (!store.database["presets"].contains(id) || store.database["presets"][id].value("actor", "player") != currentActor) return;
        const auto group = store.database["presets"][id].value("group", "");
        if (!Group(group)) { status = "Group not found: " + group; return; }
        if (!MultiplePresets() && id != ActorProfile().value("defaultPreset", "")) return;
        ActivatePreset(id);
        working = GenerateGroup(group);
        MarkManual();
        presetBaseline_ = working;
        SaveWorking();
        if (livePreview && !batching_) Apply(false, false);
    }

    void Service::OverwritePreset()
    {
        if (activePreset.empty() || !store.database["presets"].contains(activePreset)) return;
        undoPreset_ = activePreset;
        undo_ = store.database["presets"][activePreset];
        const auto [collection, key] = SplitGroupID(store.database["presets"][activePreset].value("group", ""));
        if (collection != "groups" || !store.database[collection].contains(key)) { undo_ = nullptr; return; }
        undoGroupID_ = "groups:" + key;
        undoGroup_ = store.database[collection][key];
        store.database[collection][key]["items"] = working;
        presetBaseline_ = working;
        store.database["presets"][activePreset]["items"] = working; Save();
    }

    void Service::UndoOverwrite()
    {
        if (!undo_.is_null())
        {
            const auto [collection, key] = SplitGroupID(undoGroupID_);
            if (!collection.empty() && store.database[collection].contains(key)) store.database[collection][key] = undoGroup_;
            if (store.database["presets"].contains(undoPreset_)) store.database["presets"][undoPreset_] = undo_;
            undo_ = nullptr; Save();
        }
    }

    void Service::Apply(bool clear, bool persist)
    {
        ActorProfile()["appearanceActive"] = !clear;
        MarkManual();
        if (persist) SaveWorking();
        else StoreActorContext();
        SosBackend::Get().RequestFor(currentActor, clear ? std::vector<std::string>{} : working, clear,
            EquipmentMode(), ActorProfile().value("automatic", false), false, {}, LayersFor(currentActor));
    }

    ActualEquipmentMode Service::EquipmentMode() const
    {
        return ParseActualEquipmentMode(store.database["actors"][currentActor].value("actualEquipmentMode", std::string("off")));
    }

    void Service::SetEquipmentMode(ActualEquipmentMode mode)
    {
        if (!store.writable) return;
        ActorProfile()["actualEquipmentMode"] = ActualEquipmentModeID(mode);
        ++actorGeneration_[currentActor];
        SosBackend::Get().CancelActor(currentActor);
        if (currentActor == "player") store.database["actualEquipmentMode"] = ActualEquipmentModeID(mode);
        if (mode == ActualEquipmentMode::Off) SosBackend::Get().CleanupEquipment(currentActor);
        SaveWorking();
    }

    void Service::StoreActorContext()
    {
        auto& profile = ActorProfile();
        profile["workingItems"] = working;
        profile["activePreset"] = activePreset;
        profile["baseline"] = presetBaseline_;
        PublishActorState(store.database["actors"]);
    }

    void Service::MarkManual()
    {
        if (ActorProfile().value("automatic", false))
        {
            if (!ActorProfile().value("manual", false)) ActorProfile()["manualState"] = ActorProfile()["runtime"].value("signature", "");
            ActorProfile()["manual"] = true;
        }
        ++actorGeneration_[currentActor];
        SosBackend::Get().CancelActor(currentActor);
    }

    void Service::BeginBatch() { batching_ = true; }
    void Service::EndBatch(bool preview)
    {
        batching_ = false;
        Save();
        if (preview && livePreview) Apply(false, false);
    }
    void Service::SetLivePreview(bool enabled)
    {
        livePreview = enabled; store.database["livePreview"] = enabled; SaveWorking();
    }
    void Service::SelectActor(const std::string& key)
    {
        if (key == currentActor || !store.database["actors"].contains(key) || !store.database["actors"][key].value("managed", false)) return;
        StoreActorContext();
        currentActor = key;
        working = ActorProfile()["workingItems"].get<std::vector<std::string>>();
        activePreset = ActorProfile().value("activePreset", "");
        presetBaseline_ = ActorProfile().value("baseline", working);
        undo_ = nullptr; ++revision;
    }
    void Service::AddActor(const ActorInfo& actor)
    {
        if (!store.writable) return;
        ++actorGeneration_[actor.key]; SosBackend::Get().CancelActor(actor.key);
        if (!store.database["actors"].contains(actor.key)) store.database["actors"][actor.key] = NewActorProfile(actor.name);
        EnsureDefaultPresets(store.database);
        store.database["actors"][actor.key]["managed"] = true;
        store.database["actors"][actor.key]["releasePending"] = false;
        SelectActor(actor.key); Save();
    }
    void Service::RemoveActor(const std::string& key)
    {
        if (key == "player" || !store.writable || !store.database["actors"].contains(key)) return;
        ++actorGeneration_[key];
        SosBackend::Get().CancelActor(key);
        if (currentActor == key) SelectActor("player");
        auto& profile = store.database["actors"][key];
        profile["managed"] = false; profile["automatic"] = false; profile["manual"] = false;
        profile["takeoverPending"] = false;
        profile["releasePending"] = true;
        Save();
    }
    void Service::SetAutomatic(bool enabled)
    {
        if (!store.writable) return;
        ActorProfile()["automatic"] = enabled;
        ActorProfile()["takeoverPending"] = enabled;
        ActorProfile()["manual"] = false;
        ++actorGeneration_[currentActor];
        SosBackend::Get().CancelActor(currentActor);
        if (!enabled)
        {
            ActorProfile()["releasePending"] = true;
        }
        else
        {
            ActorProfile()["releasePending"] = false;
            ActorProfile()["runtime"].erase("signature");
        }
        SaveWorking();
    }
    void Service::ResumeAutomatic(bool reset)
    {
        ActorProfile()["manual"] = false;
        ActorProfile().erase("manualState");
        if (reset || !ActorProfile()["runtime"].value("error", "").empty()) ActorProfile()["runtime"].erase("signature");
        unavailableActors_.insert(currentActor);
        ++actorGeneration_[currentActor]; SosBackend::Get().CancelActor(currentActor); SaveWorking();
    }
    void Service::SetMultiplePresets(bool enabled)
    {
        if (!store.writable) return;
        StoreActorContext();
        for (auto& profile : store.database["actors"])
        {
            const auto id = profile.value("activePreset", "");
            if (!id.empty()) profile["presetRuntime"][id] = profile["runtime"];
        }
        store.database["multiplePresets"] = enabled;
        EnsureDefaultPresets(store.database);
        for (auto& [key, profile] : store.database["actors"].items())
        {
            ++actorGeneration_[key]; SosBackend::Get().CancelActor(key);
            profile["manual"] = false; profile.erase("manualState");
            // 关闭时恢复默认预设的独立运行记录，避免沿用其他预设的自动结果。
            if (!enabled)
                profile["runtime"] = profile.value("presetRuntime", Json::object()).value(profile.value("activePreset", ""), Json::object());
            profile["runtime"].erase("signature");
        }
        activePreset = ActorProfile().value("activePreset", "");
        Save();
    }

    void Service::ActivatePreset(const std::string& id)
    {
        if (!store.database["presets"].contains(id) || store.database["presets"][id].value("actor", "player") != currentActor) return;
        if (!MultiplePresets() && id != ActorProfile().value("defaultPreset", "")) return;
        auto& profile = ActorProfile();
        if (!activePreset.empty()) profile["presetRuntime"][activePreset] = profile["runtime"];
        activePreset = id;
        profile["activePreset"] = id;
        profile["runtime"] = profile.value("presetRuntime", Json::object()).value(id, Json::object());
        ResumeAutomatic();
    }
    std::vector<std::string> Service::GenerateGroup(const std::string& id) const
    {
        std::vector<std::pair<std::string, std::uint32_t>> candidates;
        for (const auto& entry : catalog) if (InGroup(entry, id)) candidates.emplace_back(entry.key, entry.slots);
        static std::mt19937 generator(std::random_device{}());
        return RandomOutfit(std::move(candidates), generator);
    }
    void Service::ResetActorSession()
    {
        for (auto& [key, value] : actorGeneration_) ++value;
        autoBusy_.clear(); unavailableActors_.clear();
        equipmentCleanupPending_.clear();
        for (auto& [key, actor] : store.database["actors"].items())
        {
            unavailableActors_.insert(key);
            if (actor.value("actualEquipmentMode", "off") == "off") equipmentCleanupPending_.insert(key);
            actor["runtime"] = Json::object(); actor.erase("presetRuntime"); actor["manual"] = false; actor.erase("manualState");
            actor["takeoverPending"] = actor.value("managed", true) && actor.value("automatic", false);
        }
        actorSnapshot = {}; nextActorPoll_ = {};
        PublishActorState(store.database["actors"]);
        std::lock_guard lock(autoMutex_); autoCompletions_.clear();
    }
    void Service::QueueSessionReset(bool ready)
    {
        worldReady_.store(ready);
        sessionReset_.store(true);
    }

    void Service::TickActors()
    {
        if (sessionReset_.exchange(false)) ResetActorSession();
        if (!worldReady_.load()) return;
        if (!SosBackend::Get().IsInstalled() || !store.writable) return;
        if (auto loaded = TakeLoadedActorState())
        {
            // 配置仍共享 JSON；实际结果及轮换进度以正在加载的存档为准。
            for (auto& [key, profile] : store.database["actors"].items())
            {
                if (!loaded->contains(key)) { profile["runtime"] = Json::object(); profile.erase("presetRuntime"); continue; }
                const auto& saved = (*loaded)[key];
                for (const auto* field : {"runtime", "presetRuntime", "workingItems", "activePreset", "baseline", "manual", "manualState", "automatic", "actualEquipmentMode", "releasePending", "backupItems", "backupEnabled", "originalEnabled", "hiddenSlots", "appearanceActive"})
                    if (saved.contains(field)) profile[field] = saved[field];
                if (!profile.value("managed", true)) { profile["automatic"] = false; profile["releasePending"] = true; }
                profile["takeoverPending"] = profile.value("managed", true) && profile.value("automatic", false);
            }
            if (!MultiplePresets())
            {
                // 旧存档可带回其他活动预设；默认模式不接受这个绑定。
                for (auto& profile : store.database["actors"])
                {
                    const auto current = profile.value("activePreset", "");
                    const auto pinned = profile.value("defaultPreset", "");
                    if (current != pinned)
                    {
                        if (!current.empty()) profile["presetRuntime"][current] = profile["runtime"];
                        profile["runtime"] = profile.value("presetRuntime", Json::object()).value(pinned, Json::object());
                        profile["manual"] = false; profile.erase("manualState");
                    }
                }
                EnsureDefaultPresets(store.database);
            }
            working = ActorProfile()["workingItems"].get<std::vector<std::string>>();
            activePreset = ActorProfile().value("activePreset", "");
            presetBaseline_ = ActorProfile().value("baseline", working);
            ++revision;
            SaveWorking();
        }
        std::vector<AutoCompletion> completions;
        { std::lock_guard lock(autoMutex_); completions.swap(autoCompletions_); }
        for (auto& result : completions)
        {
            autoBusy_.erase(result.actor);
            if (actorGeneration_[result.actor] != result.generation) continue;
            auto& profile = store.database["actors"][result.actor];
            if (result.release || result.takeover)
            {
                if (result.success) profile[result.takeover ? "takeoverPending" : "releasePending"] = false;
                else status = "Unable to restore actor SOS settings; will retry when available";
                SaveWorking(); continue;
            }
            if (result.success) { profile["runtime"] = std::move(result.runtime); profile["appearanceActive"] = true; }
            else profile["runtime"]["signature"] = result.runtime.value("signature", "");
            profile["runtime"]["error"] = result.success ? "" : "Appearance application failed; retry with Resume automatic";
            SaveWorking();
        }
        if (auto next = TakeActorSnapshot())
        {
            actorSnapshot = std::move(*next);
            for (const auto& actor : actorSnapshot.managed)
            {
                if (!store.database["actors"].contains(actor.key)) continue;
                auto& profile = store.database["actors"][actor.key];
                if (!actor.name.empty()) profile["name"] = actor.name;
                if (!actor.ready) { unavailableActors_.insert(actor.key); continue; }
                if (equipmentCleanupPending_.erase(actor.key) > 0) SosBackend::Get().CleanupEquipment(actor.key);
                if (profile.value("takeoverPending", false))
                {
                    if (!autoBusy_.contains(actor.key))
                    {
                        autoBusy_.insert(actor.key);
                        const auto generation = actorGeneration_[actor.key];
                        SosBackend::Get().RequestFor(actor.key, {}, false, ActualEquipmentMode::Off, true, true,
                            [this, key = actor.key, generation](bool success)
                            {
                                std::lock_guard lock(autoMutex_);
                                autoCompletions_.push_back({key, generation, success, Json::object(), false, true});
                            });
                    }
                    continue;
                }
                if (profile.value("releasePending", false))
                {
                    // 未加载时延迟解除接管，重新加载后再恢复原设置。
                    if (!autoBusy_.contains(actor.key))
                    {
                        autoBusy_.insert(actor.key);
                        const auto generation = actorGeneration_[actor.key];
                        const bool clear = !profile.value("managed", true);
                        SosBackend::Get().RequestFor(actor.key, {}, clear, ActualEquipmentMode::Off, false, !clear,
                            [this, key = actor.key, generation](bool success)
                            {
                                std::lock_guard lock(autoMutex_);
                                autoCompletions_.push_back({key, generation, success, Json::object(), true});
                            });
                    }
                    continue;
                }
                if (profile.value("managed", true))
                {
                    if ((!profile.value("automatic", false) || profile.value("manual", false)) && profile.value("appearanceActive", false) && unavailableActors_.erase(actor.key))
                        SosBackend::Get().RequestFor(actor.key, profile["workingItems"].get<std::vector<std::string>>(), false,
                            ParseActualEquipmentMode(profile.value("actualEquipmentMode", "off")), profile.value("automatic", false), false, {}, LayersFor(actor.key));
                    EvaluateAutomatic(actor);
                }
            }
        }
        if (std::chrono::steady_clock::now() < nextActorPoll_) return;
        nextActorPoll_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        std::vector<std::string> keys;
        for (const auto& [key, profile] : store.database["actors"].items())
            if (profile.value("managed", true) || profile.value("releasePending", false)) keys.push_back(key);
        RequestActorSnapshot(std::move(keys));
    }

    void Service::EvaluateAutomatic(const ActorInfo& actor)
    {
        auto& profile = store.database["actors"][actor.key];
        if (!profile.value("automatic", false) || autoBusy_.contains(actor.key)) return;
        const auto presetID = profile.value("activePreset", "");
        if (!store.database["presets"].contains(presetID)) return;
        const auto& preset = store.database["presets"][presetID];
        if (preset.value("actor", "player") != actor.key) return;
        auto runtime = profile.value("runtime", Json::object());
        std::string signature = presetID;
        for (const auto& state : actor.states) signature += ":" + state;
        const bool reload = unavailableActors_.erase(actor.key) > 0;
        if (profile.value("manual", false))
        {
            const auto manualState = profile.value("manualState", "");
            if (manualState.empty()) { profile["manualState"] = signature; return; }
            if (manualState == signature) return;
            profile["manual"] = false; profile.erase("manualState");
        }
        if (runtime.value("signature", "") == signature && !reload) return;
        const auto& rules = preset["rules"];
        std::string state, group;
        std::vector<std::string> items;
        // 重新加载复用已应用结果；短暂战斗结束也复用战斗前结果。
        if (runtime.value("signature", "") == signature && reload && runtime.contains("items"))
            items = runtime["items"].get<std::vector<std::string>>();
        else if (runtime.value("state", "") == "combat" && !actor.states.empty() && actor.states.front() != "combat" &&
            runtime.contains("resume") && runtime["resume"].value("signature", "") == signature)
        {
            runtime = RestoreCombatOutfit(runtime);
            items = runtime.value("items", std::vector<std::string>{});
        }
        else
        {
            static std::mt19937 generator(std::random_device{}());
            bool useWorking = false;
            for (const auto& candidateState : actor.states)
            {
                if (!rules.contains(candidateState) || !rules[candidateState].value("enabled", true)) continue;
                const auto& rule = rules[candidateState];
                // 命中工作搭配规则即终止匹配；空工作搭配也不能回落到低优先级规则。
                if (rule.value("useWorking", false))
                {
                    state = candidateState; useWorking = true;
                    items = profile.value("workingItems", std::vector<std::string>{}); break;
                }
                std::vector<std::string> valid;
                for (const auto& groupID : RuleCandidates(store.database, rule))
                {
                    const auto* metadata = Group(groupID);
                    if (!metadata) continue;
                    if (SplitGroupID(groupID).first == "groups")
                    {
                        const auto members = metadata->value("items", std::vector<std::string>{});
                        if (std::any_of(members.begin(), members.end(), [&](const auto& key) { return !Find(key); })) continue;
                    }
                    if (!GenerateGroup(groupID).empty() && std::find(valid.begin(), valid.end(), groupID) == valid.end()) valid.push_back(groupID);
                }
                if (valid.empty()) continue;
                state = candidateState;
                const auto progress = runtime.value("progress", Json::object()).value(state, Json::object());
                group = ChooseRuleGroup(rule, valid, progress, generator);
                items = GenerateGroup(group); break;
            }
            if (items.empty() && !useWorking)
            {
                profile["runtime"]["signature"] = signature;
                profile["runtime"]["error"] = "No usable outfit group for current conditions"; return;
            }
            if (state == "combat" && runtime.value("state", "") != "combat" && runtime.contains("items"))
            {
                auto resume = runtime; resume.erase("resume"); runtime["resume"] = std::move(resume);
            }
            runtime["state"] = state; runtime["group"] = group; runtime["useWorking"] = useWorking;
            if (!useWorking) runtime["progress"][state]["last"] = group;
        }
        // 恢复或重新加载工作搭配规则时，读取当前成员而不是旧运行快照。
        if (runtime.value("useWorking", false)) items = profile.value("workingItems", std::vector<std::string>{});
        runtime["signature"] = signature; runtime["items"] = items;
        const auto generation = actorGeneration_[actor.key]; autoBusy_.insert(actor.key);
        const auto mode = ParseActualEquipmentMode(profile.value("actualEquipmentMode", "off"));
        SosBackend::Get().RequestFor(actor.key, std::move(items), false, mode, true, false,
            [this, key = actor.key, generation, runtime = std::move(runtime)](bool success) mutable
            {
                std::lock_guard lock(autoMutex_);
                autoCompletions_.push_back({key, generation, success, std::move(runtime)});
            }, LayersFor(actor.key));
    }

    void OnGameMessage(std::uint32_t type)
    {
        // 缺失依赖时不恢复设置或执行读档后的装备清理。
        if (!SosBackend::Get().IsInstalled()) return;
        if (type == SKSE::MessagingInterface::kPreLoadGame || type == SKSE::MessagingInterface::kNewGame)
        {
            SosBackend::Get().Reset();
            Service::Get().QueueSessionReset(type == SKSE::MessagingInterface::kNewGame);
        }
        if (type == SKSE::MessagingInterface::kPostLoadGame)
        {
            // 当前设置为关闭时，读档后也回收该存档中记录的临时物品。
            Service::Get().QueueSessionReset(true);
        }
    }
}
