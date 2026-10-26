#include "ColonySim/Colonist.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Mood.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Temperature.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define TRAITNEEDS_REQUIRE(...) \
    do {                        \
        CHECK((__VA_ARGS__));   \
        if (!(__VA_ARGS__)) {   \
            return;             \
        }                       \
    } while (0)

namespace {

// The game's real data: traits, thoughts and temperature numbers are part of what is tested.
std::shared_ptr<const DefDatabase> traitNeedsDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

std::unique_ptr<Sim> traitNeedsSim(float outdoor) {
    auto sim = std::make_unique<Sim>(SimSettings{5, 64, 0, 0, true}, traitNeedsDefs());
    sim->setOutdoorOverride(outdoor);
    sim->temperature().setSourceRunning([](Entity) { return true; });
    return sim;
}

Entity traitNeedsPawn(Sim& sim, CellPos cell, const std::string& trait = std::string()) {
    const Entity pawn = sim.things().spawn("Human", cell);
    sim.jobs().addWorker(pawn);
    if (!trait.empty()) {
        Colonist colonist;
        colonist.traits.push_back(TraitRecord{trait, 0});
        sim.things().registry().emplace_or_replace<Colonist>(pawn, colonist);
    }
    return pawn;
}

bool traitNeedsHas(Sim& sim, Entity pawn, const std::string& def) {
    const std::vector<ThoughtView> thoughts = sim.mood().thoughts(pawn, sim.time().seconds);
    return std::any_of(thoughts.begin(), thoughts.end(), [&](const ThoughtView& t) { return t.def == def; });
}

}  // namespace

TEST_CASE("Colonist traits: a gourmand gets hungry half as fast again") {
    auto sim = traitNeedsSim(20.0f);
    const Entity plain = traitNeedsPawn(*sim, CellPos{10, 10, 0});
    const Entity gourmand = traitNeedsPawn(*sim, CellPos{14, 10, 0}, "Gourmand");
    sim->stepMany(3600);
    const float plainLoss = 1.0f - sim->things().registry().get<Needs>(plain).food;
    const float gourmandLoss = 1.0f - sim->things().registry().get<Needs>(gourmand).food;
    CHECK(plainLoss > 0.0f);
    CHECK(gourmandLoss == doctest::Approx(plainLoss * 1.5f).epsilon(0.02));
}

TEST_CASE("Colonist traits: a quick sleeper rests half as fast again") {
    auto sim = traitNeedsSim(20.0f);
    const Entity plain = traitNeedsPawn(*sim, CellPos{10, 10, 0});
    const Entity sleeper = traitNeedsPawn(*sim, CellPos{14, 10, 0}, "QuickSleeper");
    for (const Entity pawn : {plain, sleeper}) {
        sim->things().registry().get<Needs>(pawn).rest = 0.2f;
    }
    const auto lying = [&](Entity pawn) { return jobIsLyingDown(sim->things().registry().get<Worker>(pawn).job); };
    for (int i = 0; i < 300 && !(lying(plain) && lying(sleeper)); ++i) {
        sim->step();
    }
    TRAITNEEDS_REQUIRE(lying(plain) && lying(sleeper));
    const float before = sim->things().registry().get<Needs>(plain).rest;
    sim->stepMany(1800);
    const float plainGain = sim->things().registry().get<Needs>(plain).rest - before;
    const float sleeperGain = sim->things().registry().get<Needs>(sleeper).rest - before;
    CHECK(plainGain > 0.0f);
    CHECK(sleeperGain == doctest::Approx(plainGain * 1.5f).epsilon(0.05));
}

TEST_CASE("Colonist traits: a night owl likes being up at night and not by day, and a sleeping one has neither") {
    auto sim = traitNeedsSim(20.0f);
    const Entity owl = traitNeedsPawn(*sim, CellPos{10, 10, 0}, "NightOwl");
    const Entity plain = traitNeedsPawn(*sim, CellPos{14, 10, 0});
    bool sawNight = false;
    bool sawDay = false;
    for (int hour = 0; hour < 30 && !(sawNight && sawDay); ++hour) {
        sim->stepMany(3600);
        const bool night = sim->mood().rules().isNightHour(sim->time().hourOfDay());
        CHECK(traitNeedsHas(*sim, owl, "NightOwlAwakeAtNight") == night);
        CHECK(traitNeedsHas(*sim, owl, "NightOwlAwakeInDay") == !night);
        CHECK_FALSE(traitNeedsHas(*sim, plain, "NightOwlAwakeAtNight"));
        CHECK_FALSE(traitNeedsHas(*sim, plain, "NightOwlAwakeInDay"));
        (night ? sawNight : sawDay) = true;
    }
    CHECK(sawNight);
    CHECK(sawDay);
    // Asleep it feels neither.
    sim->things().registry().get<Needs>(owl).rest = 0.1f;
    sim->stepMany(120);
    TRAITNEEDS_REQUIRE(jobIsLyingDown(sim->things().registry().get<Worker>(owl).job));
    CHECK_FALSE(traitNeedsHas(*sim, owl, "NightOwlAwakeAtNight"));
    CHECK_FALSE(traitNeedsHas(*sim, owl, "NightOwlAwakeInDay"));
}

