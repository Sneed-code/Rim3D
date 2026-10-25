#include "ColonySim/Map.h"
#include "ColonySim/Pathfinder.h"
#include "ColonySim/Random.h"
#include "ColonySim/Regions.h"
#include "ColonySim/Things.h"

#include "TestDefs.h"

#include <doctest.h>

#include <cstdlib>
#include <string>
#include <vector>

using namespace colony;

namespace {

// A map with things on it, for placing walls and doors.
struct PathTestWorld {
    explicit PathTestWorld(MapSettings settings) : map(settings), things(map, testDefs()), finder(map) {}
    Map map;
    Things things;
    Pathfinder finder;

    void wall(int x0, int y0, int x1, int y1) {
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                things.spawn("Wall", CellPos{x, y, 0});
            }
        }
    }
};

// Empty if `path` is a walkable walk from `start` to `goal` in 8-connected steps that never cut
// a corner, else what is wrong.
std::string pathTestProblem(const Map& map, CellPos start, CellPos goal, const std::vector<CellPos>& path) {
    if (path.empty()) {
        return start == goal ? "" : "empty path";
    }
    if (!(path.back() == goal)) {
        return "doesn't end at the goal";
    }
    CellPos prev = start;
    for (const CellPos& c : path) {
        const int dx = std::abs(c.x - prev.x);
        const int dy = std::abs(c.y - prev.y);
        if (c.level != start.level || dx > 1 || dy > 1 || dx + dy == 0) {
            return "not a step at " + std::to_string(c.x) + "," + std::to_string(c.y);
        }
        if (!map.walkable(c)) {
            return "unwalkable cell " + std::to_string(c.x) + "," + std::to_string(c.y);
        }
        if (dx + dy == 2 && (!map.walkable(CellPos{c.x, prev.y, c.level}) || !map.walkable(CellPos{prev.x, c.y, c.level}))) {
            return "cuts a corner at " + std::to_string(c.x) + "," + std::to_string(c.y);
        }
        prev = c;
    }
    return "";
}

float pathTestLength(CellPos start, const std::vector<CellPos>& path) {
    float length = 0.0f;
    CellPos prev = start;
    for (const CellPos& c : path) {
        length += (c.x != prev.x && c.y != prev.y) ? 1.41421356f : 1.0f;
        prev = c;
    }
    return length;
}

CellPos pathTestRandomCell(Rng& rng, const Map& map) {
    return CellPos{static_cast<int>(rng.below(static_cast<uint32_t>(map.width()))),
                   static_cast<int>(rng.below(static_cast<uint32_t>(map.height()))), 0};
}

}  // namespace

TEST_CASE("Pathfinder: paths on a generated map are valid and close to the best path") {
    Map map(MapSettings{256, 256, 3});
    Pathfinder finder(map);
    Rng rng(9);
    std::vector<CellPos> path;
    std::vector<CellPos> best;
    int compared = 0;
    float total = 0.0f;
    float bestTotal = 0.0f;
    uint64_t hierarchicalWork = 0;
    uint64_t fullWork = 0;
    for (int tries = 0; tries < 2000 && compared < 60; ++tries) {
        const CellPos a = pathTestRandomCell(rng, map);
        const CellPos b = pathTestRandomCell(rng, map);
        const bool reach = map.regions().canReach(a, b);
        const uint64_t before = finder.stats().cellsExpanded + finder.stats().regionsExpanded;
        const bool found = finder.findPath(a, b, path);
        const uint64_t between = finder.stats().cellsExpanded + finder.stats().regionsExpanded;
        CHECK(found == reach);
        if (!found) {
            continue;
        }
        const std::string problem = pathTestProblem(map, a, b, path);
        CHECK_MESSAGE(problem.empty(), problem);
        CHECK(finder.findPathFullGrid(a, b, best));
        const uint64_t after = finder.stats().cellsExpanded + finder.stats().regionsExpanded;
        const float length = pathTestLength(a, path);
        const float bestLength = pathTestLength(a, best);
        CHECK(length >= bestLength - 0.001f);
        CHECK(length <= bestLength * 1.5f + 2.0f);  // cell paths; smoothing (S4) straightens them
        total += length;
        bestTotal += bestLength;
        hierarchicalWork += between - before;
        fullWork += after - between;
        ++compared;
    }
    CHECK(compared == 60);
    CHECK(total <= bestTotal * 1.06f);
    CHECK(hierarchicalWork * 3 < fullWork);  // the corridor and the guide save most of the search
    CHECK(finder.stats().failedSearches == 0);
}

