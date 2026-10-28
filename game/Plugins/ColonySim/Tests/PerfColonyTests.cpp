#include "ColonySim/Animals.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/PerfColony.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <memory>
#include <string>
#include <vector>

using namespace colony;

namespace {

// Tests for the performance colony (docs/design/sim-cost.md), on the game's real data.
std::shared_ptr<const DefDatabase> perfColonyTestDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        for (const std::string& e : errors) {
            MESSAGE(e);
        }
        return db;
    }();
    return defs;
}

std::unique_ptr<Sim> perfColonyTestSim() {
    SimSettings settings{42, 256, 0, 0};
    settings.scenario = "PerfColony";
    return std::make_unique<Sim>(settings, perfColonyTestDefs());
}

}  // namespace

TEST_CASE("PerfColony: 30 colonists, 300 tame animals, plans that the sim takes") {
    std::unique_ptr<Sim> sim = perfColonyTestSim();
    PerfColonyCounts counts;
    CHECK(setUpPerfColony(*sim, counts).empty());
    CHECK(counts.colonists == 30);
    CHECK(counts.tameAnimals == 300);
    CHECK(counts.wildAnimals == 0);

    int tame = 0;
    for (const auto [e, animal] : sim->things().registry().view<const Animal>().each()) {
        (void)e;
        tame += animal.wild ? 0 : 1;
    }
    CHECK(tame == 300);

    // Every planned room, floor, zone and mark is a command the first step applies without a refusal.
    sim->step();
    int failed = 0;
    for (const Event& e : sim->events()) {
        failed += e.type == EventType::CommandFailed ? 1 : 0;
    }
    CHECK(failed == 0);
    feedPerfColonyAnimals(*sim);  // nothing to check but that it runs on a live colony
}

TEST_CASE("PerfColony: a sim without the scenario says so") {
    SimSettings settings{42, 64, 0, 0};
    Sim sim(settings, perfColonyTestDefs());
    PerfColonyCounts counts;
    CHECK_FALSE(setUpPerfColony(sim, counts).empty());
    CHECK(counts.colonists == 0);
}

TEST_CASE("PerfColony: every colonist can be armed for the raid") {
    std::unique_ptr<Sim> sim = perfColonyTestSim();
    PerfColonyCounts counts;
    CHECK(setUpPerfColony(*sim, counts).empty());
    CHECK(armPerfColonists(*sim) == counts.colonists);
}
