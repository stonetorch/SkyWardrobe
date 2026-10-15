#pragma once
#include "wardrobe/Store.h"
#include <mutex>
#include <future>
#include "wardrobe/ActorRuntime.h"
#include <set>
#include <atomic>

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
        void Apply(bool clear = false, bool persist = true);
        ActualEquipmentMode EquipmentMode() const;
        void SetEquipmentMode(ActualEquipmentMode mode);
        void Add(std::string key);
        void Remove(std::string_view key);
        void AddBackup(const std::string& key);
        void RemoveBackup(const std::string& key);
        void MoveLayer(const std::string& key, bool toBackup);
        void UseBackupGroup(const std::string& id);
        void SetLayerOptions(bool backupEnabled, bool originalEnabled, std::uint32_t hiddenSlots);
        LayerSettings LayersFor(const std::string& actor) const;
        std::vector<LayerResult> LayerPreview() const;
        void BeginBatch();
        void EndBatch(bool preview);
        void SetLivePreview(bool enabled);
        void SelectActor(const std::string& key);
        void AddActor(const ActorInfo& actor);
        void RemoveActor(const std::string& key);
        void SetAutomatic(bool enabled);
        void ResumeAutomatic(bool reset = false);
        /**
         * @return 增强功能-多预设 是否启用
         */
        bool MultiplePresets() const { return store.database.value("multiplePresets", false); }
        void SetMultiplePresets(bool enabled);
        void ActivatePreset(const std::string& id);
        void TickActors();
        void ResetActorSession();
        void QueueSessionReset(bool ready);
        void EnterSafeMode();
        bool ExitSafeMode();
        Json& ActorProfile() { return store.database["actors"][currentActor]; }
        std::string currentActor = "player";
        ActorSnapshot actorSnapshot;
        const Json* Group(const std::string& id) const;
        bool InGroup(const ClothingEntry& entry, const std::string& id) const;
        void SetGroupItem(const std::string& id, const ClothingEntry& entry, bool included);
        void UseGroup(const std::string& id, bool apply = true);
        std::string CreateSubgroup(const std::string& parent);
        void SelectPreset(const std::string& id);
        void OverwritePreset();
        void UndoOverwrite();
        bool Save();
        bool GenerateAliasTemplate(const std::string& plugin, bool overwrite = false);
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
        bool livePreview = true;
        std::uint64_t revision{};
    private:
        void RebuildIndex();
        void SaveWorking();
        void FlushSave();
        void StoreActorContext();
        void MarkManual();
        std::vector<std::string> GenerateGroup(const std::string& id) const;
        void EvaluateAutomatic(const ActorInfo& actor);
        std::map<std::string, AutomaticStateFilter> automaticStateFilters_;
        bool batching_{};
        std::atomic<bool> sessionReset_{false}, worldReady_{true};
        std::chrono::steady_clock::time_point nextActorPoll_{};
        std::set<std::string> autoBusy_, unavailableActors_, equipmentCleanupPending_;
        std::map<std::string, std::uint64_t> actorGeneration_;
        struct AutoCompletion
        {
            std::string actor;
            std::uint64_t generation{};
            bool success{};
            Json runtime;
            bool release{};
            bool takeover{};
        };
        std::mutex autoMutex_;
        std::vector<AutoCompletion> autoCompletions_;
        std::future<std::string> pendingSave_;
        bool saveQueued_ = false;
        Json indexedCategories_;
        Json indexedPlugins_;
        Json undo_;
        Json undoGroup_;
        std::string undoGroupID_;
        std::vector<std::string> presetBaseline_;
        std::string undoPreset_;
    };
    void InstallConsole();
    void OnGameMessage(std::uint32_t type);
}
