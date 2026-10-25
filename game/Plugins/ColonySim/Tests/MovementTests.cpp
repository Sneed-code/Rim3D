#include "ColonySim/Map.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Pathfinder.h"
#include "ColonySim/Random.h"
#include "ColonySim/Regions.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include "TestDefs.h"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define MOVE_REQUIRE(...)      \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// Human speed in the test defs, and what it is on soil.
constexpr float kMoveTestSpeed = 0.15333f;

Sim moveTestSim(int size, uint64_t seed = 5) { return Sim(SimSettings{seed, size, 0, 0, true}, testDefs()); }

Entity moveTestPawn(Sim& sim, int x, int y) { return sim.things().spawn("Human", CellPos{x, y, 0}); }

void moveTestWalk(Sim& sim, Entity pawn, std::vector<CellPos> path) {
    const Position& pos = sim.things().registry().get<Position>(pawn);
    sim.things().registry().get<Mover>(pawn).walkTo(pos.x, pos.y, std::move(path));
}

const Position& moveTestPos(const Sim& sim, Entity pawn) { return sim.things().registry().get<Position>(pawn); }
const Mover& moveTestMover(const Sim& sim, Entity pawn) { return sim.things().registry().get<Mover>(pawn); }

float moveTestDistance(const Sim& sim, Entity a, Entity b) {
    const Position& pa = moveTestPos(sim, a);
    const Position& pb = moveTestPos(sim, b);
    return std::sqrt((pa.x - pb.x) * (pa.x - pb.x) + (pa.y - pb.y) * (pa.y - pb.y));
}

void moveTestWall(Sim& sim, int x0, int y0, int x1, int y1) {
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            sim.things().spawn("Wall", CellPos{x, y, 0});
        }
    }
}

}  // namespace

TEST_CASE("Movement: ground speed follows RimWorld's path costs") {
    CHECK(terrainSpeedFactor(Terrain::Soil) == doctest::Approx(0.87f).epsilon(0.005));
    CHECK(terrainSpeedFactor(Terrain::Sand) == doctest::Approx(0.76f).epsilon(0.005));
    CHECK(terrainSpeedFactor(Terrain::Sand) < terrainSpeedFactor(Terrain::Soil));
}

TEST_CASE("Movement: slope speed is 1 on the flat, faster on a gentle downhill, slower uphill") {
    CHECK(slopeSpeedFactor(0.0f) == doctest::Approx(1.0f));
    CHECK(slopeSpeedFactor(-0.05f) > 1.15f);
    CHECK(slopeSpeedFactor(0.05f) < 0.9f);
    CHECK(slopeSpeedFactor(0.1f) == doctest::Approx(0.7047f).epsilon(0.01));
    // Steeper is slower, uphill from the flat and downhill past the gentle slope.
    for (float s = 0.0f; s < 0.5f; s += 0.01f) {
        CHECK(slopeSpeedFactor(s + 0.01f) <= slopeSpeedFactor(s) + 1e-6f);
    }
    for (float s = -0.05f; s > -0.5f; s -= 0.01f) {
        CHECK(slopeSpeedFactor(s - 0.01f) <= slopeSpeedFactor(s) + 1e-6f);
    }
    CHECK(slopeSpeedFactor(5.0f) == slopeSpeedFactor(0.5f));  // beyond the table it stays at the end
    CHECK(slopeSpeedFactor(5.0f) > 0.0f);
}

TEST_CASE("Movement: ground height is interpolated between grid corners") {
    Map map(MapSettings{64, 64, 3});
    CHECK(map.heightAt(10.0f, 20.0f) == doctest::Approx(map.groundHeight(10, 20)));
    const float middle = 0.25f * (map.groundHeight(10, 20) + map.groundHeight(11, 20) + map.groundHeight(10, 21) +
                                  map.groundHeight(11, 21));
    CHECK(map.heightAt(10.5f, 20.5f) == doctest::Approx(middle).epsilon(1e-4));
    CHECK(map.heightAt(-5.0f, -5.0f) == doctest::Approx(map.groundHeight(0, 0)));
    CHECK(map.heightAt(500.0f, 500.0f) == doctest::Approx(map.groundHeight(63, 63)));
    // The walking speed over a real hill is lower going up than coming down.
    bool checked = false;
    for (int y = 4; y < 60 && !checked; ++y) {
        for (int x = 4; x < 60 && !checked; ++x) {
            const float rise = map.heightAt(static_cast<float>(x) + 0.5f, static_cast<float>(y)) -
                               map.heightAt(static_cast<float>(x) - 0.5f, static_cast<float>(y));
            if (rise > 0.1f && map.walkable(CellPos{x, y, 0})) {
                const float fx = static_cast<float>(x);
                const float fy = static_cast<float>(y);
                CHECK(walkSpeedFactor(map, fx, fy, 1.0f, 0.0f) < walkSpeedFactor(map, fx, fy, -1.0f, 0.0f));
                checked = true;
            }
        }
    }
    CHECK(checked);
}

