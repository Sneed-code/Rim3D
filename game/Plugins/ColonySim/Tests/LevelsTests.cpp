// Upper levels (task M1, docs/design/levels.md): sparse storage, upper floors and what holds them
// up, buildings on them, and regions, rooms, roofs and temperature on every level.

#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Map.h"
#include "ColonySim/Pathfinder.h"
#include "ColonySim/Regions.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Temperature.h"
#include "ColonySim/Things.h"
#include "ColonySim/UpperFloors.h"

#include <doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
// The condition is worked out once, so it can spawn things.
#define LEVELS_REQUIRE(...)                                 \
    do {                                                    \
        const bool levelsOk = static_cast<bool>(__VA_ARGS__); \
        CHECK_MESSAGE(levelsOk, #__VA_ARGS__);              \
        if (!levelsOk) {                                    \
            return;                                         \
        }                                                   \
    } while (0)

namespace {

// The game's real data: walls and columns hold roofs, and the roof support radius is RimWorld's.
std::shared_ptr<const DefDatabase> levelsDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

std::unique_ptr<Sim> levelsSim(int size = 48) {
    return std::make_unique<Sim>(SimSettings{5, size, 0, 0, true}, levelsDefs());
}

// A ring of walls round x0..x1 by y0..y1 on `level`.
void levelsRing(Sim& sim, int x0, int y0, int x1, int y1, int level = 0) {
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            if (x == x0 || x == x1 || y == y0 || y == y1) {
                CHECK(sim.things().spawn("Wall", CellPos{x, y, level}) != kNoEntity);
            }
        }
    }
    sim.rooms().update();
}

// Lays an upper floor over x0..x1 by y0..y1 on `level`; how many cells took one.
int levelsFloor(Sim& sim, int x0, int y0, int x1, int y1, int level = 1) {
    int laid = 0;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            laid += sim.upperFloors().lay(CellPos{x, y, level}) ? 1 : 0;
        }
    }
    sim.rooms().update();
    return laid;
}

// The last CommandFailed event's text, or empty if the last step had none.
std::string levelsFailure(const Sim& sim) {
    std::string text;
    for (const Event& e : sim.events()) {
        if (e.type == EventType::CommandFailed) {
            text = e.message;
        }
    }
    return text;
}

}  // namespace

TEST_CASE("Levels: an upper floor is stored in its own chunk, can be walked, and roofs the cell under it") {
    Map map(MapSettings{64, 64, 3, true});
    const CellPos up{20, 21, 1};
    const CellPos under{20, 21, 0};
    CHECK_FALSE(map.setUpperFloor(under, true));  // the ground is not an upper level
    CHECK(map.roof(under) == Roof::None);
    CHECK(map.setUpperFloor(up, true));
    CHECK(map.upperFloor(up));
    CHECK(map.terrain(up) == Terrain::UpperFloor);
    CHECK(map.walkable(up));
    CHECK(map.groundHeight(up) == doctest::Approx(map.groundHeight(under) + Map::kLevelHeightM));
    CHECK(map.heightAt(20.5f, 21.5f, 1) == doctest::Approx(map.heightAt(20.5f, 21.5f) + Map::kLevelHeightM));
    CHECK(map.floorAbove(under));
    CHECK(map.roof(under) == Roof::Thin);
    // Only the 16 m chunk it is in was made; the cell beside it, in the same chunk, is still air.
    CHECK(map.hasUpperChunk(CellPos{17, 17, 1}));
    CHECK_FALSE(map.hasUpperChunk(CellPos{40, 21, 1}));
    CHECK_FALSE(map.hasUpperChunk(CellPos{20, 21, 2}));
    CHECK_FALSE(map.walkable(CellPos{21, 21, 1}));
    // The ground under it is not touched.
    CHECK(map.walkable(under));
    CHECK(map.terrain(under) == Terrain::Soil);

    const uint64_t hash = map.upperFloorHash();
    CHECK(map.setUpperFloor(CellPos{50, 3, 2}, true));
    CHECK(map.upperFloorHash() != hash);
    const std::vector<CellPos> cells = map.upperFloorCells();
    LEVELS_REQUIRE(cells.size() == 2);
    CHECK(cells[0] == up);
    CHECK(cells[1] == CellPos{50, 3, 2});
    CHECK(map.upperFloorChanges() == 2);

    CHECK(map.setUpperFloor(up, false));
    CHECK_FALSE(map.walkable(up));
    CHECK(map.roof(under) == Roof::None);
    CHECK(map.upperFloorCells().size() == 1);
}

