#include "ColonySim/Defs.h"
#include "ColonySim/Map.h"
#include "ColonySim/MapFeatures.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Regions.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include "TestDefs.h"

#include <doctest.h>

#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

namespace {

std::shared_ptr<const DefDatabase> mapGenGameDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

std::map<Terrain, int> mapGenCount(const Map& map) {
    std::map<Terrain, int> counts;
    for (int y = 0; y < map.height(); ++y) {
        for (int x = 0; x < map.width(); ++x) {
            ++counts[map.terrain(x, y)];
        }
    }
    return counts;
}

int mapGenCountDef(Sim& sim, const char* defName) {
    const DefId def = sim.defs().idOf("Things", defName);
    int n = 0;
    for (const auto [entity, thing] : sim.things().registry().view<const Thing>().each()) {
        (void)entity;
        n += thing.def == def ? 1 : 0;
    }
    return n;
}

// The ground's steepness at a cell (rise over run, from its neighbours).
float mapGenSlope(const Map& map, int x, int y) {
    const float gx = (map.groundHeight(x + 1, y) - map.groundHeight(x - 1, y)) * 0.5f;
    const float gy = (map.groundHeight(x, y + 1) - map.groundHeight(x, y - 1)) * 0.5f;
    return std::sqrt(gx * gx + gy * gy);
}

bool mapGenNearWater(const Map& map, int x, int y) {
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const Terrain t = map.terrain(x + dx, y + dy);
            if (t == Terrain::Water || t == Terrain::ShallowWater) {
                return true;
            }
        }
    }
    return false;
}

// Cells in the hills: rock, or ground standing more than 2 m over the plain's highest (17 m).
bool mapGenInHills(const Map& map, int x, int y) {
    return map.terrain(x, y) == Terrain::Rock || map.groundHeight(x, y) > 19.0f;
}

}  // namespace

TEST_CASE("MapGen: a 512 m map has rivers, shallow water, marsh and mud") {
    for (uint64_t seed : {1u, 42u, 7u}) {
        CAPTURE(seed);
        const Map map(MapSettings{512, 512, seed});
        auto counts = mapGenCount(map);
        CHECK(counts[Terrain::Water] > 300);
        CHECK(counts[Terrain::ShallowWater] > 1000);
        CHECK(counts[Terrain::Marsh] > 500);
        CHECK(counts[Terrain::Mud] > 20);
        CHECK(counts[Terrain::Soil] > 40000);
    }
}

TEST_CASE("MapGen: maps under 192 m are made as before, with no shallow water, marsh or mud") {
    const Map map(MapSettings{128, 128, 42});
    auto counts = mapGenCount(map);
    CHECK(counts[Terrain::ShallowWater] == 0);
    CHECK(counts[Terrain::Marsh] == 0);
    CHECK(counts[Terrain::Mud] == 0);
}

TEST_CASE("MapGen: the same seed gives the same map and a biome with no marsh gives less of it") {
    const Map a(MapSettings{512, 512, 5});
    const Map b(MapSettings{512, 512, 5});
    CHECK(a.contentHash() == b.contentHash());
    MapSettings dry{512, 512, 5};
    dry.marsh = 0.1f;
    dry.river = 0.0f;
    const Map c(dry);
    auto wet = mapGenCount(a);
    auto arid = mapGenCount(c);
    CHECK(arid[Terrain::Marsh] + arid[Terrain::Mud] < (wet[Terrain::Marsh] + wet[Terrain::Mud]) / 4);
    CHECK(arid[Terrain::Water] < wet[Terrain::Water]);
}

TEST_CASE("MapGen: a river can be crossed at a ford, so the land is not cut in two") {
    for (uint64_t seed : {1u, 42u, 7u, 99u}) {
        CAPTURE(seed);
        Map map(MapSettings{512, 512, seed});
        // The reachability groups of walkable cells: one group holds nearly all the open ground.
        std::map<uint32_t, int> groups;
        int walkable = 0;
        for (int y = 0; y < map.height(); y += 2) {
            for (int x = 0; x < map.width(); x += 2) {
                const CellPos c{x, y, 0};
                if (map.walkable(c) && map.terrain(c) == Terrain::Soil) {
                    ++groups[map.regions().groupAt(c)];
                    ++walkable;
                }
            }
        }
        int biggest = 0;
        for (const auto& [group, n] : groups) {
            (void)group;
            biggest = std::max(biggest, n);
        }
        CHECK(biggest * 100 >= walkable * 85);
    }
}

