// Quests (task Q1, docs/design/quests.md).
#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Narrator.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Quests.h"
#include "ColonySim/Raids.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"
#include "ColonySim/Weather.h"

#include <doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

#define QUEST_REQUIRE(...)     \
    do {                       \
        CHECK((__VA_ARGS__));  \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> questDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

std::unique_ptr<Sim> questSim(uint64_t seed = 11) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = 128;
    settings.flatMap = true;
    settings.scenario = "Crashlanded";
    auto sim = std::make_unique<Sim>(settings, questDefs());
    sim->setOutdoorOverride(20.0f);
    sim->weather().force("Clear", 4000.0);
    return sim;
}

int questColonists(Sim& sim) {
    int n = 0;
    for (const auto [pawn, colonist] : sim.things().registry().view<const Colonist>().each()) {
        n += sim.fighters().factionOf(pawn) == kPlayerFaction ? 1 : 0;
    }
    return n;
}

int questItems(Sim& sim, const char* def) {
    const DefId id = sim.defs().idOf("Things", def);
    int total = 0;
    for (const auto [item, thing, stack] : sim.things().registry().view<const Thing, const StackCount>().each()) {
        if (thing.def == id && sim.things().spawned(item)) {
            total += stack.count;
        }
    }
    return total;
}

// Keeps everyone fed and rested (the flat map has no food), so the colony lives for days.
void questFeed(Sim& sim) {
    for (auto [pawn, needs] : sim.things().registry().view<Needs>().each()) {
        needs.food = 1.0f;
        needs.rest = 1.0f;
        needs.malnutrition = 0.0f;
    }
}

void questStepTo(Sim& sim, int64_t seconds) {
    while (sim.time().seconds < seconds) {
        if (sim.time().seconds % 600 == 0) {
            questFeed(sim);
        }
        sim.step();
    }
}

bool questSaw(const std::vector<Event>& seen, EventType type) {
    return std::any_of(seen.begin(), seen.end(), [&](const Event& e) { return e.type == type; });
}

void questRun(Sim& sim, int64_t seconds, std::vector<Event>& seen) {
    for (int64_t i = 0; i < seconds; ++i) {
        if (sim.time().seconds % 600 == 0) {
            questFeed(sim);
        }
        sim.step();
        for (const Event& e : sim.events()) {
            if (e.type >= EventType::QuestOffered && e.type <= EventType::QuestFailed) {
                seen.push_back(e);
            }
        }
    }
}

}  // namespace

TEST_CASE("quests: the defs name real things and the rules are read") {
    auto sim = questSim();
    QUEST_REQUIRE(sim->setupErrors().empty());
    CHECK(sim->quests().rules().present);
    CHECK(sim->defs().idsOfKind("Quests").size() >= 4);
    // On day 0 only the quests with no earliest day are available.
    const std::vector<std::string> now = sim->quests().available();
    CHECK(std::find(now.begin(), now.end(), "HoldTheLine") == now.end());
    questStepTo(*sim, 10 * kSecondsPerDay);
    const std::vector<std::string> later = sim->quests().available();
    CHECK(std::find(later.begin(), later.end(), "HoldTheLine") != later.end());
}

TEST_CASE("quests: offers can be declined, and lapse when nobody answers") {
    auto sim = questSim();
    uint32_t id = 0;
    std::string error;
    QUEST_REQUIRE(sim->quests().offer("WandererAsks", id, error));
    CHECK(sim->quests().find(id)->state == QuestState::Offered);
    CHECK_FALSE(sim->quests().offer("WandererAsks", id, error));  // one open offer of a kind at a time
    CHECK_FALSE(error.empty());
    CHECK_FALSE(sim->quests().offer("NoSuchQuest", id, error));
    QUEST_REQUIRE(sim->quests().offer("SupplyCache", id, error));
    const uint32_t cache = id;
    QUEST_REQUIRE(sim->quests().decline(cache, error));
    CHECK(sim->quests().find(cache)->state == QuestState::Declined);
    CHECK_FALSE(sim->quests().decline(cache, error));
    CHECK_FALSE(sim->quests().accept(cache, error));

    std::vector<Event> seen;
    questRun(*sim, 4 * kSecondsPerDay, seen);
    CHECK(sim->quests().count(QuestState::Expired) == 1);
    CHECK(questSaw(seen, EventType::QuestExpired));
}

