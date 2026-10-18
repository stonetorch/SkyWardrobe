#pragma once
#include <atomic>
#include <filesystem>
#include <string>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace Wardrobe
{
    class SafeMode
    {
    public:
        static SafeMode& Get();
        void Initialize(const std::filesystem::path& directory = "Data/SKSE/Plugins/SkyWardrobe");
        bool Active() const { return active_.load(); }
        bool AutomaticRecovery() const { return automaticRecovery_; }
        void SetAutomaticRecovery(bool enabled) { automaticRecovery_ = enabled; }
        void Enter(std::string reason);
        bool RemoveTriggers();
        bool Leave();
        void NormalExit();
        bool RecordBackendCall();
        const std::string& Reason() const { return reason_; }
        const std::string& Error() const { return error_; }
        static bool IsTrigger(const std::filesystem::path& path);
    private:
        bool WriteMarker();
        std::atomic<bool> active_{false};
        std::atomic<bool> automaticRecovery_{true};
        std::mutex windowMutex_;
        std::condition_variable windowChanged_;
        std::jthread windowWorker_;
        std::chrono::steady_clock::time_point windowDeadline_{};
        std::filesystem::path directory_;
        std::string reason_, error_;
    };
}
