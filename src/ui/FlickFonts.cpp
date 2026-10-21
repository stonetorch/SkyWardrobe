#include "ui/FlickFonts.h"

#include <RE/Skyrim.h>
#include <Windows.h>
#include <SKSE/SKSE.h>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <vector>

namespace FlickFonts
{
    namespace
    {
        bool iconsAvailable = false;
        std::string Trim(std::string value)
        {
            const auto first = value.find_first_not_of(" \t\r");
            if (first == std::string::npos) return {};
            return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
        }
        std::string Lower(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return value;
        }
        std::string Read(const std::filesystem::path& path)
        {
            std::ifstream input(path, std::ios::binary);
            if (!input) throw std::runtime_error("Cannot read " + path.string());
            return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        }
        void Write(const std::filesystem::path& path, const std::string& contents)
        {
            std::filesystem::create_directories(path.parent_path());
            auto temporary = path;
            temporary += L".SkyWardrobe.tmp";
            {
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
                output.flush(); output.close();
                if (!output) throw std::runtime_error("Cannot write " + temporary.string());
            }
            if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("Cannot replace " + path.string() + ": " + std::to_string(GetLastError()));
        }
        bool FindIcons(const std::filesystem::path& root)
        {
            if (!std::filesystem::is_directory(root)) return false;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
                if (entry.is_regular_file() && Lower(entry.path().filename().string()) == "fa-solid-900.ttf") return true;
            return false;
        }
    }

    std::string ConfigureDefaultTypeface(const std::string& contents, const std::string& fontPath)
    {
        const std::string newline = contents.find("\r\n") != std::string::npos ? "\r\n" : "\n";
        bool inStyle = false;
        std::optional<std::size_t> styleEnd;
        for (std::size_t start = 0; start < contents.size();)
        {
            const auto end = contents.find('\n', start);
            auto line = Trim(contents.substr(start, end == std::string::npos ? contents.size() - start : end - start));
            if (start == 0 && line.starts_with("\xef\xbb\xbf")) line.erase(0, 3);
            if (!line.empty() && line.front() == '[' && line.back() == ']')
            {
                if (inStyle) styleEnd = start;
                inStyle = Lower(Trim(line.substr(1, line.size() - 2))) == "style";
                if (inStyle) styleEnd = contents.size();
            }
            else if (inStyle && !line.empty() && line.front() != ';' && line.front() != '#')
            {
                const auto equal = line.find('=');
                if (equal != std::string::npos && Lower(Trim(line.substr(0, equal))) == "sfont")
                {
                    const auto value = Trim(line.substr(equal + 1));
                    if (!value.empty() && Lower(value) != "default") return contents;
                    auto result = contents;
                    result.replace(start, (end == std::string::npos ? contents.size() : end) - start,
                        "sFont=" + fontPath + (end != std::string::npos && newline == "\r\n" ? "\r" : ""));
                    return result;
                }
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
        auto result = contents;
        if (styleEnd)
        {
            const auto at = *styleEnd;
            const auto prefix = at > 0 && result[at - 1] != '\n' ? newline : "";
            result.insert(at, prefix + "sFont=" + fontPath + newline);
        }
        else
        {
            if (!result.empty() && result.back() != '\n') result += newline;
            result += "[Style]" + newline + "sFont=" + fontPath + newline;
        }
        return result;
    }

    Result Prepare(const std::filesystem::path& dataRoot, const std::filesystem::path& defaultTypeface)
    {
        Result result;
        try
        {
            // FLICK 只搜索这三个字体目录；插件自己的 Fonts/ 不在其中。
            const auto fonts = dataRoot / "FUCKs/FUCK/fonts";
            result.iconAvailable = FindIcons(fonts) || FindIcons(dataRoot / "Interface/ImGuiIcons/Fonts") ||
                FindIcons(dataRoot / "Interface/CommunityShaders/Fonts");
            if (!result.iconAvailable)
            {
                const auto packaged = dataRoot / "SKSE/Plugins/SkyWardrobe/Fonts/fa-solid-900.ttf";
                if (!std::filesystem::is_regular_file(packaged)) throw std::runtime_error("Packaged Font Awesome font is missing");
                std::filesystem::create_directories(fonts);
                std::filesystem::copy_file(packaged, fonts / "fa-solid-900.ttf", std::filesystem::copy_options::none);
                result.iconAvailable = true;
            }
            const auto settings = dataRoot / "FUCKs/FUCK/defaultstyle.ini";
            const auto original = std::filesystem::exists(settings) ? Read(settings) : "";
            const auto updated = ConfigureDefaultTypeface(original, defaultTypeface.generic_string());
            if (updated != original)
            {
                if (!std::filesystem::is_regular_file(defaultTypeface)) throw std::runtime_error("Chinese typeface is missing: " + defaultTypeface.string());
                Write(settings, updated);
                result.typefaceChanged = true;
                result.typeface = defaultTypeface.generic_string();
            }
        }
        catch (const std::exception& error) { result.error = error.what(); }
        return result;
    }

    void PrepareForGame()
    {
        try
        {
            wchar_t windowsDirectory[MAX_PATH]{};
            if (!GetWindowsDirectoryW(windowsDirectory, MAX_PATH))
            {
                SKSE::log::error("Cannot locate Windows fonts");
                return;
            }
            std::filesystem::path typeface = std::filesystem::path(windowsDirectory) / "Fonts/msyh.ttc";
            const auto ini = std::filesystem::path("Data/SKSE/Plugins/SkyWardrobe.ini");
            if (std::filesystem::exists(ini))
            {
                std::ifstream input(ini);
                std::string line;
                while (std::getline(input, line))
                {
                    const auto equal = line.find('=');
                    if (equal != std::string::npos && Trim(line.substr(0, equal)) == "font_path")
                    {
                        const std::filesystem::path configured = Trim(line.substr(equal + 1));
                        if (std::filesystem::is_regular_file(configured)) typeface = configured;
                    }
                }
            }
            const auto result = Prepare("Data", typeface);
            iconsAvailable = result.iconAvailable;
            if (result.typefaceChanged) SKSE::log::info("FLICK default typeface initialized to {}", result.typeface);
            if (!result.error.empty()) SKSE::log::error("FLICK font preparation: {}", result.error);
            SKSE::log::info("FLICK Font Awesome resource available: {}", result.iconAvailable);
        }
        catch (const std::exception& error) { SKSE::log::error("FLICK font preparation: {}", error.what()); }
    }
    bool IconsAvailable() { return iconsAvailable; }
}
