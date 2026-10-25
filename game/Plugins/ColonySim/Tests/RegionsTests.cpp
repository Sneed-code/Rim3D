#include "ColonySim/Map.h"
#include "ColonySim/Pathfinder.h"
#include "ColonySim/Random.h"
#include "ColonySim/Regions.h"
#include "ColonySim/Things.h"

#include "TestDefs.h"

#include <doctest.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace colony;

namespace {

// A map with things on it, for placing walls and doors.
struct RegionsTestWorld {
    explicit RegionsTestWorld(MapSettings settings) : map(settings), things(map, testDefs()) {}
    Map map;
    Things things;
    Regions& regions() { return map.regions(); }
};

int regionsTestBlockOf(const Map& map, CellPos c) {
    const int blocksX = (map.width() + Regions::kBlockSize - 1) / Regions::kBlockSize;
    return (c.y / Regions::kBlockSize) * blocksX + c.x / Regions::kBlockSize;
}

// Rebuilds everything the regions should be from the map alone and compares it with what the
// regions hold. Returns an empty string when they match, else what is wrong.
std::string regionsMismatch(Map& map) {
    Regions& regions = map.regions();
    const int w = map.width();
    const int h = map.height();
    const auto at = [&](int x, int y) { return regions.regionAt(CellPos{x, y, 0}); };

    // Every walkable cell has a live region in its own block; doors have one-cell door regions.
    std::map<RegionId, int> cells;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const CellPos c{x, y, 0};
            const RegionId id = at(x, y);
            if (!map.walkable(c)) {
                if (id != kNoRegion) {
                    return "unwalkable cell has a region";
                }
                continue;
            }
            if (id == kNoRegion) {
                return "walkable cell has no region";
            }
            const Region& r = regions.region(id);
            if (!r.alive || !r.bounds.contains(c) || regionsTestBlockOf(map, r.anchor) != regionsTestBlockOf(map, c)) {
                return "cell outside its region's block or bounds";
            }
            if ((r.kind == RegionKind::Door) != map.isDoor(c)) {
                return "door cell and door region disagree";
            }
            ++cells[id];
        }
    }
    if (cells.size() != regions.regionCount()) {
        return "region count is off";
    }
    for (const auto& [id, count] : cells) {
        const Region& r = regions.region(id);
        if (r.cellCount != count || at(r.anchor.x, r.anchor.y) != id) {
            return "region cell count or anchor is off";
        }
        if (r.kind == RegionKind::Door && count != 1) {
            return "door region with more than one cell";
        }
    }

    // Regions are as big as they can be: neighbouring non-door cells in one block share one.
    // Links are exactly the cell sides between different regions.
    std::map<std::pair<RegionId, RegionId>, int> faces;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const RegionId a = at(x, y);
            if (a == kNoRegion) {
                continue;
            }
            const CellPos sides[2] = {{x + 1, y, 0}, {x, y + 1, 0}};
            for (const CellPos& n : sides) {
                if (!map.inBounds(n)) {
                    continue;
                }
                const RegionId b = at(n.x, n.y);
                if (b == kNoRegion || b == a) {
                    continue;
                }
                const bool sameBlock = regionsTestBlockOf(map, CellPos{x, y, 0}) == regionsTestBlockOf(map, n);
                if (sameBlock && !map.isDoor(CellPos{x, y, 0}) && !map.isDoor(n)) {
                    return "two regions side by side in one block";
                }
                ++faces[{a, b}];
                ++faces[{b, a}];
            }
        }
    }
    size_t links = 0;
    for (const auto& [id, count] : cells) {
        const std::vector<RegionLink>& list = regions.region(id).links;
        links += list.size();
        for (size_t i = 0; i < list.size(); ++i) {
            if (i > 0 && list[i - 1].other >= list[i].other) {
                return "links not sorted";
            }
            const auto it = faces.find({id, list[i].other});
            if (it == faces.end() || it->second != list[i].faces) {
                return "link missing or with the wrong face count";
            }
        }
    }
    if (links != faces.size()) {
        return "a link is missing";
    }

    // Groups are the walkable areas a 4-connected flood finds, one to one.
    std::vector<int> area(static_cast<size_t>(w) * static_cast<size_t>(h), -1);
    std::map<int, uint32_t> areaToGroup;
    std::map<uint32_t, int> groupToArea;
    int areas = 0;
    for (int start = 0; start < w * h; ++start) {
        if (area[static_cast<size_t>(start)] != -1 || !map.walkable(map.cellAt(start))) {
            continue;
        }
        std::vector<int> queue{start};
        area[static_cast<size_t>(start)] = areas;
        for (size_t head = 0; head < queue.size(); ++head) {
            const CellPos c = map.cellAt(queue[head]);
            const CellPos around[4] = {{c.x + 1, c.y, 0}, {c.x - 1, c.y, 0}, {c.x, c.y + 1, 0}, {c.x, c.y - 1, 0}};
            for (const CellPos& n : around) {
                if (map.walkable(n) && area[static_cast<size_t>(map.index(n))] == -1) {
                    area[static_cast<size_t>(map.index(n))] = areas;
                    queue.push_back(map.index(n));
                }
            }
        }
        const uint32_t group = regions.groupAt(map.cellAt(start));
        if (areaToGroup.count(areas) != 0 || groupToArea.count(group) != 0) {
            return "two areas share a group";
        }
        areaToGroup[areas] = group;
        groupToArea[group] = areas;
        if (regions.groupCellCount(group) != static_cast<int>(queue.size())) {
            return "group cell count is off";
        }
        ++areas;
    }
    for (int i = 0; i < w * h; ++i) {
        const CellPos c = map.cellAt(i);
        if (map.walkable(c) && regions.groupAt(c) != areaToGroup[area[static_cast<size_t>(i)]]) {
            return "cell in the wrong group";
        }
    }
    if (regions.groupCount() != static_cast<size_t>(areas)) {
        return "group count is off";
    }
    return "";
}

}  // namespace

