// Wall climbing (sneed 2026-11-19, docs/design/climbing.md): the outside face of a full-height wall is a
// climb onto the upper floor laid over it, for every pawn; cat-strain climb twice as fast; the snapshot
// draws the pawn against the face and then mantling over the top.

#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Genes.h"
#include "ColonySim/Map.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Pathfinder.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"
#include "ColonySim/UpperFloors.h"

#include <doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define CLIMB_REQUIRE(...)                                   \
    do {                                                     \
        const bool climbOk = static_cast<bool>(__VA_ARGS__); \
        CHECK_MESSAGE(climbOk, #__VA_ARGS__);                \
        if (!climbOk) {                                      \
            return;                                          \
        }                                                    \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> climbDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

std::unique_ptr<Sim> climbSim() { return std::make_unique<Sim>(SimSettings{7, 48, 0, 0, true}, climbDefs()); }

// A ring of walls round 10..16 by 10..16 with a gap at (13, 10), and an upper floor laid exactly over
// the ring and what it holds: no stairs, so the only way up is a wall.
void climbHouse(Sim& sim) {
    for (int y = 10; y <= 16; ++y) {
        for (int x = 10; x <= 16; ++x) {
            if ((x == 10 || x == 16 || y == 10 || y == 16) && !(x == 13 && y == 10)) {
                CHECK(sim.things().spawn("Wall", CellPos{x, y, 0}) != kNoEntity);
            }
        }
    }
    sim.rooms().update();
    for (int y = 10; y <= 16; ++y) {
        for (int x = 10; x <= 16; ++x) {
            CHECK(sim.upperFloors().lay(CellPos{x, y, 1}));
        }
    }
    sim.rooms().update();
}

size_t climbWallLinks(const Map& map) {
    return static_cast<size_t>(std::count_if(map.levelLinks().begin(), map.levelLinks().end(), [](const LevelLink& l) { return l.wall; }));
}

bool climbWalk(Sim& sim, Entity pawn, CellPos goal) {
    entt::registry& reg = sim.things().registry();
    const Position& pos = reg.get<Position>(pawn);
    std::vector<CellPos> path;
    if (!sim.pathfinder().findPath(cellOf(pos), goal, path)) {
        return false;
    }
    smoothPath(sim.map(), pos.x, pos.y, path, pos.level);
    reg.get<Mover>(pawn).walkTo(pos.x, pos.y, std::move(path));
    return true;
}

// Steps until the pawn stands on level 1; the steps it spent climbing (-1 if it never got there).
int climbSteps(Sim& sim, Entity pawn, bool* sawFace = nullptr, bool* sawMantle = nullptr) {
    entt::registry& reg = sim.things().registry();
    int start = -1;
    for (int step = 0; step < 2000; ++step) {
        sim.step();
        const Mover& mover = reg.get<Mover>(pawn);
        if (start < 0 && mover.climbed > 0.0f) {
            start = step;
        }
        if (start >= 0 && mover.climbed > 0.0f && (sawFace != nullptr || sawMantle != nullptr)) {
            Snapshot snap;
            sim.writeSnapshot(snap);
            for (const ThingView& view : snap.things) {
                if (view.id == pawn && view.wallClimb) {
                    if (sawFace != nullptr && view.climb > 0.3f && view.climb < 0.6f) {
                        const Position& pos = reg.get<Position>(pawn);
                        // Against the face, part way up, not yet over the top.
                        *sawFace = *sawFace || (view.z > sim.map().heightAt(pos.x, pos.y) + 0.6f && view.z < sim.map().heightAt(pos.x, pos.y) + 2.0f);
                    }
                    if (sawMantle != nullptr && view.climb > kWallMantleFrom + 0.05f) {
                        *sawMantle = true;
                    }
                }
            }
        }
        if (start >= 0 && reg.get<Position>(pawn).level == 1) {
            return step - start;
        }
    }
    return -1;
}

}  // namespace

TEST_CASE("Climbing: every full-height wall under an upper floor can be climbed from the open air in front of it") {
    auto sim = climbSim();
    CHECK(sim->map().wallClimbSpeed() == doctest::Approx(0.15f));
    climbHouse(*sim);
    // 23 walls: the 19 along the sides have one face outside, the 4 corners two; none inside (under the floor),
    // none at the gap.
    CHECK(climbWallLinks(sim->map()) == 27);
    const LevelLink* west = sim->map().levelLinkBetween(CellPos{9, 13, 0}, CellPos{10, 13, 1});
    CLIMB_REQUIRE(west != nullptr);
    CHECK(west->wall);
    CHECK(west->length == doctest::Approx(3.5f));
    CHECK(sim->map().levelLinkBetween(CellPos{11, 13, 0}, CellPos{10, 13, 1}) == nullptr);  // from inside: through the ceiling
    CHECK(sim->map().linkAt(CellPos{9, 13, 0}));
    CHECK_FALSE(sim->map().linkAt(CellPos{8, 13, 0}));

    // The floor over a wall taken up: no climb there. Laid again: back.
    CHECK(sim->upperFloors().remove(CellPos{10, 13, 1}));
    CHECK(sim->map().levelLinkBetween(CellPos{9, 13, 0}, CellPos{10, 13, 1}) == nullptr);
    CHECK(sim->upperFloors().lay(CellPos{10, 13, 1}));
    CHECK(sim->map().levelLinkBetween(CellPos{9, 13, 0}, CellPos{10, 13, 1}) != nullptr);

    // A floor laid over the cell in front roofs the face: no climb from under it.
    CHECK(sim->upperFloors().lay(CellPos{9, 13, 1}));
    CHECK(sim->map().levelLinkBetween(CellPos{9, 13, 0}, CellPos{10, 13, 1}) == nullptr);

    // A save loads with the same climbs.
    const SaveLoadResult loaded = loadSim(saveSim(*sim), climbDefs());
    CLIMB_REQUIRE(loaded.sim != nullptr);
    CHECK(climbWallLinks(loaded.sim->map()) == climbWallLinks(sim->map()));
    CHECK(loaded.sim->stateHash() == sim->stateHash());
}

TEST_CASE("Climbing: a pawn climbs a wall to the floor above, cat-strain twice as fast, drawn on the face then over the top") {
    int human = -1;
    int cat = -1;
    for (const bool isCat : {false, true}) {
        CAPTURE(isCat);
        auto sim = climbSim();
        climbHouse(*sim);
        const Entity pawn = sim->things().spawn("Human", CellPos{5, 13, 0});
        CLIMB_REQUIRE(pawn != kNoEntity);
        if (isCat) {
            PawnGenes genes;
            std::string error;
            CLIMB_REQUIRE(makeGenes(sim->defs(), "CatStrain", genes, error));
            CHECK(genes.effects.climbSpeed == 2.0f);
            sim->things().registry().emplace_or_replace<PawnGenes>(pawn, genes);
        }
        CLIMB_REQUIRE(climbWalk(*sim, pawn, CellPos{13, 13, 1}));
        bool face = false;
        bool mantle = false;
        const int steps = climbSteps(*sim, pawn, &face, &mantle);
        CLIMB_REQUIRE(steps > 0);
        CHECK(face);
        CHECK(mantle);
        (isCat ? cat : human) = steps;
        // On to the goal upstairs.
        entt::registry& reg = sim->things().registry();
        for (int step = 0; step < 300 && !reg.get<Mover>(pawn).arrived(); ++step) {
            sim->step();
        }
        CHECK(cellOf(reg.get<Position>(pawn)) == CellPos{13, 13, 1});
    }
    CLIMB_REQUIRE(human > 0);
    CLIMB_REQUIRE(cat > 0);
    // 3.5 m at 15% of walking: a human takes a while; a cat half as long.
    CHECK(static_cast<float>(cat) == doctest::Approx(static_cast<float>(human) / 2.0f).epsilon(0.1));
}

TEST_CASE("Climbing: a pawn on top of a wall climbs back down the same face") {
    auto sim = climbSim();
    climbHouse(*sim);
    const Entity pawn = sim->things().spawn("Human", CellPos{9, 13, 0});
    CLIMB_REQUIRE(pawn != kNoEntity);
    CLIMB_REQUIRE(climbWalk(*sim, pawn, CellPos{10, 13, 1}));
    CLIMB_REQUIRE(climbSteps(*sim, pawn) > 0);
    CHECK(cellOf(sim->things().registry().get<Position>(pawn)) == CellPos{10, 13, 1});
    // And back down the same face.
    CLIMB_REQUIRE(climbWalk(*sim, pawn, CellPos{6, 13, 0}));
    entt::registry& reg = sim->things().registry();
    for (int step = 0; step < 1500 && !reg.get<Mover>(pawn).arrived(); ++step) {
        sim->step();
    }
    CHECK(cellOf(reg.get<Position>(pawn)) == CellPos{6, 13, 0});
}
