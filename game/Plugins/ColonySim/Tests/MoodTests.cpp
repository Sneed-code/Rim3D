#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Mood.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define MOOD_REQUIRE(...)      \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> moodTestDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        const std::vector<DefSource> sources = {
            {"Things", "test.json", R"([
        {"defName": "Human", "category": "Pawn", "stats": {"moveSpeed": 0.15333}, "race": {"foodPerDay": 1.6}},
        {"defName": "Meal", "category": "Item", "stackLimit": 10, "stats": {"nutrition": 0.9},
         "ingestible": {"preferability": "MealSimple", "maxNumToIngestAtOnce": 1}},
        {"defName": "FineMeal", "category": "Item", "stackLimit": 10, "stats": {"nutrition": 0.9},
         "ingestible": {"preferability": "MealFine", "maxNumToIngestAtOnce": 1}},
        {"defName": "RawMeat", "category": "Item", "stackLimit": 75, "stats": {"nutrition": 0.05},
         "ingestible": {"preferability": "RawBad"}},
        {"defName": "Berries", "category": "Item", "stackLimit": 75, "stats": {"nutrition": 0.05},
         "ingestible": {"preferability": "RawTasty"}},
        {"defName": "Bed", "category": "Building", "passability": "Standable", "stats": {"bedRestEffectiveness": 1.0},
         "building": {"bed": true, "sleepingSlots": 1}},
        {"defName": "Table", "category": "Building", "passability": "Impassable", "building": {"isTable": true}},
        {"defName": "Chair", "category": "Building", "passability": "Standable", "building": {"isSittable": true}}
    ])"},
            {"Terrain", "test.json", R"([{"defName": "Soil"}])"},
            {"Rules", "test.json", R"([{"defName": "NeedRules"}, {"defName": "MoodRules"}])"},
            {"Traits", "test.json", R"([
        {"defName": "NaturalMood", "degreeDatas": [{"degree": -2, "label": "depressive", "moodOffset": -12}, {"degree": 2, "label": "sanguine", "moodOffset": 12}]},
        {"defName": "Nerves", "degreeDatas": [{"degree": -2, "label": "volatile", "statOffsets": {"mentalBreakThreshold": 0.15}}]}
    ])"},
            {"Thoughts", "test.json", R"([
        {"defName": "Hungry", "label": "hungry", "moodOffset": -6, "situation": {"need": "food", "band": "Hungry"}},
        {"defName": "Tired", "label": "tired", "moodOffset": -12, "situation": {"need": "rest", "band": "Tired"}},
        {"defName": "AteWithoutTable", "label": "ate without a table", "moodOffset": -3, "durationDays": 0.6, "trigger": "AteWithoutTable"},
        {"defName": "AteRawFood", "label": "ate raw food", "moodOffset": -7, "durationDays": 0.6, "trigger": "AteRawFood"},
        {"defName": "AteFineMeal", "label": "ate a fine meal", "moodOffset": 5, "durationDays": 0.6, "trigger": "AteFineMeal"},
        {"defName": "SleptOnGround", "label": "slept on the ground", "moodOffset": -4, "durationDays": 0.6, "trigger": "SleptOnGround"}
    ])"},
            {"WorkTypes", "test.json", R"([
        {"defName": "Hauling", "naturalPriority": 300, "tasks": ["HaulToStockpile"]}
    ])"}};
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::build(sources, "", *db, errors);
        return db;
    }();
    return defs;
}

Sim moodSim() { return Sim(SimSettings{5, 32, 0, 0, true}, moodTestDefs()); }

Entity moodPawn(Sim& sim, CellPos cell) {
    const Entity pawn = sim.things().spawn("Human", cell);
    sim.jobs().addWorker(pawn);
    return pawn;
}

Needs& moodNeeds(Sim& sim, Entity pawn) { return sim.things().registry().get<Needs>(pawn); }

Mood& moodOf(Sim& sim, Entity pawn) { return sim.things().registry().get<Mood>(pawn); }

