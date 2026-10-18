#include "wardrobe/Store.h"
#include <Windows.h>
#include <fstream>
#include <iostream>

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }
    Wardrobe::Json Read(const std::filesystem::path& path)
    {
        std::ifstream input(path);
        Wardrobe::Json value; input >> value; return value;
    }
}

int main()
{
    using namespace Wardrobe;
    const auto root = std::filesystem::temp_directory_path() /
        ("SkyWardrobe-storage-" + std::to_string(GetCurrentProcessId()));
    try
    {
        MetadataStore store(root);
        ValidateDatabase(store.database);
        auto& player = store.database["actors"]["player"];
        player["workingItems"] = {"Skyrim.esm|000123"};
        player["backupItems"] = {"Skyrim.esm|000456"};
        player["backupEnabled"] = false;
        player["backupGroup"] = "groups:backup";
        player["baseline"] = player["workingItems"];
        player["automatic"] = true;
        player["actualEquipmentMode"] = "all";
        player["runtime"] = {{"progress", {{"default", {{"last", "groups:one"}}}}}};
        player["presetRuntime"] = {{"default-player", player["runtime"]}};
        const auto edited = player;
        store.database["actors"]["Skyrim.esm|000789"] = NewActorProfile("NPC");
        store.database["items"]["Skyrim.esm|000123"] = {{"alias", "玩家服装"}};
        MetadataStore::SetPlayerStorageMode(PlayerOutfitStorage::Global);
        Require(store.Save(), "Global JSON save");
        const auto disk = Read(root / "Wardrobe.json");
        Require(disk["actors"]["player"]["workingItems"] == edited["workingItems"], "Global outfit persisted without game save");
        const auto globalState = ActorStateForSave(store.database["actors"], PlayerOutfitStorage::Global);
        Require(!globalState.contains("player") && globalState.contains("Skyrim.esm|000789"), "Global co-save excludes player and retains NPC");

        MetadataStore::SetPlayerStorageMode(PlayerOutfitStorage::SaveLocal);
        Require(Read(root / "Wardrobe.json") == disk, "Mode switch alone does not clean JSON");
        const auto localState = ActorStateForSave(store.database["actors"], PlayerOutfitStorage::SaveLocal);
        Require(localState["player"] == edited, "Local co-save keeps full player state");
        Require(store.CleanPlayerOutfit().empty(), "Game save path cleans legacy JSON");
        auto cleaned = Read(root / "Wardrobe.json");
        for (const auto* field : PlayerOutfitFields)
        {
            Require(!cleaned["actors"]["player"].contains(field), "Actor outfit field removed");
            Require(!cleaned.contains(field), "Root outfit mirror removed");
        }
        Require(cleaned["items"] == disk["items"] && cleaned["presets"] == disk["presets"] &&
            cleaned["actors"]["Skyrim.esm|000789"] == disk["actors"]["Skyrim.esm|000789"], "Cleanup preserves NPC and shared resources");
        const auto timestamp = std::filesystem::last_write_time(root / "Wardrobe.json");
        Require(store.CleanPlayerOutfit().empty() && std::filesystem::last_write_time(root / "Wardrobe.json") == timestamp,
            "No rewrite when no legacy outfit remains");
        Require(store.Save(), "Local metadata save");
        cleaned = Read(root / "Wardrobe.json");
        Require(!cleaned.contains("workingItems") && !cleaned["actors"]["player"].contains("runtime"), "Validation cannot recreate disk outfit fields");
        MetadataStore loaded(root);
        Require(loaded.Load(), "Local JSON reload");
        Require(loaded.database["actors"]["player"]["workingItems"].empty(), "Local JSON cannot restore player outfit");

        auto profile = edited;
        RestorePlayerOutfit(profile, Json::object());
        Require(profile["workingItems"].empty() && profile["backupItems"].empty() && !profile["automatic"].get<bool>() &&
            profile["runtime"].empty() && !profile.contains("baseline") && !profile.contains("presetRuntime"), "Missing save record resets previous session");
        RestorePlayerOutfit(profile, localState);
        for (const auto* field : PlayerOutfitFields)
            Require(profile.contains(field) == edited.contains(field) && (!edited.contains(field) || profile[field] == edited[field]), "Full local restoration");
        auto otherSave = localState;
        otherSave["player"]["workingItems"] = Json::array();
        RestorePlayerOutfit(profile, otherSave);
        Require(profile["workingItems"].empty(), "Different saves restore different outfits");
        RestorePlayerOutfit(profile, localState);
        Require(profile["workingItems"] == edited["workingItems"], "Previous save restores its outfit again");

        auto selectedState = localState;
        selectedState["player"]["activePreset"] = "second-player-preset";
        const auto validatedState = ActorStateForLoad(selectedState, PlayerOutfitStorage::SaveLocal);
        Require(validatedState["player"]["activePreset"] == "second-player-preset", "Snapshot validation preserves selected preset");
        selectedState["player"] = "invalid old player record";
        const auto ignoredState = ActorStateForLoad(selectedState, PlayerOutfitStorage::Global);
        Require(!ignoredState.contains("player") && ignoredState.contains("Skyrim.esm|000789"), "Global load ignores even malformed legacy player data");
        Require(!ActorStateForLoad(Json::object(), PlayerOutfitStorage::SaveLocal).contains("player"), "Validation does not invent a missing player snapshot");

        MetadataStore::SetPlayerStorageMode(PlayerOutfitStorage::Global);
        Require(store.Save(), "First global save preserves current memory outfit");
        Require(Read(root / "Wardrobe.json")["actors"]["player"]["backupItems"] == edited["backupItems"], "Switch back persists live outfit");
        const auto preserved = Read(root / "Wardrobe.json");
        Require(store.CleanPlayerOutfit().empty() && Read(root / "Wardrobe.json") == preserved, "Global cleanup is inert");

        // 使用同名目录阻止原子替换，验证失败有返回且原文件保持可读。
        MetadataStore::SetPlayerStorageMode(PlayerOutfitStorage::SaveLocal);
        std::filesystem::create_directory(root / "Wardrobe.json.tmp");
        Require(!store.CleanPlayerOutfit().empty(), "Cleanup reports write failure");
        MetadataStore::SetPlayerStorageMode(PlayerOutfitStorage::Global);
        Require(!store.Save() && !store.error.empty(), "Global save reports write failure");
        Require(Read(root / "Wardrobe.json") == preserved, "Failed writes preserve original JSON");
        std::filesystem::remove_all(root);
        std::cout << "Player outfit storage checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        std::filesystem::remove_all(root);
        return 1;
    }
}
