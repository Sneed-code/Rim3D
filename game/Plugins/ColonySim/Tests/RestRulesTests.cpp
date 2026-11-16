// sneed's rest rules (2026-11-16, docs/design/needs.md "Rest"): 40% used in a 16-hour day, 50% back from a night's
// sleep, nothing until 0% and then -25% performance; the protagonist has no rest need and sleeps only to pass the night.
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Mood.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"
#include "ColonySim/Travel.h"

#include "TestDefs.h"

#include <doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define REST_REQUIRE(...)      \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> restDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

Sim restSim() { return Sim(SimSettings{5, 64, 0, 0, true}, restDefs()); }

Entity restPawn(Sim& sim, CellPos cell, float rest) {
    const Entity pawn = sim.things().spawn("Human", cell);
    sim.jobs().addWorker(pawn);
    sim.things().registry().get<Needs>(pawn).rest = rest;
    return pawn;
}

Needs& restNeeds(Sim& sim, Entity pawn) { return sim.things().registry().get<Needs>(pawn); }
const Job& restJob(Sim& sim, Entity pawn) { return sim.things().registry().get<Worker>(pawn).job; }

void restMakeProtagonist(Sim& sim, Entity pawn) {
    Colonist colonist;
    colonist.protagonist = true;
    sim.things().registry().emplace_or_replace<Colonist>(pawn, colonist);
}

struct RestLog {
    std::vector<Event> events;
    int count(EventType type) const {
        return static_cast<int>(std::count_if(events.begin(), events.end(), [&](const Event& e) { return e.type == type; }));
    }
    const Event* last(EventType type) const {
        for (auto it = events.rbegin(); it != events.rend(); ++it) {
            if (it->type == type) {
                return &*it;
            }
        }
        return nullptr;
    }
};

void restStep(Sim& sim, RestLog& log) {
    sim.step();
    log.events.insert(log.events.end(), sim.events().begin(), sim.events().end());
}

template <typename F>
bool restRunUntil(Sim& sim, RestLog& log, int limit, F done) {
    for (int i = 0; i < limit; ++i) {
        if (done()) {
            return true;
        }
        restStep(sim, log);
    }
    return done();
}

// Runs the sim to the given hour of the day (it starts at midnight of day 0).
void restRunToHour(Sim& sim, RestLog& log, int hour) {
    while (sim.time().hourOfDay() != hour) {
        restStep(sim, log);
    }
}

}  // namespace

TEST_CASE("Rest: sneed's numbers are in the game's data") {
    const NeedRules rules = NeedRules::read(*restDefs());
    CHECK(rules.restFallPerDayRested == doctest::Approx(0.6f));
    CHECK(rules.restFallPerDayExhausted == doctest::Approx(0.6f));
    CHECK(rules.restGainPerDay == doctest::Approx(1.5f));
    // 16 waking hours use 40%, 8 hours in a bed give 50%, 8 on the ground 40%.
    CHECK(rules.restFallPerDayRested * 16.0f / 24.0f == doctest::Approx(0.4f));
    CHECK(rules.restGainPerDay * 8.0f / 24.0f == doctest::Approx(0.5f));
    CHECK(rules.restGainPerDay * rules.groundRestEffectiveness * 8.0f / 24.0f == doctest::Approx(0.4f));
    CHECK_FALSE(rules.sleepThroughNight);  // up once full: staying in bed till 6:00 cost the year colony too many work hours
    CHECK(rules.exhaustedPerformance == doctest::Approx(0.75f));
    CHECK(restBandOf(rules, 0.05f) == RestBand::Rested);
    CHECK(restBandOf(rules, 0.0f) == RestBand::Exhausted);
}

TEST_CASE("Rest: awake it falls 40% in 16 hours, and nothing happens until it is empty") {
    Sim sim = restSim();
    RestLog log;
    restRunToHour(sim, log, 6);
    const Entity pawn = restPawn(sim, CellPos{10, 10, 0}, 1.0f);
    sim.stepMany(16 * 3600);  // 6:00 to 22:00
    CHECK(restNeeds(sim, pawn).rest == doctest::Approx(0.6f).epsilon(0.01));
    // Above 0 no mood, no slowdown.
    CHECK(sim.needs().performanceFactor(pawn) == doctest::Approx(1.0f));
    const std::vector<ThoughtView> thoughts = sim.mood().thoughts(pawn, sim.time().seconds);
    CHECK(std::none_of(thoughts.begin(), thoughts.end(),
                       [](const ThoughtView& t) { return t.def == "Drowsy" || t.def == "Tired" || t.def == "Exhausted"; }));
}