bool moodHasMemory(Sim& sim, Entity pawn, const std::string& def) {
    const Mood& mood = moodOf(sim, pawn);
    return std::any_of(mood.memories.begin(), mood.memories.end(), [&](const Memory& m) { return m.def == def; });
}

// Steps until `done` says so, at most `limit` steps, keeping the events of every step.
template <typename F>
bool moodRunUntil(Sim& sim, std::vector<Event>& log, int limit, F done) {
    for (int i = 0; i < limit; ++i) {
        if (done()) {
            return true;
        }
        sim.step();
        log.insert(log.end(), sim.events().begin(), sim.events().end());
    }
    return done();
}

int moodCount(const std::vector<Event>& log, EventType type) {
    return static_cast<int>(std::count_if(log.begin(), log.end(), [&](const Event& e) { return e.type == type; }));
}

}  // namespace

TEST_CASE("Mood: a colonist starts at the base mood and trait offsets move it") {
    Sim sim = moodSim();
    CHECK(sim.mood().rules().present);
    const Entity plain = moodPawn(sim, CellPos{5, 5, 0});
    const Entity happy = moodPawn(sim, CellPos{8, 8, 0});
    Colonist colonist;
    colonist.traits.push_back(TraitRecord{"NaturalMood", 2});
    sim.things().registry().emplace<Colonist>(happy, colonist);
    sim.step();
    MOOD_REQUIRE(sim.things().registry().all_of<Mood>(plain));
    CHECK(moodOf(sim, plain).level == doctest::Approx(50.0f));
    CHECK(moodOf(sim, happy).level == doctest::Approx(62.0f));
    CHECK(moodOf(sim, plain).band == MoodBand::Stable);
    Snapshot snapshot;
    sim.writeSnapshot(snapshot);
    MOOD_REQUIRE(snapshot.find(happy) != nullptr);
    CHECK(snapshot.find(happy)->mood == doctest::Approx(62.0f));
}

TEST_CASE("Mood: hunger and tiredness are thoughts while they last, and the level follows at 8 an hour down, 12 up") {
    Sim sim = moodSim();
    const Entity pawn = moodPawn(sim, CellPos{5, 5, 0});
    sim.step();
    // Awake in the day (second 0 is midnight, so move the clock on first).
    sim.stepMany(8 * 3600);
    moodNeeds(sim, pawn).food = 0.2f;  // Hungry, but nothing to eat
    moodNeeds(sim, pawn).rest = 1.0f;
    const int64_t now = sim.time().seconds;
    const std::vector<ThoughtView> thoughts = sim.mood().thoughts(pawn, now);
    MOOD_REQUIRE(thoughts.size() == 1);
    CHECK(thoughts[0].def == "Hungry");
    CHECK(thoughts[0].situation);
    CHECK(sim.mood().target(pawn, now) == doctest::Approx(44.0f));

    const float before = moodOf(sim, pawn).level;
    sim.stepMany(3600);
    CHECK(before - moodOf(sim, pawn).level == doctest::Approx(6.0f).epsilon(0.05));  // reached 44 after 6 points: the target stops it
    sim.stepMany(3000);  // (still Hungry, not yet Ravenous)
    CHECK(moodOf(sim, pawn).level == doctest::Approx(44.0f).epsilon(0.001));

    // Fed again, it climbs back at 12 an hour.
    moodNeeds(sim, pawn).food = 1.0f;
    sim.stepMany(1800);
    CHECK(moodOf(sim, pawn).level == doctest::Approx(50.0f));
}