TEST_CASE("Smoothing: a staircase of cells in open ground becomes one straight line") {
    Map map(MapSettings{64, 64, 1, true});
    std::vector<CellPos> path;
    for (int x = 6; x <= 30; ++x) {
        path.push_back(CellPos{x, 5 + (x - 5) / 3, 0});
    }
    const CellPos goal = path.back();
    smoothPath(map, 5.5f, 5.5f, path);
    MOVE_REQUIRE(path.size() == 1);
    CHECK(path[0] == goal);

    std::vector<CellPos> empty;
    smoothPath(map, 5.5f, 5.5f, empty);
    CHECK(empty.empty());
    std::vector<CellPos> one{CellPos{7, 5, 0}};
    smoothPath(map, 5.5f, 5.5f, one);
    CHECK(one.size() == 1);
}

TEST_CASE("Smoothing: a route round a wall keeps its corners and every line is clear") {
    Sim sim = moveTestSim(64);
    moveTestWall(sim, 30, 0, 30, 40);
    std::vector<CellPos> path;
    const CellPos start{10, 20, 0};
    const CellPos goal{50, 20, 0};
    MOVE_REQUIRE(sim.pathfinder().findPath(start, goal, path));
    const size_t cells = path.size();
    smoothPath(sim.map(), 10.5f, 20.5f, path);
    CHECK(path.size() >= 2);
    CHECK(path.size() <= 4);
    CHECK(path.size() < cells / 4);
    CHECK(path.back() == goal);
    float x = 10.5f;
    float y = 20.5f;
    float length = 0.0f;
    for (const CellPos& c : path) {
        const float nx = static_cast<float>(c.x) + 0.5f;
        const float ny = static_cast<float>(c.y) + 0.5f;
        CHECK(straightWalkIsClear(sim.map(), x, y, nx, ny, kPawnRadius));
        length += std::sqrt((nx - x) * (nx - x) + (ny - y) * (ny - y));
        x = nx;
        y = ny;
    }
    // The wall's end is at y = 40, so the walk can't be shorter than going round that corner.
    CHECK(length > 2.0f * std::sqrt(20.0f * 20.0f + 20.0f * 20.0f) - 1.0f);
    CHECK(length < 1.42f * static_cast<float>(cells));
}

TEST_CASE("Smoothing: on a generated map no line touches a wall and none costs more effort") {
    Map map(MapSettings{256, 256, 3});
    Pathfinder finder(map);
    Rng rng(4);
    std::vector<CellPos> path;
    int checked = 0;
    size_t cellsTotal = 0;
    size_t pointsTotal = 0;
    for (int tries = 0; tries < 2000 && checked < 60; ++tries) {
        const CellPos a{static_cast<int>(rng.below(256)), static_cast<int>(rng.below(256)), 0};
        const CellPos b{static_cast<int>(rng.below(256)), static_cast<int>(rng.below(256)), 0};
        if (!finder.findPath(a, b, path) || path.empty()) {
            continue;
        }
        ++checked;
        const std::vector<CellPos> cells = path;
        smoothPath(map, static_cast<float>(a.x) + 0.5f, static_cast<float>(a.y) + 0.5f, path);
        cellsTotal += cells.size();
        pointsTotal += path.size();
        CHECK(path.back() == b);
        CHECK(path.size() <= cells.size());
        float x = static_cast<float>(a.x) + 0.5f;
        float y = static_cast<float>(a.y) + 0.5f;
        float effort = 0.0f;
        for (const CellPos& c : path) {
            const float nx = static_cast<float>(c.x) + 0.5f;
            const float ny = static_cast<float>(c.y) + 0.5f;
            CHECK(straightWalkIsClear(map, x, y, nx, ny, kPawnRadius));
            effort += walkEffort(map, x, y, nx, ny);
            x = nx;
            y = ny;
        }
        float before = 0.0f;
        x = static_cast<float>(a.x) + 0.5f;
        y = static_cast<float>(a.y) + 0.5f;
        for (const CellPos& c : cells) {
            const float nx = static_cast<float>(c.x) + 0.5f;
            const float ny = static_cast<float>(c.y) + 0.5f;
            before += walkEffort(map, x, y, nx, ny);
            x = nx;
            y = ny;
        }
        CHECK(effort <= before * 1.0011f + 0.01f);
    }
    CHECK(checked == 60);
    CHECK(pointsTotal * 3 < cellsTotal);  // it really does straighten
}

