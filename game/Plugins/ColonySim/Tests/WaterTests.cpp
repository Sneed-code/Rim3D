#include "doctest.h"

#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Events.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"
#include "ColonySim/Water.h"

#include "TestDefs.h"

#include <cmath>
#include <vector>

using namespace colony;

// Water as a surface, wading and swimming (docs/design/water.md).

namespace {

bool waterTestFailed(const Sim& sim, uint64_t command) {
    for (const Event& e : sim.events()) {
        if (e.type == EventType::CommandFailed && e.command == command) {
            return true;
        }
    }
    return false;
}

// A flat map with a pond of deep water (x 10..19, y 10..19) and a strip of shallow water along y 4..6.
void waterTestPond(Map& map) {
    for (int y = 10; y < 20; ++y) {
        for (int x = 10; x < 20; ++x) {
            map.setTerrain(CellPos{x, y, 0}, Terrain::Water);
        }
    }
    for (int y = 4; y <= 6; ++y) {
        for (int x = 0; x < map.width(); ++x) {
            map.setTerrain(CellPos{x, y, 0}, Terrain::ShallowWater);
        }
    }
}

}  // namespace

TEST_CASE("Water: depth bands follow the body's height") {
    CHECK(waterBandFor(0.0f, 1.8f) == WaterBand::Dry);
    CHECK(waterBandFor(0.03f, 1.8f) == WaterBand::Dry);
    CHECK(waterBandFor(0.3f, 1.8f) == WaterBand::Ankle);
    CHECK(waterBandFor(0.7f, 1.8f) == WaterBand::Waist);
    CHECK(waterBandFor(1.2f, 1.8f) == WaterBand::Chest);
    CHECK(waterBandFor(1.5f, 1.8f) == WaterBand::Swimming);
    // Knee-deep for a man is over a cat's back.
    CHECK(waterBandFor(0.5f, 0.45f) == WaterBand::Swimming);
    // RimWorld's numbers: shallow water 30, chest-deep 42 (7.7% a point), and swimming is slower still.
    CHECK(waterSpeedFactor(WaterBand::Waist) == doctest::Approx(1.0f / (1.0f + 30.0f * 0.077f)));
    CHECK(waterSpeedFactor(WaterBand::Chest) == doctest::Approx(1.0f / (1.0f + 42.0f * 0.077f)));
    CHECK(waterSpeedFactor(WaterBand::Swimming) < waterSpeedFactor(WaterBand::Chest));
    CHECK(waterSpeedFactor(WaterBand::Dry) == doctest::Approx(1.0f));
}

TEST_CASE("Water: hand-made water stands over its ground, marsh has a film, dry ground none") {
    Map map(MapSettings{32, 32, 1, true});
    waterTestPond(map);
    map.setTerrain(CellPos{25, 25, 0}, Terrain::Marsh);
    CHECK(std::isnan(map.waterTable()));
    CHECK(map.waterSurface(CellPos{12, 12, 0}) == doctest::Approx(Map::kHandDeepM));
    CHECK(map.waterSurface(CellPos{3, 5, 0}) == doctest::Approx(Map::kHandShallowM));
    CHECK(map.waterSurface(CellPos{25, 25, 0}) == doctest::Approx(Map::kMarshFilmM));
    CHECK(std::isnan(map.waterSurface(CellPos{2, 2, 0})));
    CHECK(map.waterDepthAt(15.0f, 15.0f) == doctest::Approx(Map::kHandDeepM));
    CHECK(map.waterDepthAt(2.5f, 2.5f) == 0.0f);
    CHECK(map.waterDepthAt(15.0f, 15.0f, 1) == 0.0f);  // upper levels are dry

    // Walking speed by depth: the shallow strip is shin-deep for a man (shallow water's cost), over a cat's head.
    CHECK(groundSpeedFactor(map, 3.5f, 5.5f) == doctest::Approx(waterSpeedFactor(WaterBand::Waist)));
    CHECK(groundSpeedFactor(map, 3.5f, 5.5f, 0.4f) == doctest::Approx(kSwimSpeedFactor));
    CHECK(groundSpeedFactor(map, 15.5f, 15.5f) == doctest::Approx(kSwimSpeedFactor));
    // Marsh keeps its own cost (14) for a man; dry soil its 2.
    CHECK(groundSpeedFactor(map, 25.5f, 25.5f) == doctest::Approx(terrainSpeedFactor(Terrain::Marsh)));
    CHECK(groundSpeedFactor(map, 2.5f, 2.5f) == doctest::Approx(terrainSpeedFactor(Terrain::Soil)));

    // A swimmer fits in deep water; a walker does not.
    CHECK(swimmerFitsAt(map, 15.0f, 15.0f, kPawnRadius));
    CHECK_FALSE(pawnFitsAt(map, 15.0f, 15.0f, kPawnRadius));
    CellPos shore{0, 0, 0};
    CHECK(nearestShore(map, 11.0f, 15.0f, 0, 24, shore));
    CHECK(shore.x == 9);
    CHECK(map.walkable(shore));
}