TEST_CASE("Mood: eating gives memories by where and what: no table, raw, fine meal, and a fade after 0.6 days") {
    Sim sim = moodSim();
    const Entity pawn = moodPawn(sim, CellPos{5, 5, 0});
    sim.stepMany(8 * 3600);  // daytime
    std::vector<Event> log;

    // A simple meal with no table: ate without a table.
    sim.things().spawn("Meal", CellPos{7, 5, 0});
    moodNeeds(sim, pawn).food = 0.1f;
    MOOD_REQUIRE(moodRunUntil(sim, log, 4000, [&] { return moodCount(log, EventType::PawnAte) == 1; }));
    sim.step();
    CHECK(moodHasMemory(sim, pawn, "AteWithoutTable"));
    CHECK(!moodHasMemory(sim, pawn, "AteFineMeal"));

    // A fine meal at a table: a good memory and no bad one for the table.
    sim.things().spawn("Table", CellPos{12, 12, 0});
    sim.things().spawn("Chair", CellPos{12, 13, 0});
    sim.things().spawn("FineMeal", CellPos{9, 5, 0});
    moodNeeds(sim, pawn).food = 0.1f;
    MOOD_REQUIRE(moodRunUntil(sim, log, 4000, [&] { return moodCount(log, EventType::PawnAte) == 2; }));
    sim.step();
    CHECK(moodHasMemory(sim, pawn, "AteFineMeal"));
    CHECK(sim.things().registry().get<Mood>(pawn).memories.size() == 2);

    // Raw meat is a bad memory; berries are nothing.
    sim.things().spawn("RawMeat", CellPos{14, 5, 0}, 20);
    moodNeeds(sim, pawn).food = 0.1f;
    MOOD_REQUIRE(moodRunUntil(sim, log, 4000, [&] { return moodCount(log, EventType::PawnAte) == 3; }));
    sim.step();
    CHECK(moodHasMemory(sim, pawn, "AteRawFood"));

    // They all show in the thoughts list and are gone 0.6 days after the last one.
    CHECK(sim.mood().thoughts(pawn, sim.time().seconds).size() >= 3);
    sim.stepMany(static_cast<int>(0.6 * 86400.0) + 10);
    CHECK(moodOf(sim, pawn).memories.empty());
}

TEST_CASE("Mood: sleeping on the ground is a bad memory and the level stands still asleep") {
    Sim sim = moodSim();
    const Entity pawn = moodPawn(sim, CellPos{5, 5, 0});
    sim.step();
    moodNeeds(sim, pawn).rest = 0.3f;  // second 0 is midnight: a Sleep hour, no bed
    std::vector<Event> log;
    MOOD_REQUIRE(moodRunUntil(sim, log, 200, [&] { return jobIsLyingDown(sim.things().registry().get<Worker>(pawn).job); }));
    const float asleepAt = moodOf(sim, pawn).level;
    moodOf(sim, pawn).level = 80.0f;  // would fall if awake
    sim.stepMany(1800);
    CHECK(moodOf(sim, pawn).level == doctest::Approx(80.0f));
    CHECK(asleepAt > 0.0f);
    MOOD_REQUIRE(moodRunUntil(sim, log, 40000, [&] { return moodCount(log, EventType::PawnWokeUp) == 1; }));
    sim.step();
    CHECK(moodHasMemory(sim, pawn, "SleptOnGround"));

    // In a bed there is no such memory.
    Sim bedSim = moodSim();
    const Entity sleeper = moodPawn(bedSim, CellPos{5, 5, 0});
    bedSim.things().spawn("Bed", CellPos{8, 5, 0});
    moodNeeds(bedSim, sleeper).rest = 0.3f;
    std::vector<Event> bedLog;
    MOOD_REQUIRE(moodRunUntil(bedSim, bedLog, 40000, [&] { return moodCount(bedLog, EventType::PawnWokeUp) == 1; }));
    bedSim.step();
    CHECK(!moodHasMemory(bedSim, sleeper, "SleptOnGround"));
}

