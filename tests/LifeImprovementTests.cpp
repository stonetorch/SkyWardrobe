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
        ValidateLifeData(db);
        const auto before = db;
        ValidateLifeData(db);
        Require(db == before, "Migration is idempotent and preserves other actors");
        Require(!db["multiplePresets"].get<bool>(), "Multiple presets default off");
        Require(db["actors"]["player"]["defaultPreset"] == "old", "Existing active preset becomes default");
        const auto npcDefault = db["actors"]["npc"]["defaultPreset"].get<std::string>();
        Require(db["presets"][npcDefault]["actor"] == "npc", "New actor receives its own default");
        db["presets"]["extra"] = {{"actor", "player"}, {"group", "groups:extra"}};
        db["actors"]["player"]["activePreset"] = "extra";
        ValidateLifeData(db);
        Require(db["actors"]["player"]["activePreset"] == "old" && db["presets"].contains("extra"), "Disabled enhancement pins default and retains extra presets");
        db["multiplePresets"] = true; db["actors"]["player"]["activePreset"] = "extra";
        ValidateLifeData(db);
        Require(db["actors"]["player"]["activePreset"] == "extra", "Enhancement permits alternative binding");
        db["groups"] = {{"list", {{"references", {"groups:b", "groups:a", "groups:b", "groups:list"}}}}};
        LifeJson listRule = {{"mode", "sequence"}, {"groups", {"groups:manual"}}, {"listGroup", "groups:list"}};
        Require(RuleCandidates(db, listRule) == std::vector<std::string>{"groups:b", "groups:a"}, "Reference list preserves order and removes duplicates and self references");
        db["groups"]["list"]["references"] = {"groups:c", "groups:a"};
        Require(RuleCandidates(db, listRule) == std::vector<std::string>{"groups:c", "groups:a"}, "Reference changes feed candidates dynamically");
        listRule["listGroup"] = "groups:missing";
        Require(RuleCandidates(db, listRule).empty(), "Missing list does not fall back to manual candidates");
        listRule["mode"] = "single";
        Require(RuleCandidates(db, listRule) == std::vector<std::string>{"groups:manual"}, "Single group ignores list source");
        db["presets"]["old"]["rules"]["combat"] = {{"enabled", true}, {"mode", "single"}, {"groups", LifeJson::array()}, {"useWorking", true}};
        ValidateLifeData(db);
        Require(db["presets"]["old"]["rules"]["combat"]["useWorking"].get<bool>(), "Working outfit rule persists without candidates");
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
        LifeJson stableRules = {
            {"combat", {{"enabled", false}, {"mode", "sequence"}, {"groups", {"battle"}}}},
            {"city", {{"enabled", true}, {"mode", "sequence"}, {"groups", {"a", "b"}}}},
            {"default", {{"enabled", true}, {"mode", "sequence"}, {"groups", {"c"}}}}};
        const auto usable = [](const std::string& id) { return id != "missing"; };
        auto selected = ResolveAutomaticRule(db, stableRules, {"city", "default"}, usable);
        const auto citySignature = AutomaticRuleSignature("old", selected);
        Require(selected.state == "city", "Highest usable rule selected");
        Require(AutomaticRuleSignature("old", ResolveAutomaticRule(db, stableRules, {"combat", "city", "town", "default"}, usable)) == citySignature,
            "Disabled combat and lower priority town flags do not trigger a city rotation");
        AutomaticStateFilter stateFilter;
        std::string previousSignature, last;
        int rotations = 0, groupChecks = 0;
        const auto poll = [&](const std::vector<std::string>& states)
        {
            const auto stable = stateFilter.Observe(states);
            const auto adopted = AdoptAutomaticRule(stableRules, stable);
            const auto signature = std::string("old:rule:") + adopted;
            if (signature == previousSignature) return;
            previousSignature = signature; ++rotations;
            const auto selection = ResolveAutomaticRule(db, stableRules, stable, [&](const std::string& id)
            { ++groupChecks; return usable(id); });
            last = ChooseRuleGroup(stableRules[selection.state], selection.groups, {{"last", last}}, generator);
        };
        for (int i = 0; i < 500; ++i) poll(i % 2 ? std::vector<std::string>{"combat", "city", "town", "default"} : std::vector<std::string>{"city", "default"});
        Require(rotations == 1 && last == "a" && groupChecks == 2, "500 stable polls resolve members only once");
        poll({"city", "default"});
        stableRules["combat"]["enabled"] = true;
        poll({"combat", "city", "default"});
        Require(rotations == 1, "One noisy combat sample does not apply");
        poll({"city", "default"});
        Require(rotations == 1, "Combat blip is discarded");
        poll({"combat", "city", "default"});
        poll({"combat", "city", "default"});
        Require(rotations == 2 && last == "battle", "Confirmed combat entry triggers one application");
        const auto checksAfterCombat = groupChecks;
        for (int i = 0; i < 500; ++i) poll(i % 2 ? std::vector<std::string>{"combat", "default"} : std::vector<std::string>{"combat", "city", "default"});
        Require(rotations == 2 && groupChecks == checksAfterCombat, "Lower priority noise does not resolve members or apply");
        Require(AdoptAutomaticRule({{"combat", {{"enabled", true}}}}, {"combat", "default"}) == "combat", "Condition adoption requires no group configuration");
        stableRules["combat"]["useWorking"] = true;
        selected = ResolveAutomaticRule(db, stableRules, {"combat", "city", "default"}, usable);
        Require(selected.state == "combat" && selected.useWorking && selected.groups.empty(), "Working rule takes priority over all candidate groups");
        stableRules["combat"]["useWorking"] = false; stableRules["combat"]["groups"] = {"missing"};
        selected = ResolveAutomaticRule(db, stableRules, {"combat", "city", "default"}, usable);
        Require(selected.state == "city", "Unusable higher rule falls back to city without triggering a different state");
        Require(AutomaticRuleSignature("other", selected) != citySignature, "Changing preset is a separate effective state");
        LifeJson combat = {{"state", "combat"}, {"items", {"battle"}}, {"progress", {{"combat", {{"last", "battle-b"}}}}},
            {"resume", {{"state", "city"}, {"items", {"city-outfit"}}, {"progress", {{"combat", {{"last", "battle-a"}}}}}}}};
        const auto restored = RestoreCombatOutfit(combat);
        Require(restored["state"] == "city" && restored["items"][0] == "city-outfit", "Combat exit restores previous outfit");
        Require(restored["progress"]["combat"]["last"] == "battle-b", "Combat restore does not roll sequence progress back");
        Reject([&] { auto bad = db; bad["multiplePresets"] = "yes"; ValidateLifeData(bad); });
        Reject([&] { auto bad = db; bad["presets"]["old"]["rules"]["combat"]["useWorking"] = "yes"; ValidateLifeData(bad); });
        Reject([&] { auto bad = db; bad["presets"]["old"]["rules"]["default"]["listGroup"] = 1; ValidateLifeData(bad); });
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
