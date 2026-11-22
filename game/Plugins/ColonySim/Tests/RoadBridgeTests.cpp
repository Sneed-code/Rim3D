// Roads on generated maps and bridges you can build, blow up and fall off (docs/design/roads-bridges.md).

#include "ColonySim/Commands.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Floors.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/MapRoads.h"
#include "ColonySim/Regions.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"
#include "ColonySim/World.h"

#include <doctest.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

namespace {

std::shared_ptr<const DefDatabase> roadGameDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

// A 32 m flat map with a river of deep water down x 12 to 16 (flowing), shallow banks at x 11 and 17, and a still
// pond of deep water at x 24 to 28, y 4 to 8.
std::unique_ptr<Sim> roadRiverSim() {
    SimSettings settings;
    settings.seed = 5;
    settings.mapSize = 32;
    settings.flatMap = true;
    auto sim = std::make_unique<Sim>(settings, roadGameDefs());
    Map& map = sim->editMap();
    for (int y = 0; y < 32; ++y) {
        map.setTerrain(CellPos{11, y, 0}, Terrain::ShallowWater);
        map.setTerrain(CellPos{17, y, 0}, Terrain::ShallowWater);
        for (int x = 12; x <= 16; ++x) {
            map.setTerrain(CellPos{x, y, 0}, Terrain::Water);
            map.setFlowing(CellPos{x, y, 0}, true);
        }
    }
    for (int y = 4; y <= 8; ++y) {
        for (int x = 24; x <= 28; ++x) {
            map.setTerrain(CellPos{x, y, 0}, Terrain::Water);
        }
    }
    return sim;
}

DefId roadDef(const Sim& sim, const char* name) { return sim.defs().idOf("Things", name); }

int roadCount(Sim& sim, const char* defName) {
    const DefId def = roadDef(sim, defName);
    int n = 0;
    for (const auto [entity, thing] : sim.things().registry().view<const Thing>().each()) {
        (void)entity;
        n += thing.def == def ? 1 : 0;
    }
    return n;
}

}  // namespace

TEST_CASE("Bridges: each kind goes where RimWorld lets it, and the plan says why not") {
    auto sim = roadRiverSim();
    const Construction& build = sim->construction();
    const DefId wood = roadDef(*sim, "Bridge");
    const DefId heavy = roadDef(*sim, "HeavyBridge");
    CHECK(wood.valid());
    if (!(wood.valid())) {
        return;
    }
    CHECK(heavy.valid());
    if (!(heavy.valid())) {
        return;
    }
    // A wooden bridge: shallow water yes; a river's deep water, dry land no.
    CHECK(build.whyNotBlueprint(wood, DefId{}, CellPos{11, 5, 0}, Rot4::North).empty());
    CHECK(build.whyNotBlueprint(wood, DefId{}, CellPos{14, 5, 0}, Rot4::North).find("shallow water") != std::string::npos);
    CHECK_FALSE(build.whyNotBlueprint(wood, DefId{}, CellPos{5, 5, 0}, Rot4::North).empty());
    // A heavy bridge also spans the river's deep water, but not a still pond's.
    CHECK(build.whyNotBlueprint(heavy, DefId{}, CellPos{14, 5, 0}, Rot4::North).empty());
    CHECK_FALSE(build.whyNotBlueprint(heavy, DefId{}, CellPos{26, 6, 0}, Rot4::North).empty());
    // The old stone bridge (map generation) goes over any water.
    CHECK(sim->things().spawn(roadDef(*sim, "AncientBridge"), CellPos{26, 6, 0}) != kNoEntity);
    // Road surfaces are not on the Floors menu.
    CHECK_FALSE(build.whyNotFloorPlan(sim->defs().idOf("Terrain", "PackedDirt"), DefId{}, CellPos{5, 5, 0}).empty());
}