TEST_CASE("Regions: an open map has one region per 16 m block, all in one group") {
    Map map(MapSettings{40, 20, 1, true});  // 3 by 2 blocks, the last column and row cut short
    Regions& regions = map.regions();
    CHECK(regions.blocksX() == 3);
    CHECK(regions.blocksY() == 2);
    CHECK(regions.regionCount() == 6);
    CHECK(regions.groupCount() == 1);
    CHECK(regions.groupCellCount(0) == 800);

    const RegionId corner = regions.regionAt(CellPos{0, 0, 0});
    CHECK(regions.regionAt(CellPos{15, 15, 0}) == corner);
    CHECK(regions.regionAt(CellPos{16, 0, 0}) != corner);
    const Region& r = regions.region(corner);
    CHECK(r.kind == RegionKind::Normal);
    CHECK(r.cellCount == 256);
    CHECK(r.bounds.minX == 0);
    CHECK(r.bounds.sizeX == 16);
    CHECK(r.anchor == CellPos{0, 0, 0});
    // Linked to the blocks east and north, not the one diagonally across.
    if (!(r.links.size() == 2)) {
        CHECK(false);
        return;
    }
    const RegionId east = regions.regionAt(CellPos{16, 0, 0});
    const RegionId north = regions.regionAt(CellPos{0, 16, 0});
    for (const RegionLink& link : r.links) {
        CHECK((link.other == east || link.other == north));
        if (link.other == east) {
            CHECK(link.faces == 16);
            CHECK(link.x == doctest::Approx(16.0f));
            CHECK(link.y == doctest::Approx(8.0f));
        } else {
            CHECK(link.faces == 16);
            CHECK(link.x == doctest::Approx(8.0f));
            CHECK(link.y == doctest::Approx(16.0f));
        }
    }
    const Region& last = regions.region(regions.regionAt(CellPos{39, 19, 0}));
    CHECK(last.cellCount == 8 * 4);
    CHECK(regions.regionsInBlock(2, 1, 0).size() == 1);
    CHECK(regions.regionsInBlock(3, 0, 0).empty());
    CHECK(regionsMismatch(map).empty());
}

