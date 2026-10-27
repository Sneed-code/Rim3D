#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Fire.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Narrator.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"
#include "ColonySim/Travel.h"

#include "TestDefs.h"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define TRAVEL_REQUIRE(...)    \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// The game's real data: the rules the skip reads, the needs, the narrators and the beds are all part of what is tested.
std::shared_ptr<const DefDatabase> travelDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

Sim travelSim() { return Sim(SimSettings{5, 128, 0, 0, true}, travelDefs()); }

Entity travelPawn(Sim& sim, CellPos cell, float rest = 0.3f) {
    const Entity pawn = sim.things().spawn("Human", cell);
    sim.jobs().addWorker(pawn);
    sim.things().registry().get<Needs>(pawn).rest = rest;
    return pawn;
}

struct TravelLog {
    std::vector<Event> events;
    const Event* last(EventType type) const {
        for (auto it = events.rbegin(); it != events.rend(); ++it) {
            if (it->type == type) {
                return &*it;
            }
        }
        return nullptr;
    }
    int count(EventType type) const {
        return static_cast<int>(std::count_if(events.begin(), events.end(), [&](const Event& e) { return e.type == type; }));
    }
};

// One Sim::step, keeping its events.
void travelStep(Sim& sim, TravelLog& log) {
    sim.step();
    log.events.insert(log.events.end(), sim.events().begin(), sim.events().end());
}

template <typename F>
bool travelRunUntil(Sim& sim, TravelLog& log, int limit, F done) {
    for (int i = 0; i < limit; ++i) {
        if (done()) {
            return true;
        }
        travelStep(sim, log);
    }
    return done();
}

bool travelLying(Sim& sim, Entity pawn) { return jobIsLyingDown(sim.things().registry().get<Worker>(pawn).job); }

// Two tired colonists in their beds at midnight (the sim starts there, in a Sleep hour), the sim step by step
// until both lie down.
struct TravelNight {
    Sim sim = travelSim();
    TravelLog log;
    Entity a = kNoEntity;
    Entity b = kNoEntity;
    bool ready = false;
    TravelNight() {
        a = travelPawn(sim, CellPos{20, 20, 0});
        b = travelPawn(sim, CellPos{20, 24, 0});
        sim.things().spawn("Bed", CellPos{22, 20, 0});
        sim.things().spawn("Bed", CellPos{22, 24, 0});
        ready = travelRunUntil(sim, log, 3000, [&] { return travelLying(sim, a) && travelLying(sim, b); });
    }
};

}  // namespace

TEST_CASE("Travel: the rules are in the game's data") {
    const TravelRules rules = TravelRules::read(*travelDefs());
    CHECK(rules.visitedBlock == 8);
    CHECK(rules.minDistance == doctest::Approx(20.0f));
    CHECK(rules.nightBatch == 1200);
    CHECK(rules.freezeAnimals);
}

TEST_CASE("Travel: a night can't be skipped by day, with nobody asleep, or with a tired colonist still up") {
    TravelNight n;
    TRAVEL_REQUIRE(n.ready);
    std::string why;
    CHECK(n.sim.travel().canSkipNight(why));

    // A third colonist is tired and has not gone to bed.
    const Entity c = travelPawn(n.sim, CellPos{40, 40, 0});
    CHECK_FALSE(n.sim.travel().canSkipNight(why));
    CHECK(why.find("in bed yet") != std::string::npos);
    CHECK(n.sim.things().registry().get<Worker>(c).job.type != JobType::LayDown);
    n.sim.submit(SkipNightCommand{});
    travelStep(n.sim, n.log);
    CHECK(n.log.count(EventType::CommandFailed) == 1);
    CHECK_FALSE(n.sim.travel().skipping());

    // By day: past 6:00 (the colonists wake rested by then).
    Sim day = travelSim();
    const Entity lone = travelPawn(day, CellPos{20, 20, 0});
    day.stepMany(8 * 3600);
    CHECK(day.time().hourOfDay() == 8);
    CHECK_FALSE(day.travel().canSkipNight(why));
    CHECK(why.find("not night") != std::string::npos);
    (void)lone;
}

