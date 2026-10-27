// Roof support and collapse (task M3, docs/design/roofs.md): built roofs that lose their walls, the
// mountain's roof over a cave dug too wide, and upper floors that lose what holds them up.

#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Health.h"
#include "ColonySim/Map.h"
#include "ColonySim/RoofCollapse.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"
#include "ColonySim/UpperFloors.h"

#include <doctest.h>

#include <memory>
#include <string>
#include <vector>

using namespace colony;

#define ROOF_REQUIRE(...)                                 \
    do {                                                  \
        const bool roofOk = static_cast<bool>(__VA_ARGS__); \
        CHECK_MESSAGE(roofOk, #__VA_ARGS__);              \
        if (!roofOk) {                                    \
            return;                                       \
        }                                                 \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> roofDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

std::unique_ptr<Sim> roofSim() {
    return std::make_unique<Sim>(SimSettings{5, 64, 0, 0, true}, roofDefs());
}

// A ring of walls round x0..x1 by y0..y1 on `level`; the walls are returned.
std::vector<Entity> roofRing(Sim& sim, int x0, int y0, int x1, int y1, int level = 0) {
    std::vector<Entity> walls;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            if (x == x0 || x == x1 || y == y0 || y == y1) {
                const Entity wall = sim.things().spawn("Wall", CellPos{x, y, level});
                CHECK(wall != kNoEntity);
                walls.push_back(wall);
            }
        }
    }
    sim.rooms().update();
    return walls;
}

void roofFloor(Sim& sim, int x0, int y0, int x1, int y1, int level) {
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            CHECK(sim.upperFloors().lay(CellPos{x, y, level}));
        }
    }
    sim.rooms().update();
}

void roofDestroy(Sim& sim, const std::vector<Entity>& things) {
    for (const Entity e : things) {
        sim.things().destroy(e);
    }
}

// The RoofCollapsed events of the last step with this message.
std::vector<Event> roofEvents(const Sim& sim, const char* message) {
    std::vector<Event> out;
    for (const Event& e : sim.events()) {
        if (e.type == EventType::RoofCollapsed && e.message == message) {
            out.push_back(e);
        }
    }
    return out;
}

// The sim's map for a test to shape: nothing else changes the rock.
Map& roofMap(Sim& sim) {
    return const_cast<Map&>(sim.map());
}

// Rock over x0..x1 by y0..y1 with the mountain's roof (the test map has no mountain of its own).
void roofMountain(Sim& sim, int x0, int y0, int x1, int y1) {
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            roofMap(sim).setTerrain(CellPos{x, y, 0}, Terrain::Rock);
            roofMap(sim).setThickRoof(CellPos{x, y, 0}, true);
        }
    }
}

void roofMine(Sim& sim, int x0, int y0, int x1, int y1) {
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            roofMap(sim).setTerrain(CellPos{x, y, 0}, Terrain::RoughStone);
        }
    }
}

}  // namespace

TEST_CASE("Roofs: a built roof stays while one wall is gone and falls when the last holder goes") {
    auto sim = roofSim();
    const std::vector<Entity> walls = roofRing(*sim, 10, 10, 16, 16);
    sim->step();
    const CellPos middle{13, 13, 0};
    ROOF_REQUIRE(sim->map().roof(middle) == Roof::Thin);
    const uint64_t before = sim->roofCollapse().collapses();

    // Half the ring: the roof is still within reach of the rest.
    roofDestroy(*sim, std::vector<Entity>(walls.begin(), walls.begin() + 12));
    sim->step();
    CHECK(sim->roofCollapse().collapses() == before);
    CHECK(roofEvents(*sim, "roof").empty());

    // A pawn and a stack of steel under the roof when the last of it goes.
    const Entity pawn = sim->things().spawn("Human", middle);
    const Entity steel = sim->things().spawn("Steel", CellPos{12, 13, 0}, 5);
    ROOF_REQUIRE(pawn != kNoEntity);
    ROOF_REQUIRE(steel != kNoEntity);
    roofDestroy(*sim, std::vector<Entity>(walls.begin() + 12, walls.end()));
    sim->step();
    CHECK(sim->map().roof(middle) == Roof::None);
    const std::vector<Event> fell = roofEvents(*sim, "roof");
    CHECK_FALSE(fell.empty());
    CHECK(sim->roofCollapse().collapses() > before);
    const Health& health = sim->things().registry().get<Health>(pawn);
    CHECK((!health.injuries.empty() || health.dead));
    bool hurtPawn = false;
    for (const Event& e : fell) {
        hurtPawn = hurtPawn || (e.cell == middle && e.count >= 1);
    }
    CHECK(hurtPawn);
    Snapshot snap;
    sim->writeSnapshot(snap);
    CHECK(snap.roofCollapses == sim->roofCollapse().collapses());
}