TEST_CASE("Regions: cells off the map or on other levels have no region") {
    Map map(MapSettings{32, 32, 1, true});
    Regions& regions = map.regions();
    CHECK(regions.regionAt(CellPos{-1, 0, 0}) == kNoRegion);
    CHECK(regions.regionAt(CellPos{32, 0, 0}) == kNoRegion);
    CHECK(regions.regionAt(CellPos{3, 3, 1}) == kNoRegion);
    CHECK(regions.groupAt(CellPos{3, 3, 1}) == kNoGroup);
    CHECK_FALSE(regions.canReach(CellPos{3, 3, 0}, CellPos{3, 3, 1}));
    CHECK_FALSE(regions.canReach(CellPos{-5, 3, 0}, CellPos{-5, 3, 0}));
    CHECK(regions.canReach(CellPos{3, 3, 0}, CellPos{30, 30, 0}));
}

TEST_CASE("Regions: a wall splits the map and a door in it joins the two sides") {
    RegionsTestWorld w(MapSettings{32, 32, 1, true});
    std::vector<Entity> walls;
    for (int y = 0; y < 32; ++y) {
        walls.push_back(w.things.spawn("Wall", CellPos{20, y, 0}));
    }
    Regions& regions = w.regions();
    CHECK(regions.groupCount() == 2);
    CHECK_FALSE(regions.canReach(CellPos{2, 2, 0}, CellPos{30, 30, 0}));
    CHECK(regions.canReach(CellPos{2, 2, 0}, CellPos{19, 30, 0}));
    CHECK(regions.regionAt(CellPos{20, 5, 0}) == kNoRegion);
    CHECK(regions.regionCount() == 6);  // the two right-hand blocks are cut in two
    CHECK(regionsMismatch(w.map).empty());

    // Swap one wall for a door.
    w.things.destroy(walls[10]);
    const Entity door = w.things.spawn("Door", CellPos{20, 10, 0});
    CHECK(door != kNoEntity);
    CHECK(w.map.walkable(CellPos{20, 10, 0}));
    CHECK(w.map.isDoor(CellPos{20, 10, 0}));
    CHECK(regions.groupCount() == 1);
    CHECK(regions.canReach(CellPos{2, 2, 0}, CellPos{30, 30, 0}));
    const RegionId doorRegion = regions.regionAt(CellPos{20, 10, 0});
    const Region& d = regions.region(doorRegion);
    CHECK(d.kind == RegionKind::Door);
    CHECK(d.cellCount == 1);
    if (!(d.links.size() == 2)) {
        CHECK(false);
        return;
    }
    CHECK(d.links[0].faces == 1);
    CHECK(d.links[1].faces == 1);
    CHECK(regions.regionCount() == 7);
    CHECK(regionsMismatch(w.map).empty());

    // Taking the door away leaves a gap: the gap cell joins the region beside it.
    w.things.destroy(door);
    CHECK(regions.groupCount() == 1);
    CHECK(regions.region(regions.regionAt(CellPos{20, 10, 0})).kind == RegionKind::Normal);
    CHECK(regions.regionCount() == 5);
    CHECK(regionsMismatch(w.map).empty());
}

TEST_CASE("Regions: a walled room with a door, across block boundaries") {
    RegionsTestWorld w(MapSettings{48, 48, 1, true});
    // Walls around x 10..20, y 12..22, which straddles four blocks.
    for (int i = 10; i <= 20; ++i) {
        w.things.spawn("Wall", CellPos{i, 12, 0});
        w.things.spawn("Wall", CellPos{i, 22, 0});
        w.things.spawn("Wall", CellPos{10, i + 2, 0});
        w.things.spawn("Wall", CellPos{20, i + 2, 0});
    }
    Regions& regions = w.regions();
    const CellPos inside{15, 17, 0};
    const CellPos outside{2, 2, 0};
    CHECK_FALSE(regions.canReach(inside, outside));
    CHECK(regions.groupCount() == 2);
    CHECK(regions.groupCellCount(regions.groupAt(inside)) == 9 * 9);
    CHECK(regionsMismatch(w.map).empty());

    // A bed inside changes nothing.
    const uint64_t before = regions.blocksRebuilt();
    w.things.spawn("Bed", CellPos{12, 14, 0});
    CHECK(regions.regionCount() > 0);
    CHECK(regions.blocksRebuilt() == before);

    w.things.destroy(w.map.buildingAt(CellPos{15, 12, 0}));
    w.things.spawn("Door", CellPos{15, 12, 0});
    CHECK(regions.canReach(inside, outside));
    CHECK(regions.groupCount() == 1);
    CHECK(regionsMismatch(w.map).empty());
}