TEST_CASE("Travel: a night skip runs to morning, rests the sleepers, and holds the narrator") {
    TravelNight n;
    TRAVEL_REQUIRE(n.ready);
    Sim& sim = n.sim;
    std::string error;
    CHECK(sim.narrator().setNarrator("Orrin", "", error));
    // One step plans the narrator's cycle (the first check after the setting).
    while (sim.time().seconds % 1440 != 0) {
        travelStep(sim, n.log);
    }
    travelStep(sim, n.log);
    const int64_t nextThreat = sim.narrator().nextThreatAt();
    CHECK(sim.narrator().state().cycleStart >= 0);
    const float restBefore = sim.things().registry().get<Needs>(n.a).rest;

    const int64_t from = sim.time().seconds;
    sim.submit(SkipNightCommand{});
    travelStep(sim, n.log);
    CHECK(sim.travel().skipping());
    CHECK(sim.travel().kind() == SkipKind::Night);
    CHECK(n.log.count(EventType::SkipStarted) == 1);
    Snapshot snapshot;
    sim.writeSnapshot(snapshot);
    CHECK(snapshot.travel.skipKind == 1);
    CHECK(snapshot.travel.skipEndsAt == 6 * kSecondsPerHour);

    TRAVEL_REQUIRE(travelRunUntil(sim, n.log, 100, [&] { return !sim.travel().skipping(); }));
    CHECK(sim.time().seconds == 6 * kSecondsPerHour);
    CHECK(sim.time().hourOfDay() == 6);
    const Event* ended = n.log.last(EventType::SkipEnded);
    TRAVEL_REQUIRE(ended != nullptr);
    CHECK(ended->message == "night: morning");
    CHECK(ended->amount == doctest::Approx(static_cast<float>(sim.time().seconds - from)).epsilon(0.001));
    CHECK(sim.travel().progress().nights == 1);
    // They slept: rest rose.
    CHECK(sim.things().registry().get<Needs>(n.a).rest > restBefore + 0.2f);
    // The narrator was held: its next threat moved by the time that passed (it was not left behind and fired).
    CHECK(sim.narrator().nextThreatAt() - nextThreat == sim.time().seconds - from);
    CHECK(sim.stats().failedJobs == 0);
    sim.writeSnapshot(snapshot);
    CHECK(snapshot.travel.skipKind == 0);
    CHECK(snapshot.travel.nights == 1);
}

TEST_CASE("Travel: a threat or a cancel ends a skip at once") {
    {
        TravelNight n;
        TRAVEL_REQUIRE(n.ready);
        n.sim.submit(SkipNightCommand{});
        travelStep(n.sim, n.log);
        CHECK(n.sim.travel().skipping());
        // A fire starts while they sleep.
        CHECK(n.sim.fire().ignite(CellPos{60, 60, 0}, 1.0f, true));
        TRAVEL_REQUIRE(travelRunUntil(n.sim, n.log, 10, [&] { return !n.sim.travel().skipping(); }));
        const Event* ended = n.log.last(EventType::SkipEnded);
        TRAVEL_REQUIRE(ended != nullptr);
        CHECK(ended->message == "night: something is on fire");
        CHECK(n.sim.time().hourOfDay() < 6);
        CHECK(n.sim.travel().progress().nights == 0);
        // And it can't be started again while the fire burns.
        n.sim.submit(SkipNightCommand{});
        travelStep(n.sim, n.log);
        CHECK(n.log.count(EventType::CommandFailed) == 1);
    }
    {
        TravelNight n;
        TRAVEL_REQUIRE(n.ready);
        n.sim.submit(SkipNightCommand{});
        travelStep(n.sim, n.log);
        n.sim.submit(CancelSkipCommand{});
        travelStep(n.sim, n.log);
        CHECK_FALSE(n.sim.travel().skipping());
        CHECK(n.log.last(EventType::SkipEnded)->message == "night: cancelled");
        n.sim.submit(CancelSkipCommand{});
        travelStep(n.sim, n.log);
        CHECK(n.log.count(EventType::CommandFailed) == 1);  // nothing to cancel
    }
}