TEST_CASE("Roofs: opening a wall under a roof that other walls still hold drops nothing") {
    auto sim = roofSim();
    const std::vector<Entity> walls = roofRing(*sim, 10, 10, 16, 16);
    sim->step();
    const Entity pawn = sim->things().spawn("Human", CellPos{13, 13, 0});
    ROOF_REQUIRE(pawn != kNoEntity);
    sim->things().destroy(walls.front());
    for (int i = 0; i < 5; ++i) {
        sim->step();
    }
    CHECK(sim->roofCollapse().collapses() == 0);
    CHECK(sim->things().registry().get<Health>(pawn).injuries.empty());
}

TEST_CASE("Roofs: the mountain's roof falls over a cave dug wider than the support reach") {
    auto sim = roofSim();
    roofMountain(*sim, 10, 10, 40, 40);
    CHECK(sim->map().hollowRoofCells().empty());

    // A tunnel three wide never falls: rock is always in reach.
    roofMine(*sim, 11, 25, 39, 27);
    sim->step();
    CHECK(sim->roofCollapse().collapses() == 0);
    CHECK(sim->map().roof(CellPos{25, 26, 0}) == Roof::Thick);

    // A cave 19 by 19: its middle is more than 6.9 m from any rock.
    roofMine(*sim, 15, 15, 33, 33);
    const Entity pawn = sim->things().spawn("Human", CellPos{24, 24, 0});
    const Entity steel = sim->things().spawn("Steel", CellPos{23, 24, 0}, 5);
    ROOF_REQUIRE(pawn != kNoEntity);
    ROOF_REQUIRE(steel != kNoEntity);
    sim->step();
    CHECK(sim->map().roof(CellPos{24, 24, 0}) == Roof::None);
    CHECK(sim->map().roof(CellPos{15, 15, 0}) == Roof::Thick);  // next to the rock it stays
    CHECK(sim->map().roof(CellPos{24, 16, 0}) == Roof::Thick);
    const std::vector<Event> fell = roofEvents(*sim, "mountain");
    CHECK_FALSE(fell.empty());
    CHECK(sim->roofCollapse().collapses() == fell.size());
    CHECK(sim->roofCollapse().fallenThick().size() == fell.size());
    // What was under the middle is crushed: the steel is gone, the pawn dead or hurt.
    CHECK_FALSE(sim->things().registry().valid(steel));
    const Health& health = sim->things().registry().get<Health>(pawn);
    CHECK((!health.injuries.empty() || health.dead));
    // The fallen cells do not fall twice.
    const uint64_t falls = sim->roofCollapse().collapses();
    for (int i = 0; i < 5; ++i) {
        sim->step();
    }
    CHECK(sim->roofCollapse().collapses() == falls);
}

TEST_CASE("Roofs: a wall inside a cave holds the mountain's roof up") {
    auto sim = roofSim();
    roofMountain(*sim, 10, 10, 40, 40);
    roofMine(*sim, 15, 15, 33, 33);
    // One column in the middle reaches every cell the rock does not (the cave is dug and furnished
    // before the step that looks at it).
    const Entity column = sim->things().spawn("Wall", CellPos{24, 24, 0});
    ROOF_REQUIRE(column != kNoEntity);
    sim->step();
    CHECK(sim->roofCollapse().collapses() == 0);
    CHECK(sim->map().roof(CellPos{22, 22, 0}) == Roof::Thick);
    // Take it away again and the middle falls.
    sim->things().destroy(column);
    sim->step();
    CHECK(sim->map().roof(CellPos{22, 22, 0}) == Roof::None);
    CHECK(sim->map().roof(CellPos{15, 15, 0}) == Roof::Thick);
}

TEST_CASE("Roofs: an upper floor falls with its things when the walls under it go") {
    auto sim = roofSim();
    const std::vector<Entity> walls = roofRing(*sim, 10, 10, 16, 16);
    roofFloor(*sim, 9, 9, 17, 17, 1);
    const CellPos up{13, 13, 1};
    const Entity pawn = sim->things().spawn("Human", up);
    const Entity steel = sim->things().spawn("Steel", CellPos{12, 13, 1}, 5);
    const Entity table = sim->things().spawn("Table1x2", CellPos{14, 14, 1});
    ROOF_REQUIRE(pawn != kNoEntity);
    ROOF_REQUIRE(steel != kNoEntity);
    ROOF_REQUIRE(table != kNoEntity);
    sim->step();
    CHECK(sim->roofCollapse().collapses() == 0);
    CHECK(sim->map().upperFloor(up));

    // One wall short: the floor still has the rest to hold it.
    sim->things().destroy(walls.front());
    sim->step();
    CHECK(sim->map().upperFloor(up));

    roofDestroy(*sim, std::vector<Entity>(walls.begin() + 1, walls.end()));
    sim->step();
    CHECK_FALSE(sim->map().upperFloor(up));
    CHECK(sim->map().upperFloorCells().empty());
    CHECK_FALSE(roofEvents(*sim, "floor").empty());
    // The pawn and the steel dropped to the ground below; the table is lost.
    const entt::registry& reg = sim->things().registry();
    ROOF_REQUIRE(reg.valid(pawn));
    CHECK(reg.get<Position>(pawn).level == 0);
    CHECK(reg.get<Health>(pawn).injuries.size() >= 1);
    ROOF_REQUIRE(reg.valid(steel));
    CHECK(reg.get<Position>(steel).level == 0);
    CHECK_FALSE(reg.valid(table));
}

