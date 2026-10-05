#pragma once

#include <filesystem>
#include <string_view>

namespace Wardrobe
{
    bool InitializePinyinSearch(const std::filesystem::path& dictionaryPath);
    void ClearPinyinSearchCache();
    void CachePinyinSearch(std::string_view value, bool explicitPinyin = false);
    int PinyinSearchScore(std::string_view normalizedQuery, std::string_view value, bool explicitPinyin = false);
}