TEST_CASE("Colonist traits: a greedy colonist minds having no bedroom of their own") {
    auto sim = traitNeedsSim(20.0f);
    const Entity greedy = traitNeedsPawn(*sim, CellPos{10, 10, 0}, "Greedy");
    const Entity plain = traitNeedsPawn(*sim, CellPos{14, 10, 0});
    CHECK(traitNeedsHas(*sim, greedy, "GreedyNoBedroom"));
    CHECK_FALSE(traitNeedsHas(*sim, plain, "GreedyNoBedroom"));

    // A bed in a walled room, owned by the greedy one.
    for (int y = 30; y <= 36; ++y) {
        for (int x = 30; x <= 36; ++x) {
            if (x == 30 || x == 36 || y == 30 || y == 36) {
                sim->things().spawn("Wall", CellPos{x, y, 0});
            }
        }
    }
    const Entity bed = sim->things().spawn("Bed", CellPos{33, 33, 0});
    TRAITNEEDS_REQUIRE(bed != kNoEntity);
    std::string error;
    TRAITNEEDS_REQUIRE(sim->jobs().assignBed(bed, greedy, error));
    sim->rooms().update();
    CHECK_FALSE(traitNeedsHas(*sim, greedy, "GreedyNoBedroom"));
}

TEST_CASE("Colonist traits: a colonist out in hard frost goes into a heated room and stays warm") {
    auto sim = traitNeedsSim(-25.0f);
    const CellPos doorCell{13, 10, 0};
    for (int y = 10; y <= 16; ++y) {
        for (int x = 10; x <= 16; ++x) {
            const CellPos c{x, y, 0};
            if ((x == 10 || x == 16 || y == 10 || y == 16) && c != doorCell) {
                sim->things().spawn("Wall", c);
            }
        }
    }
    sim->things().spawn("Door", doorCell);
    sim->things().spawn("Heater", CellPos{12, 12, 0});
    sim->rooms().update();
    sim->stepMany(86400 / 3);  // the room warms up
    const Entity pawn = traitNeedsPawn(*sim, CellPos{30, 30, 0});
    TRAITNEEDS_REQUIRE(sim->temperature().outsideSafeRange(pawn));

    bool sought = false;
    for (int i = 0; i < 900; ++i) {
        sim->step();
        for (const Event& e : sim->events()) {
            sought = sought || (e.type == EventType::JobStarted && e.job == JobType::SeekShelter && e.thing == pawn);
        }
    }
    CHECK(sought);
    CHECK_FALSE(sim->temperature().outsideSafeRange(pawn));
    float min = 0.0f;
    float max = 0.0f;
    TRAITNEEDS_REQUIRE(sim->temperature().comfortRange(pawn, min, max));
    const float ambient = sim->temperature().ambientOf(pawn);
    CHECK(ambient >= min);
    CHECK(ambient <= max);
}

TEST_CASE("Colonist traits: with no warm place anywhere a cold colonist does not go looking again and again") {
    auto sim = traitNeedsSim(-25.0f);
    const Entity pawn = traitNeedsPawn(*sim, CellPos{30, 30, 0});
    int started = 0;
    for (int i = 0; i < 600; ++i) {
        sim->step();
        for (const Event& e : sim->events()) {
            started += (e.type == EventType::JobStarted && e.job == JobType::SeekShelter && e.thing == pawn) ? 1 : 0;
        }
    }
    CHECK(started == 0);
    CHECK(sim->things().registry().valid(pawn));
}

TEST_CASE("Colonist traits: a comfortable colonist never seeks shelter") {
    auto sim = traitNeedsSim(20.0f);
    const Entity pawn = traitNeedsPawn(*sim, CellPos{30, 30, 0});
    for (int i = 0; i < 300; ++i) {
        sim->step();
        CHECK(sim->things().registry().get<Worker>(pawn).job.type != JobType::SeekShelter);
    }
}
