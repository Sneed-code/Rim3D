#include "ColonySim/Map.h"

#include <doctest.h>

using namespace colony;

TEST_CASE("Map: same seed generates the same map") {
    const Map a(MapSettings{128, 96, 1234});
    const Map b(MapSettings{128, 96, 1234});
    CHECK(a.contentHash() == b.contentHash());
}

TEST_CASE("Map: different seeds generate different maps") {
    const Map a(MapSettings{128, 128, 1});
    const Map b(MapSettings{128, 128, 2});
    CHECK(a.contentHash() != b.contentHash());
}

TEST_CASE("Map: size, bounds and heights") {
    const Map map(MapSettings{64, 32, 9});
    CHECK(map.width() == 64);
    CHECK(map.height() == 32);
    CHECK(map.inBounds(CellPos{0, 0, 0}));
    CHECK(map.inBounds(CellPos{63, 31, 0}));
    CHECK_FALSE(map.inBounds(CellPos{64, 0, 0}));
    CHECK_FALSE(map.inBounds(CellPos{0, -1, 0}));
    for (int y = 0; y < map.height(); ++y) {
        for (int x = 0; x < map.width(); ++x) {
            CHECK(map.groundHeight(CellPos{x, y, 0}) >= 0.0f);
            CHECK(map.groundHeight(CellPos{x, y, 0}) <= 60.0f);
            CHECK(map.groundHeight(x, y) == map.groundHeight(CellPos{x, y, 0}));
        }
    }
}

TEST_CASE("Map: upper levels are empty sky until something is built there") {
    const Map map(MapSettings{16, 16, 9});
    CHECK(map.levels() == Map::kLevels);
    CHECK(map.inBounds(CellPos{3, 3, 1}));
    CHECK_FALSE(map.onGround(CellPos{3, 3, 1}));
    CHECK_FALSE(map.inBounds(CellPos{3, 3, Map::kLevels}));
    CHECK_FALSE(map.inBounds(CellPos{3, 3, -1}));
    CHECK(map.terrain(CellPos{3, 3, 1}) == Terrain::Air);
    CHECK_FALSE(map.walkable(CellPos{3, 3, 1}));
    CHECK_FALSE(map.hasUpperChunk(CellPos{3, 3, 1}));
    CHECK(map.upperFloorCells().empty());
}

TEST_CASE("Map: cell index round trip") {
    const Map map(MapSettings{40, 20, 9});
    const CellPos c{17, 13, 0};
    CHECK(map.cellAt(map.index(c)) == c);
}

TEST_CASE("Map: flat test maps are all walkable soil") {
    const Map map(MapSettings{32, 32, 5, true});
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            CHECK(map.terrain(CellPos{x, y, 0}) == Terrain::Soil);
            CHECK(map.walkable(CellPos{x, y, 0}));
            CHECK(map.buildingAt(CellPos{x, y, 0}) == kNoEntity);
        }
    }
}