TEST_CASE("Bridges: a deck makes the river walkable at its own height, and goes with the bridge") {
    auto sim = roadRiverSim();
    Map& map = sim->editMap();
    const CellPos west{6, 20, 0};
    const CellPos east{22, 20, 0};
    CHECK_FALSE(map.regions().canReach(west, east));

    std::vector<Entity> span;
    span.push_back(sim->things().spawn(roadDef(*sim, "Bridge"), CellPos{11, 20, 0}));
    for (int x = 12; x <= 16; ++x) {
        span.push_back(sim->things().spawn(roadDef(*sim, "HeavyBridge"), CellPos{x, 20, 0}));
    }
    span.push_back(sim->things().spawn(roadDef(*sim, "Bridge"), CellPos{17, 20, 0}));
    for (const Entity e : span) {
        CHECK(e != kNoEntity);
    if (!(e != kNoEntity)) {
        return;
    }
    }
    CHECK(map.deckCount() == 7);
    CHECK(map.regions().canReach(west, east));
    // On the deck a pawn stands dry, just over the water, and walks at full speed.
    const float surface = map.waterSurface(CellPos{14, 20, 0});
    CHECK(map.heightAt(14.5f, 20.5f) == doctest::Approx(surface + Map::kDeckLiftM));
    CHECK(map.waterDepthAt(14.5f, 20.5f) == doctest::Approx(0.0f));
    CHECK(map.waterDepthAt(14.5f, 21.5f) > 1.0f);
    // Nothing else can be built on a deck, not even a floor.
    CHECK_FALSE(sim->construction().whyNotFloorPlan(sim->defs().idOf("Terrain", "WoodFloor"), DefId{}, CellPos{14, 20, 0}).empty());

    // A pawn walks across.
    const Entity pawn = sim->things().spawn("Human", west);
    CHECK(pawn != kNoEntity);
    if (!(pawn != kNoEntity)) {
        return;
    }
    sim->jobs().addWorker(pawn);
    sim->submit(DraftCommand{pawn, true});
    sim->step();
    sim->submit(MoveOrderCommand{pawn, east});
    bool crossed = false;
    for (int i = 0; i < 900 && !crossed; ++i) {
        sim->step();
        crossed = sim->things().registry().get<Position>(pawn).x > 19.0f;
    }
    CHECK(crossed);

    // Take out a middle span: the river can't be crossed there any more.
    sim->things().destroy(span[3]);
    sim->step();
    CHECK_FALSE(map.walkable(CellPos{14, 20, 0}));
    CHECK(map.deckCount() == 6);
    CHECK_FALSE(map.regions().canReach(west, east));
}

TEST_CASE("Bridges: a big blast takes a wooden bridge at once; a heavy bridge shrugs it off") {
    auto sim = roadRiverSim();
    const Entity wood = sim->things().spawn(roadDef(*sim, "Bridge"), CellPos{11, 10, 0});
    const Entity heavy = sim->things().spawn(roadDef(*sim, "HeavyBridge"), CellPos{12, 10, 0});
    const Entity woodFar = sim->things().spawn(roadDef(*sim, "Bridge"), CellPos{11, 25, 0});
    CHECK(wood != kNoEntity);
    if (!(wood != kNoEntity)) {
        return;
    }
    CHECK(heavy != kNoEntity);
    if (!(heavy != kNoEntity)) {
        return;
    }
    CHECK(woodFar != kNoEntity);
    if (!(woodFar != kNoEntity)) {
        return;
    }
    auto& reg = sim->things().registry();
    const int heavyHp = reg.get<HitPoints>(heavy).current;
    // A frag grenade's blast (50 Bomb) next to them: the wooden deck has 100 hit points but goes at 35 or more.
    sim->submit(ExplodeCommand{12.0f, 10.5f, 1.9f, 50.0f, "Bomb", 0.1f});
    sim->step();
    CHECK_FALSE(reg.valid(wood));
    CHECK(reg.valid(heavy));
    CHECK(reg.get<HitPoints>(heavy).current == heavyHp);
    // A weak blast only dents one.
    sim->submit(ExplodeCommand{11.5f, 25.5f, 1.9f, 20.0f, "Bomb", 0.1f});
    sim->step();
    CHECK(reg.valid(woodFar));
    if (!(reg.valid(woodFar))) {
        return;
    }
    CHECK(reg.get<HitPoints>(woodFar).current < 100);
}

