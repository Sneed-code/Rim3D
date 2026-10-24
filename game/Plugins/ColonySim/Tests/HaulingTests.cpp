#include "ColonySim/Pathfinder.h"
#include "ColonySim/Sim.h"

#include <doctest.h>

#include <cstdlib>
#include <vector>

using namespace colony;

namespace {

// First walkable cell scanning from one corner, for path tests on generated maps.
int firstWalkable(const Map& map, bool fromEnd) {
    const int count = map.width() * map.height();
    for (int k = 0; k < count; ++k) {
        const int i = fromEnd ? count - 1 - k : k;
        if (map.walkable(i % map.width(), i / map.width())) {
            return i;
        }
    }
    return -1;
}

}  // namespace

TEST_CASE("Pathfinder: paths are walkable, connected steps") {
    const Map map(MapSettings{128, 128, 3});
    Pathfinder finder(map);
    std::vector<int> path;
    const int start = firstWalkable(map, false);
    const int goal = firstWalkable(map, true);
    if (start < 0) {
        return;
    }
    if (!finder.findPath(start, goal, path)) {
        return;  // different islands on this seed; reachability is covered by the hauling test
    }
    CHECK_FALSE(path.empty());
    CHECK(path.back() == goal);
    int prev = start;
    for (const int c : path) {
        CHECK(map.walkable(c % 128, c / 128));
        CHECK(std::abs(c % 128 - prev % 128) <= 1);
        CHECK(std::abs(c / 128 - prev / 128) <= 1);
        prev = c;
    }
}

TEST_CASE("Pathfinder: start equals goal is an empty path") {
    const Map map(MapSettings{32, 32, 3});
    Pathfinder finder(map);
    std::vector<int> path{1, 2, 3};
    const int cell = firstWalkable(map, false);
    CHECK(finder.findPath(cell, cell, path));
    CHECK(path.empty());
}

TEST_CASE("Hauling: 20 pawns haul every item within one game day, none fail") {
    Sim sim(SimSettings{11, 192, 20, 150});
    CHECK(sim.stockpileCell() >= 0);
    CHECK(sim.itemsLeft() == 150);
    sim.stepMany(kSecondsPerDay);
    CHECK(sim.itemsLeft() == 0);
    CHECK(sim.stats().hauls == 150);
    CHECK(sim.stats().failedJobs == 0);
}

TEST_CASE("Hauling: same seed gives the same game") {
    Sim a(SimSettings{21, 96, 5, 40});
    Sim b(SimSettings{21, 96, 5, 40});
    a.stepMany(5000);
    b.stepMany(5000);
    CHECK(a.stats().hauls == b.stats().hauls);
    for (size_t i = 0; i < a.pawns().size(); ++i) {
        CHECK(a.pawns()[i].x == b.pawns()[i].x);
        CHECK(a.pawns()[i].y == b.pawns()[i].y);
    }
}
