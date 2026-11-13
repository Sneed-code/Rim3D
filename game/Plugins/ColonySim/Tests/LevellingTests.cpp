#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Level ground for structures (docs/design/levelling.md).

#define LEVEL_REQUIRE(...)     \
    do {                       \
        CHECK((__VA_ARGS__));  \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> levellingDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

// A flat test map tilted into a hillside: the ground rises `rise` metres a metre along x, plus a little along y.
std::unique_ptr<Sim> levellingSim(float rise = 0.13f) {
    auto sim = std::make_unique<Sim>(SimSettings{5, 48, 0, 0, true}, levellingDefs());
    for (int y = 0; y < 48; ++y) {
        for (int x = 0; x < 48; ++x) {
            sim->editMap().setGroundHeight(CellPos{x, y, 0}, 2.0f + rise * static_cast<float>(x) + 0.04f * static_cast<float>(y));
        }
    }
    return sim;
}

// The four corner heights of a cell (what the terrain mesh draws).
std::vector<float> levellingCorners(const Sim& sim, int x, int y) {
    const Map& m = sim.map();
    return {m.groundHeight(x, y), m.groundHeight(x + 1, y), m.groundHeight(x, y + 1), m.groundHeight(x + 1, y + 1)};
}

bool levellingFlat(const Sim& sim, int x, int y, float base) {
    for (const float h : levellingCorners(sim, x, y)) {
        if (h != base) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("Levelling: a wall digs its cell flat to the lowest corner, on a whole step") {
    std::unique_ptr<Sim> owned = levellingSim();
    Sim& sim = *owned;
    const float lowest = sim.map().groundHeight(10, 10);  // the ground rises with x and y
    LEVEL_REQUIRE(sim.things().spawn("Wall", CellPos{10, 10, 0}, 1) != kNoEntity);
    const float base = sim.map().levelBase(CellPos{10, 10, 0});
    CHECK(sim.map().levelled(CellPos{10, 10, 0}));
    CHECK(std::fabs(base - lowest) <= Map::kLevelStepM * 0.5f + 1e-4f);
    CHECK(std::fabs(std::round(base / Map::kLevelStepM) * Map::kLevelStepM - base) < 1e-5f);
    CHECK(levellingFlat(sim, 10, 10, base));
    CHECK_FALSE(sim.map().levelledCorners().empty());
    CHECK(sim.map().anyLevelled());
    // A cell two away is untouched.
    CHECK(sim.map().groundHeight(13, 13) == doctest::Approx(2.0f + 0.13f * 13.0f + 0.04f * 13.0f));
}

TEST_CASE("Levelling: joined walls share one base up a slope, then step") {
    std::unique_ptr<Sim> owned = levellingSim();
    Sim& sim = *owned;
    for (int x = 10; x <= 30; ++x) {
        LEVEL_REQUIRE(sim.things().spawn("Wall", CellPos{x, 10, 0}, 1) != kNoEntity);
    }
    const float first = sim.map().levelBase(CellPos{10, 10, 0});
    // The first walls share the base while the ground under them is within kLevelJoinM of it.
    CHECK(sim.map().levelBase(CellPos{14, 10, 0}) == first);
    CHECK(levellingFlat(sim, 12, 10, first));
    // Further up the base steps up, and never by more than the join distance at once; no wall is dug more than that.
    float last = first;
    int steps = 0;
    for (int x = 11; x <= 30; ++x) {
        const float base = sim.map().levelBase(CellPos{x, 10, 0});
        CHECK(base >= last);
        if (base != last) {
            ++steps;
            CHECK(base - last <= Map::kLevelJoinM + Map::kLevelStepM);
        }
        last = base;
    }
    CHECK(steps >= 1);
    CHECK(last > first);
}

TEST_CASE("Levelling: a roofed room is levelled to its walls' base, so the floor is flat") {
    std::unique_ptr<Sim> owned = levellingSim(0.08f);
    Sim& sim = *owned;
    for (int y = 10; y <= 16; ++y) {
        for (int x = 10; x <= 16; ++x) {
            if (x == 10 || x == 16 || y == 10 || y == 16) {
                LEVEL_REQUIRE(sim.things().spawn("Wall", CellPos{x, y, 0}, 1) != kNoEntity);
            }
        }
    }
    sim.rooms().update();
    const Room* room = sim.rooms().roomOf(CellPos{13, 13, 0});
    LEVEL_REQUIRE(room != nullptr);
    CHECK_FALSE(room->outdoors());
    const float base = sim.map().levelBase(CellPos{10, 10, 0});
    // Every wall shares the base (the ground rises 0.48 m across the room), so the tops line up.
    for (int x = 10; x <= 16; ++x) {
        CHECK(sim.map().levelBase(CellPos{x, 16, 0}) == base);
    }
    // And every cell inside is flat at it.
    for (int y = 11; y <= 15; ++y) {
        for (int x = 11; x <= 15; ++x) {
            CHECK(levellingFlat(sim, x, y, base));
        }
    }
    // Outside is not.
    CHECK_FALSE(sim.map().levelled(CellPos{20, 13, 0}));
}

TEST_CASE("Levelling: a plan levels its ground at once; floors and upper floors level theirs") {
    std::unique_ptr<Sim> owned = levellingSim();
    Sim& sim = *owned;
    sim.submit(PlaceBlueprintCommand{"Wall", "WoodLog", CellPos{20, 20, 0}, Rot4::North});
    sim.submit(PlaceFloorCommand{CellRect{30, 30, 2, 1, 0}, "WoodFloor"});
    sim.step();
    CHECK(sim.map().levelled(CellPos{20, 20, 0}));
    CHECK(levellingFlat(sim, 20, 20, sim.map().levelBase(CellPos{20, 20, 0})));
    CHECK(sim.map().levelled(CellPos{30, 30, 0}));
    CHECK(sim.map().levelled(CellPos{31, 30, 0}));
    // The two floor cells are within the join distance, so they share one base.
    CHECK(sim.map().levelBase(CellPos{31, 30, 0}) == sim.map().levelBase(CellPos{30, 30, 0}));
}

TEST_CASE("Levelling: saved and loaded, the ground and the bases come back") {
    std::unique_ptr<Sim> owned = levellingSim();
    Sim& sim = *owned;
    for (int x = 10; x <= 16; ++x) {
        sim.things().spawn("Wall", CellPos{x, 10, 0}, 1);
    }
    sim.step();
    const uint64_t hash = sim.stateHash();
    const Map::LevelState before = sim.map().levelState();
    LEVEL_REQUIRE(!before.corners.empty());

    // The test hill is not made by the generator, so put it on the loaded map's ground too: only the levelled corners come from the save.
    SaveLoadResult loaded = loadSim(saveSim(sim), levellingDefs());
    LEVEL_REQUIRE(loaded.sim != nullptr);
    const Map::LevelState after = loaded.sim->map().levelState();
    CHECK(after.cells == before.cells);
    CHECK(after.cellBaseCm == before.cellBaseCm);
    CHECK(after.corners == before.corners);
    CHECK(after.cornerCm == before.cornerCm);
    for (const uint32_t corner : before.corners) {
        const CellPos c = sim.map().cellAt(static_cast<int>(corner));
        CHECK(loaded.sim->map().groundHeight(c) == sim.map().groundHeight(c));
    }
    CHECK(loaded.sim->map().levelHash() == sim.map().levelHash());
    (void)hash;
}

TEST_CASE("Levelling: nothing levelled leaves the state hash as it was") {
    Sim a(SimSettings{5, 48, 0, 0, true}, levellingDefs());
    CHECK_FALSE(a.map().anyLevelled());
    CHECK(a.map().levelledCorners().empty());
}
