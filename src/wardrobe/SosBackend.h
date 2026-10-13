#pragma once
#include "wardrobe/ActualEquipmentPlan.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

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
        std::string Status() const override;
        void Reset();
        void Finish(std::uint64_t epoch, std::string message);
        bool Current(std::uint64_t epoch) const;
        void SetEquipmentMode(ActualEquipmentMode mode);
        ActualEquipmentMode EquipmentMode() const { return equipmentMode_.load(); }
    private:
        void StartNext();
        mutable std::mutex mutex_;
        std::vector<std::string> pending_;
        bool pendingClear_{}, hasPending_{}, busy_{};
        std::uint64_t epoch_{};
        std::string status_ = "SOS idle";
        std::atomic<ActualEquipmentMode> equipmentMode_{ActualEquipmentMode::Off};
        std::atomic<bool> installed_{false};
    };
}
