#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <memory>
#include <string>
#include <vector>

using namespace colony;

namespace {

// Fixture defs: a pawn that carries 10, a light item (0.5 each, stacks of 25) and a heavy one (4 each).
std::shared_ptr<const DefDatabase> carryDefs() {
    const std::vector<DefSource> sources = {{"Things", "carry.json", R"([
        {"defName": "Human", "category": "Pawn", "stats": {"moveSpeed": 0.15333, "carryingCapacity": 10}},
        {"defName": "Light", "category": "Item", "stackLimit": 25, "stats": {"mass": 0.5}},
        {"defName": "Heavy", "category": "Item", "stackLimit": 10, "stats": {"mass": 4}},
        {"defName": "Plain", "category": "Item", "stackLimit": 75}
    ])"},
                                            {"Terrain", "carry.json", R"([{"defName": "Soil"}])"},
                                            {"WorkTypes", "carry.json", R"([
        {"defName": "Hauling", "naturalPriority": 300, "tasks": ["HaulToStockpile"]}
    ])"}};
    auto db = std::make_shared<DefDatabase>();
    std::vector<std::string> errors;
    DefDatabase::build(sources, "", *db, errors);
    return db;
}

Entity carrySpawn(Sim& sim, const std::string& def, CellPos cell, int count) {
    sim.submit(SpawnThingCommand{def, cell, count});
    sim.step();
    for (const Event& e : sim.events()) {
        if (e.type == EventType::ThingSpawned && e.cell == cell) {
            return e.thing;
        }
    }
    return kNoEntity;
}

int carryTotal(const Sim& sim, const std::string& def) {
    int total = 0;
    const entt::registry& reg = sim.things().registry();
    reg.view<const Thing, const StackCount>().each([&](const Thing& thing, const StackCount& stack) {
        total += sim.defs().get(thing.def).defName() == def ? stack.count : 0;
    });
    return total;
}

}  // namespace

TEST_CASE("Carry: capacity over item mass decides how many fit") {
    Sim sim(SimSettings{5, 32, 0, 0, true}, carryDefs());
    const Entity pawn = carrySpawn(sim, "Human", CellPos{10, 10, 0}, 1);
    const JobSystem& jobs = sim.jobs();
    CHECK(jobs.carryLimit(pawn, sim.defs().idOf("Things", "Light")) == 20);
    CHECK(jobs.carryLimit(pawn, sim.defs().idOf("Things", "Heavy")) == 2);
    CHECK(jobs.carryLimit(pawn, sim.defs().idOf("Things", "Plain")) == 10);  // no mass: 1 each
    CHECK(jobs.loadSpeedFactor(pawn) == doctest::Approx(1.0f));
}

TEST_CASE("Carry: a pawn takes beside stacks along in one trip and walks slower loaded") {
    Sim sim(SimSettings{5, 32, 0, 0, true}, carryDefs());
    sim.submit(CreateStockpileCommand{CellRect{2, 2, 3, 3, 0}});
    const Entity a = carrySpawn(sim, "Light", CellPos{20, 10, 0}, 12);
    carrySpawn(sim, "Light", CellPos{21, 10, 0}, 12);
    const Entity pawn = carrySpawn(sim, "Human", CellPos{20, 10, 0}, 1);
    (void)a;
    int mostCarried = 0;
    float slowest = 1.0f;
    for (int i = 0; i < 3600 && sim.itemsLeft() > 0; ++i) {
        sim.step();
        const Carrying* carrying = sim.things().registry().try_get<Carrying>(pawn);
        if (carrying != nullptr && sim.things().registry().valid(carrying->thing)) {
            mostCarried = std::max(mostCarried, sim.things().registry().get<StackCount>(carrying->thing).count);
            slowest = std::min(slowest, sim.jobs().loadSpeedFactor(pawn));
        }
    }
    CHECK(mostCarried == 20);  // 12 + 8 of the next stack: its hands are full
    CHECK(slowest == doctest::Approx(0.75f));
    CHECK(sim.itemsLeft() == 0);
    CHECK(carryTotal(sim, "Light") == 24);  // nothing lost or made
    // Every item ended on the stockpile, none above a stack.
    sim.things().registry().view<const Thing, const StackCount>().each([&](const Thing& thing, const StackCount& stack) {
        if (sim.defs().get(thing.def).defName() == "Light") {
            CHECK(stack.count <= 25);
        }
    });
}

TEST_CASE("Carry: a load bigger than a stock cell spills into the next and keeps every item") {
    Sim sim(SimSettings{5, 32, 0, 0, true}, carryDefs());
    sim.submit(CreateStockpileCommand{CellRect{2, 2, 3, 3, 0}});
    carrySpawn(sim, "Heavy", CellPos{20, 10, 0}, 2);
    carrySpawn(sim, "Heavy", CellPos{21, 10, 0}, 1);
    carrySpawn(sim, "Human", CellPos{20, 10, 0}, 1);
    for (int i = 0; i < 5000 && sim.itemsLeft() > 0; ++i) {
        sim.step();
    }
    CHECK(sim.itemsLeft() == 0);
    CHECK(carryTotal(sim, "Heavy") == 3);
}