TEST_CASE("quests: a wanderer who is accepted joins the colony") {
    auto sim = questSim();
    const int before = questColonists(*sim);
    uint32_t id = 0;
    std::string error;
    QUEST_REQUIRE(sim->quests().offer("WandererAsks", id, error));
    QUEST_REQUIRE(sim->quests().accept(id, error));
    CHECK(sim->quests().find(id)->state == QuestState::Completed);
    CHECK(questColonists(*sim) == before + 1);
    CHECK(sim->quests().find(id)->people.size() == 1);
}

TEST_CASE("quests: a supply cache is delivered at once") {
    auto sim = questSim();
    const int steel = questItems(*sim, "Steel");
    uint32_t id = 0;
    std::string error;
    QUEST_REQUIRE(sim->quests().offer("SupplyCache", id, error));
    const Quest* quest = sim->quests().find(id);
    int promised = 0;
    for (const Quest::Reward& r : quest->rewards) {
        promised += r.thing == "Steel" ? r.count : 0;
    }
    CHECK(promised >= 80);
    QUEST_REQUIRE(sim->quests().accept(id, error));
    CHECK(sim->quests().find(id)->state == QuestState::Completed);
    CHECK(questItems(*sim, "Steel") == steel + promised);
}

TEST_CASE("quests: refugees arrive at once, the raid follows, and the quest ends when it does") {
    auto sim = questSim();
    questStepTo(*sim, 6 * kSecondsPerDay);
    const int before = questColonists(*sim);
    uint32_t id = 0;
    std::string error;
    QUEST_REQUIRE(sim->quests().offer("RefugeesChased", id, error));
    const int people = sim->quests().find(id)->count;
    CHECK(people >= 1);
    CHECK(people <= 3);
    QUEST_REQUIRE(sim->quests().accept(id, error));
    CHECK(sim->quests().find(id)->state == QuestState::Active);
    CHECK(questColonists(*sim) == before + people);
    const int64_t silver = questItems(*sim, "Silver");

    // The raid is sent after the delay.
    std::vector<Event> seen;
    questRun(*sim, 4 * kSecondsPerHour + 60, seen);
    QUEST_REQUIRE(sim->quests().find(id)->launched);
    QUEST_REQUIRE(sim->quests().find(id)->squad != 0);
    CHECK(sim->raids().state().squads.size() == 1);
    CHECK(sim->quests().find(id)->state == QuestState::Active);

    // Remove the raiders: the raid is over and the reward is paid.
    for (const Entity raider : std::vector<Entity>(sim->raids().state().squads[0].members)) {
        sim->submit(DestroyThingCommand{raider});
    }
    questRun(*sim, 60, seen);
    CHECK(sim->quests().find(id)->state == QuestState::Completed);
    CHECK(questItems(*sim, "Silver") > silver);
    CHECK(questSaw(seen, EventType::QuestCompleted));
}

TEST_CASE("quests: hold the line is paid when the raid is over, and fails if it never ends") {
    auto sim = questSim();
    questStepTo(*sim, 9 * kSecondsPerDay);
    uint32_t id = 0;
    std::string error;
        QUEST_REQUIRE(sim->quests().offer("HoldTheLine", id, error));
    QUEST_REQUIRE(sim->quests().accept(id, error));
    CHECK(sim->quests().find(id)->launchAt > sim->time().seconds);
    CHECK(sim->raids().state().squads.empty());
    std::vector<Event> seen;
    // Wait for the raid to come, then for the quest to settle one way or the other within the deadline.
    questRun(*sim, 13 * kSecondsPerHour, seen);
    QUEST_REQUIRE(sim->quests().find(id)->launched);
    const Quest& quest = *sim->quests().find(id);
    questRun(*sim, std::max<int64_t>(0, quest.deadline - sim->time().seconds) + 60, seen);
    const QuestState end = sim->quests().find(id)->state;
    CHECK((end == QuestState::Completed || end == QuestState::Failed));
    CHECK(sim->raids().state().squads.empty() == (end == QuestState::Completed));
}

