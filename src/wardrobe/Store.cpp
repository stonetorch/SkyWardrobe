#include "wardrobe/Store.h"
#include <Windows.h>
#include <fstream>

namespace Wardrobe
{
    bool MetadataStore::Load()
    {
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

    bool MetadataStore::Save()
    {
        if (!writable) return false;
        try
        {
            ValidateDatabase(database);
            std::filesystem::create_directories(root_ / "AliasPacks");
            const auto path = root_ / "Wardrobe.json", temporary = root_ / "Wardrobe.json.tmp";
            {
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                output << database.dump(2, ' ', false) << '\n';
                output.flush();
                output.close();
                if (!output) throw std::runtime_error("Cannot write wardrobe temporary file");
            }
            if (std::filesystem::exists(path))
                std::filesystem::copy_file(path, root_ / "Wardrobe.json.bak", std::filesystem::copy_options::overwrite_existing);
            if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("Cannot replace Wardrobe.json: " + std::to_string(GetLastError()));
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