TEST_CASE("Rest: every night pawns go to bed, get up once full, gain 50% from a night, and catch up over nights") {
    Sim sim = restSim();
    RestLog log;
    restRunToHour(sim, log, 21);
    const Entity pawn = restPawn(sim, CellPos{10, 10, 0}, 0.6f);  // after a normal 16-hour day
    sim.things().spawn("Bed", CellPos{12, 10, 0});
    REST_REQUIRE(restRunUntil(sim, log, 2 * 3600, [&] { return jobIsLyingDown(restJob(sim, pawn)); }));
    CHECK(sim.time().hourOfDay() == 22);
    // 40% back takes 6.4 hours: up full at about 4:30, and not back to bed that night.
    REST_REQUIRE(restRunUntil(sim, log, 8 * 3600, [&] { return log.count(EventType::PawnWokeUp) == 1; }));
    CHECK(sim.time().hourOfDay() == 4);
    CHECK(restNeeds(sim, pawn).rest == doctest::Approx(1.0f));
    restRunToHour(sim, log, 6);
    CHECK_FALSE(jobIsLyingDown(restJob(sim, pawn)));

    // An exhausted pawn gets about 50% from a night, and is still asleep at 6:00 if under 50%: it naps on till then.
    Sim tired = restSim();
    RestLog tlog;
    restRunToHour(tired, tlog, 21);
    const Entity worn = restPawn(tired, CellPos{10, 10, 0}, 0.0f);
    tired.things().spawn("Bed", CellPos{12, 10, 0});
    restRunToHour(tired, tlog, 22);
    REST_REQUIRE(restRunUntil(tired, tlog, 3600, [&] { return jobIsLyingDown(restJob(tired, worn)); }));
    restRunToHour(tired, tlog, 6);
    CHECK(restNeeds(tired, worn).rest == doctest::Approx(0.5f).epsilon(0.05));
    REST_REQUIRE(restRunUntil(tired, tlog, 2 * 3600, [&] { return tlog.count(EventType::PawnWokeUp) == 1; }));
    CHECK(restNeeds(tired, worn).rest >= 0.5f);
}

TEST_CASE("Rest: outside the Sleep hours pawns keep going until nearly empty (Work hours: until empty)") {
    Sim sim = restSim();
    RestLog log;
    restRunToHour(sim, log, 10);
    const Entity pawn = restPawn(sim, CellPos{10, 10, 0}, 0.2f);
    sim.stepMany(60);
    CHECK(restJob(sim, pawn).type != JobType::LayDown);
    restNeeds(sim, pawn).rest = 0.05f;  // under napBelow
    REST_REQUIRE(restRunUntil(sim, log, 120, [&] { return restJob(sim, pawn).type == JobType::LayDown; }));
    // It naps to 50%, not to full.
    REST_REQUIRE(restRunUntil(sim, log, 12 * 3600, [&] { return log.count(EventType::PawnWokeUp) == 1; }));
    CHECK(restNeeds(sim, pawn).rest == doctest::Approx(0.5f).epsilon(0.02));

    // A Work hour: on at 5%, to bed only at 0.
    Sim work = restSim();
    RestLog wlog;
    restRunToHour(work, wlog, 10);
    const Entity worker = restPawn(work, CellPos{10, 10, 0}, 0.05f);
    for (int h = 0; h < 24; ++h) {
        work.submit(SetScheduleCommand{worker, h, ScheduleKind::Work});
    }
    work.stepMany(60);
    CHECK(restJob(work, worker).type != JobType::LayDown);
    restNeeds(work, worker).rest = 0.0f;
    REST_REQUIRE(restRunUntil(work, wlog, 120, [&] { return restJob(work, worker).type == JobType::LayDown; }));
}

TEST_CASE("Rest: at 0% a pawn is Exhausted: the mood, 25% slower work and attacks, 25% less accurate") {
    Sim sim = restSim();
    RestLog log;
    restRunToHour(sim, log, 10);
    const Entity pawn = restPawn(sim, CellPos{10, 10, 0}, 0.01f);
    const float workBefore = sim.jobs().workFactor(pawn);
    const float attackBefore = sim.combat().attackTimeFactor(pawn);
    CHECK(sim.needs().performanceFactor(pawn) == doctest::Approx(1.0f));
    restNeeds(sim, pawn).rest = 0.0f;
    CHECK(sim.needs().performanceFactor(pawn) == doctest::Approx(0.75f));
    CHECK(sim.combat().performance(pawn) == doctest::Approx(0.75f));
    CHECK(sim.jobs().workFactor(pawn) == doctest::Approx(workBefore * 0.75f));
    CHECK(sim.combat().attackTimeFactor(pawn) == doctest::Approx(attackBefore / 0.75f));
    sim.step();
    const std::vector<ThoughtView> thoughts = sim.mood().thoughts(pawn, sim.time().seconds);
    CHECK(std::any_of(thoughts.begin(), thoughts.end(), [](const ThoughtView& t) { return t.def == "Exhausted"; }));
}

