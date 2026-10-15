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
            error_ = "Cannot create session marker: " + path.string();
            return false;
        }
        constexpr char contents[] = "SkyWardrobe session has not exited normally.\n";
        DWORD written = 0;
        const bool success = WriteFile(file, contents, sizeof(contents) - 1, &written, nullptr) &&
            written == sizeof(contents) - 1 && FlushFileBuffers(file);
        CloseHandle(file);
        if (!success) error_ = "Cannot flush session marker: " + path.string();
        return success;
    }

    void SafeMode::Initialize(const std::filesystem::path& directory)
    {
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
            if (automaticRecovery_ && std::filesystem::exists(directory_ / "Session.running"))
                Enter("Previous session did not exit normally / 上次运行未正常退出");
            if (requested) Enter("INI: safe_mode=1");
            for (const auto& entry : std::filesystem::directory_iterator(directory_))
                if (entry.is_regular_file() && IsTrigger(entry.path()))
                    Enter("SAFEMODE file / SAFEMODE 文件");
            // 标记在任何外观操作前同步落盘；退出安全模式不清除它。
            if (!WriteMarker()) Enter("Session marker unavailable / 无法记录运行状态");
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
        if (!RemoveTriggers() || !WriteMarker()) return false;
        active_.store(false); reason_.clear();
        return true;
    }

    void SafeMode::NormalExit()
    {
        std::error_code error;
        std::filesystem::remove(directory_ / "Session.running", error);
    }
}