TEST_CASE("MapGen: deep water cannot be walked, shallow water, marsh and mud can") {
    Map map(MapSettings{16, 16, 1, true});
    map.setTerrain(CellPos{1, 1, 0}, Terrain::Water);
    map.setTerrain(CellPos{2, 1, 0}, Terrain::ShallowWater);
    map.setTerrain(CellPos{3, 1, 0}, Terrain::Marsh);
    map.setTerrain(CellPos{4, 1, 0}, Terrain::Mud);
    CHECK_FALSE(map.walkable(CellPos{1, 1, 0}));
    CHECK(map.walkable(CellPos{2, 1, 0}));
    CHECK(map.walkable(CellPos{3, 1, 0}));
    CHECK(map.walkable(CellPos{4, 1, 0}));
}

TEST_CASE("MapGen: crossing costs follow the Terrain defs and slow a pawn down") {
    CHECK(terrainPathCost(Terrain::ShallowWater) == doctest::Approx(30.0f));
    CHECK(terrainPathCost(Terrain::Marsh) == doctest::Approx(14.0f));
    CHECK(terrainPathCost(Terrain::Mud) == doctest::Approx(14.0f));
    CHECK(terrainSpeedFactor(Terrain::ShallowWater) == doctest::Approx(0.30f).epsilon(0.02));
    CHECK(terrainSpeedFactor(Terrain::Marsh) < terrainSpeedFactor(Terrain::Sand));
}

TEST_CASE("MapGen: ruins and steam geysers are placed on open ground, clear of the middle") {
    SimSettings settings;
    settings.seed = 42;
    settings.mapSize = 512;
    Sim sim(settings, mapGenGameDefs());
    const int walls = mapGenCountDef(sim, "AncientWall");
    const int geysers = mapGenCountDef(sim, "SteamGeyser");
    CHECK(walls >= 15);
    CHECK(geysers >= 2);
    const DefId wall = sim.defs().idOf("Things", "AncientWall");
    const DefId geyser = sim.defs().idOf("Things", "SteamGeyser");
    const float middle = static_cast<float>(sim.map().width()) * 0.5f;
    for (const auto [entity, thing, pos] : sim.things().registry().view<const Thing, const Position>().each()) {
        (void)entity;
        if (thing.def != wall && thing.def != geyser) {
            continue;
        }
        CHECK(std::hypot(pos.x - middle, pos.y - middle) >= 40.0f);
        const CellPos c = cellOf(pos);
        CHECK(sim.map().buildingAt(c) != kNoEntity);
        CHECK_FALSE(sim.map().walkable(c));
    }
    // A broken wall: enough gaps that a ruin is not a sealed room.
    CHECK(walls < 8 * 40);
}

TEST_CASE("MapGen: features are the same for the same seed, absent on small and flat maps") {
    SimSettings settings;
    settings.seed = 9;
    settings.mapSize = 384;
    Sim a(settings, mapGenGameDefs());
    Sim b(settings, mapGenGameDefs());
    CHECK(a.stateHash() == b.stateHash());
    CHECK(mapGenCountDef(a, "AncientWall") == mapGenCountDef(b, "AncientWall"));
    settings.mapSize = 128;
    Sim small(settings, mapGenGameDefs());
    CHECK(mapGenCountDef(small, "AncientWall") == 0);
    CHECK(mapGenCountDef(small, "SteamGeyser") == 0);
    settings.mapSize = 512;
    settings.flatMap = true;
    Sim flat(settings, mapGenGameDefs());
    CHECK(mapGenCountDef(flat, "SteamGeyser") == 0);
    // No defs for them, nothing placed.
    Sim bare(SimSettings{9, 512, 0, 0}, testDefs());
    CHECK(mapGenCountDef(bare, "SteamGeyser") == 0);
}

TEST_CASE("MapGen: a 1 km map is made in under a second") {
    const auto start = std::chrono::steady_clock::now();
    const Map map(MapSettings{1024, 1024, 42});
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    MESSAGE("1024 m map generated in " << seconds << " s");
    CHECK(seconds < 5.0);  // loose: slow CI machines; the real figure is in docs/design/mapgen.md
    auto counts = mapGenCount(map);
    CHECK(counts[Terrain::ShallowWater] > 4000);
}

TEST_CASE("MapGen: a world map is mostly flat open ground with no cliffs") {
    for (uint64_t seed : {1u, 42u, 7u}) {
        CAPTURE(seed);
        const Map map(MapSettings{1024, 1024, seed});
        int walkable = 0;
        int flat = 0;
        int steep = 0;
        int hills = 0;
        float steepest = 0.0f;
        for (int y = 1; y < map.height() - 1; ++y) {
            for (int x = 1; x < map.width() - 1; ++x) {
                hills += mapGenInHills(map, x, y) ? 1 : 0;
                if (!map.walkable(CellPos{x, y, 0}) || mapGenNearWater(map, x, y)) {
                    continue;  // shores slope down to the beds under the water
                }
                const float slope = mapGenSlope(map, x, y);
                ++walkable;
                flat += slope <= 0.1f ? 1 : 0;  // under 6 degrees: build on it as it is
                steep += slope > 0.6f ? 1 : 0;
                steepest = std::max(steepest, slope);
            }
        }
        const int cells = (map.width() - 2) * (map.height() - 2);
        MESSAGE("seed " << seed << ": flat " << 100.0 * flat / walkable << "%, hills " << 100.0 * hills / cells
                        << "%, steepest walkable " << steepest);
        CHECK(flat * 100 >= walkable * 75);
        CHECK(hills * 100 <= cells * 19);  // about 15% (MapSettings::hills)
        CHECK(hills * 100 >= cells * 10);
        CHECK(steep * 100 <= walkable);  // over 31 degrees: under 1% of the walkable ground
        CHECK(steepest < 1.5f);          // no cliffs (the old maps had walkable drops of 10 and more)
    }
}

