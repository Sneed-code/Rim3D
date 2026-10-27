#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Health.h"
#include "ColonySim/Narrator.h"
#include "ColonySim/Raids.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

#define EVENT_REQUIRE(...)     \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

// Second bug hunt: every incident the narrator knows is fired into a running colony, played on for a few hours,
// saved and loaded in the middle, and checked for sane pawns. Not a check of each event's own rules (their own
// test files do that), only that none of them breaks the game or another system.

namespace {

std::shared_ptr<const DefDatabase> evScenarioDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

std::unique_ptr<Sim> evScenarioSim(uint64_t seed) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = 96;
    settings.flatMap = true;
    settings.scenario = "Crashlanded";
    settings.wildAnimals = 12;
    return std::make_unique<Sim>(settings, evScenarioDefs());
}

// Steps `seconds`, collecting the failure messages of commands. Returns false if a colonist or animal went wrong.
bool evScenarioSane(Sim& sim, std::string& why) {
    entt::registry& reg = sim.things().registry();
    for (const auto [e, pos] : reg.view<const Position>().each()) {
        if (!std::isfinite(pos.x) || !std::isfinite(pos.y)) {
            why = "a pawn or thing has a position that is not a number";
            return false;
        }
        if (pos.x < -1.0f || pos.y < -1.0f || pos.x > 97.0f || pos.y > 97.0f) {
            // Raiders and visitors come from the edge, so a little outside is fine; far outside is not.
            if (pos.x < -40.0f || pos.y < -40.0f || pos.x > 140.0f || pos.y > 140.0f) {
                why = "something is far outside the map";
                return false;
            }
        }
    }
    return true;
}

}  // namespace

TEST_CASE("Event scenarios: every incident fires, plays on for hours, and saves and loads to the same game") {
    const auto defs = evScenarioDefs();
    const std::vector<DefId> incidents = defs->idsOfKind("Incidents");
    EVENT_REQUIRE(incidents.size() > 20);
    std::vector<std::string> unwired;
    for (const DefId id : incidents) {
        const std::string name = defs->get(id).defName();
        CAPTURE(name);
        auto sim = evScenarioSim(11);
        for (int i = 0; i < 600; ++i) {
            sim->step();
        }
        sim->submit(FireIncidentCommand{name, 0.0f});
        bool failed = false;
        std::string failure;
        for (int i = 0; i < 3600 && !failed; ++i) {
            sim->step();
            for (const Event& e : sim->events()) {
                if (e.type == EventType::CommandFailed) {
                    failed = true;
                    failure = e.message;
                }
            }
        }
        if (failed) {
            unwired.push_back(name + ": " + failure);
            continue;
        }
        std::string why;
        CHECK_MESSAGE(evScenarioSane(*sim, why), name << ": " << why);
        // Saved and loaded in the middle, both go on the same.
        const std::vector<uint8_t> bytes = saveSim(*sim);
        SaveLoadResult loaded = loadSim(bytes, defs);
        EVENT_REQUIRE(loaded.sim != nullptr);
        CHECK_MESSAGE(loaded.sim->stateHash() == sim->stateHash(), name << ": the loaded game differs at once");
        for (int i = 0; i < 3600; ++i) {
            sim->step();
            loaded.sim->step();
        }
        CHECK_MESSAGE(loaded.sim->stateHash() == sim->stateHash(), name << ": the loaded game drifts from the played one");
        CHECK_MESSAGE(evScenarioSane(*sim, why), name << ": " << why);
    }
    for (const std::string& u : unwired) {
        MESSAGE("could not fire: " << u);
    }
}