TEST_CASE("Levels: an upper floor needs a wall or column within the roof span on the level below") {
    auto sim = levelsSim();
    const float span = sim->rooms().numbers().roofSupportRadius;
    CHECK(span == doctest::Approx(6.9f));
    // Nothing below: nothing holds it up.
    std::string why;
    CHECK_FALSE(sim->upperFloors().lay(CellPos{20, 20, 1}, &why));
    CHECK(why.find("holds it up") != std::string::npos);
    CHECK_FALSE(sim->map().upperFloor(CellPos{20, 20, 1}));
    // Not on the ground, nor above the top level.
    CHECK_FALSE(sim->upperFloors().lay(CellPos{20, 20, 0}, &why));
    CHECK_FALSE(sim->upperFloors().lay(CellPos{20, 20, Map::kLevels}, &why));

    // One column holds up a floor out to the span and no further.
    LEVELS_REQUIRE(sim->things().spawn("Column", CellPos{20, 20, 0}) != kNoEntity);
    CHECK(sim->upperFloors().lay(CellPos{20, 20, 1}));  // right on top of it
    CHECK(sim->upperFloors().lay(CellPos{26, 20, 1}));  // 6 m away
    CHECK(sim->upperFloors().lay(CellPos{24, 25, 1}));  // 4 by 5: 6.4 m
    CHECK_FALSE(sim->upperFloors().lay(CellPos{27, 20, 1}));  // 7 m
    CHECK_FALSE(sim->upperFloors().lay(CellPos{25, 25, 1}));  // 5 by 5: 7.07 m

    // A wall on level 1 (on that floor) holds up a floor on level 2; the column below does not.
    CHECK_FALSE(sim->upperFloors().lay(CellPos{20, 20, 2}));
    LEVELS_REQUIRE(sim->things().spawn("Wall", CellPos{20, 20, 1}) != kNoEntity);
    CHECK(sim->upperFloors().lay(CellPos{21, 20, 2}));
}

TEST_CASE("Levels: no upper floor over a mountain") {
    // Find a cell under the mountain's thick roof that has a roof holder (rock) next to it.
    Sim sim(SimSettings{42, 128, 0, 0, false}, levelsDefs());
    const Map& map = sim.map();
    CellPos rock{-1, -1, 0};
    for (int y = 0; y < map.height() && rock.x < 0; ++y) {
        for (int x = 0; x < map.width(); ++x) {
            if (map.roof(CellPos{x, y, 0}) == Roof::Thick) {
                rock = CellPos{x, y, 0};
                break;
            }
        }
    }
    LEVELS_REQUIRE(rock.x >= 0);
    std::string why;
    CHECK_FALSE(sim.upperFloors().lay(CellPos{rock.x, rock.y, 1}, &why));
    CHECK(why == "the mountain is in the way");
}

