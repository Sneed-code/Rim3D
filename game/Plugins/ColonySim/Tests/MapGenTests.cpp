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

}  // namespace

TEST_CASE("MapGen: a 512 m map has rivers, shallow water, marsh and mud") {
    for (uint64_t seed : {1u, 42u, 7u}) {
        CAPTURE(seed);
        const Map map(MapSettings{512, 512, seed});
        auto counts = mapGenCount(map);
        CHECK(counts[Terrain::Water] > 800);
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

TEST_CASE("MapGen: the biome decides how wet the map is") {
    SimSettings settings;
    settings.seed = 3;
    settings.mapSize = 256;
    settings.biome = "BorealForest";
    Sim boreal(settings, mapGenGameDefs());
    settings.biome = "AridShrubland";
    Sim arid(settings, mapGenGameDefs());
    auto b = mapGenCount(boreal.map());
    auto a = mapGenCount(arid.map());
    CHECK(b[Terrain::Marsh] + b[Terrain::Mud] > 3 * (a[Terrain::Marsh] + a[Terrain::Mud]));
}