TEST_CASE("Travel: the colonist under control goes to sleep with the rest and can be taken again") {
    TravelNight n;
    TRAVEL_REQUIRE(n.ready);
    const Entity player = travelPawn(n.sim, CellPos{30, 30, 0}, 0.9f);
    n.sim.things().registry().emplace<Colonist>(player);
    n.sim.submit(ControlPawnCommand{player});
    travelStep(n.sim, n.log);
    CHECK(n.sim.things().registry().all_of<PlayerControl>(player));
    n.sim.submit(SkipNightCommand{});
    travelStep(n.sim, n.log);
    CHECK_FALSE(n.sim.things().registry().all_of<PlayerControl>(player));
    CHECK(n.log.last(EventType::SkipStarted)->thing == player);
    TRAVEL_REQUIRE(travelRunUntil(n.sim, n.log, 100, [&] { return !n.sim.travel().skipping(); }));
    CHECK(n.log.last(EventType::SkipEnded)->thing == player);
    n.sim.submit(ControlPawnCommand{player});
    travelStep(n.sim, n.log);
    CHECK(n.sim.things().registry().all_of<PlayerControl>(player));
}

TEST_CASE("Travel: the controlled colonist marks where it has been, and can fast travel there") {
    Sim sim = travelSim();
    TravelLog log;
    const Entity pawn = travelPawn(sim, CellPos{20, 20, 0}, 1.0f);
    sim.things().registry().emplace<Colonist>(pawn);
    sim.submit(ControlPawnCommand{pawn});
    travelStep(sim, log);
    CHECK(sim.travel().visited(CellPos{20, 20, 0}));
    CHECK(sim.travel().visited(CellPos{30, 20, 0}));
    CHECK_FALSE(sim.travel().visited(CellPos{100, 100, 0}));

    // Walk east 60 m by direct control: 0.5 m a step.
    float x = 20.5f;
    for (int i = 0; i < 130; ++i) {
        x += 0.5f;
        sim.submit(DirectMoveCommand{pawn, x, 20.5f});
        travelStep(sim, log);
    }
    const Position& pos = sim.things().registry().get<Position>(pawn);
    CHECK(pos.x > 80.0f);
    CHECK(sim.travel().visited(CellPos{70, 20, 0}));
    CHECK_FALSE(sim.travel().visited(CellPos{70, 100, 0}));

    std::string why;
    // Nowhere you haven't been, and nowhere close.
    CHECK(sim.travel().travelSeconds(pawn, 70.5f, 100.5f, why) == 0);
    CHECK(why == "you haven't been there");
    CHECK(sim.travel().travelSeconds(pawn, pos.x - 5.0f, 20.5f, why) == 0);
    CHECK(why == "that is close enough to walk");
    CHECK(sim.travel().travelSeconds(pawn, std::nanf(""), 20.5f, why) == 0);

    // Back to the start: the straight line times 1.3 at 0.15333 m a second.
    const float distance = std::hypot(pos.x - 22.5f, pos.y - 20.5f);
    const int64_t expected = static_cast<int64_t>(std::ceil(distance * 1.3f / 0.15333f));
    CHECK(sim.travel().travelSeconds(pawn, 22.5f, 20.5f, why) == expected);
    const int64_t from = sim.time().seconds;
    sim.submit(FastTravelCommand{pawn, 22.5f, 20.5f});
    travelStep(sim, log);
    CHECK(sim.travel().kind() == SkipKind::Travel);
    CHECK(log.count(EventType::SkipStarted) == 1);
    CHECK(sim.things().registry().get<Position>(pawn).x > 80.0f);  // still there until it arrives
    TRAVEL_REQUIRE(travelRunUntil(sim, log, 400, [&] { return !sim.travel().skipping(); }));
    const Event* ended = log.last(EventType::SkipEnded);
    TRAVEL_REQUIRE(ended != nullptr);
    CHECK(ended->message == "travel: arrived");
    CHECK(sim.time().seconds - from >= expected);
    CHECK(sim.time().seconds - from < expected + sim.travel().rules().travelBatch + 1);
    const Position& landed = sim.things().registry().get<Position>(pawn);
    CHECK(landed.x == doctest::Approx(22.5f));
    CHECK(landed.y == doctest::Approx(20.5f));
    CHECK(sim.travel().progress().travels == 1);
    CHECK(sim.things().registry().all_of<PlayerControl>(pawn));
}

