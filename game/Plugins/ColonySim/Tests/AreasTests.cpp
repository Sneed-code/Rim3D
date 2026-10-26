// Allowed areas (docs/design/areas.md).
#include "ColonySim/Areas.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include "TestDefs.h"

#include <doctest.h>

#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define AREA_REQUIRE(...)      \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

TEST_CASE("Areas: painting, assigning and deleting") {
    Areas areas;
    const AreaId a = areas.create("Orchard");
    const AreaId b = areas.create("");
    CHECK(a == 1);
    CHECK(b == 2);
    CHECK(areas.find(b)->name == "Area 2");
    CHECK(areas.paint(a, CellRect{4, 4, 3, 2, 0}, true) == 6);
    CHECK(areas.paint(a, CellRect{4, 4, 3, 2, 0}, true) == 0);  // already there
    CHECK(areas.paint(a, CellRect{5, 4, 1, 1, 0}, false) == 1);
    CHECK(areas.paint(99, CellRect{0, 0, 1, 1, 0}, true) == 0);
    CHECK(areas.contains(a, CellPos{4, 4, 0}));
    CHECK_FALSE(areas.contains(a, CellPos{5, 4, 0}));
    CHECK_FALSE(areas.contains(a, CellPos{4, 4, 1}));  // another level
    CHECK(areas.cellsIn(a).size() == 5);

    const Entity pawn = static_cast<Entity>(7);
    CHECK(areas.allows(pawn, CellPos{50, 50, 0}));  // nobody is limited yet
    std::string error;
    CHECK_FALSE(areas.assign(pawn, 99, error));
    CHECK(areas.assign(pawn, a, error));
    CHECK(areas.areaOf(pawn) == a);
    CHECK(areas.allows(pawn, CellPos{4, 4, 0}));
    CHECK_FALSE(areas.allows(pawn, CellPos{50, 50, 0}));
    CHECK(areas.allows(static_cast<Entity>(8), CellPos{50, 50, 0}));  // a pawn with no area
    CHECK(areas.remove(a));
    CHECK(areas.areaOf(pawn) == kNoArea);  // freed with the area
    CHECK(areas.allows(pawn, CellPos{50, 50, 0}));
    CHECK_FALSE(areas.remove(a));
    CHECK(areas.create("Later") == 3);  // ids are not used twice
}

namespace {

// How many items the haulers have put in the stockpile after a day, with every hauler limited to `rect`
// (a zero-sized rectangle leaves them free).
int areaTestHauls(const CellRect& rect, bool limit) {
    Sim sim(SimSettings{11, 96, 3, 40}, testDefs());
    if (limit) {
        sim.submit(CreateAreaCommand{"Pen"});
        sim.submit(PaintAreaCommand{1, rect, true});
        for (const Entity pawn : sim.haulers()) {
            sim.submit(AssignAreaCommand{pawn, 1});
        }
    }
    sim.stepMany(kSecondsPerDay);
    return static_cast<int>(sim.stats().hauls);
}

}  // namespace

TEST_CASE("Areas: a pawn limited to an area takes no work outside it") {
    const int free = areaTestHauls(CellRect{}, false);
    CHECK(free > 0);
    // An area far from the items and the stockpile: nothing is hauled.
    CHECK(areaTestHauls(CellRect{0, 0, 2, 2, 0}, true) == 0);
    // An area over the whole map changes nothing.
    CHECK(areaTestHauls(CellRect{0, 0, 96, 96, 0}, true) == free);
}

TEST_CASE("Areas: commands fail in plain words, and the snapshot lists areas") {
    Sim sim(SimSettings{11, 64, 1, 1, true}, testDefs());
    AREA_REQUIRE(!sim.haulers().empty());
    const Entity pawn = sim.haulers()[0];
    sim.submit(PaintAreaCommand{5, CellRect{1, 1, 2, 2, 0}, true});  // no such area
    sim.submit(AssignAreaCommand{pawn, 5});                          // no such area
    sim.submit(DeleteAreaCommand{5});
    sim.submit(CreateAreaCommand{"Home"});
    sim.submit(PaintAreaCommand{1, CellRect{1, 1, 2, 2, 0}, true});
    sim.submit(PaintAreaCommand{1, CellRect{1, 1, 2, 2, 0}, true});  // nothing changes
    sim.submit(AssignAreaCommand{pawn, 1});
    sim.step();
    int failed = 0;
    for (const Event& event : sim.events()) {
        if (event.type == EventType::CommandFailed) {
            ++failed;
        }
    }
    CHECK(failed == 4);
    Snapshot snap;
    sim.writeSnapshot(snap);
    AREA_REQUIRE(snap.areas.size() == 1);
    CHECK(snap.areas[0].name == "Home");
    CHECK(snap.areas[0].cellCount == 4);
    CHECK(snap.areaCells.size() == 4);
    bool limited = false;
    for (const ThingView& view : snap.things) {
        if (view.id == pawn) {
            limited = view.area == 1;
        }
    }
    CHECK(limited);
}

TEST_CASE("Areas: saving and loading keeps them, and a replay plays the same game") {
    const auto defs = testDefs();
    Sim original(SimSettings{11, 96, 3, 40}, defs);
    original.submit(CreateAreaCommand{"Pen"});
    original.submit(PaintAreaCommand{1, CellRect{0, 0, 96, 96, 0}, true});
    original.submit(AssignAreaCommand{original.haulers()[0], 1});
    original.stepMany(600);
    const std::vector<uint8_t> bytes = saveSim(original);
    SaveLoadResult loaded = loadSim(bytes, defs);
    AREA_REQUIRE(loaded.error.empty());
    AREA_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->stateHash() == original.stateHash());
    CHECK(loaded.sim->areas().areaOf(original.haulers()[0]) == 1);
    CHECK(loaded.sim->areas().cellsIn(1).size() == 96u * 96u);
    CHECK(saveSim(*loaded.sim) == bytes);
    original.stepMany(600);
    loaded.sim->stepMany(600);
    CHECK(loaded.sim->stateHash() == original.stateHash());
}