TEST_CASE("Pathfinder: start equals goal is an empty path") {
    Map map(MapSettings{32, 32, 3, true});
    Pathfinder finder(map);
    std::vector<CellPos> path{{1, 1, 0}, {2, 2, 0}};
    CHECK(finder.findPath(CellPos{4, 4, 0}, CellPos{4, 4, 0}, path));
    CHECK(path.empty());
    const PathTicket ticket = finder.request(CellPos{4, 4, 0}, CellPos{4, 4, 0});
    CHECK(finder.status(ticket) == PathStatus::Found);
    path.push_back(CellPos{1, 1, 0});
    CHECK(finder.takePath(ticket, path));
    CHECK(path.empty());
}

TEST_CASE("Pathfinder: a goal on another level or off the map is refused") {
    Map map(MapSettings{16, 16, 3, true});
    Pathfinder finder(map);
    std::vector<CellPos> path;
    CHECK_FALSE(finder.findPath(CellPos{1, 1, 0}, CellPos{5, 5, 1}, path));
    CHECK_FALSE(finder.findPath(CellPos{1, 1, 1}, CellPos{5, 5, 1}, path));
    CHECK_FALSE(finder.findPath(CellPos{1, 1, 0}, CellPos{16, 5, 0}, path));
    CHECK(finder.status(finder.request(CellPos{1, 1, 0}, CellPos{5, 5, 1})) == PathStatus::Failed);
    CHECK(finder.stats().searches == 0);
}

TEST_CASE("Pathfinder: the queue answers next update, and takePath frees the ticket") {
    PathTestWorld w(MapSettings{64, 64, 1, true});
    const PathTicket ticket = w.finder.request(CellPos{2, 2, 0}, CellPos{60, 50, 0});
    CHECK(ticket.valid());
    CHECK(w.finder.status(ticket) == PathStatus::Queued);
    CHECK(w.finder.queued() == 1);
    std::vector<CellPos> path;
    CHECK_FALSE(w.finder.takePath(ticket, path));  // not ready: the ticket stays
    CHECK(w.finder.status(ticket) == PathStatus::Queued);

    w.finder.update();
    CHECK(w.finder.status(ticket) == PathStatus::Found);
    CHECK(w.finder.queued() == 0);
    CHECK(w.finder.takePath(ticket, path));
    CHECK(pathTestProblem(w.map, CellPos{2, 2, 0}, CellPos{60, 50, 0}, path).empty());
    CHECK(pathTestLength(CellPos{2, 2, 0}, path) == doctest::Approx(10 + 48 * 1.41421356).epsilon(0.001));
    CHECK(w.finder.status(ticket) == PathStatus::Unknown);
    CHECK_FALSE(w.finder.takePath(ticket, path));
    CHECK(path.empty());

    // The freed slot is used again, and the old ticket doesn't see the new request.
    const PathTicket next = w.finder.request(CellPos{3, 3, 0}, CellPos{5, 5, 0});
    CHECK(next.slot == ticket.slot);
    CHECK(w.finder.status(ticket) == PathStatus::Unknown);
    CHECK(w.finder.status(next) == PathStatus::Queued);
}

TEST_CASE("Pathfinder: unreachable requests are refused at once, without a search") {
    PathTestWorld w(MapSettings{48, 48, 1, true});
    w.wall(20, 0, 20, 47);  // splits the map in two
    const PathTicket ticket = w.finder.request(CellPos{2, 2, 0}, CellPos{40, 2, 0});
    CHECK(w.finder.status(ticket) == PathStatus::Failed);
    CHECK(w.finder.queued() == 0);
    CHECK(w.finder.stats().refused == 1);
    std::vector<CellPos> path{{1, 1, 0}};
    CHECK_FALSE(w.finder.takePath(ticket, path));  // frees it
    CHECK(path.empty());
    CHECK(w.finder.status(ticket) == PathStatus::Unknown);
    CHECK_FALSE(w.finder.findPath(CellPos{2, 2, 0}, CellPos{40, 2, 0}, path));
    CHECK(w.finder.stats().searches == 0);
    CHECK(w.finder.stats().cellsExpanded == 0);
}