TEST_CASE("Travel: a fire stops fast travel and leaves the colonist where it was") {
    Sim sim = travelSim();
    TravelLog log;
    const Entity pawn = travelPawn(sim, CellPos{20, 20, 0}, 1.0f);
    sim.things().registry().emplace<Colonist>(pawn);
    sim.submit(ControlPawnCommand{pawn});
    travelStep(sim, log);
    float x = 20.5f;
    for (int i = 0; i < 100; ++i) {
        x += 0.5f;
        sim.submit(DirectMoveCommand{pawn, x, 20.5f});
        travelStep(sim, log);
    }
    sim.submit(FastTravelCommand{pawn, 22.5f, 20.5f});
    travelStep(sim, log);
    TRAVEL_REQUIRE(sim.travel().skipping());
    CHECK(sim.fire().ignite(CellPos{90, 90, 0}, 1.0f, true));
    TRAVEL_REQUIRE(travelRunUntil(sim, log, 50, [&] { return !sim.travel().skipping(); }));
    CHECK(log.last(EventType::SkipEnded)->message == "travel: something is on fire");
    CHECK(sim.things().registry().get<Position>(pawn).x > 60.0f);
    CHECK(sim.travel().progress().travels == 0);
}

TEST_CASE("Travel: save and load keep the visited places and a skip in progress, and the run goes on the same") {
    TravelNight n;
    TRAVEL_REQUIRE(n.ready);
    const Entity player = travelPawn(n.sim, CellPos{30, 30, 0}, 1.0f);
    n.sim.things().registry().emplace<Colonist>(player);
    n.sim.submit(ControlPawnCommand{player});
    travelStep(n.sim, n.log);
    n.sim.submit(SkipNightCommand{});
    travelStep(n.sim, n.log);
    TRAVEL_REQUIRE(n.sim.travel().skipping());

    const std::vector<uint8_t> bytes = saveSim(n.sim);
    SaveLoadResult loaded = loadSim(bytes, travelDefs());
    TRAVEL_REQUIRE(loaded.error.empty());
    TRAVEL_REQUIRE(loaded.sim != nullptr);
    Sim& copy = *loaded.sim;
    CHECK(copy.stateHash() == n.sim.stateHash());
    CHECK(copy.travel().skipping());
    CHECK(copy.travel().visited(CellPos{30, 30, 0}));
    CHECK(copy.travel().visitedBlocks() == n.sim.travel().visitedBlocks());
    CHECK(copy.travel().progress().endsAt == n.sim.travel().progress().endsAt);

    n.sim.stepMany(60);
    copy.stepMany(60);
    CHECK(copy.stateHash() == n.sim.stateHash());
    CHECK(copy.time().seconds == n.sim.time().seconds);
    CHECK(copy.time().seconds >= 6 * kSecondsPerHour);
    CHECK_FALSE(copy.travel().skipping());
    CHECK(copy.travel().progress().nights == 1);
    CHECK(saveSim(copy) == saveSim(n.sim));
}

TEST_CASE("Travel: commands survive their names and a replay") {
    Command command;
    CHECK(commandFromName("SkipNight", command));
    CHECK(std::holds_alternative<SkipNightCommand>(command));
    CHECK(commandFromName("FastTravel", command));
    CHECK(std::holds_alternative<FastTravelCommand>(command));
    CHECK(commandFromName("CancelSkip", command));
    CHECK(std::holds_alternative<CancelSkipCommand>(command));
    CHECK(std::string(commandName(FastTravelCommand{})) == "FastTravel");
}

TEST_CASE("Travel: the commands go through a replay file") {
    Replay replay;
    replay.settings = SimSettings{5, 128, 0, 0, true};
    replay.commands.push_back(LoggedCommand{10, SkipNightCommand{}});
    replay.commands.push_back(LoggedCommand{20, FastTravelCommand{static_cast<Entity>(7), 33.5f, 44.25f}});
    replay.commands.push_back(LoggedCommand{30, CancelSkipCommand{}});
    const std::string text = writeReplay(replay);
    Replay back;
    std::string error;
    TRAVEL_REQUIRE(readReplay(text, back, error));
    TRAVEL_REQUIRE(back.commands.size() == 3);
    CHECK(std::holds_alternative<SkipNightCommand>(back.commands[0].command));
    const auto* travel = std::get_if<FastTravelCommand>(&back.commands[1].command);
    TRAVEL_REQUIRE(travel != nullptr);
    CHECK(travel->pawn == static_cast<Entity>(7));
    CHECK(travel->x == doctest::Approx(33.5f));
    CHECK(travel->y == doctest::Approx(44.25f));
    CHECK(std::holds_alternative<CancelSkipCommand>(back.commands[2].command));
}
