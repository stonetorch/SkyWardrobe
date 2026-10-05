#include "wardrobe/Service.h"
#include "wardrobe/SosBackend.h"
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

namespace Wardrobe
{
    Service& Service::Get() { static Service service; return service; }

    Service::~Service() { FlushWorkingSave(); }

    void Service::SaveWorking()
    {
        store.database["workingItems"] = working;
        store.database["activePreset"] = activePreset;
        if (!store.writable) { status = store.error; return; }
        // 穿脱不改变搜索元数据；连续操作只保留最新待写状态，避免全目录重建。
        workingSaveQueued_ = true;
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
        if (!workingSaveQueued_) return;
        try
        {
            // 同一时刻只有一个写入者，旧快照完成后才提交最新快照。
            pendingSave_ = std::async(std::launch::async, [this, snapshot = store.database]() mutable
            {
                return store.SaveSnapshot(std::move(snapshot));
            });
            workingSaveQueued_ = false;
            status = "Saving";
        }
        catch (const std::exception& e)
        {
            workingSaveQueued_ = false;
            store.error = e.what();
            status = store.error;
        }
    }

    void Service::FlushWorkingSave()
    {
        // 显式保存、重载和退出必须先排空后台写入，防止旧快照覆盖新数据。
        while (pendingSave_.valid() || workingSaveQueued_)
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
        FlushWorkingSave();
        if (store.Load())
        {
            working = store.database["workingItems"].get<std::vector<std::string>>();
            activePreset = store.database["activePreset"].get<std::string>();
            undo_ = nullptr;
            status = "JSON loaded";
            RebuildIndex();
        }
        else { status = store.error; RebuildIndex(); }
    }

    bool Service::Save()
    {
        FlushWorkingSave();
        store.database["workingItems"] = working;
        store.database["activePreset"] = activePreset;
        RebuildIndex();
        if (!store.Save()) { status = store.error; return false; }
        status = "Saved";
        return true;
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
        ClearPinyinSearchCache();
        ++revision;
        for (auto& entry : catalog)
        {
            entry.metadata = store.Effective(entry.key);
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
        // 侧栏与预设也提前建立索引，未被服装引用的分类同样支持自动拼音。
        for (const char* collection : {"categories", "pluginCategories", "presets"})
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
            store.database["presets"][activePreset].value("items", std::vector<std::string>{}) != working;
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

    void Service::SelectPreset(const std::string& id)
    {
        if (!store.database["presets"].contains(id)) return;
        working = store.database["presets"][id].value("items", std::vector<std::string>{});
        activePreset = id; SaveWorking();
        if (livePreview) Apply();
    }

    void Service::OverwritePreset()
    {
        if (activePreset.empty() || !store.database["presets"].contains(activePreset)) return;
        undoPreset_ = activePreset;
        undo_ = store.database["presets"][activePreset];
        store.database["presets"][activePreset]["items"] = working; Save();
    }

    void Service::UndoOverwrite()
    {
        if (!undo_.is_null()) { store.database["presets"][undoPreset_] = undo_; undo_ = nullptr; Save(); }
    }

    void Service::Apply(bool clear)
    {
        SosBackend::Get().Request(clear ? std::vector<std::string>{} : working, clear);
    }

    void OnGameMessage(std::uint32_t type)
    {
        if (type == SKSE::MessagingInterface::kPreLoadGame || type == SKSE::MessagingInterface::kNewGame)
        {
            SosBackend::Get().Reset();
        }
    }
}