TEST_CASE("Movement: a pawn walks a line at its ground speed and stops at the goal") {
    Sim sim = moveTestSim(64);
    const Entity pawn = moveTestPawn(sim, 5, 5);
    moveTestWalk(sim, pawn, {CellPos{45, 5, 0}});
    const float soilSpeed = kMoveTestSpeed * terrainSpeedFactor(Terrain::Soil);
    float fastest = 0.0f;
    int steps = 0;
    while (!moveTestMover(sim, pawn).arrived() && steps < 1000) {
        sim.step();
        ++steps;
        const Mover& m = moveTestMover(sim, pawn);
        fastest = std::max(fastest, std::sqrt(m.vx * m.vx + m.vy * m.vy));
    }
    CHECK(fastest <= soilSpeed + 1e-5f);
    CHECK(fastest >= soilSpeed * 0.99f);
    // 40 m at that speed, a few steps lost speeding up and slowing down.
    CHECK(steps >= static_cast<int>(40.0f / soilSpeed));
    CHECK(steps <= static_cast<int>(40.0f / soilSpeed) + 20);
    CHECK(std::fabs(moveTestPos(sim, pawn).y - 5.5f) < 1e-4f);
    // It comes to rest on the goal cell's middle.
    sim.stepMany(30);
    const Mover& m = moveTestMover(sim, pawn);
    CHECK(m.vx == 0.0f);
    CHECK(m.vy == 0.0f);
    CHECK(std::fabs(moveTestPos(sim, pawn).x - 45.5f) < 0.3f);
}

TEST_CASE("Movement: two pawns walking at each other step round one another") {
    Sim sim = moveTestSim(64);
    const Entity a = moveTestPawn(sim, 5, 30);
    const Entity b = moveTestPawn(sim, 55, 30);
    moveTestWalk(sim, a, {CellPos{55, 30, 0}});
    moveTestWalk(sim, b, {CellPos{5, 30, 0}});
    float closest = 1000.0f;
    float widest = 0.0f;
    int steps = 0;
    while ((!moveTestMover(sim, a).arrived() || !moveTestMover(sim, b).arrived()) && steps < 2000) {
        sim.step();
        ++steps;
        closest = std::min(closest, moveTestDistance(sim, a, b));
        widest = std::max(widest, std::fabs(moveTestPos(sim, a).y - 30.5f));
    }
    CHECK(moveTestMover(sim, a).arrived());
    CHECK(moveTestMover(sim, b).arrived());
    CHECK(closest >= 2.0f * kPawnRadius);  // they never touch
    CHECK(widest > 0.2f);                  // and it took a step to the side
    CHECK(widest < 3.0f);                  // not a long detour
    CHECK(steps < 700);
}

TEST_CASE("Movement: pawns crossing at right angles pass each other") {
    Sim sim = moveTestSim(64);
    const Entity a = moveTestPawn(sim, 10, 30);
    const Entity b = moveTestPawn(sim, 30, 10);
    moveTestWalk(sim, a, {CellPos{50, 30, 0}});
    moveTestWalk(sim, b, {CellPos{30, 50, 0}});
    float closest = 1000.0f;
    for (int i = 0; i < 600; ++i) {
        sim.step();
        closest = std::min(closest, moveTestDistance(sim, a, b));
    }
    CHECK(moveTestMover(sim, a).arrived());
    CHECK(moveTestMover(sim, b).arrived());
    CHECK(closest >= 2.0f * kPawnRadius);
}

TEST_CASE("Movement: a pawn stands out of the way of one walking past") {
    Sim sim = moveTestSim(64);
    const Entity walker = moveTestPawn(sim, 5, 30);
    const Entity standing = moveTestPawn(sim, 20, 30);
    moveTestWalk(sim, walker, {CellPos{40, 30, 0}});
    float closest = 1000.0f;
    for (int i = 0; i < 500; ++i) {
        sim.step();
        closest = std::min(closest, moveTestDistance(sim, walker, standing));
    }
    CHECK(moveTestMover(sim, walker).arrived());
    CHECK(closest >= 2.0f * kPawnRadius);
}

