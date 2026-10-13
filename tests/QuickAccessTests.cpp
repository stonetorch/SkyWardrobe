#include "wardrobe/QuickAccessModel.h"
#include <iostream>

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }
    void Reject(nlohmann::json options)
    {
        try { Wardrobe::ValidateQuickAccess(options); }
        catch (const std::runtime_error&) { return; }
        throw std::runtime_error("Malformed shortcut settings accepted");
    }
}

int main()
{
    try
    {
        auto options = nlohmann::json::object();
        Wardrobe::ValidateQuickAccess(options);
        Require(options == Wardrobe::DefaultQuickAccess(), "Partial settings get defaults");
        options["cells"] = {{{"action", "category"}, {"target", "categories:deleted"}, {"icon", "unknown"}},
            {{"action", "applyPreset"}, {"target", "preset-1"}}, {{"action", "useGroup"}, {"target", "groups:outfit"}}};
        Wardrobe::ValidateQuickAccess(options);
        options["rows"] = 1; options["columns"] = 1;
        const auto restored = nlohmann::json::parse(options.dump());
        Require(restored["cells"].size() == 3 && restored["cells"][2]["target"] == "groups:outfit",
            "Shrinking and serializing preserve hidden bindings and stable IDs");
        const nlohmann::json invalidFields{{"rows", 0}, {"columns", 13}, {"cellSize", 31},
            {"enabled", "true"}, {"attached", 1}, {"cells", nlohmann::json::object()}};
        for (const auto& [key, value] : invalidFields.items())
        {
            auto invalid = options; invalid[key] = value; Reject(invalid);
        }
        auto invalid = options; invalid["cells"] = {{{"action", "execute"}}}; Reject(invalid);
        invalid = options; invalid["cells"] = {{{"target", 7}}}; Reject(invalid);
        invalid = options; invalid["cells"] = nlohmann::json::array();
        for (int i = 0; i < 145; ++i) invalid["cells"].push_back(nlohmann::json::object());
        Reject(invalid);
        options["rows"] = 12; options["columns"] = 12; options["cellSize"] = 128;
        options["cells"].erase(options["cells"].begin(), options["cells"].end());
        for (int i = 0; i < 144; ++i) options["cells"].push_back(nlohmann::json::object());
        Wardrobe::ValidateQuickAccess(options);
        std::cout << "Quick access settings tests passed\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
