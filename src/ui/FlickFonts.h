#pragma once

#include <filesystem>
#include <string>

namespace FlickFonts
{
    struct Result
    {
        bool iconAvailable{};
        bool typefaceChanged{};
        std::string typeface;
        std::string error;
    };

    // 仅替换缺省字体，显式选择的字体和其他 FLICK 样式配置原样保留。
    std::string ConfigureDefaultTypeface(const std::string& contents, const std::string& fontPath);
    Result Prepare(const std::filesystem::path& dataRoot, const std::filesystem::path& defaultTypeface);
    void PrepareForGame();
    bool IconsAvailable();
}