TEST_CASE("Levels: buildings and items go on an upper level only where a floor is laid") {
    auto sim = levelsSim();
    levelsRing(*sim, 10, 10, 16, 16);
    CHECK(sim->things().spawn("Wall", CellPos{12, 12, 1}) == kNoEntity);   // air
    CHECK(sim->things().spawn("Steel", CellPos{12, 13, 1}) == kNoEntity);  // air
    CHECK(levelsFloor(*sim, 10, 10, 16, 16) == 49);
    const Entity wall = sim->things().spawn("Wall", CellPos{12, 12, 1});
    LEVELS_REQUIRE(wall != kNoEntity);
    CHECK(sim->map().buildingAt(CellPos{12, 12, 1}) == wall);
    CHECK(sim->map().buildingAt(CellPos{12, 12, 0}) == kNoEntity);
    CHECK_FALSE(sim->map().walkable(CellPos{12, 12, 1}));
    CHECK(sim->map().walkable(CellPos{12, 12, 0}));
    const Entity steel = sim->things().spawn("Steel", CellPos{12, 13, 1});
    LEVELS_REQUIRE(steel != kNoEntity);
    CHECK(sim->things().itemsAt(CellPos{12, 13, 1}).size() == 1);
    CHECK(sim->things().itemsAt(CellPos{12, 13, 0}).empty());

    // A plan on an upper level needs a floor too.
    sim->submit(PlaceBlueprintCommand{"Wall", "Steel", CellPos{30, 30, 1}, Rot4::North});
    sim->step();
    CHECK_FALSE(levelsFailure(*sim).empty());
    sim->submit(PlaceBlueprintCommand{"Wall", "Steel", CellPos{14, 14, 1}, Rot4::North});
    sim->step();
    CHECK(levelsFailure(*sim).empty());

    // A floor with something on it stays.
    std::string why;
    CHECK_FALSE(sim->upperFloors().remove(CellPos{12, 12, 1}, &why));
    CHECK(why == "a building stands on it");
    CHECK_FALSE(sim->upperFloors().remove(CellPos{12, 13, 1}, &why));
    CHECK(why == "something lies on it");
    const Entity pawn = sim->things().spawn("Human", CellPos{11, 15, 1});
    LEVELS_REQUIRE(pawn != kNoEntity);
    CHECK_FALSE(sim->upperFloors().remove(CellPos{11, 15, 1}, &why));
    CHECK(why == "someone stands on it");
    CHECK(sim->upperFloors().remove(CellPos{15, 11, 1}));
    CHECK_FALSE(sim->map().upperFloor(CellPos{15, 11, 1}));
}

TEST_CASE("Levels: the upper floor command lays and takes up floors and says why when it can't") {
    auto sim = levelsSim();
    sim->submit(PlaceUpperFloorCommand{CellRect{20, 20, 3, 3, 1}, false});
    sim->step();
    CHECK(levelsFailure(*sim).find("holds it up") != std::string::npos);
    levelsRing(*sim, 10, 10, 16, 16);
    // 9..17 is in reach of the walls; the whole 9 by 9 is laid.
    sim->submit(PlaceUpperFloorCommand{CellRect{9, 9, 9, 9, 1}, false});
    sim->step();
    CHECK(levelsFailure(*sim).empty());
    CHECK(sim->map().upperFloorCells().size() == 81);
    sim->submit(PlaceUpperFloorCommand{CellRect{9, 9, 9, 1, 1}, true});
    sim->step();
    CHECK(levelsFailure(*sim).empty());
    CHECK(sim->map().upperFloorCells().size() == 72);
    sim->submit(PlaceUpperFloorCommand{CellRect{9, 9, 9, 1, 1}, true});  // nothing left to take up
    sim->step();
    CHECK(levelsFailure(*sim) == "no upper floor changed there");
}

TEST_CASE("Levels: every level has its own regions, and paths on an upper floor go round its walls") {
    auto sim = levelsSim();
    levelsRing(*sim, 10, 10, 20, 20);
    Regions& regions = sim->regions();
    const size_t groundGroups = regions.groupCount();
    CHECK(regions.regionAt(CellPos{15, 15, 1}) == kNoRegion);
    CHECK(regions.regionIdsOnLevel(1) == nullptr);  // nothing stored up there yet
    CHECK(levelsFloor(*sim, 10, 10, 20, 20) == 121);
    const RegionId up = regions.regionAt(CellPos{15, 15, 1});
    LEVELS_REQUIRE(up != kNoRegion);
    CHECK(regions.region(up).bounds.level == 1);
    CHECK(regions.regionIdsOnLevel(1) != nullptr);
    // No stairs yet: the upper floor is a group of its own, not reachable from the ground.
    CHECK(regions.groupCount() == groundGroups + 1);
    CHECK_FALSE(regions.canReach(CellPos{15, 15, 0}, CellPos{15, 15, 1}));
    CHECK(regions.canReach(CellPos{10, 10, 1}, CellPos{20, 20, 1}));
    CHECK(regions.groupCellCount(regions.groupAt(CellPos{15, 15, 1})) == 121);

    // A wall across the upper floor with one gap: a path from one side to the other goes through it.
    for (int y = 10; y <= 20; ++y) {
        if (y != 18) {
            LEVELS_REQUIRE(sim->things().spawn("Wall", CellPos{15, y, 1}) != kNoEntity);
        }
    }
    std::vector<CellPos> path;
    LEVELS_REQUIRE(sim->pathfinder().findPath(CellPos{12, 12, 1}, CellPos{18, 12, 1}, path));
    CHECK(std::find(path.begin(), path.end(), CellPos{15, 18, 1}) != path.end());
    for (const CellPos& c : path) {
        CHECK(c.level == 1);
        CHECK(sim->map().walkable(c));
    }
    // Close the gap: two groups up there, and no path.
    LEVELS_REQUIRE(sim->things().spawn("Wall", CellPos{15, 18, 1}) != kNoEntity);
    CHECK_FALSE(regions.canReach(CellPos{12, 12, 1}, CellPos{18, 12, 1}));
    CHECK_FALSE(sim->pathfinder().findPath(CellPos{12, 12, 1}, CellPos{18, 12, 1}, path));
    // The ground under it is one room's worth of cells as before.
    CHECK(regions.canReach(CellPos{12, 12, 0}, CellPos{18, 12, 0}));
}

