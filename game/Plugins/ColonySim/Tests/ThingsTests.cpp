#include "ColonySim/Map.h"
#include "ColonySim/Pathfinder.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include "TestDefs.h"

#include <doctest.h>

#include <vector>

using namespace colony;

namespace {

// A flat 32 m test map with its things.
struct ThingsTestWorld {
    Map map{MapSettings{32, 32, 1, true}};
    Things things{map, testDefs()};
};

}  // namespace

TEST_CASE("Things: an item gets its def, a cell-centre position, a stack and hit points") {
    ThingsTestWorld w;
    const Entity steel = w.things.spawn("Steel", CellPos{4, 7, 0}, 30);
    if (!(steel != kNoEntity)) {
        CHECK(false);
        return;
    }
    const entt::registry& reg = w.things.registry();
    CHECK(reg.get<Thing>(steel).category == ThingCategory::Item);
    CHECK(w.things.defs().get(reg.get<Thing>(steel).def).defName() == "Steel");
    CHECK(reg.get<Position>(steel).x == doctest::Approx(4.5f));
    CHECK(reg.get<Position>(steel).y == doctest::Approx(7.5f));
    CHECK(cellOf(reg.get<Position>(steel)) == CellPos{4, 7, 0});
    CHECK(reg.get<StackCount>(steel).count == 30);
    CHECK(reg.get<HitPoints>(steel).current == 100);
    CHECK(reg.get<HitPoints>(steel).max == 100);
    CHECK_FALSE(reg.all_of<Footprint>(steel));
    CHECK_FALSE(reg.all_of<HitPoints>(w.things.spawn("WoodLog", CellPos{5, 7, 0})));  // no maxHitPoints in its def
    CHECK(w.things.count() == 2);
}

TEST_CASE("Things: stacks are clamped to the def's stack limit") {
    ThingsTestWorld w;
    const entt::registry& reg = w.things.registry();
    CHECK(reg.get<StackCount>(w.things.spawn("Steel", CellPos{1, 1, 0}, 500)).count == 75);
    CHECK(reg.get<StackCount>(w.things.spawn("Steel", CellPos{1, 1, 0}, 0)).count == 1);
}

TEST_CASE("Things: a pawn moves at its def's speed") {
    ThingsTestWorld w;
    const Entity pawn = w.things.spawn("Human", CellPos{10, 10, 0});
    if (!(pawn != kNoEntity)) {
        CHECK(false);
        return;
    }
    const entt::registry& reg = w.things.registry();
    CHECK(reg.get<Thing>(pawn).category == ThingCategory::Pawn);
    CHECK(reg.get<Mover>(pawn).metresPerStep == doctest::Approx(0.15333f));
    CHECK_FALSE(reg.all_of<HitPoints>(pawn));
    CHECK_FALSE(reg.all_of<StackCount>(pawn));
}

TEST_CASE("Things: unknown defs, other def kinds and plants make nothing") {
    ThingsTestWorld w;
    CHECK(w.things.spawn("NoSuchThing", CellPos{1, 1, 0}) == kNoEntity);
    CHECK(w.things.spawn("Plant_Grass", CellPos{1, 1, 0}) == kNoEntity);
    CHECK(w.things.spawn(w.things.defs().idOf("Terrain", "Soil"), CellPos{1, 1, 0}) == kNoEntity);
    CHECK(w.things.spawn(DefId{}, CellPos{1, 1, 0}) == kNoEntity);
    CHECK(w.things.count() == 0);
}

TEST_CASE("Things: nothing spawns off the map or on another level") {
    ThingsTestWorld w;
    CHECK(w.things.spawn("Steel", CellPos{32, 0, 0}) == kNoEntity);
    CHECK(w.things.spawn("Human", CellPos{-1, 3, 0}) == kNoEntity);
    CHECK(w.things.spawn("Wall", CellPos{3, 3, 1}) == kNoEntity);
    CHECK(w.things.spawn("Bed", CellPos{31, 31, 0}) == kNoEntity);  // second cell would be off the map
    CHECK(w.things.count() == 0);
}

TEST_CASE("Things: a building fills the building layer under its footprint") {
    ThingsTestWorld w;
    const Entity bed = w.things.spawn("Bed", CellPos{5, 5, 0});
    if (!(bed != kNoEntity)) {
        CHECK(false);
        return;
    }
    CHECK(w.map.buildingAt(CellPos{5, 5, 0}) == bed);
    CHECK(w.map.buildingAt(CellPos{5, 6, 0}) == bed);
    CHECK(w.map.buildingAt(CellPos{5, 7, 0}) == kNoEntity);
    CHECK(w.map.buildingAt(CellPos{6, 5, 0}) == kNoEntity);
    const Position& pos = w.things.registry().get<Position>(bed);
    CHECK(pos.x == doctest::Approx(5.5f));
    CHECK(pos.y == doctest::Approx(6.0f));
    CHECK(w.map.walkable(CellPos{5, 5, 0}));  // beds don't block movement

    // Overlapping another building fails and leaves the first one alone.
    CHECK(w.things.spawn("Wall", CellPos{5, 6, 0}) == kNoEntity);
    CHECK(w.map.buildingAt(CellPos{5, 6, 0}) == bed);
    CHECK(w.things.count() == 1);
}

