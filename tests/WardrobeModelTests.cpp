#include "wardrobe/Store.h"
#include <Windows.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }
}

int main()
{
    using namespace Wardrobe;
    const auto root = std::filesystem::temp_directory_path() / ("SkyWardrobe-tests-" + std::to_string(GetCurrentProcessId()));
    try
    {
        Require(FormKey::Parse("Skyrim.esm|000ABC")->String() == "Skyrim.esm|000ABC", "Stable form key roundtrip");
        Require(FormKey::Parse("Light.esl|000FFF").has_value(), "ESL local ID");
        Require(!FormKey::Parse("plugin|FE123ABC"), "Runtime FormID must be rejected");
        Require(!FormKey::Parse("plugin|xyz"), "Malformed hex must be rejected");
        Require(!FormKey::Parse("|ABC"), "Empty plugin must be rejected");
        for (const char* query : {"qz", "qzi", "qun", "qunzi", "qun zi", "QZI"})
            Require(SearchScore(query, "qun zi", true) > 0, "Pinyin matching");
        Require(SearchScore("zq", "qun zi", true) == 0, "Pinyin order");
        Require(SearchScore("qzi", "qunzi", false) == 0, "Do not fuzzy match regular names");
        Require(SearchScore("裙", "黑色裙子") > 0, "Chinese UTF-8 substring");
        Require(SearchMetadata("qz", Json{{"name", "裙装"}, {"search", {"qun zhuang"}}}) > 0, "Category pinyin");
        std::vector<PartGroup> groups(3);
        groups[0].items.resize(46); groups[1].items.resize(12); groups[2].items.resize(34);
        AllocateLanes(groups, 5, true);
        Require(groups[0].lanes == 2 && groups[1].lanes == 1 && groups[2].lanes == 2, "Balanced extra lanes");
        AllocateLanes(groups, 2, true);
        Require(groups[0].lanes == 1 && groups[1].lanes == 1 && groups[2].lanes == 1, "Part overflow keeps separate lanes");
        AllocateLanes(groups, 8, false);
        Require(groups[0].lanes == 1, "Manual lanes");

        std::filesystem::create_directories(root / "AliasPacks");
        {
            std::ofstream output(root / "AliasPacks" / "test.json", std::ios::binary);
            output << Json{{"plugin", "Clothes.esp"}, {"items", {{"000ABC", {{"name", "裙子"}, {"pinyin", "qun zi"}, {"note", "包备注"}}}}}}.dump();
        }
        MetadataStore store(root);
        Require(store.Load(), "Initial load");
        Require(store.Effective("Clothes.esp|000ABC").value("alias", "") == "裙子", "Alias pack fallback");
        store.database["items"]["Clothes.esp|000ABC"] = {{"favorite", true}};
        Require(store.Effective("Clothes.esp|000ABC").value("alias", "") == "裙子", "Favorite does not shadow pack alias");
        store.database["items"]["Clothes.esp|000ABC"]["alias"] = "用户裙子";
        store.database["presets"]["city"] = {{"name", "城市"}, {"items", {"Clothes.esp|000ABC"}}};
        Require(store.Save(), "Atomic save");
        Require(store.Load(), "UTF-8 roundtrip load");
        Require(store.Effective("Clothes.esp|000ABC").value("alias", "") == "用户裙子", "User precedence");
        Require(store.Save() && std::filesystem::exists(root / "Wardrobe.json.bak"), "Backup preserved");
        {
            std::ofstream corrupt(root / "Wardrobe.json"); corrupt << "{broken";
        }
        Require(!store.Load() && !store.writable, "Malformed database disables writes");
        Require(!store.Save(), "Do not overwrite corrupt source");
        Require(store.database["presets"].contains("city"), "Failed reload retains memory");
        Json invalid = EmptyDatabase(); invalid["items"]["bad"] = {{"categories", 7}};
        bool rejected = false;
        try { ValidateDatabase(invalid); } catch (...) { rejected = true; }
        Require(rejected, "Invalid metadata shape rejected");
        invalid = EmptyDatabase(); invalid["partNames"] = {"bad"};
        rejected = false;
        try { ValidateDatabase(invalid); } catch (...) { rejected = true; }
        Require(rejected, "Invalid custom parts rejected");
        invalid = EmptyDatabase(); invalid["viewOptions"] = {{"columns", 100}};
        rejected = false;
        try { ValidateDatabase(invalid); } catch (...) { rejected = true; }
        Require(rejected, "Invalid view column count rejected");
        std::filesystem::remove_all(root);
        std::cout << "Wardrobe model and persistence checks passed\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        std::filesystem::remove_all(root);
        return 1;
    }
}
