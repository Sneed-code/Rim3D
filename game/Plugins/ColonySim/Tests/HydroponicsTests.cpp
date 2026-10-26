#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Farming.h"
#include "ColonySim/Lights.h"
#include "ColonySim/Map.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Power.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"
#include "FuelTestHelper.h"

#include <doctest.h>

#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define HYDRO_REQUIRE(...)     \
    do {                       \
        CHECK((__VA_ARGS__));  \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// The game's real data: the basin, the lamps and the crops are part of what is tested.
std::shared_ptr<const DefDatabase> hydroDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

// A mild 64 m map with a clear sky, and two 4 x 4 solar generators (3400 W in the full sun) at (4,4) and (4,8)
// with a conduit column at x = 8 and a row along y = 12 to x = 13.
std::unique_ptr<Sim> hydroSimPtr(bool withPower) {
    auto owned = std::make_unique<Sim>(SimSettings{5, 64, 0, 0, true}, hydroDefs());
    Sim& sim = *owned;
    sim.setOutdoorOverride(20.0f);
    sim.submit(SetWeatherCommand{"Clear", 100000.0f});
    sim.power().setSun([](const GameTime&) { return 1.0; });
    if (withPower) {
        sim.things().spawn("SolarGenerator", CellPos{4, 4, 0});
        sim.things().spawn("SolarGenerator", CellPos{4, 8, 0});
        for (int y = 4; y <= 12; ++y) {
            sim.things().spawn("PowerConduit", CellPos{8, y, 0});
        }
        for (int x = 9; x <= 13; ++x) {
            sim.things().spawn("PowerConduit", CellPos{x, 12, 0});
        }
    }
    return owned;
}

DefId hydroDef(const Sim& sim, const char* name) { return sim.defs().idOf("Things", name); }

// Rice sown by hand at growth 0 in a one-cell growing zone.
void hydroSow(Sim& sim, CellPos c) {
    sim.submit(CreateGrowingZoneCommand{CellRect{c.x, c.y, 1, 1, 0}, "Plant_Rice"});
    sim.step();
    sim.plants().remove(c);  // a wild plant may stand there
    sim.plants().place(c, hydroDef(sim, "Plant_Rice"), 0.0f);
}

}  // namespace

TEST_CASE("Hydroponics: a powered basin gives 2.8 fertility and crops on it grow faster") {
    auto simOwner = hydroSimPtr(true);
    Sim& sim = *simOwner;
    const CellPos basin{11, 13, 0};
    const CellPos soil{30, 30, 0};
    sim.things().spawn("HydroponicsBasin", basin);
    sim.step();
    HYDRO_REQUIRE(sim.plants().fertility(soil) > 0.0f);
    CHECK(sim.plants().fertility(basin) == doctest::Approx(2.8f));
    CHECK(sim.farming().spotFertility(static_cast<size_t>(sim.map().index(basin))) == doctest::Approx(2.8f));

    hydroSow(sim, basin);
    hydroSow(sim, soil);
    HYDRO_REQUIRE(sim.farming().zoneAt(basin) != kNoGrowingZone);  // a zone takes the basin's cell
    HYDRO_REQUIRE(sim.plants().has(basin));
    CHECK(sim.farming().sowBlocker(CellPos{12, 13, 0}).empty() == false);  // empty ground off the zone
    sim.stepMany(9 * kSecondsPerHour);  // 06:00 to 09:00 are growing hours; keep short, growth is tuned fast
    const float onBasin = sim.plants().growth(basin);
    const float onSoil = sim.plants().growth(soil);
    HYDRO_REQUIRE(onSoil > 0.0f);
    // Rice's fertility sensitivity is 1: the rate is the fertility itself.
    CHECK(onBasin / onSoil == doctest::Approx(2.8f / sim.plants().fertility(soil)).epsilon(0.05));
}

TEST_CASE("Hydroponics: a basin with no power stops the crop and cannot be sown") {
    auto simOwner = hydroSimPtr(false);
    Sim& sim = *simOwner;
    const CellPos basin{11, 13, 0};
    sim.things().spawn("HydroponicsBasin", basin);
    sim.submit(CreateGrowingZoneCommand{CellRect{11, 13, 1, 1, 0}, "Plant_Rice"});
    sim.step();
    HYDRO_REQUIRE(sim.farming().zoneAt(basin) != kNoGrowingZone);
    CHECK(sim.farming().sowBlocker(basin) == "its basin has no power");
    sim.plants().remove(basin);  // a wild plant may stand there
    sim.plants().place(basin, hydroDef(sim, "Plant_Rice"), 0.0f);
    sim.stepMany(kSecondsPerDay);
    CHECK(sim.plants().has(basin));
    CHECK(sim.plants().growth(basin) == 0.0f);
}

TEST_CASE("Hydroponics: a sun lamp grows crops through the night, a standing lamp does not") {
    // One sim with a sun lamp 2 m from the crop, one with a standing lamp: both light the cell, only the first
    // lets it grow in the night hours.
    auto growthByDawn = [](const char* lamp) {
        auto simOwner = hydroSimPtr(true);
    Sim& sim = *simOwner;
        sim.things().spawn(lamp, CellPos{13, 13, 0});  // next to the conduit at (13,12)
        const CellPos crop{15, 13, 0};
        sim.step();
        hydroSow(sim, crop);
        sim.stepMany(5 * kSecondsPerHour);  // midnight to 05:00: nothing but the lamp lights it
        return std::pair<float, float>(sim.lights().lampLightAt(crop), sim.plants().growth(crop));
    };
    const auto sun = growthByDawn("SunLamp");
    const auto standing = growthByDawn("StandingLamp");
    CHECK(sun.first >= 0.51f);
    CHECK(standing.first >= 0.51f);
    CHECK(sun.second > 0.0f);
    CHECK(standing.second == 0.0f);
}