TEST_CASE("Things: rotation turns the footprint") {
    ThingsTestWorld w;
    const Entity north = w.things.spawn("FueledStove", CellPos{2, 2, 0}, 1, Rot4::North);
    const Entity east = w.things.spawn("FueledStove", CellPos{10, 10, 0}, 1, Rot4::East);
    if (!(north != kNoEntity)) {
        CHECK(false);
        return;
    }
    if (!(east != kNoEntity)) {
        CHECK(false);
        return;
    }
    const CellRect a = w.things.registry().get<Footprint>(north).rect();
    const CellRect b = w.things.registry().get<Footprint>(east).rect();
    CHECK(a.sizeX == 3);
    CHECK(a.sizeY == 1);
    CHECK(b.sizeX == 1);
    CHECK(b.sizeY == 3);
    CHECK(w.map.buildingAt(CellPos{4, 2, 0}) == north);
    CHECK(w.map.buildingAt(CellPos{10, 12, 0}) == east);
    CHECK(w.map.buildingAt(CellPos{11, 10, 0}) == kNoEntity);
    CHECK(w.things.registry().get<Footprint>(east).rotation == Rot4::East);
}

TEST_CASE("Things: a wall blocks paths and destroying it opens them again") {
    ThingsTestWorld w;
    // A wall across the whole map at x = 16.
    std::vector<Entity> walls;
    for (int y = 0; y < 32; ++y) {
        walls.push_back(w.things.spawn("Wall", CellPos{16, y, 0}));
        if (!(walls.back() != kNoEntity)) {
        CHECK(false);
        return;
    }
    }
    CHECK_FALSE(w.map.walkable(CellPos{16, 5, 0}));
    Pathfinder finder(w.map);
    std::vector<CellPos> path;
    CHECK_FALSE(finder.findPath(CellPos{2, 10, 0}, CellPos{30, 10, 0}, path));

    w.things.destroy(walls[20]);
    CHECK_FALSE(w.things.exists(walls[20]));
    CHECK(w.map.buildingAt(CellPos{16, 20, 0}) == kNoEntity);
    CHECK(w.map.walkable(CellPos{16, 20, 0}));
    if (!(finder.findPath(CellPos{2, 10, 0}, CellPos{30, 10, 0}, path))) {
        CHECK(false);
        return;
    }
    bool throughGap = false;
    for (const CellPos& c : path) {
        CHECK((c.x != 16 || c.y == 20));
        throughGap = throughGap || c == CellPos{16, 20, 0};
    }
    CHECK(throughGap);
}

TEST_CASE("Things: despawn takes a thing off the map and spawnAt puts it back") {
    ThingsTestWorld w;
    const Entity wall = w.things.spawn("Wall", CellPos{8, 8, 0});
    const Entity steel = w.things.spawn("Steel", CellPos{3, 3, 0}, 10);
    w.things.despawn(wall);
    w.things.despawn(steel);
    CHECK(w.things.exists(wall));
    CHECK_FALSE(w.things.spawned(wall));
    CHECK_FALSE(w.things.spawned(steel));
    CHECK(w.map.buildingAt(CellPos{8, 8, 0}) == kNoEntity);
    CHECK(w.map.walkable(CellPos{8, 8, 0}));
    CHECK(w.things.registry().get<StackCount>(steel).count == 10);  // components stay

    CHECK(w.things.spawnAt(steel, CellPos{20, 21, 0}));
    CHECK(cellOf(w.things.registry().get<Position>(steel)) == CellPos{20, 21, 0});
    CHECK_FALSE(w.things.spawnAt(steel, CellPos{1, 1, 0}));  // already spawned
    CHECK(w.things.spawnAt(wall, CellPos{9, 9, 0}));
    CHECK(w.map.buildingAt(CellPos{9, 9, 0}) == wall);
    CHECK_FALSE(w.map.walkable(CellPos{9, 9, 0}));
}

TEST_CASE("Things: buildings can't go on water or rock") {
    Map map(MapSettings{256, 256, 3});
    Things things(map, testDefs());
    int tried = 0;
    for (int y = 0; y < 256; ++y) {
        for (int x = 0; x < 256; ++x) {
            const CellPos c{x, y, 0};
            if (!map.groundWalkable(c)) {
                CHECK(things.spawn("Wall", c) == kNoEntity);
                CHECK(things.spawn("Steel", c) == kNoEntity);
                ++tried;
            }
        }
    }
    CHECK(tried > 0);  // this seed has water or rock on the map
    CHECK(things.count() == 0);
}

TEST_CASE("Things: building changes show in the map hash") {
    ThingsTestWorld w;
    const uint64_t before = w.map.contentHash();
    const Entity wall = w.things.spawn("Wall", CellPos{3, 3, 0});
    CHECK(w.map.contentHash() != before);
    w.things.destroy(wall);
    CHECK(w.map.contentHash() == before);
}
