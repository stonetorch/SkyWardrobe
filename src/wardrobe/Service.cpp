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
        store.database["workingItems"] = working;
        store.database["activePreset"] = activePreset;
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
        if (store.Load())
        {
            ClearPinyinSearchCache();
            indexedCategories_ = nullptr;
            indexedPlugins_ = nullptr;
            working = store.database["workingItems"].get<std::vector<std::string>>();
            activePreset = store.database["activePreset"].get<std::string>();
            presetBaseline_ = working;
            undo_ = nullptr;
            status = "JSON loaded";
            RebuildIndex();
            SosBackend::Get().SetEquipmentMode(EquipmentMode());
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
        SaveWorking();
        if (livePreview) Apply();
    }

    void Service::Remove(std::string_view key)
    {
        if (std::erase(working, std::string(key)) == 0) return;
        SaveWorking();
        if (livePreview) Apply();
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
        std::vector<std::pair<std::string, std::uint32_t>> candidates;
        // 引用仅用于导航，绝不递归收集引用组的服装。
        for (const auto& entry : catalog) if (InGroup(entry, id)) candidates.emplace_back(entry.key, entry.slots);
        static std::mt19937 generator(std::random_device{}());
        working = RandomOutfit(std::move(candidates), generator);
        activePreset.clear();
        ++revision;
        SaveWorking();
        if (apply) Apply();
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
        if (!store.database["presets"].contains(id)) return;
        const auto group = store.database["presets"][id].value("group", "");
        if (!Group(group)) { status = "Group not found: " + group; return; }
        UseGroup(group, false);
        presetBaseline_ = working;
        activePreset = id; SaveWorking();
        if (livePreview) Apply();
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

    void Service::Apply(bool clear)
    {
        SosBackend::Get().Request(clear ? std::vector<std::string>{} : working, clear);
    }

    ActualEquipmentMode Service::EquipmentMode() const
    {
        return ParseActualEquipmentMode(store.database.value("actualEquipmentMode", std::string("off")));
    }

    void Service::SetEquipmentMode(ActualEquipmentMode mode)
    {
        if (!store.writable) return;
        store.database["actualEquipmentMode"] = ActualEquipmentModeID(mode);
        SosBackend::Get().SetEquipmentMode(mode);
        SaveWorking();
    }

    void OnGameMessage(std::uint32_t type)
    {
        if (type == SKSE::MessagingInterface::kPreLoadGame || type == SKSE::MessagingInterface::kNewGame)
        {
            SosBackend::Get().Reset();
        }
        if (type == SKSE::MessagingInterface::kPostLoadGame)
        {
            // 当前设置为关闭时，读档后也回收该存档中记录的临时物品。
            SosBackend::Get().SetEquipmentMode(Service::Get().EquipmentMode());
        }
    }
}