TEST_CASE("Levels: a floor over a room is its roof, and walls on the floor make an upper room with a thin roof") {
    auto sim = levelsSim();
    levelsRing(*sim, 10, 10, 16, 16);
    CHECK(sim->rooms().thinRoofCells().size() == 49);  // the walls' own thin roof
    CHECK(levelsFloor(*sim, 10, 10, 16, 16) == 49);
    // The floor took the thin roof's place: still roofed, nothing for Rooms to put up.
    CHECK(sim->rooms().thinRoofCells().empty());
    const Room* below = sim->rooms().roomOf(CellPos{13, 13, 0});
    LEVELS_REQUIRE(below != nullptr);
    CHECK(below->enclosed());
    CHECK_FALSE(below->outdoors());
    CHECK(sim->map().roof(CellPos{13, 13, 0}) == Roof::Thin);
    CHECK(sim->rooms().indoors(CellPos{13, 13, 0}));

    // A bare floor up there is open to the air: outdoors.
    const Room* open = sim->rooms().roomOf(CellPos{13, 13, 1});
    LEVELS_REQUIRE(open != nullptr);
    CHECK(open->bounds.level == 1);
    CHECK(open->openToAir);
    CHECK(open->outdoors());
    CHECK_FALSE(sim->rooms().indoors(CellPos{13, 13, 1}));

    // Walls round its edge: an enclosed room on level 1, roofed by Rooms.
    levelsRing(*sim, 10, 10, 16, 16, 1);
    const Room* up = sim->rooms().roomOf(CellPos{13, 13, 1});
    LEVELS_REQUIRE(up != nullptr);
    CHECK(up->enclosed());
    CHECK_FALSE(up->outdoors());
    CHECK(up->cellCount == 25);
    CHECK(up->anchor == CellPos{11, 11, 1});
    CHECK(sim->rooms().thinRoofCells().size() == 49);
    for (const CellPos& c : sim->rooms().thinRoofCells()) {
        CHECK(c.level == 1);
    }
    CHECK(sim->map().roof(CellPos{13, 13, 1}) == Roof::Thin);
    CHECK(sim->rooms().indoors(CellPos{13, 13, 1}));
    // The room below is a different room.
    CHECK(sim->rooms().roomAt(CellPos{13, 13, 0}) != sim->rooms().roomAt(CellPos{13, 13, 1}));

    // A floor wider than the walls: the ledge outside them is open to the air, the room inside is not.
    LEVELS_REQUIRE(sim->upperFloors().lay(CellPos{17, 13, 1}));
    sim->rooms().update();
    const Room* ledge = sim->rooms().roomOf(CellPos{17, 13, 1});
    LEVELS_REQUIRE(ledge != nullptr);
    CHECK(ledge->outdoors());
    up = sim->rooms().roomOf(CellPos{13, 13, 1});
    LEVELS_REQUIRE(up != nullptr);
    CHECK(up->enclosed());

    // Take a wall up there away: the upper room opens to the ledge and the air, its roof goes.
    sim->things().destroy(sim->map().buildingAt(CellPos{16, 13, 1}));
    sim->rooms().update();
    up = sim->rooms().roomOf(CellPos{13, 13, 1});
    LEVELS_REQUIRE(up != nullptr);
    CHECK(up->outdoors());
    CHECK(sim->rooms().thinRoofCells().empty());
    CHECK(sim->map().roof(CellPos{13, 13, 0}) == Roof::Thin);  // the floor still roofs the room below
}