TEST_CASE("Pathfinder: a small budget spreads a search over steps and gives the same path") {
    PathTestWorld w(MapSettings{128, 128, 1, true});
    // A serpentine: walls with gaps at alternate ends, so the path winds through every lane.
    for (int i = 1; i < 8; ++i) {
        const int x = i * 16;
        if (i % 2 == 1) {
            w.wall(x, 0, x, 120);
        } else {
            w.wall(x, 7, x, 127);
        }
    }
    const CellPos start{2, 2, 0};
    const CellPos goal{125, 125, 0};
    std::vector<CellPos> direct;
    CHECK(w.finder.findPath(start, goal, direct));
    CHECK(pathTestProblem(w.map, start, goal, direct).empty());

    const PathTicket ticket = w.finder.request(start, goal);
    int updates = 0;
    while (w.finder.status(ticket) == PathStatus::Queued && updates < 1000) {
        w.finder.update(40);
        ++updates;
    }
    CHECK(updates > 5);
    CHECK(w.finder.stats().maxWorkInStep <= 40);
    CHECK(w.finder.stats().maxWaitSteps == static_cast<uint32_t>(updates));
    std::vector<CellPos> queued;
    CHECK(w.finder.takePath(ticket, queued));
    CHECK(queued == direct);
}

TEST_CASE("Pathfinder: requests are answered in order") {
    PathTestWorld w(MapSettings{96, 96, 1, true});
    const PathTicket a = w.finder.request(CellPos{1, 1, 0}, CellPos{90, 90, 0});
    const PathTicket b = w.finder.request(CellPos{1, 90, 0}, CellPos{90, 1, 0});
    const PathTicket c = w.finder.request(CellPos{40, 40, 0}, CellPos{41, 41, 0});
    CHECK(w.finder.stats().maxQueue == 3);
    int updates = 0;
    while (w.finder.status(a) == PathStatus::Queued && updates < 1000) {
        w.finder.update(10);
        ++updates;
    }
    CHECK(w.finder.status(a) == PathStatus::Found);
    CHECK(w.finder.status(b) == PathStatus::Queued);
    CHECK(w.finder.status(c) == PathStatus::Queued);  // short, but behind b
    w.finder.update(1000);
    CHECK(w.finder.status(b) == PathStatus::Found);
    CHECK(w.finder.status(c) == PathStatus::Found);
}

TEST_CASE("Pathfinder: cancelling a request in the middle of its search or in the queue") {
    PathTestWorld w(MapSettings{96, 96, 1, true});
    const PathTicket a = w.finder.request(CellPos{1, 1, 0}, CellPos{90, 90, 0});
    const PathTicket b = w.finder.request(CellPos{1, 90, 0}, CellPos{90, 1, 0});
    const PathTicket c = w.finder.request(CellPos{5, 5, 0}, CellPos{9, 9, 0});
    w.finder.update(20);  // a is half done
    CHECK(w.finder.status(a) == PathStatus::Queued);
    w.finder.cancel(a);
    w.finder.cancel(b);
    CHECK(w.finder.status(a) == PathStatus::Unknown);
    CHECK(w.finder.status(b) == PathStatus::Unknown);
    CHECK(w.finder.queued() == 1);
    w.finder.update();
    std::vector<CellPos> path;
    CHECK(w.finder.takePath(c, path));
    CHECK(pathTestProblem(w.map, CellPos{5, 5, 0}, CellPos{9, 9, 0}, path).empty());
    w.finder.cancel(c);  // already taken: nothing happens
    CHECK(w.finder.queued() == 0);
}

