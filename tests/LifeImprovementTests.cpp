#include "wardrobe/LifeModel.h"
#include <iostream>
#include <stdexcept>

namespace
{
    void Require(bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    }
    template<class Function> void Reject(Function function)
    {
        bool rejected = false;
        try { function(); } catch (const std::exception&) { rejected = true; }
        Require(rejected, "Invalid data was accepted");
    }
}
int main()
{
    using namespace Wardrobe;
    try
    {
        LifeJson db = {{"workingItems", {"Skyrim.esm|000123"}}, {"activePreset", "old"}, {"actualEquipmentMode", "shoes"},
            {"presets", {{"old", {{"name", "旧预设"}, {"group", "groups:old"}}}}}};
        ValidateLifeData(db);
        Require(db["actors"]["player"]["workingItems"] == db["workingItems"], "Legacy working outfit migration");
        Require(db["actors"]["player"]["activePreset"] == "old", "Legacy preset binding migration");
        Require(db["actors"]["player"]["actualEquipmentMode"] == "shoes", "Legacy equipment mode migration");
        Require(!db["actors"]["player"]["automatic"].get<bool>(), "Migration must not enable automation");
        Require(db["livePreview"].get<bool>(), "Preview defaults on");
        Require(db["presets"]["old"]["rules"]["default"]["groups"][0] == "groups:old", "Legacy default rule migration");
        db["actors"]["npc"] = NewActorProfile("人物");
        db["actors"]["npc"]["workingItems"] = {"Skyrim.esm|000456"};
        const auto before = db;
        ValidateLifeData(db);
        Require(db == before, "Migration is idempotent and preserves other actors");
        db["livePreview"] = false; ValidateLifeData(db);
        Require(!db["livePreview"].get<bool>(), "Explicit preview preference preserved");
        Require(RuntimeOutfitName("Npc.esp|000123") == RuntimeOutfitName("Npc.esp|000123"), "Runtime outfit name is stable");
        Require(RuntimeOutfitName("Npc.esp|000123") != RuntimeOutfitName("Npc.esp|000124"), "Separate actor references have separate outfits");
        Require(RuntimeOutfitName("player").size() == 21, "Runtime outfit name remains short");
        auto order = OrderedIDs(LifeJson::array({"b", "deleted", "b"}), {"a", "b", "c"});
        Require(order == std::vector<std::string>{"b", "a", "c"}, "Ordering discards stale IDs and duplicates, appends new entries");
        MoveOrdered(order, "c", 0);
        Require(order == std::vector<std::string>{"c", "b", "a"}, "Move to beginning");
        MoveOrdered(order, "c", 999);
        Require(order == std::vector<std::string>{"b", "a", "c"}, "Move beyond end is clamped");
        std::mt19937 generator(123);
        LifeJson rule = {{"mode", "sequence"}};
        Require(ChooseRuleGroup(rule, {"a", "b", "c"}, LifeJson::object(), generator) == "a", "Sequence begins at first");
        Require(ChooseRuleGroup(rule, {"a", "b", "c"}, {{"last", "a"}}, generator) == "b", "Sequence advances");
        Require(ChooseRuleGroup(rule, {"a", "b", "c"}, {{"last", "c"}}, generator) == "a", "Sequence loops");
        Require(ChooseRuleGroup(rule, {"a", "c"}, {{"last", "deleted"}}, generator) == "a", "Removed previous candidate falls back to first");
        rule["mode"] = "random";
        for (int i = 0; i < 100; ++i) Require(ChooseRuleGroup(rule, {"a", "b", "c"}, {{"last", "b"}}, generator) != "b", "Random avoids repeat");
        Require(ChooseRuleGroup(rule, {"a"}, {{"last", "a"}}, generator) == "a", "Single available random candidate remains valid");
        Require(ChooseRuleGroup(rule, {}, LifeJson::object(), generator).empty(), "No candidate preserves appearance");
        rule["mode"] = "single";
        Require(ChooseRuleGroup(rule, {"a", "b"}, {{"last", "a"}}, generator) == "a", "Single uses first valid candidate");
        LifeJson combat = {{"state", "combat"}, {"items", {"battle"}}, {"progress", {{"combat", {{"last", "battle-b"}}}}},
            {"resume", {{"state", "city"}, {"items", {"city-outfit"}}, {"progress", {{"combat", {{"last", "battle-a"}}}}}}}};
        const auto restored = RestoreCombatOutfit(combat);
        Require(restored["state"] == "city" && restored["items"][0] == "city-outfit", "Combat exit restores previous outfit");
        Require(restored["progress"]["combat"]["last"] == "battle-b", "Combat restore does not roll sequence progress back");
        Reject([&] { auto bad = db; bad["livePreview"] = "yes"; ValidateLifeData(bad); });
        Reject([&] { auto bad = db; bad["navigation"]["placements"]["groups:a"] = 3; ValidateLifeData(bad); });
        Reject([&] { auto bad = db; bad["presets"]["old"]["rules"]["default"]["mode"] = "invalid"; ValidateLifeData(bad); });
        Reject([&] { auto bad = db; bad["actors"]["npc"]["actualEquipmentMode"] = "invalid"; ValidateLifeData(bad); });
        Reject([&] { auto bad = db; bad["actors"]["npc"]["runtime"]["progress"] = "invalid"; ValidateLifeData(bad); });
        Reject([&] { auto bad = db; bad["actors"]["npc"]["baseline"] = 1; ValidateLifeData(bad); });
        std::cout << "Life improvement migration, ordering and rule selection checks passed\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