TEST_CASE("Mood: the break band follows the thresholds, traits move them, and a PawnMoodBand event says so") {
    Sim sim = moodSim();
    const Entity pawn = moodPawn(sim, CellPos{5, 5, 0});
    sim.step();
    float minor = 0.0f, major = 0.0f, extreme = 0.0f;
    sim.mood().breakLevels(pawn, minor, major, extreme);
    CHECK(minor == doctest::Approx(35.0f));
    CHECK(major == doctest::Approx(20.0f));
    CHECK(extreme == doctest::Approx(5.0f));

    // A depressive pawn (12 below the base) that is also hungry (6) heads for 32: under minor,
    // not under major.
    Colonist sad;
    sad.traits.push_back(TraitRecord{"NaturalMood", -2});
    sim.things().registry().emplace<Colonist>(pawn, sad);
    sim.stepMany(8 * 3600);
    moodNeeds(sim, pawn).food = 0.2f;
    moodNeeds(sim, pawn).rest = 1.0f;
    std::vector<Event> log;
    MOOD_REQUIRE(moodRunUntil(sim, log, 4 * 3600, [&] { return moodOf(sim, pawn).band == MoodBand::Minor; }));
    MOOD_REQUIRE(moodCount(log, EventType::PawnMoodBand) == 1);
    const Event& event = *std::find_if(log.begin(), log.end(), [](const Event& e) { return e.type == EventType::PawnMoodBand; });
    CHECK(event.message == "minor");
    CHECK(event.thing == pawn);
    CHECK(moodOf(sim, pawn).level < 35.0f);
    Snapshot snapshot;
    sim.writeSnapshot(snapshot);
    CHECK(snapshot.find(pawn)->moodBand == static_cast<int>(MoodBand::Minor));

    // Forced down it goes through major and extreme, one event each, and back up when fed and rested.
    moodOf(sim, pawn).level = 4.0f;
    sim.step();
    CHECK(moodOf(sim, pawn).band == MoodBand::Extreme);
    moodNeeds(sim, pawn).food = 1.0f;
    moodNeeds(sim, pawn).rest = 1.0f;
    sim.stepMany(6 * 3600);
    CHECK(moodOf(sim, pawn).band == MoodBand::Stable);  // the trait alone is 38, over minor
    CHECK(moodOf(sim, pawn).level == doctest::Approx(38.0f));

    // A volatile pawn breaks easier: every threshold is 15 points higher.
    sim.things().registry().get<Colonist>(pawn).traits.push_back(TraitRecord{"Nerves", -2});
    sim.mood().breakLevels(pawn, minor, major, extreme);
    CHECK(minor == doctest::Approx(50.0f));
    CHECK(major == doctest::Approx(35.0f));
    CHECK(extreme == doctest::Approx(20.0f));
}

TEST_CASE("Mood: pawns without needs have no mood, and the dead keep theirs") {
    Sim sim = moodSim();
    const Entity worker = sim.things().spawn("Human", CellPos{5, 5, 0});
    sim.jobs().addWorker(worker, {}, false);
    const Entity pawn = moodPawn(sim, CellPos{8, 8, 0});
    sim.stepMany(10);
    CHECK(!sim.things().registry().all_of<Mood>(worker));
    MOOD_REQUIRE(sim.things().registry().all_of<Mood>(pawn));
    Snapshot snapshot;
    sim.writeSnapshot(snapshot);
    CHECK(snapshot.find(worker)->mood == -1.0f);
}

TEST_CASE("Mood: save and load keep the level, band and memories, and the game carries on the same") {
    Sim original = moodSim();
    const Entity pawn = moodPawn(original, CellPos{5, 5, 0});
    original.stepMany(8 * 3600);
    original.things().spawn("Meal", CellPos{7, 5, 0});
    moodNeeds(original, pawn).food = 0.1f;
    std::vector<Event> log;
    MOOD_REQUIRE(moodRunUntil(original, log, 4000, [&] { return moodCount(log, EventType::PawnAte) == 1; }));
    original.step();
    MOOD_REQUIRE(moodHasMemory(original, pawn, "AteWithoutTable"));

    const std::vector<uint8_t> bytes = saveSim(original);
    SaveLoadResult loaded = loadSim(bytes, moodTestDefs());
    MOOD_REQUIRE(loaded.error.empty());
    MOOD_REQUIRE(loaded.sim != nullptr);
    Sim& copy = *loaded.sim;
    CHECK(copy.stateHash() == original.stateHash());
    CHECK(moodOf(copy, pawn).level == doctest::Approx(moodOf(original, pawn).level));
    CHECK(moodHasMemory(copy, pawn, "AteWithoutTable"));

    original.stepMany(20000);
    copy.stepMany(20000);
    CHECK(copy.stateHash() == original.stateHash());
    CHECK(saveSim(copy) == saveSim(original));
}