TEST_CASE("Roofs: floors fall in a chain when the walls under the lowest go") {
    auto sim = roofSim();
    const std::vector<Entity> ground = roofRing(*sim, 10, 10, 16, 16);
    roofFloor(*sim, 9, 9, 17, 17, 1);
    roofRing(*sim, 10, 10, 16, 16, 1);
    roofFloor(*sim, 9, 9, 17, 17, 2);
    CHECK(sim->map().upperFloorCells().size() == 162);
    sim->step();
    CHECK(sim->roofCollapse().collapses() == 0);

    roofDestroy(*sim, ground);
    sim->step();
    // Both floors, in the one step, and the walls that stood on the first.
    CHECK(sim->map().upperFloorCells().empty());
    CHECK(roofEvents(*sim, "floor").size() == 162);
    CHECK(sim->map().buildingAt(CellPos{10, 10, 1}) == kNoEntity);
}

TEST_CASE("Roofs: fallen mountain roofs are put back from a saved state") {
    auto a = roofSim();
    roofMountain(*a, 10, 10, 40, 40);
    roofMine(*a, 15, 15, 33, 33);
    a->step();
    ROOF_REQUIRE(a->roofCollapse().collapses() > 0);

    // The map is made from its seed again on load and the mining is put back, so the same cave under
    // the same roof; the fallen cells are what the save adds.
    auto b = roofSim();
    roofMountain(*b, 10, 10, 40, 40);
    roofMine(*b, 15, 15, 33, 33);
    ROOF_REQUIRE(b->roofCollapse().restore(a->roofCollapse().state()));
    CHECK(b->map().roof(CellPos{24, 24, 0}) == Roof::None);
    CHECK(b->map().roof(CellPos{15, 15, 0}) == Roof::Thick);
    CHECK(b->roofCollapse().fallenThick() == a->roofCollapse().fallenThick());
    b->step();
    CHECK(b->roofCollapse().collapses() == a->roofCollapse().collapses());  // nothing falls a second time
    CHECK(b->map().contentHash() == a->map().contentHash());
}

TEST_CASE("Roofs: a roof about to fall is saved, and a loaded game drops it the same") {
    auto sim = roofSim();
    const std::vector<Entity> walls = roofRing(*sim, 10, 10, 16, 16);
    sim->step();
    const Entity pawn = sim->things().spawn("Human", CellPos{13, 13, 0});
    ROOF_REQUIRE(pawn != kNoEntity);
    roofDestroy(*sim, walls);
    sim->rooms().update();  // the roofs are known to be unsupported, but have not fallen yet
    CHECK_FALSE(sim->rooms().unsupportedRoofs().empty());

    const SaveLoadResult loaded = loadSim(saveSim(*sim), roofDefs());
    ROOF_REQUIRE(loaded.sim != nullptr);
    Sim& copy = *loaded.sim;
    CHECK(copy.rooms().unsupportedRoofs() == sim->rooms().unsupportedRoofs());
    CHECK(copy.stateHash() == sim->stateHash());
    sim->step();
    copy.step();
    CHECK(sim->roofCollapse().collapses() > 0);
    CHECK(copy.roofCollapse().collapses() == sim->roofCollapse().collapses());
    CHECK(copy.stateHash() == sim->stateHash());
}

TEST_CASE("Roofs: a roof kept up by a wall over an opened room is saved") {
    auto sim = roofSim();
    const std::vector<Entity> walls = roofRing(*sim, 10, 10, 16, 16);
    sim->step();
    roofDestroy(*sim, std::vector<Entity>(walls.begin(), walls.begin() + 12));
    sim->step();
    const std::vector<CellPos> thin = sim->rooms().thinRoofCells();
    CHECK_FALSE(thin.empty());
    CHECK(sim->map().roof(CellPos{13, 13, 0}) == Roof::Thin);

    const SaveLoadResult loaded = loadSim(saveSim(*sim), roofDefs());
    ROOF_REQUIRE(loaded.sim != nullptr);
    loaded.sim->step();
    CHECK(loaded.sim->rooms().thinRoofCells() == thin);
    CHECK(loaded.sim->map().roof(CellPos{13, 13, 0}) == Roof::Thin);
    CHECK(loaded.sim->roofCollapse().collapses() == 0);
}