TEST_CASE("Movement: two pawns meeting in a corridor one cell wide both get through") {
    Sim sim = moveTestSim(64);
    moveTestWall(sim, 15, 29, 35, 29);
    moveTestWall(sim, 15, 31, 35, 31);
    const Entity a = moveTestPawn(sim, 5, 30);
    const Entity b = moveTestPawn(sim, 45, 30);
    moveTestWalk(sim, a, {CellPos{45, 30, 0}});
    moveTestWalk(sim, b, {CellPos{5, 30, 0}});
    int steps = 0;
    bool inWall = false;
    while ((!moveTestMover(sim, a).arrived() || !moveTestMover(sim, b).arrived()) && steps < 3000) {
        sim.step();
        ++steps;
        for (const Entity pawn : {a, b}) {
            const Position& p = moveTestPos(sim, pawn);
            inWall = inWall || !pawnFitsAt(sim.map(), p.x, p.y, kPawnRadius);
        }
    }
    CHECK(moveTestMover(sim, a).arrived());
    CHECK(moveTestMover(sim, b).arrived());
    CHECK_FALSE(inWall);
    CHECK(steps < 1200);
}

TEST_CASE("Movement: a pawn held up by a wall gives up its walk and says so") {
    Sim sim = moveTestSim(64);
    const Entity pawn = moveTestPawn(sim, 5, 30);
    moveTestWalk(sim, pawn, {CellPos{40, 30, 0}});
    sim.stepMany(20);
    moveTestWall(sim, 20, 20, 20, 40);  // built across its way
    int steps = 0;
    while (!moveTestMover(sim, pawn).gaveUp && steps < 1500) {
        sim.step();
        ++steps;
    }
    CHECK(moveTestMover(sim, pawn).gaveUp);
    CHECK(moveTestMover(sim, pawn).arrived());
    CHECK(moveTestPos(sim, pawn).x < 20.0f);  // it never went into the wall
    CHECK(pawnFitsAt(sim.map(), moveTestPos(sim, pawn).x, moveTestPos(sim, pawn).y, kPawnRadius));
    // A new walk clears it.
    moveTestWalk(sim, pawn, {CellPos{10, 30, 0}});
    CHECK_FALSE(moveTestMover(sim, pawn).gaveUp);
}

TEST_CASE("Movement: a pawn slides along a wall it meets at an angle") {
    Sim sim = moveTestSim(64);
    moveTestWall(sim, 30, 10, 30, 40);
    const Entity pawn = moveTestPawn(sim, 20, 30);
    // Aimed at the wall's far side; it can't go through, but it must not stop dead against it.
    moveTestWalk(sim, pawn, {CellPos{40, 45, 0}});
    sim.stepMany(400);
    CHECK(moveTestPos(sim, pawn).y > 35.0f);
}

TEST_CASE("Movement: a pile of pawns on one cell spreads out and comes to rest") {
    // A small pile ends up with room for everyone; a crowd packs tight but nobody stays stacked.
    for (const int count : {10, 30}) {
        Sim sim = moveTestSim(64);
        std::vector<Entity> pawns;
        for (int i = 0; i < count; ++i) {
            pawns.push_back(moveTestPawn(sim, 20, 20));
        }
        sim.stepMany(900);
        float closest = 1000.0f;
        float fastest = 0.0f;
        for (size_t i = 0; i < pawns.size(); ++i) {
            const Mover& m = moveTestMover(sim, pawns[i]);
            fastest = std::max(fastest, std::sqrt(m.vx * m.vx + m.vy * m.vy));
            for (size_t j = i + 1; j < pawns.size(); ++j) {
                closest = std::min(closest, moveTestDistance(sim, pawns[i], pawns[j]));
            }
        }
        CHECK(closest >= (count <= 10 ? 2.0f * kPawnRadius : 0.2f));
        CHECK(fastest < 0.01f);
    }
}