TEST_CASE("Pathfinder: a wall built during a search restarts it, and the path goes around") {
    PathTestWorld w(MapSettings{64, 64, 1, true});
    const CellPos start{2, 30, 0};
    const CellPos goal{60, 30, 0};
    const PathTicket ticket = w.finder.request(start, goal);
    w.finder.update(30);
    CHECK(w.finder.status(ticket) == PathStatus::Queued);
    w.wall(32, 10, 32, 50);  // across the straight line
    while (w.finder.status(ticket) == PathStatus::Queued) {
        w.finder.update(30);
    }
    CHECK(w.finder.stats().restarts == 1);
    std::vector<CellPos> path;
    CHECK(w.finder.takePath(ticket, path));
    CHECK(pathTestProblem(w.map, start, goal, path).empty());

    // A wall that cuts the goal off makes the restarted request fail without a search.
    const PathTicket cut = w.finder.request(start, goal);
    w.finder.update(30);
    w.wall(40, 0, 40, 63);
    const uint64_t searches = w.finder.stats().searches;
    w.finder.update(30);
    CHECK(w.finder.status(cut) == PathStatus::Failed);
    CHECK(w.finder.stats().searches == searches);
    CHECK(w.finder.stats().restarts == 2);
}

TEST_CASE("Pathfinder: doors are walked through") {
    PathTestWorld w(MapSettings{48, 48, 1, true});
    w.wall(24, 0, 24, 39);
    w.wall(24, 41, 24, 47);
    CHECK(w.things.spawn("Door", CellPos{24, 40, 0}) != kNoEntity);
    std::vector<CellPos> path;
    CHECK(w.finder.findPath(CellPos{2, 2, 0}, CellPos{45, 2, 0}, path));
    CHECK(pathTestProblem(w.map, CellPos{2, 2, 0}, CellPos{45, 2, 0}, path).empty());
    bool throughDoor = false;
    for (const CellPos& c : path) {
        throughDoor = throughDoor || c == CellPos{24, 40, 0};
    }
    CHECK(throughDoor);
}

TEST_CASE("Pathfinder: random walls and doors, every request agrees with reachability") {
    PathTestWorld w(MapSettings{80, 80, 13});
    Rng rng(41);
    std::vector<CellPos> path;
    std::vector<PathTicket> tickets;
    for (int round = 0; round < 30; ++round) {
        for (int i = 0; i < 30; ++i) {
            const CellPos c = pathTestRandomCell(rng, w.map);
            w.things.spawn(rng.below(6) == 0 ? "Door" : "Wall", c);
        }
        for (int i = 0; i < 6; ++i) {
            const CellPos a = pathTestRandomCell(rng, w.map);
            const CellPos b = pathTestRandomCell(rng, w.map);
            if (!w.map.walkable(a) || !w.map.walkable(b)) {
                continue;
            }
            const bool reach = w.map.regions().canReach(a, b);
            CHECK(w.finder.findPath(a, b, path) == reach);
            CHECK(w.finder.findPathFullGrid(a, b, path) == reach);
            tickets.push_back(w.finder.request(a, b));
        }
        w.finder.update(200);
    }
    while (w.finder.queued() > 0) {
        w.finder.update(200);
    }
    // Queued requests cut off by later walls fail without a search; no search may fail.
    int found = 0;
    for (const PathTicket& t : tickets) {
        found += w.finder.takePath(t, path) ? 1 : 0;
    }
    CHECK(found > 20);
    CHECK(w.finder.stats().failedSearches == 0);
}

TEST_CASE("Pathfinder: same map and requests give the same paths") {
    const auto run = []() {
        Map map(MapSettings{192, 192, 17});
        Pathfinder finder(map);
        Rng rng(5);
        std::vector<PathTicket> tickets;
        for (int i = 0; i < 40; ++i) {
            tickets.push_back(finder.request(pathTestRandomCell(rng, map), pathTestRandomCell(rng, map)));
        }
        std::vector<CellPos> all;
        std::vector<CellPos> path;
        for (int step = 0; step < 200; ++step) {
            finder.update(500);
        }
        for (const PathTicket& t : tickets) {
            finder.takePath(t, path);
            all.insert(all.end(), path.begin(), path.end());
            all.push_back(CellPos{-1, -1, 0});
        }
        return all;
    };
    const std::vector<CellPos> first = run();
    CHECK(first.size() > 200);
    CHECK(first == run());
}