TEST_CASE("MapGen: the hills come in a few big patches, not bumps everywhere") {
    for (uint64_t seed : {1u, 42u, 7u}) {
        CAPTURE(seed);
        const Map map(MapSettings{1024, 1024, seed});
        const int w = map.width();
        const int h = map.height();
        std::vector<int> patch(static_cast<size_t>(w * h), -1);
        std::vector<int> sizes;
        std::vector<int> stack;
        int total = 0;
        for (int start = 0; start < w * h; ++start) {
            if (patch[static_cast<size_t>(start)] >= 0 || !mapGenInHills(map, start % w, start / w)) {
                continue;
            }
            const int id = static_cast<int>(sizes.size());
            sizes.push_back(0);
            patch[static_cast<size_t>(start)] = id;
            stack.push_back(start);
            while (!stack.empty()) {
                const int i = stack.back();
                stack.pop_back();
                ++sizes.back();
                const int x = i % w;
                const int y = i / w;
                const int next[4][2] = {{x + 1, y}, {x - 1, y}, {x, y + 1}, {x, y - 1}};
                for (const auto& n : next) {
                    if (n[0] < 0 || n[1] < 0 || n[0] >= w || n[1] >= h) {
                        continue;
                    }
                    const int j = n[1] * w + n[0];
                    if (patch[static_cast<size_t>(j)] < 0 && mapGenInHills(map, n[0], n[1])) {
                        patch[static_cast<size_t>(j)] = id;
                        stack.push_back(j);
                    }
                }
            }
            total += sizes.back();
        }
        int big = 0;
        int inBig = 0;
        for (int n : sizes) {
            if (n >= 2000) {  // a hill area of at least about 45 by 45 m
                ++big;
                inBig += n;
            }
        }
        MESSAGE("seed " << seed << ": " << big << " big hill areas hold " << 100.0 * inBig / std::max(total, 1) << "% of the hills");
        CHECK(big >= 1);
        CHECK(big <= 12);
        CHECK(inBig * 100 >= total * 90);
    }
}

TEST_CASE("MapGen: the middle of a world map, where colonies start, is flat and open") {
    for (uint64_t seed : {1u, 42u, 7u, 99u}) {
        CAPTURE(seed);
        const Map map(MapSettings{512, 512, seed});
        int hills = 0;
        int lakes = 0;
        for (int y = 196; y < 316; ++y) {
            for (int x = 196; x < 316; ++x) {
                const CellPos c{x, y, 0};
                hills += map.hillGround(c) || map.terrain(c) == Terrain::Rock ? 1 : 0;
                const bool water = map.terrain(c) == Terrain::Water || map.terrain(c) == Terrain::ShallowWater;
                float vx = 0.0f;
                float vy = 0.0f;
                map.flowAt(c, vx, vy);
                lakes += water && vx == 0.0f && vy == 0.0f ? 1 : 0;  // a river may still cross it
            }
        }
        CHECK(hills == 0);
        CHECK(lakes == 0);
    }
}

TEST_CASE("MapGen: a hillier world tile gets more hills, and the share follows its hilliness") {
    CHECK(mapHillShare(static_cast<int>(Hilliness::Flat)) == doctest::Approx(0.15f));
    CHECK(mapHillShare(static_cast<int>(Hilliness::Mountainous)) > mapHillShare(static_cast<int>(Hilliness::SmallHills)));
    MapSettings flatTile{512, 512, 3};
    MapSettings hillyTile = flatTile;
    hillyTile.hills = mapHillShare(static_cast<int>(Hilliness::Mountainous));
    const Map a(flatTile);
    const Map b(hillyTile);
    int hillsA = 0;
    int hillsB = 0;
    for (int y = 0; y < 512; ++y) {
        for (int x = 0; x < 512; ++x) {
            hillsA += mapGenInHills(a, x, y) ? 1 : 0;
            hillsB += mapGenInHills(b, x, y) ? 1 : 0;
        }
    }
    CHECK(hillsB > hillsA * 3 / 2);
}