TEST_CASE("Rest: the protagonist has no rest need: held full, never sent to bed, none in the snapshot") {
    Sim sim = restSim();
    RestLog log;
    const Entity hero = restPawn(sim, CellPos{10, 10, 0}, 0.0f);
    restMakeProtagonist(sim, hero);
    sim.things().spawn("Bed", CellPos{12, 10, 0});
    sim.step();
    CHECK(restNeeds(sim, hero).rest == doctest::Approx(1.0f));
    CHECK(sim.needs().performanceFactor(hero) == doctest::Approx(1.0f));
    // Midnight, a Sleep hour: a free hour for them.
    sim.stepMany(2 * 3600);
    CHECK(restJob(sim, hero).type != JobType::LayDown);
    CHECK(log.count(EventType::PawnWokeUp) == 0);
    Snapshot snapshot;
    sim.writeSnapshot(snapshot);
    REST_REQUIRE(snapshot.find(hero) != nullptr);
    CHECK(snapshot.find(hero)->rest == -1.0f);
    CHECK(snapshot.find(hero)->food >= 0.0f);
}

TEST_CASE("Rest: the player sleeps the night away: in a bed or on the ground, to morning, still in control") {
    Sim sim = restSim();
    RestLog log;
    const Entity hero = restPawn(sim, CellPos{10, 10, 0}, 1.0f);
    restMakeProtagonist(sim, hero);
    const Entity bed = sim.things().spawn("Bed", CellPos{11, 10, 0});
    const Entity farBed = sim.things().spawn("Bed", CellPos{30, 30, 0});
    sim.submit(ControlPawnCommand{hero});
    restRunToHour(sim, log, 12);

    const auto run = [&](Command command) {
        const size_t before = log.events.size();
        sim.submit(std::move(command));
        restStep(sim, log);
        return std::none_of(log.events.begin() + static_cast<std::ptrdiff_t>(before), log.events.end(),
                            [](const Event& e) { return e.type == EventType::CommandFailed; });
    };
    // Noon: not yet.
    CHECK_FALSE(run(SleepCommand{hero, bed, false}));
    CHECK(log.last(EventType::CommandFailed)->message.find("18:00") != std::string::npos);
    restRunToHour(sim, log, 19);
    CHECK_FALSE(run(SleepCommand{hero, farBed, false}));  // out of reach
    CHECK_FALSE(run(SleepCommand{hero, bed, true}));      // not asleep, nothing to wake from
    const int64_t before = sim.time().seconds;
    REST_REQUIRE(run(SleepCommand{hero, bed, false}));
    CHECK(sim.jobs().sleepingByHand(hero));
    CHECK(sim.jobs().bedOf(hero) == bed);
    CHECK(sim.travel().kind() == SkipKind::Night);
    CHECK(sim.travel().playerNight());
    CHECK(cellOf(sim.things().registry().get<Position>(hero)) == CellPos{11, 10, 0});
    // The body stays put however Unreal pushes it.
    sim.submit(DirectMoveCommand{hero, 15.0f, 15.0f});
    restStep(sim, log);
    CHECK(cellOf(sim.things().registry().get<Position>(hero)) == CellPos{11, 10, 0});

    REST_REQUIRE(restRunUntil(sim, log, 5000, [&] { return !sim.travel().skipping(); }));
    CHECK(sim.time().hourOfDay() == 6);
    CHECK(sim.time().seconds - before > 10 * 3600);
    CHECK(log.last(EventType::SkipEnded)->message == "night: morning");
    CHECK_FALSE(sim.jobs().sleepingByHand(hero));
    CHECK(sim.controlledPawn() == hero);
    CHECK(restNeeds(sim, hero).rest == doctest::Approx(1.0f));

    // The next evening on the ground, woken early by the player.
    restRunToHour(sim, log, 20);
    REST_REQUIRE(run(SleepCommand{hero, kNoEntity, false}));
    CHECK(restJob(sim, hero).thing == kNoEntity);
    restStep(sim, log);
    REST_REQUIRE(run(SleepCommand{hero, kNoEntity, true}));
    CHECK_FALSE(sim.jobs().sleepingByHand(hero));
    CHECK_FALSE(sim.travel().skipping());
    CHECK(sim.time().hourOfDay() < 23);
    CHECK(sim.controlledPawn() == hero);
}

TEST_CASE("Rest: the player's sleep is refused when not in control or with a threat, and survives save and load") {
    Sim sim = restSim();
    RestLog log;
    const Entity hero = restPawn(sim, CellPos{10, 10, 0}, 1.0f);
    restMakeProtagonist(sim, hero);
    restRunToHour(sim, log, 20);
    sim.submit(SleepCommand{hero, kNoEntity, false});  // not under control
    restStep(sim, log);
    CHECK(log.count(EventType::CommandFailed) == 1);
    CHECK_FALSE(sim.jobs().sleepingByHand(hero));

    sim.submit(ControlPawnCommand{hero});
    sim.submit(SleepCommand{hero, kNoEntity, false});
    restStep(sim, log);
    REST_REQUIRE(sim.jobs().sleepingByHand(hero));
    std::string error;
    SaveLoadResult loaded = loadSim(saveSim(sim), restDefs());
    REST_REQUIRE(loaded.sim != nullptr);
    Sim& again = *loaded.sim;
    CHECK(again.jobs().sleepingByHand(hero));
    CHECK(again.travel().playerNight());
    CHECK(again.stateHash() == sim.stateHash());
    sim.stepMany(20);
    again.stepMany(20);
    CHECK(again.stateHash() == sim.stateHash());
}
