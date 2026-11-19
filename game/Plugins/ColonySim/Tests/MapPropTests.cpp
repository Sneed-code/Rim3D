#include "ColonySim/Construction.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Floors.h"
#include "ColonySim/Map.h"
#include "ColonySim/MapProps.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include "TestDefs.h"

#include <doctest.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

namespace {

std::shared_ptr<const DefDatabase> mapPropGameDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

std::vector<Entity> mapPropAll(Sim& sim, const char* defName) {
    const DefId def = sim.defs().idOf("Things", defName);
    std::vector<Entity> out;
    for (const auto [entity, thing] : sim.things().registry().view<const Thing>().each()) {
        if (thing.def == def) {
            out.push_back(entity);
        }
    }
    return out;
}

int mapPropCountPrefix(Sim& sim, const std::string& prefix) {
    int n = 0;
    for (const auto [entity, thing] : sim.things().registry().view<const Thing>().each()) {
        (void)entity;
        n += sim.defs().get(thing.def).defName().rfind(prefix, 0) == 0 ? 1 : 0;
    }
    return n;
}

// Items of a def lying within `radius` cells of `c`, counted by stack.
int mapPropItemsNear(Sim& sim, CellPos c, const char* defName, int radius) {
    const DefId def = sim.defs().idOf("Things", defName);
    int n = 0;
    for (int y = c.y - radius; y <= c.y + radius; ++y) {
        for (int x = c.x - radius; x <= c.x + radius; ++x) {
            const CellPos p{x, y, 0};
            if (!sim.map().inBounds(p)) {
                continue;
            }
            for (const Entity e : sim.things().itemsAt(p)) {
                if (sim.things().registry().get<Thing>(e).def == def) {
                    const StackCount* s = sim.things().registry().try_get<StackCount>(e);
                    n += s != nullptr ? s->count : 1;
                }
            }
        }
    }
    return n;
}

Sim& mapPropSim() {
    static std::unique_ptr<Sim> sim = [] {
        SimSettings settings;
        settings.seed = 42;
        settings.mapSize = 512;
        return std::make_unique<Sim>(settings, mapPropGameDefs());
    }();
    return *sim;
}

}  // namespace

TEST_CASE("MapProps: a 512 m map gets boulders, fallen logs, stone chunks and wreckage, clear of the middle") {
    Sim& sim = mapPropSim();
    CHECK(mapPropCountPrefix(sim, "Boulder") >= 10);
    CHECK(mapPropAll(sim, "FallenLog").size() >= 5);
    const int chunks = mapPropCountPrefix(sim, "Chunk") - static_cast<int>(mapPropAll(sim, "ChunkSlagSteel").size());
    CHECK(chunks >= 20);
    CHECK(mapPropAll(sim, "ShipChunk").size() >= 1);
    CHECK(mapPropAll(sim, "ChunkSlagSteel").size() >= 2);
    const float middle = static_cast<float>(sim.map().width()) * 0.5f;
    for (const char* name : {"FallenLog", "ShipChunk", "AncientCryptosleepCasket"}) {
        for (const Entity e : mapPropAll(sim, name)) {
            const Position& p = sim.things().registry().get<Position>(e);
            CHECK(std::hypot(p.x - middle, p.y - middle) >= 18.0f);
            // Nothing grows under it.
            CHECK_FALSE(sim.plants().has(cellOf(p)));
        }
    }
}

TEST_CASE("MapProps: an ancient danger room is sealed and roofed, with caskets and loot inside") {
    Sim& sim = mapPropSim();
    const std::vector<Entity> caskets = mapPropAll(sim, "AncientCryptosleepCasket");
    CHECK(caskets.size() >= 2);
    if (!(caskets.size() >= 2)) {
        return;
    }
    const CellPos c = cellOf(sim.things().registry().get<Position>(caskets[0]));
    // Flood out from beside the casket through walkable cells: the room is closed, so it stays small.
    std::vector<CellPos> open;
    std::vector<char> seen(static_cast<size_t>(sim.map().width() * sim.map().height()), 0);
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const CellPos p{c.x + dx, c.y + dy, 0};
            if (sim.map().walkable(p)) {
                open.push_back(p);
                seen[static_cast<size_t>(sim.map().index(p))] = 1;
            }
        }
    }
    CHECK_FALSE(open.empty());
    if (open.empty()) {
        return;
    }
    size_t reached = 0;
    while (!open.empty() && reached < 1000) {
        const CellPos p = open.back();
        open.pop_back();
        ++reached;
        CHECK(sim.map().roof(p) != Roof::None);
        static constexpr int kStep[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (const auto& s : kStep) {
            const CellPos q{p.x + s[0], p.y + s[1], 0};
            if (sim.map().walkable(q) && seen[static_cast<size_t>(sim.map().index(q))] == 0) {
                seen[static_cast<size_t>(sim.map().index(q))] = 1;
                open.push_back(q);
            }
        }
    }
    CHECK(reached < 15 * 11);
    // The ancient danger loot: survival meals always.
    CHECK(mapPropItemsNear(sim, c, "MealSurvivalPack", 15) >= 3);
}