namespace {

void evScenarioRun(Sim& sim, int64_t seconds) {
    for (int64_t i = 0; i < seconds; ++i) {
        sim.step();
    }
}

// Runs `sim` and a copy of it made by a save and a load `seconds` further, and says whether they stayed the same game.
bool evScenarioSurvivesSave(Sim& sim, int64_t seconds, std::string& why) {
    SaveLoadResult loaded = loadSim(saveSim(sim), evScenarioDefs());
    if (loaded.sim == nullptr) {
        why = "the save did not load: " + loaded.error;
        return false;
    }
    if (loaded.sim->stateHash() != sim.stateHash()) {
        why = "the loaded game differs at once";
        return false;
    }
    for (int64_t i = 0; i < seconds; ++i) {
        sim.step();
        loaded.sim->step();
        if (loaded.sim->stateHash() != sim.stateHash()) {
            why = "the loaded game drifts from the played one after " + std::to_string(i + 1) + " steps";
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("Event scenarios: every raid strategy starts, ends, and saves and loads to the same game") {
    for (const char* strategy : {"Assault", "Sappers", "DropPods", "Thieves", "Kidnappers", "Auto"}) {
        CAPTURE(strategy);
        auto sim = evScenarioSim(3);
        sim->things().spawn("Steel", CellPos{40, 40, 0}, 100);
        evScenarioRun(*sim, 300);
        sim->submit(StartStrategyRaidCommand{"Pirate", 300.0f, -1, strategy, -1.0f});
        evScenarioRun(*sim, 1);
        size_t failures = 0;
        for (const Event& e : sim->events()) {
            if (e.type == EventType::CommandFailed) {
                MESSAGE(strategy << " could not start: " << e.message);
                ++failures;
            }
        }
        if (failures > 0) {
            continue;
        }
        evScenarioRun(*sim, 600);
        std::string why;
        CHECK_MESSAGE(evScenarioSurvivesSave(*sim, 900, why), strategy << ": " << why);
        bool over = false;
        for (int i = 0; i < 2 * 86400 && !over; ++i) {
            sim->step();
            over = sim->raids().state().squads.empty() && sim->raids().state().pending.empty();
        }
        CHECK_MESSAGE(over, strategy << ": the raid is still going after two game days");
    }
}

TEST_CASE("Event scenarios: a night raid waiting for its hour is kept by a save and still comes") {
    auto sim = evScenarioSim(9);
    evScenarioRun(*sim, 600);  // about ten past midnight on day 0
    sim->submit(StartStrategyRaidCommand{"Pirate", 200.0f, -1, "Assault", 3.0f});
    evScenarioRun(*sim, 10);
    EVENT_REQUIRE(sim->raids().state().pending.size() == 1);
    SaveLoadResult loaded = loadSim(saveSim(*sim), evScenarioDefs());
    EVENT_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->raids().state().pending.size() == 1);
    bool arrived = false;
    int64_t arrivedAt = 0;
    for (int64_t i = 0; i < 4 * 3600 && !arrived; ++i) {
        loaded.sim->step();
        for (const Event& e : loaded.sim->events()) {
            if (e.type == EventType::RaidArrived) {
                arrived = true;
                arrivedAt = loaded.sim->time().seconds;
            }
        }
    }
    CHECK(arrived);
    CHECK(arrivedAt >= 3 * 3600);
    CHECK(arrivedAt <= 3 * 3600 + 60);
    CHECK(loaded.sim->raids().state().pending.empty());
}


TEST_CASE("Event scenarios: the narrator's raids come in more ways than a plain assault") {
    std::map<std::string, int> seen;
    for (uint64_t seed = 1; seed <= 40; ++seed) {
        auto sim = evScenarioSim(seed);
        sim->things().spawn("Steel", CellPos{40, 40, 0}, 100);
        sim->submit(FireIncidentCommand{"RaidEnemy", 400.0f});
        sim->step();
        if (!sim->raids().state().squads.empty()) {
            ++seen[raidStrategyName(sim->raids().state().squads.back().strategy)];
        }
    }
    CHECK(seen["Assault"] > 0);
    int other = 0;
    for (const auto& [name, count] : seen) {
        other += name == "Assault" ? 0 : count;
    }
    CHECK_MESSAGE(other > 0, "forty narrator raids and every one was a plain assault");
}
