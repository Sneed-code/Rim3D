// The life pace (docs/design/pace.md): one tuning value speeds plant growth, animals growing up,
// pregnancy and hatching alike.
#include "ColonySim/Animals.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

#define PACE_REQUIRE(...)     \
    do {                      \
        CHECK(__VA_ARGS__);   \
        if (!(__VA_ARGS__)) { \
            return;           \
        }                     \
    } while (false)

namespace {

std::shared_ptr<const DefDatabase> paceTestDefs() {
    auto db = std::make_shared<DefDatabase>();
    std::vector<std::string> errors;
    DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
    return db;
}

SimSettings paceTestSettings() {
    SimSettings s;
    s.seed = 42;
    s.mapSize = 128;
    s.checkpointEvery = 0;
    return s;
}

}  // namespace

TEST_CASE("Pace: the life pace is set and speeds plant growth") {
    const auto defs = paceTestDefs();
    const double pace = defs->tuning(kPaceLifeTuning);
    // sneed (2026-11-16): crops in 2 to 4 days, so the pace is well above RimWorld's 1.
    CHECK(pace > 2.0);
    CHECK(pace < 4.0);
    Sim sim(paceTestSettings(), defs);
    CHECK(sim.plants().growthTuning() == doctest::Approx(defs->tuning("plantGrowth") * pace));

    // Rice in a full-fertility cell, from a seed: grown in growDays x 24 / 13 / pace days.
    const DefId rice = defs->idOf("Things", "Plant_Rice");
    PACE_REQUIRE(rice.valid());
    CellPos cell{-1, -1, 0};
    for (int y = 10; y < 118 && cell.x < 0; ++y) {
        for (int x = 10; x < 118; ++x) {
            if (std::fabs(sim.plants().fertility(CellPos{x, y, 0}) - 1.0f) < 0.001f) {
                cell = CellPos{x, y, 0};
                break;
            }
        }
    }
    PACE_REQUIRE(cell.x >= 0);
    sim.plants().remove(cell);
    PACE_REQUIRE(sim.plants().place(cell, rice, 0.0f));
    const double expectedDays = defs->get(rice).number("plant.growDays", 1.0) * 24.0 / 13.0 / pace;
    CHECK(expectedDays > 1.5);
    CHECK(expectedDays < 2.5);
    int hours = 0;
    while (!sim.plants().mature(cell) && hours < 24 * 10) {
        sim.stepMany(static_cast<int>(kSecondsPerHour));
        ++hours;
        PACE_REQUIRE(sim.plants().defAt(cell) == rice);  // nobody cut it
    }
    const double days = static_cast<double>(hours) / 24.0;
    MESSAGE("rice grew in " << days << " days, expected about " << expectedDays);
    // Growing hours fall at fixed times of day, so the whole-day count can be up to half a day off.
    CHECK(days > expectedDays - 0.6);
    CHECK(days < expectedDays + 0.6);
}

TEST_CASE("Pace: animals grow up that many times faster") {
    const auto defs = paceTestDefs();
    const double pace = defs->tuning(kPaceLifeTuning);
    Sim sim(paceTestSettings(), defs);
    const DefId hare = defs->idOf("Things", "Hare");
    PACE_REQUIRE(hare.valid());
    const Def& def = defs->get(hare);
    const size_t stages = def.count("race.lifeStages");
    PACE_REQUIRE(stages >= 2);
    const double adultYears = def.number(("race.lifeStages." + std::to_string(stages - 1) + ".minAgeYears").c_str(), 0.0);
    const double adultSeconds = adultYears * static_cast<double>(kDaysPerYear * kSecondsPerDay) / pace;

    CellPos cell{64, 64, 0};
    for (int i = 0; i < 64 && !sim.map().walkable(cell); ++i) {
        cell.x += 1;
    }
    const Entity e = sim.things().spawn(hare, cell);
    PACE_REQUIRE(e != kNoEntity);
    Animal& a = sim.things().registry().get<Animal>(e);
    const int64_t now = sim.time().seconds;
    a.bornAt = now - static_cast<int64_t>(adultSeconds * 0.95);
    CHECK_FALSE(sim.animals().isAdult(e));
    a.bornAt = now - static_cast<int64_t>(adultSeconds * 1.05);
    CHECK(sim.animals().isAdult(e));
}
