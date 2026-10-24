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
    CHECK(map.inBounds(0, 0));
    CHECK(map.inBounds(63, 31));
    CHECK_FALSE(map.inBounds(64, 0));
    CHECK_FALSE(map.inBounds(0, -1));
    for (int y = 0; y < map.height(); ++y) {
        for (int x = 0; x < map.width(); ++x) {
            CHECK(map.groundHeight(x, y) >= 0.0f);
            CHECK(map.groundHeight(x, y) <= 60.0f);
        }
    }
}