TEST_CASE("quests: commands offer, accept and decline, and bad ones fail") {
    auto sim = questSim();
    std::vector<Event> failed;
    const auto run = [&](Command command) {
        sim->submit(std::move(command));
        sim->step();
        for (const Event& e : sim->events()) {
            if (e.type == EventType::CommandFailed) {
                failed.push_back(e);
            }
        }
    };
    run(QuestCommand{"offer", 0, "WandererAsks"});
    CHECK(failed.empty());
    QUEST_REQUIRE(sim->quests().state().quests.size() == 1);
    const uint32_t id = sim->quests().state().quests[0].id;
    Snapshot snapshot;
    sim->writeSnapshot(snapshot);
    CHECK(snapshot.quests.size() == 1);
    run(QuestCommand{"accept", id, ""});
    CHECK(failed.empty());
    CHECK(sim->quests().find(id)->state == QuestState::Completed);
    run(QuestCommand{"accept", id, ""});
    run(QuestCommand{"decline", 999, ""});
    run(QuestCommand{"dance", 0, ""});
    run(QuestCommand{"offer", 0, "NoSuchQuest"});
    CHECK(failed.size() == 4);
    run(QuestCommand{"offer", 0, ""});  // one chosen by weight
    CHECK(failed.size() == 4);
    CHECK(sim->quests().state().quests.size() == 2);
}

TEST_CASE("quests: saving and loading keeps open and finished quests, and the game goes on the same") {
    auto sim = questSim();
    questStepTo(*sim, 6 * kSecondsPerDay);
    uint32_t id = 0;
    std::string error;
    QUEST_REQUIRE(sim->quests().offer("WandererAsks", id, error));
    QUEST_REQUIRE(sim->quests().offer("SupplyCache", id, error));
    QUEST_REQUIRE(sim->quests().decline(id, error));
    QUEST_REQUIRE(sim->quests().offer("RefugeesChased", id, error));
    QUEST_REQUIRE(sim->quests().accept(id, error));
    sim->stepMany(120);
    const std::vector<uint8_t> bytes = saveSim(*sim);
    SaveLoadResult loaded = loadSim(bytes, questDefs());
    QUEST_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->stateHash() == sim->stateHash());
    CHECK(loaded.sim->quests().state().quests.size() == 3);
    CHECK(loaded.sim->quests().find(id)->people.size() == sim->quests().find(id)->people.size());
    for (int i = 0; i < 4 * 3600; i += 60) {
        sim->stepMany(60);
        loaded.sim->stepMany(60);
        if (sim->stateHash() != loaded.sim->stateHash()) {
            MESSAGE("hashes differ " << i << " steps after loading");
            CHECK(false);
            break;
        }
    }
    CHECK(loaded.sim->quests().find(id)->launched == sim->quests().find(id)->launched);
}

TEST_CASE("quests: old finished quests are dropped beyond the history limit") {
    auto sim = questSim();
    const int keep = sim->quests().rules().historyKept;
    for (int i = 0; i < keep + 5; ++i) {
        uint32_t id = 0;
        std::string error;
        QUEST_REQUIRE(sim->quests().offer("WandererAsks", id, error));
        QUEST_REQUIRE(sim->quests().decline(id, error));
    }
    CHECK(static_cast<int>(sim->quests().state().quests.size()) == keep);
    CHECK(sim->quests().state().quests.front().id == 6);
}

TEST_CASE("quests: the narrator's QuestOffer incident offers a quest") {
    auto sim = questSim();
    questStepTo(*sim, 3 * kSecondsPerDay);
    CHECK(sim->narrator().hasWorker("QuestOffer"));
    sim->submit(FireIncidentCommand{"QuestOffer", 0.0f});
    sim->step();
    CHECK(sim->quests().count(QuestState::Offered) == 1);
}
