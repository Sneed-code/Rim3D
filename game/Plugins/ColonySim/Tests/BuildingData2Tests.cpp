#include "ColonySim/Defs.h"

#include <doctest.h>

#include <string>
#include <vector>

using namespace colony;

// D11: power, climate, light, floors and furniture defs (docs/design/starting-data.md).
TEST_CASE("buildings 2: power, climate, light, floors and furniture load from the real data") {
    DefDatabase db;
    std::vector<std::string> errors;
    const bool ok = DefDatabase::loadDirectory(COLONY_DATA_DIR, db, errors);
    for (const std::string& e : errors) MESSAGE(e);
    if (!ok) { CHECK(false); return; }

    for (const char* name : {"PowerConduit", "PowerSwitch", "WoodFiredGenerator", "SolarGenerator", "WindTurbine",
                             "Battery", "Heater", "Cooler", "Vent", "PassiveCooler", "Campfire", "StandingLamp",
                             "SunLamp", "TorchLamp", "Armchair", "EndTable", "Dresser", "PlantPot", "Column"}) {
        const Def* d = db.find("Things", name);
        CHECK_MESSAGE(d != nullptr, name);
        if (d == nullptr) continue;
        CHECK_MESSAGE(d->text("category") == "Building", name);
        CHECK_MESSAGE(d->number("stats.workToBuild") > 0, name);
        CHECK_MESSAGE(d->number("stats.maxHitPoints") > 0, name);
    }
    for (const char* name : {"WoodFloor", "StoneTile", "Flagstone", "Carpet", "SmoothedStone"}) {
        const Def* d = db.find("Terrain", name);
        CHECK_MESSAGE(d != nullptr, name);
        if (d != nullptr) CHECK_MESSAGE(d->number("workToBuild") > 0, name);
    }
    CHECK(db.find("Terrain", "FloorBase") == nullptr);

    CHECK(db.find("Things", "WoodFiredGenerator")->number("power.output") == doctest::Approx(1000));
    CHECK(db.find("Things", "SolarGenerator")->number("power.output") == doctest::Approx(1700));
    CHECK(db.find("Things", "Heater")->number("heatPusher.heatPerSecond") > 0);
    CHECK(db.find("Things", "Cooler")->number("heatPusher.heatPerSecond") < 0);
    CHECK(db.find("Things", "TorchLamp")->number("glower.radius") == doctest::Approx(10.0));
    CHECK(db.find("Things", "Campfire")->number("glower.radius") > 0);
    CHECK(db.find("Things", "Armchair")->number("stats.comfort") == doctest::Approx(0.8));
    CHECK(db.find("Things", "Column")->text("passability") == "Impassable");
}
