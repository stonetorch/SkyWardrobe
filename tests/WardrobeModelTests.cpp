#include "wardrobe/Store.h"
#include <Windows.h>
#include <fstream>
#include <future>
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
        auto layout = PlanPartColumns(groups, 5, true, 100, 10, 10);
        Require(groups[0].lanes == 2 && groups[1].lanes == 1 && groups[2].lanes == 2, "Balanced extra lanes");
        layout = PlanPartColumns(groups, 2, true, 100, 10, 10);
        Require(groups[0].lanes == 1 && groups[1].lanes == 1 && groups[2].lanes == 1, "Part overflow keeps separate lanes");
        layout = PlanPartColumns(groups, 8, false, 100, 10, 10);
        Require(groups[0].lanes == 1, "Manual lanes");
        Require(layout.size() == 3, "Manual layout keeps categories separate");

        groups.resize(4);
        groups[0].items.resize(20); groups[1].items.resize(2);
        groups[2].items.resize(3); groups[3].items.resize(1);
        layout = PlanPartColumns(groups, 4, true, 100, 10, 10);
        Require(layout.size() == 4 && groups[0].lanes == 3, "Merged short categories free lanes for overflowing category");
        Require(layout.back().sections.size() == 3 && layout.back().usedHeight == 90, "All headings count toward packed height");
        Require(layout.back().usedHeight <= 100, "Merged column fits without scrolling");
        layout = PlanPartColumns(groups, 4, true, 89, 10, 10);
        Require(groups[0].lanes == 2, "Smaller viewport repacks categories and reduces spare lanes");
        for (const auto& column : layout)
            Require(column.sections.size() == 1 || column.usedHeight <= 89, "Merged columns never exceed viewport");
        layout = PlanPartColumns(groups, 4, true, 90, 10, 10);
        Require(layout.back().sections.size() == 3, "Exact fit accepts all categories");
        layout = PlanPartColumns(groups, 12, true, 1000, 10, 10);
        Require(layout.size() == 4, "Spare configured columns prevent short category merging");
        layout = PlanPartColumns(groups, 5, true, 100, 10, 10);
        Require(layout.back().sections.size() == 3, "Merging starts after extra lane allocation fills configured columns");
        layout = PlanPartColumns(groups, 4, true, 100, 10, 10, 10);
        Require(layout.back().sections.size() == 1 && groups[0].lanes == 2, "Section padding prevents an otherwise oversized merge");
        layout = PlanPartColumns(groups, 4, true, 110, 10, 10, 10);
        Require(layout.back().sections.size() == 3 && layout.back().usedHeight == 110, "Exact fit includes padding above subsequent categories");
        layout = PlanPartColumns(groups, 1, true, 0, 10, 10);
        Require(layout.size() == 4, "Tiny viewport preserves all categories as scrolling columns");
        std::vector<PartGroup> noGroups;
        Require(PlanPartColumns(noGroups, 4, true, 100, 10, 10).empty(), "Reference-only view has no clothing columns");

        // 多种窗口高度及列数下，每件服装恰好分配一次；合并列必须完整可见。
        for (int available = 30; available <= 300; available += 30)
            for (int count = 1; count <= 12; ++count)
            {
                layout = PlanPartColumns(groups, count, true, static_cast<float>(available), 10, 10, 10);
                std::vector<std::vector<int>> seen;
                for (const auto& group : groups) seen.emplace_back(group.items.size(), 0);
                for (const auto& column : layout)
                {
                    Require(column.sections.size() == 1 || column.usedHeight <= available, "Packed height invariant");
                    for (const auto& section : column.sections)
                    {
                        const auto& group = groups[section.group];
                        for (auto i = group.items.size() * section.lane / group.lanes;
                            i < group.items.size() * (section.lane + 1) / group.lanes; ++i) ++seen[section.group][i];
                    }
                }
                for (const auto& items : seen)
                    for (int occurrences : items) Require(occurrences == 1, "No duplicated or omitted clothing across columns");
            }

        Json legacy = EmptyDatabase();
        legacy.erase("groups");
        legacy["items"]["Clothes.esp|000ABC"] = {{"alias", "共享别名"}, {"note", "共享备注"}, {"part", "top"}, {"categories", {"city"}}};
        legacy["presets"]["city"] = {{"name", "城市"}, {"items", {"Clothes.esp|abc", "Clothes.esp|000ABC"}}};
        ValidateDatabase(legacy);
        const auto groupID = legacy["presets"]["city"]["group"].get<std::string>();
        const auto [collection, id] = SplitGroupID(groupID);
        Require(collection == "groups" && legacy[collection][id]["items"].size() == 1, "Legacy preset group migration and canonicalization");
        const auto migrated = legacy;
        ValidateDatabase(legacy);
        Require(legacy == migrated, "Migration is idempotent");
        Require(legacy["items"]["Clothes.esp|000ABC"]["alias"] == "共享别名", "Migration preserves global metadata");
        Require(SplitGroupID("pluginCategories:Clothes.esp").second == "Clothes.esp", "Plugin group identity");
        Require(SplitGroupID("items:bad").first.empty(), "Invalid group kind");
        std::mt19937 random(42);
        bool firstSeen = false, secondSeen = false;
        for (int i = 0; i < 100; ++i)
        {
            const auto outfit = RandomOutfit({{"a", 1}, {"b", 1}, {"c", 2}, {"d", 0}}, random);
            Require(outfit.size() == 3, "Random outfit excludes overlapping slots and includes independent slots");
            firstSeen |= std::find(outfit.begin(), outfit.end(), "a") != outfit.end();
            secondSeen |= std::find(outfit.begin(), outfit.end(), "b") != outfit.end();
        }
        Require(firstSeen && secondSeen, "Random selection reaches both conflicting alternatives");
        Require(RandomOutfit({}, random).empty(), "Empty group produces empty outfit");

        std::filesystem::create_directories(root / "AliasPacks");
        {
            std::ofstream output(root / "AliasPacks" / "test.json", std::ios::binary);
            output << Json{{"plugin", "Clothes.esp"}, {"items", {{"000ABC", {{"name", "裙子"}, {"pinyin", "qun zi"}, {"note", "包备注"}}}}}}.dump();
        }
        MetadataStore store(root);
        Json templateItems = {{"000ABC", {{"name", "原始裙子"}, {"part", "body"}}},
            {"000DEF", {{"name", "丝袜"}, {"part", "stockings"}}}};
        Require(store.WriteAliasTemplate("Clothes.esp", templateItems, false), "Generate alias template");
        const auto templatePath = root / "AliasPacks" / "00_Generated.Clothes.esp.json";
        Json generated;
        { std::ifstream input(templatePath); input >> generated; }
        Require(generated["plugin"] == "Clothes.esp" && generated["items"] == templateItems, "Template names and recognized parts roundtrip");
        auto updatedItems = templateItems;
        updatedItems["000ABC"]["name"] = "更新原名";
        Require(store.WriteAliasTemplate("Clothes.esp", updatedItems, false), "Keep existing template on registration");
        { std::ifstream input(templatePath); input >> generated; }
        Require(generated["items"] == templateItems, "Registration preserves edited template");
        Require(store.WriteAliasTemplate("Clothes.esp", updatedItems, true), "Manual regeneration");
        { std::ifstream input(templatePath); input >> generated; }
        Require(generated["items"] == updatedItems, "Regeneration replaces generated template");
        Require(!store.WriteAliasTemplate("../Escape.esp", templateItems, true), "Reject template path traversal");
        Require(store.Load(), "Initial load");
        Require(store.Effective("Clothes.esp|000DEF").value("part", "") == "stockings", "Generated template loads recognized part");
        Require(store.Effective("Clothes.esp|000ABC").value("alias", "") == "裙子", "Alias pack fallback");
        store.database["items"]["Clothes.esp|000ABC"] = {{"favorite", true}};
        Require(store.Effective("Clothes.esp|000ABC").value("alias", "") == "裙子", "Favorite does not shadow pack alias");
        store.database["items"]["Clothes.esp|000ABC"]["alias"] = "用户裙子";
        store.database["presets"]["city"] = {{"name", "城市"}, {"items", {"Clothes.esp|000ABC"}}};
        Require(store.Save(), "Atomic save");
        Require(store.Load(), "UTF-8 roundtrip load");
        Require(store.Effective("Clothes.esp|000ABC").value("alias", "") == "用户裙子", "User precedence");
        Require(store.Save() && std::filesystem::exists(root / "Wardrobe.json.bak"), "Backup preserved");
        auto snapshot = store.database;
        snapshot["workingItems"] = {"Clothes.esp|000ABC"};
        auto pending = std::async(std::launch::async, [&store, snapshot]() { return store.SaveSnapshot(snapshot); });
        store.database["workingItems"] = Json::array();
        Require(pending.get().empty(), "Background snapshot save");
        Require(store.database["workingItems"].empty(), "Snapshot writer leaves live database untouched");
        MetadataStore readback(root);
        Require(readback.Load() && readback.database["workingItems"] == snapshot["workingItems"], "Snapshot persisted independently");
        Require(store.SaveSnapshot(store.database).empty(), "Latest snapshot save");
        Require(readback.Load() && readback.database["workingItems"].empty(), "Latest snapshot wins");
        auto invalidSnapshot = store.database;
        invalidSnapshot["viewOptions"] = {{"columns", 100}};
        Require(!store.SaveSnapshot(invalidSnapshot).empty(), "Snapshot validation error reported");
        Require(readback.Load() && readback.database["workingItems"].empty(), "Failed snapshot retains valid file");
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