TEST_CASE("Bridges: what stood on a bridge over deep water is swept away; a pawn swims ashore") {
    auto sim = roadRiverSim();
    std::vector<Entity> span;
    for (int y = 4; y <= 8; ++y) {
        span.push_back(sim->things().spawn(roadDef(*sim, "AncientBridge"), CellPos{26, y, 0}));
        CHECK(span.back() != kNoEntity);
    if (!(span.back() != kNoEntity)) {
        return;
    }
    }
    const Entity steel = sim->things().spawn("Steel", CellPos{26, 5, 0}, 20);
    const Entity pawn = sim->things().spawn("Human", CellPos{26, 7, 0});
    CHECK(steel != kNoEntity);
    if (!(steel != kNoEntity)) {
        return;
    }
    CHECK(pawn != kNoEntity);
    if (!(pawn != kNoEntity)) {
        return;
    }
    for (const Entity e : span) {
        sim->things().destroy(e);
    }
    sim->step();
    auto& reg = sim->things().registry();
    CHECK_FALSE(reg.valid(steel));
    CHECK(reg.valid(pawn));
    if (!(reg.valid(pawn))) {
        return;
    }
    for (int i = 0; i < 600 && !sim->map().walkable(cellOf(reg.get<Position>(pawn))); ++i) {
        sim->step();
    }
    CHECK(sim->map().walkable(cellOf(reg.get<Position>(pawn))));
}

TEST_CASE("Roads: a world tile's roads come in from its sides and bridge the river") {
    const auto defs = roadGameDefs();
    const World world = World::generate(*defs, 42);
    CHECK_FALSE(world.empty());
    if (world.empty()) {
        return;
    }
    int tried = 0;
    int bridged = 0;
    for (int tile = 0; tile < world.tileCount() && tried < 3; ++tile) {
        if (!world.canStartAt(tile) || world.tile(tile).road == RoadKind::None) {
            continue;
        }
        ++tried;
        CAPTURE(tile);
        SimSettings settings;
        settings.seed = 42;
        settings.mapSize = 512;
        settings.world.enabled = true;
        settings.world.startTile = tile;
        Sim sim(settings, defs);
        CHECK(sim.site() != nullptr);
    if (!(sim.site() != nullptr)) {
        return;
    }
        const std::array<RoadKind, 8>& edges = sim.site()->roadEdges;
        int roadSides = 0;
        for (const RoadKind k : edges) {
            roadSides += k != RoadKind::None ? 1 : 0;
        }
        CHECK(roadSides > 0);
        // Road surface on the map, and some of it on the edge of each straight side the world names.
        int roadCells = 0;
        for (const auto& [index, floor] : sim.floors().all()) {
            (void)index;
            const std::string& name = sim.defs().get(floor).defName();
            roadCells += name == "PackedDirt" || name == "Flagstone" || name == "BrokenAsphalt" ? 1 : 0;
        }
        CHECK(roadCells > 300);
        for (int dir = 0; dir < 8; dir += 2) {
            if (edges[static_cast<size_t>(dir)] == RoadKind::None) {
                continue;
            }
            CAPTURE(dir);
            bool onEdge = false;
            for (int t = 0; t < 512 && !onEdge; ++t) {
                const int x = World::kNeighbourDx[dir] == 0 ? t : (World::kNeighbourDx[dir] > 0 ? 511 : 0);
                const int y = World::kNeighbourDy[dir] == 0 ? t : (World::kNeighbourDy[dir] > 0 ? 511 : 0);
                const CellPos c{x, y, 0};
                onEdge = sim.floors().at(c).valid() || sim.map().hasDeck(c);
            }
            CHECK(onEdge);
        }
        const int bridges = roadCount(sim, "AncientBridge");
        CHECK(bridges == static_cast<int>(sim.map().deckCount()));
        bridged += bridges > 0 ? 1 : 0;
        // The same world and tile give the same roads.
        Sim again(settings, defs);
        CHECK(again.floors().all() == sim.floors().all());
        CHECK(roadCount(again, "AncientBridge") == bridges);
    }
    CHECK(tried == 3);
    CHECK(bridged > 0);
}