TEST_CASE("Regions: a change rebuilds only its own block and keeps other regions' ids") {
    RegionsTestWorld w(MapSettings{64, 64, 1, true});
    Regions& regions = w.regions();
    CHECK(regions.regionCount() == 16);
    CHECK(regions.blocksRebuilt() == 16);  // the first query builds every block
    const RegionId far = regions.regionAt(CellPos{60, 60, 0});
    const RegionId neighbour = regions.regionAt(CellPos{16, 20, 0});

    // A short wall inside block (0, 1) splits nothing; a wall right across it splits it in two.
    w.things.spawn("Wall", CellPos{5, 20, 0});
    CHECK(regions.regionCount() == 16);
    CHECK(regions.blocksRebuilt() == 17);
    for (int x = 0; x < 16; ++x) {
        w.things.spawn("Wall", CellPos{x, 24, 0});
    }
    CHECK(regions.regionCount() == 17);
    CHECK(regions.blocksRebuilt() == 18);  // sixteen walls, one rebuild: changes wait for a query
    CHECK(regions.regionAt(CellPos{60, 60, 0}) == far);
    CHECK(regions.regionAt(CellPos{16, 20, 0}) == neighbour);
    // The block to the east now borders both halves, as well as its other three neighbours.
    CHECK(regions.region(neighbour).links.size() == 5);
    CHECK(regionsMismatch(w.map).empty());

    // Changes in two blocks rebuild both, no more.
    w.things.spawn("Wall", CellPos{40, 40, 0});
    w.things.spawn("Wall", CellPos{50, 10, 0});
    regions.update();
    CHECK(regions.blocksRebuilt() == 20);
    CHECK(regionsMismatch(w.map).empty());
}

TEST_CASE("Regions: water and rock on a generated map") {
    Map map(MapSettings{128, 128, 3});
    Regions& regions = map.regions();
    CHECK(regions.regionCount() >= 64);
    CHECK(regionsMismatch(map).empty());
}

TEST_CASE("Regions: random walls and doors always match a fresh flood, and the pathfinder agrees") {
    RegionsTestWorld w(MapSettings{64, 64, 11});
    Rng rng(77);
    std::vector<Entity> built;
    Pathfinder finder(w.map);
    std::vector<CellPos> path;
    for (int round = 0; round < 40; ++round) {
        for (int i = 0; i < 25; ++i) {
            if (!built.empty() && rng.below(4) == 0) {
                const uint32_t pick = rng.below(static_cast<uint32_t>(built.size()));
                w.things.destroy(built[pick]);
                built.erase(built.begin() + static_cast<std::ptrdiff_t>(pick));
                continue;
            }
            const CellPos c{static_cast<int>(rng.below(64)), static_cast<int>(rng.below(64)), 0};
            const Entity e = w.things.spawn(rng.below(5) == 0 ? "Door" : "Wall", c);
            if (e != kNoEntity) {
                built.push_back(e);
            }
        }
        const std::string mismatch = regionsMismatch(w.map);
        CHECK_MESSAGE(mismatch.empty(), mismatch);
        if (!mismatch.empty()) {
            return;
        }
        for (int i = 0; i < 5; ++i) {
            const CellPos a{static_cast<int>(rng.below(64)), static_cast<int>(rng.below(64)), 0};
            const CellPos b{static_cast<int>(rng.below(64)), static_cast<int>(rng.below(64)), 0};
            if (w.map.walkable(a) && w.map.walkable(b)) {
                CHECK(w.regions().canReach(a, b) == finder.findPath(a, b, path));
            }
        }
    }
    CHECK(built.size() > 200);
}

TEST_CASE("Regions: the same changes give the same region ids") {
    const auto run = []() {
        RegionsTestWorld w(MapSettings{48, 48, 5});
        Rng rng(3);
        std::vector<RegionId> ids;
        for (int i = 0; i < 300; ++i) {
            w.things.spawn("Wall", CellPos{static_cast<int>(rng.below(48)), static_cast<int>(rng.below(48)), 0});
            if (i % 30 == 0) {
                w.regions().update();
            }
        }
        for (int i = 0; i < 48 * 48; ++i) {
            ids.push_back(w.regions().regionAt(w.map.cellAt(i)));
        }
        return ids;
    };
    CHECK(run() == run());
}
