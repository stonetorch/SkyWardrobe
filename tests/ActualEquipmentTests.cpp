#include "wardrobe/Model.h"
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
    try
    {
        const std::uint32_t feet = 1u << 7, calves = 1u << 8, body = 1u << 2;
        Require(!ShouldActuallyEquip(ActualEquipmentMode::Off, feet | body), "Off must leave all equipment alone");
        Require(ShouldActuallyEquip(ActualEquipmentMode::Shoes, feet | calves), "Multi-slot shoes must be equipped");
        Require(!ShouldActuallyEquip(ActualEquipmentMode::Shoes, calves), "Calf-only stockings are not shoes");
        Require(!ShouldActuallyEquip(ActualEquipmentMode::Shoes, body), "Shoes mode must leave body armor alone");
        Require(!ShouldActuallyEquip(ActualEquipmentMode::Shoes, 0), "No-slot accessories are not shoes");
        Require(ShouldActuallyEquip(ActualEquipmentMode::All, body), "All must include body armor");
        Require(ShouldActuallyEquip(ActualEquipmentMode::All, 0), "All includes no-slot armor");
        Json oldDatabase = EmptyDatabase();
        oldDatabase.erase("actualEquipmentMode");
        ValidateDatabase(oldDatabase);
        Require(oldDatabase["actualEquipmentMode"] == "off", "Existing users must keep appearance-only behavior");
        for (const auto mode : {ActualEquipmentMode::Off, ActualEquipmentMode::Shoes, ActualEquipmentMode::All})
        {
            auto database = EmptyDatabase();
            database["actualEquipmentMode"] = ActualEquipmentModeID(mode);
            ValidateDatabase(database);
            Require(ParseActualEquipmentMode(database["actualEquipmentMode"].get<std::string>()) == mode, "Mode survives JSON roundtrip");
        }
        for (const auto& invalid : {Json("unknown"), Json(1), Json(true), Json::object()})
        {
            auto database = EmptyDatabase();
            database["actualEquipmentMode"] = invalid;
            bool rejected = false;
            try { ValidateDatabase(database); } catch (const std::runtime_error&) { rejected = true; }
            Require(rejected, "Invalid persisted modes must be rejected");
        }
        std::cout << "Actual equipment tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
