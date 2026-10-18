#include "wardrobe/Store.h"
#include "wardrobe/PlayerOutfitStorage.h"
#include <mutex>
#include <Windows.h>
#include <fstream>

namespace Wardrobe
{
    namespace
    {
        // 后台 JSON 写入和游戏保存触发的清理共用锁，避免旧快照重新写回玩家数据。
        std::mutex persistenceMutex;

        void WriteDatabase(const std::filesystem::path& root, const Json& database)
        {
            std::filesystem::create_directories(root / "AliasPacks");
            const auto path = root / "Wardrobe.json", temporary = root / "Wardrobe.json.tmp";
            {
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                output << database.dump(2, ' ', false) << '\n';
                output.flush(); output.close();
                if (!output) throw std::runtime_error("Cannot write wardrobe temporary file");
            }
            if (std::filesystem::exists(path))
                std::filesystem::copy_file(path, root / "Wardrobe.json.bak", std::filesystem::copy_options::overwrite_existing);
            if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("Cannot replace Wardrobe.json: " + std::to_string(GetLastError()));
        }
    }

    void MetadataStore::SetPlayerStorageMode(PlayerOutfitStorage mode)
    {
        std::lock_guard lock(persistenceMutex);
        playerOutfitStorage.store(mode);
    }

    std::string MetadataStore::CleanPlayerOutfit() const
    {
        std::lock_guard lock(persistenceMutex);
        if (playerOutfitStorage.load() != PlayerOutfitStorage::SaveLocal) return {};
        try
        {
            const auto path = root_ / "Wardrobe.json";
            if (!std::filesystem::exists(path)) return {};
            std::ifstream input(path, std::ios::binary);
            if (!input) throw std::runtime_error("Cannot read Wardrobe.json for player cleanup");
            Json snapshot; input >> snapshot;
            input.close();
            if (!snapshot.is_object()) throw std::runtime_error("Invalid Wardrobe.json for player cleanup");
            // 没有遗留数据时不写盘；不规范化或改写其他角色及共享资源。
            if (StripPlayerOutfit(snapshot)) WriteDatabase(root_, snapshot);
            return {};
        }
        catch (const std::exception& e) { return e.what(); }
    }

    bool MetadataStore::Load()
    {
        std::lock_guard lock(persistenceMutex);
        try
        {
            Json candidate = EmptyDatabase();
            const auto path = root_ / "Wardrobe.json";
            if (std::filesystem::exists(path))
            {
                std::ifstream input(path, std::ios::binary);
                if (!input) throw std::runtime_error("Cannot read Wardrobe.json");
                input >> candidate;
            }
            if (playerOutfitStorage.load() == PlayerOutfitStorage::SaveLocal) StripPlayerOutfit(candidate);
            ValidateDatabase(candidate);
            std::map<std::string, Json> packs;
            std::vector<std::string> packWarnings;
            const auto directory = root_ / "AliasPacks";
            std::vector<std::filesystem::path> paths;
            if (std::filesystem::exists(directory))
                for (const auto& entry : std::filesystem::directory_iterator(directory))
                    if (entry.is_regular_file() && entry.path().extension() == ".json") paths.push_back(entry.path());
            std::sort(paths.begin(), paths.end());
            for (const auto& file : paths)
            {
                try
                {
                    std::ifstream input(file, std::ios::binary);
                    Json pack; input >> pack;
                    const auto plugin = pack.at("plugin").get<std::string>();
                    if (plugin.empty() || plugin.find('|') != std::string::npos) throw std::runtime_error("Invalid plugin");
                    if (!pack.at("items").is_object()) throw std::runtime_error("items must be an object");
                    std::map<std::string, Json> validated;
                    for (const auto& [local, value] : pack["items"].items())
                    {
                        const auto key = FormKey::Parse(plugin + "|" + local);
                        if (!key) throw std::runtime_error("Invalid local FormID: " + local);
                        Json check = EmptyDatabase(); check["items"][key->String()] = value; ValidateDatabase(check);
                        Json metadata = value;
                        if (metadata.contains("name")) metadata["alias"] = metadata["name"];
                        validated[key->String()] = std::move(metadata);
                    }
                    for (auto& [key, value] : validated) packs[key] = std::move(value);
                }
                catch (const std::exception& e) { packWarnings.push_back(file.filename().string() + ": " + e.what()); }
            }
            // 完整解析成功后才替换内存，外部编辑错误不会清空现有用户数据。
            database = std::move(candidate);
            aliases_ = std::move(packs);
            warnings = std::move(packWarnings);
            writable = true;
            error.clear();
            return true;
        }
        catch (const std::exception& e)
        {
            writable = false;
            error = e.what();
            return false;
        }
    }

    bool MetadataStore::WriteAliasTemplate(const std::string& plugin, const Json& items, bool overwrite)
    {
        if (!writable) return false;
        try
        {
            // 插件名必须是文件名，防止模板路径越出别名包目录。
            if (plugin.empty() || plugin.find_first_of("<>:\"/\\|?*") != std::string::npos ||
                plugin.back() == '.' || plugin.back() == ' ')
                throw std::runtime_error("Invalid plugin filename");
            Json check = EmptyDatabase();
            for (const auto& [local, metadata] : items.items()) check["items"][plugin + "|" + local] = metadata;
            ValidateDatabase(check);
            const auto directory = root_ / "AliasPacks";
            std::filesystem::create_directories(directory);
            const auto path = directory / ("00_Generated." + plugin + ".json");
            // 重新注册不会覆盖用户已经编辑过的模板；只有手动重新生成才替换。
            if (!overwrite && std::filesystem::exists(path)) { error.clear(); return true; }
            auto temporary = path; temporary += ".tmp";
            {
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                output << Json{{"plugin", plugin}, {"items", items}}.dump(2, ' ', false) << '\n';
                output.flush();
                output.close();
                if (!output) throw std::runtime_error("Cannot write alias template");
            }
            if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("Cannot replace alias template: " + std::to_string(GetLastError()));
            error.clear();
            return true;
        }
        catch (const std::exception& e) { error = e.what(); return false; }
    }

    bool MetadataStore::Save()
    {
        std::lock_guard lock(persistenceMutex);
        if (!writable) return false;
        try
        {
            ValidateDatabase(database);
            auto snapshot = database;
            if (playerOutfitStorage.load() == PlayerOutfitStorage::SaveLocal) StripPlayerOutfit(snapshot);
            WriteDatabase(root_, snapshot);
            error.clear();
            return true;
        }
        catch (const std::exception& e) { error = e.what(); return false; }
    }

    std::string MetadataStore::SaveSnapshot(Json snapshot) const
    {
        // 后台只操作独立快照，不读写界面正在修改的数据库和错误状态。
        MetadataStore writer(root_);
        writer.database = std::move(snapshot);
        writer.Save();
        return writer.error;
    }

    Json MetadataStore::Effective(std::string_view key) const
    {
        const std::string id(key);
        Json result = Json::object();
        if (const auto found = aliases_.find(id); found != aliases_.end()) result = found->second;
        if (database["items"].contains(id))
            for (const auto& [field, value] : database["items"][id].items()) result[field] = value;
        return result;
    }
}
