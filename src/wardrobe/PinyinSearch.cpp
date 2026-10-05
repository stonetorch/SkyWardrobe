#include "wardrobe/PinyinSearch.h"
#include <cpp-pinyin/Pinyin.h>
#include <cpp-pinyin/G2pglobal.h>
#include <algorithm>
#include <cctype>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace Wardrobe
{
    namespace
    {
        struct SearchIndex
        {
            std::vector<std::string> syllables;
            std::string compact;
            std::string initials;
        };

        std::unique_ptr<Pinyin::Pinyin> converter;
        std::unordered_map<std::string, SearchIndex> automaticCache, explicitCache;

        bool IsLetters(std::string_view value)
        {
            return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c)
            {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
            });
        }

        const SearchIndex& Index(std::string_view value, bool explicitPinyin)
        {
            auto& cache = explicitPinyin ? explicitCache : automaticCache;
            const auto [it, inserted] = cache.try_emplace(std::string(value));
            if (!inserted) return it->second;
            auto& index = it->second;
            if (explicitPinyin && IsLetters(value))
                index.syllables.emplace_back(value);
            else if (explicitPinyin && std::all_of(value.begin(), value.end(), [](unsigned char c) { return c < 128; }))
            {
                std::istringstream stream{std::string(value)};
                for (std::string syllable; stream >> syllable;)
                    index.syllables.push_back(std::move(syllable));
            }
            else if (converter && std::any_of(value.begin(), value.end(), [](unsigned char c) { return c >= 128; }))
            {
                // 保留未转换字符作为边界，避免跨标点或未知汉字拼接出虚假的读音。
                for (const auto& result : converter->hanziToPinyin(std::string(value), Pinyin::ManTone::Style::NORMAL, Pinyin::Error::Default, false))
                    index.syllables.push_back(result.pinyin);
            }
            for (auto& syllable : index.syllables)
            {
                for (auto& c : syllable) if (static_cast<unsigned char>(c) < 128) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                index.compact += syllable;
                index.initials += IsLetters(syllable) ? syllable.substr(0, 1) : syllable;
            }
            return index;
        }

        bool MatchPrefixes(std::string_view query, const SearchIndex& index)
        {
            std::vector<unsigned char> previous(query.size() + 1), next(query.size() + 1);
            // 每个音节可消费非空前缀；从任意音节开始，但中途不能跳字或跳音节。
            // 两行 DP 避免枚举所有缩写组合，q + zi 能命中 qun + zi。
            for (const auto& syllable : index.syllables)
            {
                std::fill(next.begin(), next.end(), 0);
                previous[0] = 1;
                if (IsLetters(syllable))
                    for (std::size_t offset = 0; offset < query.size(); ++offset)
                        if (previous[offset])
                            for (std::size_t length = 1; length <= syllable.size() && offset + length <= query.size(); ++length)
                            {
                                if (query[offset + length - 1] != syllable[length - 1]) break;
                                next[offset + length] = 1;
                            }
                if (next.back()) return true;
                previous.swap(next);
            }
            return false;
        }
    }

    bool InitializePinyinSearch(const std::filesystem::path& dictionaryPath)
    {
        converter.reset();
        ClearPinyinSearchCache();
        Pinyin::setDictionaryPath(dictionaryPath);
        try
        {
            auto candidate = std::make_unique<Pinyin::Pinyin>();
            if (!candidate->initialized()) return false;
            converter = std::move(candidate);
            return true;
        }
        catch (const std::exception&)
        {
            // 字典缺失或损坏时保留原文及手工搜索词，不阻止衣柜加载。
            return false;
        }
    }

    void ClearPinyinSearchCache()
    {
        automaticCache.clear();
        explicitCache.clear();
    }

    void CachePinyinSearch(std::string_view value, bool explicitPinyin)
    {
        Index(value, explicitPinyin);
    }

    int PinyinSearchScore(std::string_view query, std::string_view value, bool explicitPinyin)
    {
        if (!IsLetters(query)) return 0;
        const auto& index = Index(value, explicitPinyin);
        if (index.compact.empty()) return 0;
        if (index.compact == query) return 65;
        if (index.compact.starts_with(query)) return 60;
        if (index.compact.find(query) != std::string::npos) return 55;
        if (index.initials.find(query) != std::string::npos) return 50;
        return MatchPrefixes(query, index) ? 45 : 0;
    }
}
