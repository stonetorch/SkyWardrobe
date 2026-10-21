#include "ui/FlickFonts.h"
#include <Windows.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{
    void Check(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }
    void Write(const std::filesystem::path& path, const std::string& value)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary);
        output << value;
    }
    std::string Read(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }
}

int main()
{
    const auto root = std::filesystem::temp_directory_path() / ("SkyWardrobe-FlickFonts-" + std::to_string(GetCurrentProcessId()));
    try
    {
        const std::string font = "C:/Windows/Fonts/msyh.ttc";
        const auto configure = [&](const std::string& input) { return FlickFonts::ConfigureDefaultTypeface(input, font); };
        Check(configure("") == "[Style]\nsFont=" + font + "\n", "Missing style not initialized");
        Check(configure("[Style]\nsFont=Default\nfScale=0.85\n") == "[Style]\nsFont=" + font + "\nfScale=0.85\n", "Default font not replaced narrowly");
        const std::string custom = "; User theme\r\n[Style]\r\nsFont=MyChineseFont.ttf\r\nfScale=0.85\r\n";
        Check(configure(custom) == custom, "Custom font or theme was overwritten");
        const std::string other = "[Style]\nfScale=0.85\n[Other]\nsFont=Unrelated\n";
        Check(configure(other) == "[Style]\nfScale=0.85\nsFont=" + font + "\n[Other]\nsFont=Unrelated\n", "Font inserted into wrong section");
        Check(configure("\xef\xbb\xbf[STYLE]\r\n sFont = Default\r\n") == "\xef\xbb\xbf[STYLE]\r\nsFont=" + font + "\r\n", "BOM or CRLF lost");
        Check(configure(configure(other)) == configure(other), "Configuration not idempotent");

        const auto typeface = root / "system/Chinese.ttc";
        Write(typeface, "test-font");
        const auto data = root / "Data";
        Write(data / "SKSE/Plugins/SkyWardrobe/Fonts/fa-solid-900.ttf", "packaged-icons");
        auto result = FlickFonts::Prepare(data, typeface);
        Check(result.error.empty() && result.iconAvailable && result.typefaceChanged, "First preparation failed");
        Check(Read(data / "FUCKs/FUCK/fonts/fa-solid-900.ttf") == "packaged-icons", "Icon font not in FLICK search path");
        Check(Read(data / "FUCKs/FUCK/defaultstyle.ini") == "[Style]\nsFont=" + typeface.generic_string() + "\n", "Typeface path not configured");
        result = FlickFonts::Prepare(data, typeface);
        Check(result.error.empty() && !result.typefaceChanged, "Repeated launch rewrote settings");
        Write(data / "FUCKs/FUCK/fonts/fa-solid-900.ttf", "user-icons");
        Write(data / "FUCKs/FUCK/defaultstyle.ini", custom);
        result = FlickFonts::Prepare(data, root / "missing.ttc");
        Check(result.error.empty() && Read(data / "FUCKs/FUCK/defaultstyle.ini") == custom, "Explicit font replaced by fallback");
        Check(Read(data / "FUCKs/FUCK/fonts/fa-solid-900.ttf") == "user-icons", "Existing icon resource overwritten");

        const auto missing = root / "MissingData";
        Write(missing / "FUCKs/FUCK/fonts/fa-solid-900.ttf", "icons");
        result = FlickFonts::Prepare(missing, root / "missing.ttc");
        Check(!result.error.empty() && !std::filesystem::exists(missing / "FUCKs/FUCK/defaultstyle.ini"), "Missing font produced a broken configuration");
        std::filesystem::remove_all(root);
        std::cout << "FLICK font resource, configuration preservation and startup checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::filesystem::remove_all(root);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
