#include "wardrobe/SafeMode.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <Windows.h>

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }
}

int main()
{
    const auto root = std::filesystem::temp_directory_path() /
        ("SkyWardrobeSafetyTests-" + std::to_string(GetCurrentProcessId()));
    const auto directory = root / "SkyWardrobe";
    try
    {
        std::filesystem::create_directories(directory);
        auto& safety = Wardrobe::SafeMode::Get();
        Require(safety.IsTrigger("enableSaFeMoDe.flag"), "case insensitive substring");
        Require(safety.IsTrigger("SAFEMODE"), "extension is optional");
        Require(safety.IsTrigger("safeMode.backup.txt"), "substring before last extension");
        Require(!safety.IsTrigger("normal.SAFEMODE"), "ignore extension");
        Require(!safety.IsTrigger("Session.running"), "session marker is not a trigger");
        safety.Initialize(directory);
        Require(!safety.Active(), "first session is normal");
        Require(safety.AutomaticRecovery(), "automatic recovery defaults to on");
        safety.SetAutomaticRecovery(false);
        Require(!safety.AutomaticRecovery() && !safety.Active(), "changing option does not enter safety");
        Require(!std::filesystem::exists(directory / "Session.running"), "idle session has no risk marker");
        safety.Initialize(directory);
        Require(!safety.Active(), "unclean session without backend call does not enter safety");
        std::ofstream(directory / "Session.running") << "SkyWardrobe session has not exited normally.\n";
        safety.Initialize(directory);
        Require(!safety.Active(), "legacy whole-session marker does not trigger safety");
        Require(safety.RecordBackendCall(), "backend call arms protection");
        Require(std::filesystem::exists(directory / "Session.running"), "backend risk marker created");
        safety.Initialize(directory);
        Require(safety.Active(), "interrupted backend window enters safety");
        Require(safety.Leave(), "can leave automatically entered safety");
        Require(!std::filesystem::exists(directory / "Session.running"), "leaving does not arm a backend window");
        Require(safety.RecordBackendCall(), "new backend call arms another window");
        std::this_thread::sleep_for(std::chrono::seconds(3));
        Require(safety.RecordBackendCall(), "subsequent backend call extends window");
        std::this_thread::sleep_for(std::chrono::seconds(3));
        Require(std::filesystem::exists(directory / "Session.running"), "window remains after latest call");
        std::this_thread::sleep_for(std::chrono::milliseconds(2300));
        Require(!std::filesystem::exists(directory / "Session.running"), "five second window expires without game frames");
        safety.Initialize(directory);
        Require(!safety.Active(), "crash outside window does not enter safety");
        Require(safety.RecordBackendCall(), "arm before normal exit");
        safety.NormalExit();
        safety.Initialize(directory);
        Require(!safety.Active(), "normal exit does not trigger safety");
        safety.NormalExit();
        std::ofstream(directory / "pleaseSafeMode.txt") << "";
        std::ofstream(directory / "SAFEMODE") << "";
        std::ofstream(directory / "normal.txt") << "preserve";
        std::filesystem::create_directory(directory / "SAFEMODE-folder");
        std::filesystem::create_directory(directory / "nested");
        std::ofstream(directory / "nested" / "SAFEMODE.txt") << "preserve";
        safety.Initialize(directory);
        Require(safety.Active(), "file requests safety");
        Require(safety.Leave(), "all trigger files can be removed");
        Require(!std::filesystem::exists(directory / "pleaseSafeMode.txt") &&
            !std::filesystem::exists(directory / "SAFEMODE"), "all matching files removed");
        Require(std::filesystem::exists(directory / "normal.txt") &&
            std::filesystem::exists(directory / "SAFEMODE-folder") &&
            std::filesystem::exists(directory / "nested" / "SAFEMODE.txt"), "unrelated and nested files preserved");
        std::ofstream(root / "SkyWardrobe.ini") << "safe_mode=1\nsafe_mode_on_unclean_exit=0\n";
        safety.Initialize(directory);
        Require(safety.Active(), "INI request works with detection off");
        Require(safety.Leave(), "INI can be overridden for this session");
        safety.Initialize(directory);
        Require(safety.Active(), "INI request persists across launches");
        std::ofstream(root / "SkyWardrobe.ini") << "safe_mode=0\nsafe_mode_on_unclean_exit=0\n";
        safety.Initialize(directory);
        Require(!safety.Active(), "automatic detection can be disabled");
        Require(!safety.AutomaticRecovery(), "disabled option is loaded from INI");
        Require(safety.RecordBackendCall() && !std::filesystem::exists(directory / "Session.running"), "disabled detection does not arm a window");
        safety.Enter("Manual");
        Require(safety.Active(), "manual entry");
        const auto trigger = directory / "lockedSafeMode.txt";
        std::ofstream(trigger) << "";
        const auto locked = CreateFileW(trigger.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Require(locked != INVALID_HANDLE_VALUE, "lock trigger fixture");
        const bool left = safety.Leave();
        CloseHandle(locked);
        Require(!left && safety.Active() && !safety.Error().empty(), "failed deletion keeps safety active");
        Require(safety.Leave(), "retry after unlocking");
        safety.NormalExit();
        std::filesystem::remove_all(root);
        std::cout << "Safe mode checks passed\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        std::filesystem::remove_all(root);
        return 1;
    }
}