TEST_CASE("Movement: 40 pawns crossing a field with walls all arrive and none enters a wall") {
    const auto run = [](uint64_t seed) {
        Sim sim = moveTestSim(96, seed);
        moveTestWall(sim, 30, 10, 30, 70);
        moveTestWall(sim, 60, 25, 60, 90);
        moveTestWall(sim, 45, 40, 52, 40);
        Rng rng(seed);
        std::vector<Entity> pawns;
        std::vector<CellPos> goals;
        while (pawns.size() < 40) {
            const CellPos from{static_cast<int>(rng.below(96)), static_cast<int>(rng.below(96)), 0};
            const CellPos to{static_cast<int>(rng.below(96)), static_cast<int>(rng.below(96)), 0};
            if (!sim.map().walkable(from) || !sim.map().walkable(to) || !sim.regions().canReach(from, to)) {
                continue;
            }
            const Entity pawn = moveTestPawn(sim, from.x, from.y);
            std::vector<CellPos> path;
            sim.pathfinder().findPath(from, to, path);
            smoothPath(sim.map(), static_cast<float>(from.x) + 0.5f, static_cast<float>(from.y) + 0.5f, path);
            moveTestWalk(sim, pawn, path);
            pawns.push_back(pawn);
        }
        bool inWall = false;
        int steps = 0;
        const auto allArrived = [&] {
            return std::all_of(pawns.begin(), pawns.end(), [&](Entity p) { return moveTestMover(sim, p).arrived(); });
        };
        while (!allArrived() && steps < 3000) {
            sim.step();
            ++steps;
            for (const Entity pawn : pawns) {
                const Position& p = moveTestPos(sim, pawn);
                inWall = inWall || !pawnFitsAt(sim.map(), p.x, p.y, kPawnRadius);
            }
        }
        CHECK(allArrived());
        CHECK_FALSE(inWall);
        sim.stepMany(60);
        return sim.stateHash();
    };
    const uint64_t first = run(12);
    CHECK(first == run(12));  // same seed, same walk, step for step
    CHECK(first != run(13));
}

TEST_CASE("Movement: hundreds of pawns walking cost little per step") {
    Sim sim(SimSettings{8, 256, 0, 0, true}, testDefs());
    Rng rng(8);
    for (int i = 0; i < 300; ++i) {
        const CellPos from{static_cast<int>(rng.below(256)), static_cast<int>(rng.below(256)), 0};
        const Entity pawn = moveTestPawn(sim, from.x, from.y);
        moveTestWalk(sim, pawn, {CellPos{static_cast<int>(rng.below(256)), static_cast<int>(rng.below(256)), 0}});
    }
    sim.stepMany(200);
    int moving = 0;
    sim.things().registry().view<const Mover>().each([&](const Mover& m) { moving += (m.vx != 0.0f || m.vy != 0.0f) ? 1 : 0; });
    CHECK(moving > 250);
}

TEST_CASE("Movement: the snapshot shows velocity, height and the route ahead") {
    Sim sim(SimSettings{3, 64, 0, 0, false}, testDefs());
    CellPos from{0, 0, 0};
    CellPos to{0, 0, 0};
    for (int y = 10; y < 50 && !(from.x > 0); ++y) {
        for (int x = 10; x < 50; ++x) {
            if (sim.map().walkable(CellPos{x, y, 0}) && sim.map().walkable(CellPos{x + 4, y, 0}) &&
                straightWalkIsClear(sim.map(), static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f,
                                    static_cast<float>(x + 4) + 0.5f, static_cast<float>(y) + 0.5f, kPawnRadius)) {
                from = CellPos{x, y, 0};
                to = CellPos{x + 4, y, 0};
                break;
            }
        }
    }
    MOVE_REQUIRE(from.x > 0);
    const Entity pawn = moveTestPawn(sim, from.x, from.y);
    moveTestWalk(sim, pawn, {CellPos{from.x + 2, from.y, 0}, to});
    Snapshot before;
    sim.writeSnapshot(before);
    const ThingView* idle = before.find(pawn);
    MOVE_REQUIRE(idle != nullptr);
    CHECK(idle->vx == 0.0f);
    CHECK(idle->waypointCount == 2);
    sim.stepMany(10);
    Snapshot snap;
    sim.writeSnapshot(snap);
    const ThingView* view = snap.find(pawn);
    MOVE_REQUIRE(view != nullptr);
    CHECK(view->vx > 0.0f);
    CHECK(view->z == doctest::Approx(sim.map().heightAt(view->x, view->y)));
    MOVE_REQUIRE(view->waypointCount == 2);
    CHECK(snap.waypoints[view->firstWaypoint + 1].x == doctest::Approx(static_cast<float>(to.x) + 0.5f));
}