TEST_CASE("Water: a generated map has one water table, banks above it and rivers that flow") {
    Map map(MapSettings{512, 512, 42, false});
    const float table = map.waterTable();
    CHECK(table == doctest::Approx(13.2f));
    CHECK(map.hasFlow());
    int wet = 0;
    int dryBelow = 0;
    int flowing = 0;
    float fastest = 0.0f;
    for (int y = 0; y < map.height(); ++y) {
        for (int x = 0; x < map.width(); ++x) {
            const CellPos c{x, y, 0};
            const Terrain t = map.terrain(c);
            float vx = 0.0f;
            float vy = 0.0f;
            map.flowAt(c, vx, vy);
            const float speed = std::sqrt(vx * vx + vy * vy);
            if (t == Terrain::Water || t == Terrain::ShallowWater) {
                ++wet;
                CHECK(map.waterSurface(c) == doctest::Approx(table));
                CHECK(map.groundHeight(c) < table);
                if (speed > 0.0f) {
                    ++flowing;
                }
                fastest = std::max(fastest, speed);
            } else {
                CHECK(speed == 0.0f);  // only water flows
                if (t != Terrain::Marsh && map.groundHeight(c) < table - 0.001f) {
                    ++dryBelow;  // a dry pit under the water's level would show a wall of water beside it
                }
            }
        }
    }
    CHECK(wet > 1800);
    CHECK(dryBelow == 0);
    CHECK(flowing > 500);
    CHECK(fastest <= 1.01f);
    CHECK(fastest > 0.9f);
}

TEST_CASE("Water: the current carries a wader downstream faster than upstream") {
    const auto walkSteps = [](bool downstream) {
        Sim sim(SimSettings{3, 40, 0, 0, true}, testDefs());
        Map& map = sim.editMap();
        map.setWaterTable(1.2f);  // chest-deep for a man: the current carries half
        for (int y = 4; y <= 6; ++y) {
            for (int x = 0; x < map.width(); ++x) {
                map.setTerrain(CellPos{x, y, 0}, Terrain::ShallowWater);
                map.setFlow(CellPos{x, y, 0}, 0.8f, 0.0f);
            }
        }
        const int startX = downstream ? 5 : 30;
        const int goalX = downstream ? 30 : 5;
        const Entity pawn = sim.things().spawn("Human", CellPos{startX, 5, 0});
        auto& reg = sim.things().registry();
        const Position& pos = reg.get<Position>(pawn);
        reg.get<Mover>(pawn).walkTo(pos.x, pos.y, std::vector<CellPos>{CellPos{goalX, 5, 0}});
        int steps = 0;
        while (!reg.get<Mover>(pawn).arrived() && steps < 2000) {
            sim.step();
            ++steps;
        }
        return steps;
    };
    const int down = walkSteps(true);
    const int up = walkSteps(false);
    CHECK(down < 2000);
    CHECK(up < 2000);
    CHECK(down * 3 < up * 2);
}

TEST_CASE("Water: the player's colonist swims into deep water, can't fight there, and swims out when let go") {
    Sim sim(SimSettings{3, 32, 0, 0, true}, testDefs());
    waterTestPond(sim.editMap());
    const Entity pawn = sim.things().spawn("Human", CellPos{8, 15, 0});
    sim.things().registry().emplace<Colonist>(pawn);
    sim.jobs().addWorker(pawn);
    sim.submit(ControlPawnCommand{pawn});
    sim.step();
    auto& reg = sim.things().registry();
    // Walk east into the pond, 0.1 m a step.
    for (int i = 0; i < 40; ++i) {
        const Position& pos = reg.get<Position>(pawn);
        sim.submit(DirectMoveCommand{pawn, pos.x + 0.1f, pos.y});
        sim.step();
    }
    const Position& in = reg.get<Position>(pawn);
    CHECK(in.x > 12.0f);
    CHECK(sim.pawnSwimming(pawn));
    Snapshot snap;
    sim.writeSnapshot(snap);
    const ThingView* view = snap.find(pawn);
    CHECK(view != nullptr);
    if (view == nullptr) {
        return;
    }
    CHECK(view->swimming);
    CHECK(view->waterDepth == doctest::Approx(Map::kHandDeepM));

    const uint64_t roll = sim.submit(DodgeRollCommand{pawn, 1.0f, 0.0f});
    const uint64_t swing = sim.submit(MeleeCommand{pawn, kNoEntity, MeleeMode::Block});
    sim.step();
    CHECK(waterTestFailed(sim, roll));
    CHECK(waterTestFailed(sim, swing));

    // Let go: it swims to the nearest shore by itself.
    sim.submit(ControlPawnCommand{kNoEntity});
    for (int i = 0; i < 300 && !sim.map().walkable(cellOf(reg.get<Position>(pawn))); ++i) {
        sim.step();
    }
    CHECK(sim.map().walkable(cellOf(reg.get<Position>(pawn))));
    CHECK_FALSE(sim.pawnSwimming(pawn));
}
