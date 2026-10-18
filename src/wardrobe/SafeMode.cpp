#include "wardrobe/SafeMode.h"
#include <Windows.h>
#include <algorithm>
#include <fstream>

namespace Wardrobe
{
    SafeMode& SafeMode::Get() { static auto* state = new SafeMode; return *state; }

    bool SafeMode::IsTrigger(const std::filesystem::path& path)
    {
        auto name = path.stem().wstring();
        std::transform(name.begin(), name.end(), name.begin(), [](wchar_t character)
        {
            return character >= L'a' && character <= L'z' ? character - L'a' + L'A' : character;
        });
        return name.find(L"SAFEMODE") != std::wstring::npos;
    }

    void SafeMode::Enter(std::string reason)
    {
        reason_ = std::move(reason);
        active_.store(true);
    }

    bool SafeMode::WriteMarker()
    {
        const auto path = directory_ / "Session.running";
        const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        constexpr char contents[] = "SOSBackend risk window v1\n";
        DWORD written = 0;
        const bool success = WriteFile(file, contents, sizeof(contents) - 1, &written, nullptr) &&
            written == sizeof(contents) - 1 && FlushFileBuffers(file);
        CloseHandle(file);
        return success;
    }

    bool SafeMode::RecordBackendCall()
    {
        std::lock_guard lock(windowMutex_);
        if (Active()) return false;
        if (!AutomaticRecovery()) return true;
        // 调用前落盘，覆盖 native 调用本身发生 CTD 的情况；连续调用延长窗口。
        if (!WriteMarker()) return false;
        windowDeadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        windowChanged_.notify_all();
        return true;
    }

    void SafeMode::Initialize(const std::filesystem::path& directory)
    {
        if (windowWorker_.joinable())
        {
            windowWorker_.request_stop(); windowChanged_.notify_all(); windowWorker_.join();
        }
        windowDeadline_ = {};
        directory_ = directory;
        error_.clear(); reason_.clear(); active_.store(false);
        bool requested = false;
        automaticRecovery_ = true;
        std::ifstream settings(directory.parent_path() / "SkyWardrobe.ini");
        std::string line;
        while (std::getline(settings, line))
        {
            line = line.substr(0, line.find_first_of(";#"));
            const auto separator = line.find('=');
            if (separator == std::string::npos) continue;
            const auto trim = [](std::string value)
            {
                const auto first = value.find_first_not_of(" \t\r");
                return first == std::string::npos ? std::string{} : value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
            };
            const auto key = trim(line.substr(0, separator));
            const auto value = trim(line.substr(separator + 1));
            if (key == "safe_mode") requested = value == "1";
            else if (key == "safe_mode_on_unclean_exit") automaticRecovery_ = value != "0";
        }
        try
        {
            std::filesystem::create_directories(directory_);
            std::ifstream previous(directory_ / "Session.running");
            std::string marker;
            std::getline(previous, marker);
            previous.close();
            if (automaticRecovery_ && marker == "SOSBackend risk window v1")
                Enter("Abnormal exit within 5s of SOSBackend call / SOSBackend 调用后 5 秒内异常退出");
            // 旧版的整会话标记不再作为进入条件，也不把上次窗口带入本次会话。
            std::filesystem::remove(directory_ / "Session.running");
            if (requested) Enter("INI: safe_mode=1");
            for (const auto& entry : std::filesystem::directory_iterator(directory_))
                if (entry.is_regular_file() && IsTrigger(entry.path()))
                    Enter("SAFEMODE file / SAFEMODE 文件");
            windowWorker_ = std::jthread([this](std::stop_token stop)
            {
                std::unique_lock lock(windowMutex_);
                while (!stop.stop_requested())
                {
                    if (windowDeadline_ == std::chrono::steady_clock::time_point{})
                        windowChanged_.wait(lock, [&] { return stop.stop_requested() || windowDeadline_ != std::chrono::steady_clock::time_point{}; });
                    else
                    {
                        const auto deadline = windowDeadline_;
                        if (!windowChanged_.wait_until(lock, deadline, [&] { return stop.stop_requested() || windowDeadline_ != deadline; }))
                        {
                            // 独立线程到期解除保护，不依赖游戏帧、菜单或异步回调继续运行。
                            std::error_code error;
                            std::filesystem::remove(directory_ / "Session.running", error);
                            if (error) windowChanged_.wait_for(lock, std::chrono::milliseconds(100));
                            else windowDeadline_ = {};
                        }
                    }
                }
            });
        }
        catch (const std::exception& exception)
        {
            error_ = exception.what();
            Enter("Safety initialization failed / 安全状态初始化失败");
        }
    }

    bool SafeMode::RemoveTriggers()
    {
        error_.clear();
        try
        {
            for (const auto& entry : std::filesystem::directory_iterator(directory_))
                if (entry.is_regular_file() && IsTrigger(entry.path())) std::filesystem::remove(entry.path());
            // MO2 的覆盖层可能在删除后重新暴露同名文件，必须重新检查。
            for (const auto& entry : std::filesystem::directory_iterator(directory_))
                if (entry.is_regular_file() && IsTrigger(entry.path()))
                {
                    error_ = "SAFEMODE file is still visible: " + entry.path().string();
                    return false;
                }
            return true;
        }
        catch (const std::exception& exception) { error_ = exception.what(); return false; }
    }

    bool SafeMode::Leave()
    {
        if (!RemoveTriggers()) return false;
        active_.store(false); reason_.clear();
        return true;
    }

    void SafeMode::NormalExit()
    {
        std::lock_guard lock(windowMutex_);
        windowDeadline_ = {};
        std::error_code error;
        std::filesystem::remove(directory_ / "Session.running", error);
        windowChanged_.notify_all();
    }
}
