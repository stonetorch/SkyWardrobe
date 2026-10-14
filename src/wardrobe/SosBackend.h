#pragma once
#include "wardrobe/ActualEquipmentPlan.h"
#include "wardrobe/MultiLayer.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include <deque>
#include <functional>
#include <map>

namespace Wardrobe
{
    class IAppearanceBackend
    {
    public:
        virtual ~IAppearanceBackend() = default;
        virtual void Request(std::vector<std::string> keys, bool clear) = 0;
        virtual std::string Status() const = 0;
    };

    class SosBackend final : public IAppearanceBackend
    {
    public:
        static SosBackend& Get();
        void DetectInstallation();
        bool IsInstalled() const { return installed_.load(); }
        void Request(std::vector<std::string> keys, bool clear) override;
        void RequestFor(std::string actor, std::vector<std::string> keys, bool clear,
            ActualEquipmentMode mode, bool automatic = false, bool releaseOnly = false,
            std::function<void(bool)> completion = {}, LayerSettings layers = {});
        void RefreshLayers(const std::string& actor, const std::vector<LayerItem>& equipment);
        void TrackLayers(const std::string& actor, std::vector<std::string> keys, LayerSettings layers,
            ActualEquipmentMode mode, bool automatic, std::vector<LayerItem> equipment);
        void ReleaseActor(std::string actor, bool clear = false);
        void CleanupEquipment(std::string actor);
        void CancelActor(const std::string& actor);
        std::string Status() const override;
        void Reset();
        void Finish(std::uint64_t epoch, std::string message, bool success = false);
        bool Current(std::uint64_t epoch) const;
        bool CurrentActor(std::uint64_t epoch, const std::string& actor, std::uint64_t version) const;
        void SetEquipmentMode(ActualEquipmentMode mode);
        ActualEquipmentMode EquipmentMode() const { return equipmentMode_.load(); }
    private:
        void StartNext();
        mutable std::mutex mutex_;
        struct RequestData
        {
            std::string actor;
            std::vector<std::string> keys;
            bool clear{}, automatic{}, releaseOnly{};
            ActualEquipmentMode mode{ActualEquipmentMode::Off};
            std::function<void(bool)> completion;
            std::uint64_t version{};
            LayerSettings layers;
        };
        std::deque<RequestData> pending_;
        struct ActiveLayers { RequestData request; std::vector<LayerItem> equipment; };
        std::map<std::string, ActiveLayers> activeLayers_;
        std::function<void(bool)> completion_;
        std::map<std::string, std::uint64_t> actorVersions_;
        bool busy_{};
        std::uint64_t epoch_{};
        std::string status_ = "SOS idle";
        std::atomic<ActualEquipmentMode> equipmentMode_{ActualEquipmentMode::Off};
        std::atomic<bool> installed_{false};
    };
}