TEST_CASE("MapProps: wreckage, boulders and logs give their materials when taken down") {
    SimSettings settings;
    settings.seed = 42;
    settings.mapSize = 512;
    Sim sim(settings, mapPropGameDefs());
    const std::vector<Entity> wrecks = mapPropAll(sim, "ShipChunk");
    CHECK_FALSE(wrecks.empty());
    if (wrecks.empty()) {
        return;
    }
    const CellPos w = cellOf(sim.things().registry().get<Position>(wrecks[0]));
    const int steelBefore = mapPropItemsNear(sim, w, "Steel", 12);
    const int compBefore = mapPropItemsNear(sim, w, "ComponentIndustrial", 12);
    const bool wrecksDown = sim.construction().deconstruct(wrecks[0]);
    CHECK(wrecksDown);
    if (!wrecksDown) {
        return;
    }
    CHECK(mapPropItemsNear(sim, w, "Steel", 12) - steelBefore == 20);
    const int comps = mapPropItemsNear(sim, w, "ComponentIndustrial", 12) - compBefore;
    CHECK(comps >= 5);
    CHECK(comps <= 6);

    const std::vector<Entity> logs = mapPropAll(sim, "FallenLog");
    CHECK_FALSE(logs.empty());
    if (logs.empty()) {
        return;
    }
    const CellPos l = cellOf(sim.things().registry().get<Position>(logs[0]));
    const int woodBefore = mapPropItemsNear(sim, l, "WoodLog", 10);
    const bool logsDown = sim.construction().deconstruct(logs[0]);
    CHECK(logsDown);
    if (!logsDown) {
        return;
    }
    CHECK(mapPropItemsNear(sim, l, "WoodLog", 10) - woodBefore == 25);

    for (const char* rock : {"Granite", "Limestone", "Sandstone", "Slate", "Marble"}) {
        const std::vector<Entity> boulders = mapPropAll(sim, (std::string("Boulder") + rock).c_str());
        if (boulders.empty()) {
            continue;
        }
        const CellPos b = cellOf(sim.things().registry().get<Position>(boulders[0]));
        const std::string chunk = std::string("Chunk") + rock;
        const int before = mapPropItemsNear(sim, b, chunk.c_str(), 10);
        const bool bouldersDown = sim.construction().deconstruct(boulders[0]);
        CHECK(bouldersDown);
        if (!bouldersDown) {
            return;
        }
        CHECK(mapPropItemsNear(sim, b, chunk.c_str(), 10) - before == 1);
        break;
    }
}

TEST_CASE("MapProps: the same seed gives the same props; small and flat maps get none") {
    SimSettings settings;
    settings.seed = 7;
    settings.mapSize = 384;
    Sim a(settings, mapPropGameDefs());
    Sim b(settings, mapPropGameDefs());
    CHECK(a.stateHash() == b.stateHash());
    CHECK(mapPropCountPrefix(a, "Boulder") == mapPropCountPrefix(b, "Boulder"));
    settings.mapSize = 128;
    Sim small(settings, mapPropGameDefs());
    CHECK(mapPropCountPrefix(small, "Boulder") == 0);
    CHECK(mapPropAll(small, "ShipChunk").empty());
    settings.mapSize = 512;
    settings.flatMap = true;
    Sim flat(settings, mapPropGameDefs());
    CHECK(mapPropAll(flat, "AncientCryptosleepCasket").empty());
}

TEST_CASE("MapProps: a world road crosses the map as a strip of road floor with nothing growing on it") {
    const auto defs = mapPropGameDefs();
    MapSettings ms;
    ms.width = 300;
    ms.height = 300;
    ms.seed = 5;
    ms.flat = true;
    Map map(ms);
    Things things(map, defs);
    Floors floors(map, *defs);
    Plants plants(map, *defs, 5, "TropicalRainforest");
    MapPropInput in;
    in.map = &map;
    in.things = &things;
    in.floors = &floors;
    in.plants = &plants;
    in.road = RoadKind::Dirt;
    in.seed = 11;
    const MapPropCounts counts = placeMapProps(in);
    CHECK(counts.roadCells >= 300 * 3 - 10);
    const DefId dirt = defs->idOf("Terrain", "PackedDirt");
    int onEdge = 0;
    for (const auto& [index, def] : floors.all()) {
        CHECK(def == dirt);
        const CellPos c{index % map.width(), index / map.width(), 0};
        CHECK_FALSE(plants.has(c));
        onEdge += (c.x == 0 || c.y == 0 || c.x == map.width() - 1 || c.y == map.height() - 1) ? 1 : 0;
    }
    CHECK(onEdge >= 6);  // it runs from one edge to the other
    // Without a road, none.
    Map map2(ms);
    Things things2(map2, defs);
    Floors floors2(map2, *defs);
    in.map = &map2;
    in.things = &things2;
    in.floors = &floors2;
    in.plants = nullptr;
    in.road = RoadKind::None;
    CHECK(placeMapProps(in).roadCells == 0);
    CHECK(floors2.count() == 0);
}

TEST_CASE("MapProps: road floors can't be laid by colonists") {
    Sim& sim = mapPropSim();
    const DefId dirt = sim.defs().idOf("Terrain", "PackedDirt");
    CHECK_FALSE(sim.construction().whyNotFloorPlan(dirt, DefId{}, CellPos{256, 256, 0}).empty());
}