TEST_CASE("Levels: an upper room has its own temperature, and heat goes through the floor between rooms") {
    auto sim = levelsSim();
    sim->setOutdoorOverride(0.0f);
    sim->temperature().setSourceRunning([](Entity) { return true; });
    levelsRing(*sim, 10, 10, 16, 16);
    CHECK(levelsFloor(*sim, 10, 10, 16, 16) == 49);
    levelsRing(*sim, 10, 10, 16, 16, 1);
    LEVELS_REQUIRE(sim->things().spawn("Heater", CellPos{12, 12, 1}) != kNoEntity);
    sim->rooms().update();
    const int64_t interval = sim->temperature().numbers().intervalSeconds;
    for (int64_t t = 0; t < 24 * 3600; t += interval) {
        sim->temperature().step(t);
    }
    const float upstairs = sim->temperature().temperatureAt(CellPos{13, 13, 1});
    const float downstairs = sim->temperature().temperatureAt(CellPos{13, 13, 0});
    CHECK(upstairs == doctest::Approx(21.0f).epsilon(0.001));
    // The room under it is warmer than the outdoors thanks to the floor between them.
    CHECK(downstairs > 1.5f);
    CHECK(downstairs < upstairs);
    CHECK(sim->temperature().temperatureAt(CellPos{5, 5, 0}) == doctest::Approx(0.0f));
}

TEST_CASE("Levels: upper floors and what stands on them are saved, and a loaded game is the same") {
    auto sim = levelsSim();
    levelsRing(*sim, 10, 10, 16, 16);
    CHECK(levelsFloor(*sim, 9, 9, 17, 17) == 81);
    levelsRing(*sim, 10, 10, 16, 16, 1);
    LEVELS_REQUIRE(sim->things().spawn("Steel", CellPos{13, 13, 1}) != kNoEntity);
    for (int i = 0; i < 30; ++i) {
        sim->step();
    }
    const SaveLoadResult loaded = loadSim(saveSim(*sim), levelsDefs());
    LEVELS_REQUIRE(loaded.sim != nullptr);
    Sim& copy = *loaded.sim;
    CHECK(copy.map().upperFloorCells() == sim->map().upperFloorCells());
    CHECK(copy.map().buildingAt(CellPos{10, 13, 1}) != kNoEntity);
    CHECK(sim->things().itemsAt(CellPos{13, 13, 1}).size() == 1);
    CHECK(copy.things().itemsAt(CellPos{13, 13, 1}).size() == 1);
    const Room* up = copy.rooms().roomOf(CellPos{13, 13, 1});
    LEVELS_REQUIRE(up != nullptr);
    CHECK(up->enclosed());
    CHECK(copy.stateHash() == sim->stateHash());
    for (int i = 0; i < 30; ++i) {
        sim->step();
        copy.step();
    }
    CHECK(copy.stateHash() == sim->stateHash());

    // The command that lays them goes through a save's command log and a replay file too.
    sim->submit(PlaceUpperFloorCommand{CellRect{9, 18, 3, 1, 1}, false});
    sim->step();
    const SaveLoadResult again = loadSim(saveSim(*sim), levelsDefs());
    LEVELS_REQUIRE(again.sim != nullptr);
    CHECK(again.sim->map().upperFloor(CellPos{10, 18, 1}));
    CHECK(again.sim->stateHash() == sim->stateHash());
}

TEST_CASE("Levels: the snapshot puts a thing on an upper floor at the floor's height") {
    auto sim = levelsSim();
    levelsRing(*sim, 10, 10, 16, 16);
    CHECK(levelsFloor(*sim, 10, 10, 16, 16) == 49);
    const Entity steel = sim->things().spawn("Steel", CellPos{13, 13, 1});
    LEVELS_REQUIRE(steel != kNoEntity);
    sim->step();
    Snapshot snap;
    sim->writeSnapshot(snap);
    bool found = false;
    for (const ThingView& view : snap.things) {
        if (view.id == steel) {
            found = true;
            CHECK(view.level == 1);
            CHECK(view.z == doctest::Approx(sim->map().heightAt(view.x, view.y) + Map::kLevelHeightM));
        }
    }
    CHECK(found);
}
