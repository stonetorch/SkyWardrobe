#pragma once
#include "wardrobe/Model.h"
#include <filesystem>
#include <map>

namespace Wardrobe
{
    class MetadataStore
    {
    public:
        explicit MetadataStore(std::filesystem::path root) : root_(std::move(root)) {}
        bool Load();
        bool Save();
        std::string SaveSnapshot(Json snapshot) const;
        Json Effective(std::string_view key) const;
        Json database = EmptyDatabase();
        std::string error;
        std::vector<std::string> warnings;
        bool writable = true;
    private:
        std::filesystem::path root_;
        std::map<std::string, Json> aliases_;
    };
}
