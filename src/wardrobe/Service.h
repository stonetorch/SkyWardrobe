#pragma once
#include "wardrobe/Store.h"
#include <mutex>
#include <future>

namespace Wardrobe
{
    struct ClothingEntry
    {
        std::string key, plugin, originalName;
        std::uint32_t slots{};
        Json metadata = Json::object();
        std::string displayName;
        int part{};
        std::vector<std::pair<std::string, bool>> searchKeys;
    };

    class Service
    {
    public:
        static Service& Get();
        ~Service();
        void PollSave();
        void Initialize();
        void Reload();
        void Apply(bool clear = false);
        void Add(std::string key);
        void Remove(std::string_view key);
        void SelectPreset(const std::string& id);
        void OverwritePreset();
        void UndoOverwrite();
        bool Save();
        int Score(const ClothingEntry& entry, std::string_view query) const;
        int Part(const ClothingEntry& entry) const;
        std::string Name(const ClothingEntry& entry) const;
        bool InWorking(std::string_view key) const;
        bool Dirty() const;
        const ClothingEntry* Find(std::string_view key) const;
        MetadataStore store{ "Data/SKSE/Plugins/SkyWardrobe" };
        std::vector<ClothingEntry> catalog;
        std::map<std::string, std::size_t> plugins;
        std::vector<std::string> working;
        std::string activePreset;
        std::string status;
        bool livePreview = false;
        std::uint64_t revision{};
    private:
        void RebuildIndex();
        void SaveWorking();
        void FlushWorkingSave();
        std::future<std::string> pendingSave_;
        bool workingSaveQueued_ = false;
        Json undo_;
        std::string undoPreset_;
    };
    void InstallConsole();
    void OnGameMessage(std::uint32_t type);
}
