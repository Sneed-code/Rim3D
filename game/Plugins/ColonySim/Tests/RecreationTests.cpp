#include "FuelTestHelper.h"

#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Mood.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Recreation.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define REC_REQUIRE(...)       \
    do {                       \
        CHECK((__VA_ARGS__));  \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// The game's real data: the activities, the buildings and the thoughts are what is tested.
std::shared_ptr<const DefDatabase> recDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

Sim recSim() { return Sim(SimSettings{5, 48, 0, 0, true}, recDefs()); }

// A pawn that works and has needs, with the same kind of hour all day.
Entity recPawn(Sim& sim, CellPos cell, ScheduleKind kind) {
    const Entity pawn = sim.things().spawn("Human", cell);
    sim.jobs().addWorker(pawn);
    for (int h = 0; h < 24; ++h) {
        sim.submit(SetScheduleCommand{pawn, h, kind});
    }
    sim.step();  // applies the schedule and makes the pawn's Joy
    return pawn;
}

Joy& recJoy(Sim& sim, Entity pawn) { return sim.things().registry().get<Joy>(pawn); }

const Job& recJob(Sim& sim, Entity pawn) { return sim.things().registry().get<Worker>(pawn).job; }

CellPos recCell(Sim& sim, Entity pawn) { return cellOf(sim.things().registry().get<Position>(pawn)); }

std::string recActivity(Sim& sim, Entity pawn) {
    const Job& job = recJob(sim, pawn);
    return job.activity.valid() ? sim.defs().get(job.activity).defName() : std::string();
}

template <typename F>
bool recRunUntil(Sim& sim, int limit, F done) {
    for (int i = 0; i < limit; ++i) {
        if (done()) {
            return true;
        }
        sim.step();
    }
    return done();
}

int recDistance(CellPos a, CellPos b) { return std::abs(a.x - b.x) + std::abs(a.y - b.y); }

bool recThoughtHas(Sim& sim, Entity pawn, const std::string& def, const std::string& label, float offset) {
    for (const ThoughtView& t : sim.mood().thoughts(pawn, sim.time().seconds)) {
        if (t.def == def && t.label == label && std::fabs(t.offset - offset) < 1e-4f) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("Recreation: a colonist starts with the three needs half full and they fall and rise at the rules' rates") {
    Sim sim = recSim();
    const Entity pawn = recPawn(sim, CellPos{5, 5, 0}, ScheduleKind::Work);
    REC_REQUIRE(sim.things().registry().all_of<Joy>(pawn));
    const RecreationRules& rules = sim.recreation().rules();
    REC_REQUIRE(rules.present);
    CHECK(recJoy(sim, pawn).level == doctest::Approx(0.6f).epsilon(0.01));
    sim.stepMany(3600);
    // Joy falls 2.5% an hour awake, comfort 0.6 a day unused, outdoors rises 3 a day under the open sky.
    CHECK(recJoy(sim, pawn).level == doctest::Approx(0.6f - 0.025f).epsilon(0.01));
    CHECK(recJoy(sim, pawn).comfort == doctest::Approx(0.5f - 0.6f / 24.0f).epsilon(0.01));
    CHECK(recJoy(sim, pawn).outdoors == doctest::Approx(0.6f + 3.0f / 24.0f).epsilon(0.01));
    CHECK(recJob(sim, pawn).type != JobType::Recreate);  // a Work hour is for working
}

TEST_CASE("Recreation: indoors the Outdoors need falls") {
    Sim sim = recSim();
    for (int y = 10; y <= 16; ++y) {
        for (int x = 10; x <= 16; ++x) {
            if (x == 10 || x == 16 || y == 10 || y == 16) {
                sim.things().spawn("Wall", CellPos{x, y, 0});
            }
        }
    }
    sim.rooms().update();
    const Entity pawn = recPawn(sim, CellPos{13, 13, 0}, ScheduleKind::Work);
    REC_REQUIRE(sim.rooms().indoors(CellPos{13, 13, 0}));
    sim.stepMany(3600);
    CHECK(recJoy(sim, pawn).outdoors == doctest::Approx(0.6f - 0.5f / 24.0f).epsilon(0.01));
}

TEST_CASE("Recreation: tolerance cuts the joy of a kind and wears off") {
    Sim sim = recSim();
    const Entity pawn = recPawn(sim, CellPos{5, 5, 0}, ScheduleKind::Work);
    const RecreationRules& rules = sim.recreation().rules();
    const RecreationActivity* horseshoes = nullptr;
    for (const RecreationActivity& a : rules.activities) {
        if (a.defName == "Horseshoes") {
            horseshoes = &a;
        }
    }
    REC_REQUIRE(horseshoes != nullptr);
    Joy& joy = recJoy(sim, pawn);
    CHECK(rules.gainPerHour(*horseshoes, joy) == doctest::Approx(0.36f));
    joy.addTolerance("Dexterity", 0.5f, 1.0f);
    CHECK(rules.gainPerHour(*horseshoes, joy) == doctest::Approx(0.18f));
    joy.addTolerance("Dexterity", 5.0f, 1.0f);
    CHECK(joy.toleranceOf("Dexterity") == doctest::Approx(1.0f));
    CHECK(rules.gainPerHour(*horseshoes, joy) == doctest::Approx(0.036f));  // never under a tenth
    // 0.1 a day wears off.
    joy.tolerance.clear();
    joy.addTolerance("Dexterity", 0.5f, 1.0f);
    sim.stepMany(86400);
    CHECK(recJoy(sim, pawn).toleranceOf("Dexterity") == doctest::Approx(0.4f).epsilon(0.01));
}

TEST_CASE("Recreation: in a Joy hour a colonist plays horseshoes beside the pin and the joy and tolerance follow") {
    Sim sim = recSim();
    const Entity pin = sim.things().spawn("HorseshoesPin", CellPos{12, 12, 0});
    const Entity pawn = recPawn(sim, CellPos{5, 5, 0}, ScheduleKind::Joy);
    recJoy(sim, pawn).level = 0.3f;
    REC_REQUIRE(recRunUntil(sim, 600, [&] { return recJob(sim, pawn).type == JobType::Recreate; }));
    CHECK(recActivity(sim, pawn) == "Horseshoes");
    CHECK(recJob(sim, pawn).thing == pin);
    CHECK(recDistance(recJob(sim, pawn).cell, CellPos{12, 12, 0}) == 1);
    // It walks there, then plays.
    REC_REQUIRE(recRunUntil(sim, 600, [&] { return recJob(sim, pawn).step == 1 && recJob(sim, pawn).until != 0; }));
    CHECK(recCell(sim, pawn) == recJob(sim, pawn).cell);
    const float before = recJoy(sim, pawn).level;
    sim.stepMany(600);
    // Ten minutes at 0.36 an hour.
    CHECK(recJoy(sim, pawn).level - before == doctest::Approx(0.06f).epsilon(0.05));
    // Joy is not falling while it plays.
    REC_REQUIRE(recRunUntil(sim, 8000, [&] { return recJob(sim, pawn).type != JobType::Recreate; }));
    CHECK(recJoy(sim, pawn).level > 0.4f);
    CHECK(recJoy(sim, pawn).toleranceOf("Dexterity") > 0.04f);
}

TEST_CASE("Recreation: the activity with the least tolerance wins, and the pin takes only so many players") {
    Sim sim = recSim();
    sim.things().spawn("HorseshoesPin", CellPos{12, 12, 0});
    const Entity chess = sim.things().spawn("ChessTable", CellPos{20, 12, 0});
    const Entity a = recPawn(sim, CellPos{12, 14, 0}, ScheduleKind::Joy);
    recJoy(sim, a).level = 0.3f;
    recJoy(sim, a).addTolerance("Dexterity", 0.9f, 1.0f);
    REC_REQUIRE(recRunUntil(sim, 600, [&] { return recJob(sim, a).type == JobType::Recreate; }));
    CHECK(recActivity(sim, a) == "Chess");  // the pin is nearer, but dexterity games are worn out
    CHECK(recJob(sim, a).thing == chess);

    // The chess table has two places; the third pawn does something else.
    const Entity b = recPawn(sim, CellPos{20, 15, 0}, ScheduleKind::Joy);
    const Entity c = recPawn(sim, CellPos{20, 16, 0}, ScheduleKind::Joy);
    for (const Entity p : {b, c}) {
        recJoy(sim, p).level = 0.3f;
        recJoy(sim, p).addTolerance("Dexterity", 0.9f, 1.0f);
    }
    int mostAtChess = 0;
    for (int i = 0; i < 1500; ++i) {
        sim.step();
        int atChess = 0;
        for (const Entity p : {a, b, c}) {
            atChess += recJob(sim, p).type == JobType::Recreate && recJob(sim, p).thing == chess ? 1 : 0;
        }
        mostAtChess = std::max(mostAtChess, atChess);
    }
    CHECK(mostAtChess == 2);
}

TEST_CASE("Recreation: in an Anything hour a colonist with nothing to do plays only when bored, and work comes first") {
    Sim sim = recSim();
    const Entity pawn = recPawn(sim, CellPos{5, 5, 0}, ScheduleKind::Anything);
    recJoy(sim, pawn).level = 0.5f;
    sim.stepMany(120);
    CHECK(recJob(sim, pawn).type == JobType::Wait);
    recJoy(sim, pawn).level = 0.2f;
    REC_REQUIRE(recRunUntil(sim, 200, [&] { return recJob(sim, pawn).type == JobType::Recreate; }));
    CHECK((recActivity(sim, pawn) == "WalkNearby" || recActivity(sim, pawn) == "Skygaze"));  // no buildings: a stroll or the stars

    // With hauling to do, the same bored colonist hauls.
    Sim busy = recSim();
    busy.things().spawn("Steel", CellPos{6, 6, 0}, 5);
    busy.submit(CreateStockpileCommand{CellRect{25, 25, 3, 3, 0}, StoragePriority::Normal});
    const Entity hauler = recPawn(busy, CellPos{5, 5, 0}, ScheduleKind::Anything);
    recJoy(busy, hauler).level = 0.1f;
    busy.stepMany(5);
    CHECK(recJob(busy, hauler).type == JobType::HaulToCell);
}

TEST_CASE("Recreation: a stroll goes from place to place outdoors and stops after its hour") {
    Sim sim = recSim();
    const Entity pawn = recPawn(sim, CellPos{20, 20, 0}, ScheduleKind::Joy);
    recJoy(sim, pawn).level = 0.2f;
    REC_REQUIRE(recRunUntil(sim, 200, [&] { return recJob(sim, pawn).type == JobType::Recreate; }));
    REC_REQUIRE(recActivity(sim, pawn) == "WalkNearby" || recActivity(sim, pawn) == "Skygaze");
    if (recActivity(sim, pawn) != "WalkNearby") {
        return;  // under the stars it lies still
    }
    CHECK(recJob(sim, pawn).count == 1);  // it walks about
    const CellPos start = recCell(sim, pawn);
    int farthest = 0;
    bool indoors = false;
    int steps = 0;
    while (recJob(sim, pawn).type == JobType::Recreate && steps < 5000) {
        sim.step();
        ++steps;
        farthest = std::max(farthest, recDistance(recCell(sim, pawn), start));
        indoors = indoors || sim.rooms().indoors(recCell(sim, pawn));
    }
    CHECK(recJob(sim, pawn).type != JobType::Recreate);
    CHECK(steps < 4000);  // half an hour to an hour
    CHECK(farthest >= 4);
    CHECK_FALSE(indoors);
    CHECK(recJoy(sim, pawn).level > 0.2f);
}

TEST_CASE("Recreation: the clouds are watched by day only, and a Work hour ends the game") {
    Sim sim = recSim();
    const Entity pawn = recPawn(sim, CellPos{20, 20, 0}, ScheduleKind::Work);
    sim.stepMany(10 * 3600);  // to 10:00
    for (int h = 0; h < 24; ++h) {
        sim.submit(SetScheduleCommand{pawn, h, ScheduleKind::Joy});
    }
    recJoy(sim, pawn).level = 0.2f;
    REC_REQUIRE(recRunUntil(sim, 200, [&] { return recJob(sim, pawn).type == JobType::Recreate; }));
    CHECK(recActivity(sim, pawn) == "WatchClouds");  // under the open sky, nearest, and it is day
    CHECK(recJob(sim, pawn).count == 0);

    // The hour turns to Work: the pawn goes back to work.
    sim.submit(SetScheduleCommand{pawn, 10, ScheduleKind::Work});
    std::vector<Event> events;
    for (int i = 0; i < 5; ++i) {
        sim.step();
        events.insert(events.end(), sim.events().begin(), sim.events().end());
    }
    CHECK(recJob(sim, pawn).type != JobType::Recreate);
    CHECK(std::any_of(events.begin(), events.end(), [&](const Event& e) {
        return e.type == EventType::JobEnded && e.job == JobType::Recreate && e.message == "interrupted: time to work";
    }));

    // At night the same pawn takes a walk instead.
    Sim night = recSim();
    const Entity walker = recPawn(night, CellPos{20, 20, 0}, ScheduleKind::Joy);
    recJoy(night, walker).level = 0.2f;
    REC_REQUIRE(recRunUntil(night, 200, [&] { return recJob(night, walker).type == JobType::Recreate; }));
    CHECK(recActivity(night, walker) == "Skygaze");  // hour 0: the stars, not the clouds
}

TEST_CASE("Recreation: two colonists chat at a table, sitting in its chairs, and the chairs' comfort lifts the need") {
    Sim sim = recSim();
    const Entity table = sim.things().spawn("Table1x2", CellPos{20, 20, 0});
    const Entity stoolA = sim.things().spawn("Stool", CellPos{19, 20, 0});
    const Entity stoolB = sim.things().spawn("Stool", CellPos{21, 20, 0});
    const Entity a = recPawn(sim, CellPos{15, 20, 0}, ScheduleKind::Joy);
    const Entity b = recPawn(sim, CellPos{26, 20, 0}, ScheduleKind::Joy);
    for (const Entity p : {a, b}) {
        recJoy(sim, p).level = 0.3f;
        recJoy(sim, p).comfort = 0.0f;
        recJoy(sim, p).addTolerance("Meditative", 0.9f, 1.0f);  // not a stroll
    }
    REC_REQUIRE(recRunUntil(sim, 600, [&] { return recJob(sim, a).type == JobType::Recreate && recJob(sim, b).type == JobType::Recreate; }));
    CHECK(recActivity(sim, a) == "TalkAtTable");
    CHECK(recActivity(sim, b) == "TalkAtTable");
    CHECK(recJob(sim, a).thing == table);
    CHECK(recJob(sim, b).thing == table);
    CHECK(((recJob(sim, a).chair == stoolA && recJob(sim, b).chair == stoolB) ||
           (recJob(sim, a).chair == stoolB && recJob(sim, b).chair == stoolA)));
    REC_REQUIRE(recRunUntil(sim, 1200, [&] { return recJob(sim, a).step == 1 && recJob(sim, a).until != 0; }));
    CHECK(recCell(sim, a) == recJob(sim, a).cell);
    sim.stepMany(1800);
    // Half an hour in a stool (comfort 0.5): up from 0 by at most 0.25.
    CHECK(recJoy(sim, a).comfort > 0.15f);
    CHECK(recJoy(sim, a).comfort <= 0.5f);
}

TEST_CASE("Recreation: sleeping in a bed lifts comfort to the bed's, on the ground only to the ground's") {
    Sim bedSim = recSim();
    bedSim.things().spawn("Bed", CellPos{10, 10, 0});
    const Entity sleeper = recPawn(bedSim, CellPos{12, 10, 0}, ScheduleKind::Sleep);
    recJoy(bedSim, sleeper).comfort = 0.1f;
    bedSim.things().registry().get<Needs>(sleeper).rest = 0.3f;
    REC_REQUIRE(recRunUntil(bedSim, 600, [&] { return jobIsLyingDown(recJob(bedSim, sleeper)); }));
    bedSim.stepMany(2 * 3600);
    CHECK(recJoy(bedSim, sleeper).comfort == doctest::Approx(0.75f).epsilon(0.02));  // the bed's stats.comfort

    Sim groundSim = recSim();
    const Entity lier = recPawn(groundSim, CellPos{12, 10, 0}, ScheduleKind::Sleep);
    recJoy(groundSim, lier).comfort = 0.6f;
    groundSim.things().registry().get<Needs>(lier).rest = 0.3f;
    REC_REQUIRE(recRunUntil(groundSim, 600, [&] { return jobIsLyingDown(recJob(groundSim, lier)); }));
    groundSim.stepMany(4 * 3600);
    CHECK(recJoy(groundSim, lier).comfort == doctest::Approx(0.1f).epsilon(0.1));
}

TEST_CASE("Recreation: the needs make thoughts by stage") {
    Sim sim = recSim();
    const Entity pawn = recPawn(sim, CellPos{5, 5, 0}, ScheduleKind::Work);
    Joy& joy = recJoy(sim, pawn);
    joy.level = 0.05f;
    joy.comfort = 0.1f;
    joy.outdoors = 0.05f;
    CHECK(recThoughtHas(sim, pawn, "Recreation", "deprived of recreation", -10.0f));
    CHECK(recThoughtHas(sim, pawn, "Comfort", "uncomfortable", -3.0f));
    CHECK(recThoughtHas(sim, pawn, "Outdoors", "trapped indoors", -8.0f));
    const float low = sim.mood().target(pawn, sim.time().seconds);
    joy.level = 0.9f;
    joy.comfort = 0.8f;
    joy.outdoors = 0.9f;
    CHECK(recThoughtHas(sim, pawn, "Recreation", "fully satisfied", 10.0f));
    CHECK(recThoughtHas(sim, pawn, "Comfort", "very comfortable", 3.0f));
    CHECK(recThoughtHas(sim, pawn, "Outdoors", "fresh air", 2.0f));
    CHECK(sim.mood().target(pawn, sim.time().seconds) - low == doctest::Approx(36.0f));  // -21 to +15
    joy.level = 0.5f;
    joy.comfort = 0.4f;
    joy.outdoors = 0.5f;
    CHECK(recThoughtHas(sim, pawn, "Recreation", "content", 0.0f));
    CHECK(recThoughtHas(sim, pawn, "Comfort", "comfortable", 0.0f));
    CHECK(recThoughtHas(sim, pawn, "Outdoors", "fine", 0.0f));
}

TEST_CASE("Recreation: a colonist the player walks or a fighter does not go to play") {
    Sim sim = recSim();
    const Entity pawn = recPawn(sim, CellPos{5, 5, 0}, ScheduleKind::Joy);
    recJoy(sim, pawn).level = 0.1f;
    sim.things().registry().emplace<PlayerControl>(pawn);
    sim.stepMany(300);
    CHECK(recJob(sim, pawn).type == JobType::None);
}

TEST_CASE("Recreation: save and load keep the needs, tolerance and the game in play") {
    const auto defs = recDefs();
    Sim original(SimSettings{9, 48, 0, 0, true}, defs);
    original.things().spawn("HorseshoesPin", CellPos{12, 12, 0});
    const Entity a = recPawn(original, CellPos{5, 5, 0}, ScheduleKind::Joy);
    const Entity b = recPawn(original, CellPos{6, 5, 0}, ScheduleKind::Joy);
    recJoy(original, a).level = 0.3f;
    recJoy(original, a).addTolerance("Social", 0.25f, 1.0f);
    recJoy(original, b).level = 0.2f;
    REC_REQUIRE(recRunUntil(original, 3000, [&] {
        return recJob(original, a).type == JobType::Recreate && recJob(original, a).step == 1 && recJob(original, a).until != 0;
    }));
    original.stepMany(300);
    const std::vector<uint8_t> bytes = saveSim(original);
    SaveLoadResult loaded = loadSim(bytes, defs);
    REC_REQUIRE(loaded.error.empty());
    REC_REQUIRE(loaded.sim != nullptr);
    Sim& copy = *loaded.sim;
    CHECK(copy.stateHash() == original.stateHash());
    CHECK(saveSim(copy) == bytes);
    CHECK(recJob(copy, a).activity == recJob(original, a).activity);
    CHECK(recJob(copy, a).until == recJob(original, a).until);
    CHECK(recJoy(copy, a).toleranceOf("Social") == recJoy(original, a).toleranceOf("Social"));
    original.stepMany(6000);
    copy.stepMany(6000);
    CHECK(copy.stateHash() == original.stateHash());
    CHECK(recJoy(copy, a).level == recJoy(original, a).level);
}

namespace {

const RecreationActivity* recFind(const Sim& sim, const std::string& name) {
    for (const RecreationActivity& a : sim.recreation().rules().activities) {
        if (a.defName == name) {
            return &a;
        }
    }
    return nullptr;
}

// A pawn in a Joy hour, tired of nothing, at the given recreation.
Entity recPlayer(Sim& sim, CellPos cell, float level) {
    const Entity pawn = recPawn(sim, cell, ScheduleKind::Joy);
    recJoy(sim, pawn).level = level;
    return pawn;
}

}  // namespace

TEST_CASE("Recreation: the activity data has the wiki's kinds, tables and gain factors") {
    Sim sim = recSim();
    const RecreationRules& rules = sim.recreation().rules();
    for (const char* kind : {"Meditative", "Social", "Dexterity", "Cerebral", "Television", "Telescope"}) {
        CHECK(rules.kindIndex(kind) >= 0);
    }
    for (const char* name : {"Horseshoes", "Chess", "Billiards", "Poker", "WatchTelevision", "UseTelescope", "Meditate",
                             "WalkNearby", "WatchClouds", "Skygaze", "TalkAtTable", "SocialRelax"}) {
        CHECK_MESSAGE(recFind(sim, name) != nullptr, name);
    }
    const auto factor = [&](const char* def) {
        return sim.defs().get(sim.defs().idOf("Things", def)).number("building.joyGainFactor", 0.0);
    };
    CHECK(factor("ChessTable") == doctest::Approx(1.0));
    CHECK(factor("BilliardsTable") == doctest::Approx(1.3));
    CHECK(factor("PokerTable") == doctest::Approx(1.3));
    CHECK(factor("TubeTelevision") == doctest::Approx(1.2));
    CHECK(factor("FlatscreenTelevision") == doctest::Approx(1.4));
    CHECK(factor("MegascreenTelevision") == doctest::Approx(1.6));
    CHECK(factor("Telescope") == doctest::Approx(1.2));
    CHECK(rules.toleranceGainFraction == doctest::Approx(2.0f / 3.0f).epsilon(0.001));
}

TEST_CASE("Recreation: joy falls by the wiki's stages and not at all at nothing") {
    Sim sim = recSim();
    const Entity pawn = recPawn(sim, CellPos{5, 5, 0}, ScheduleKind::Work);
    Joy& joy = recJoy(sim, pawn);
    joy.level = 0.2f;
    sim.stepMany(3600);
    CHECK(recJoy(sim, pawn).level == doctest::Approx(0.2f - 0.0175f).epsilon(0.01));
    recJoy(sim, pawn).level = 0.1f;
    sim.stepMany(3600);
    CHECK(recJoy(sim, pawn).level == doctest::Approx(0.09f).epsilon(0.01));
    recJoy(sim, pawn).level = 0.0f;
    sim.stepMany(3600);
    CHECK(recJoy(sim, pawn).level == 0.0f);
}

TEST_CASE("Recreation: tolerance grows by two thirds of the joy gained, and a worn-out kind is boring until it wears off") {
    Sim sim = recSim();
    sim.things().spawn("HorseshoesPin", CellPos{12, 12, 0});
    const Entity pawn = recPlayer(sim, CellPos{12, 14, 0}, 0.2f);
    REC_REQUIRE(recRunUntil(sim, 600, [&] { return recJob(sim, pawn).type == JobType::Recreate; }));
    REC_REQUIRE(recRunUntil(sim, 600, [&] { return recJob(sim, pawn).step == 1 && recJob(sim, pawn).until != 0; }));
    const float before = recJoy(sim, pawn).level;
    sim.stepMany(900);
    const float gained = recJoy(sim, pawn).level - before;
    CHECK(gained > 0.05f);
    // Two thirds of it, less a quarter hour of wearing off (0.1 a day).
    CHECK(recJoy(sim, pawn).toleranceOf("Dexterity") == doctest::Approx(gained * 2.0f / 3.0f).epsilon(0.05));

    // Above half it is boring; it stays boring down to under 0.3.
    Joy& joy = recJoy(sim, pawn);
    joy.tolerance.clear();
    joy.addTolerance("Dexterity", 0.55f, 1.0f);
    sim.step();
    CHECK(recJoy(sim, pawn).boredWith("Dexterity"));
    recJoy(sim, pawn).tolerance[0].amount = 0.4f;
    sim.step();
    CHECK(recJoy(sim, pawn).boredWith("Dexterity"));
    recJoy(sim, pawn).tolerance[0].amount = 0.29f;
    sim.step();
    CHECK_FALSE(recJoy(sim, pawn).boredWith("Dexterity"));
}

TEST_CASE("Recreation: a pawn bored of a kind picks another kind, and goes back to the bored kind when nothing else is left") {
    Sim sim = recSim();
    sim.things().spawn("HorseshoesPin", CellPos{12, 12, 0});
    const Entity pawn = recPlayer(sim, CellPos{12, 14, 0}, 0.2f);
    recJoy(sim, pawn).addTolerance("Dexterity", 0.6f, 1.0f);
    sim.step();
    REC_REQUIRE(recRunUntil(sim, 600, [&] { return recJob(sim, pawn).type == JobType::Recreate; }));
    // Horseshoes (dexterity) is boring; the walk is not, though it is the lesser joy once worn in.
    CHECK(recActivity(sim, pawn) != "Horseshoes");

    // Bored of everything there is: it plays the bored kind rather than do nothing.
    Sim sim2 = recSim();
    sim2.things().spawn("HorseshoesPin", CellPos{12, 12, 0});
    const Entity other = recPlayer(sim2, CellPos{12, 14, 0}, 0.2f);
    for (const char* kind : {"Dexterity", "Meditative", "Social", "Cerebral", "Television", "Telescope"}) {
        recJoy(sim2, other).addTolerance(kind, 0.6f, 1.0f);
    }
    sim2.step();
    REC_REQUIRE(recRunUntil(sim2, 600, [&] { return recJob(sim2, other).type == JobType::Recreate; }));
    CHECK(recJoy(sim2, other).boredKinds() == 6);
    CHECK_FALSE(recActivity(sim2, other).empty());
}

TEST_CASE("Recreation: the poker table gives 1.3 times a game of chess and four can play") {
    Sim sim = recSim();
    const Entity poker = sim.things().spawn("PokerTable", CellPos{12, 12, 0});
    const Entity chess = sim.things().spawn("ChessTable", CellPos{20, 12, 0});
    REC_REQUIRE(poker != kNoEntity);
    REC_REQUIRE(chess != kNoEntity);
    std::vector<Entity> players;
    for (int i = 0; i < 5; ++i) {
        players.push_back(recPlayer(sim, CellPos{14 + i, 16, 0}, 0.3f));
    }
    int mostAtPoker = 0;
    bool pokerChosen = false;
    for (int i = 0; i < 1500; ++i) {
        sim.step();
        int at = 0;
        for (const Entity p : players) {
            if (recJob(sim, p).type == JobType::Recreate && recJob(sim, p).thing == poker) {
                ++at;
                pokerChosen = pokerChosen || recActivity(sim, p) == "Poker";
            }
        }
        mostAtPoker = std::max(mostAtPoker, at);
    }
    CHECK(pokerChosen);
    CHECK(mostAtPoker == 4);
    // At the pawn's own rate: 0.36 x 1.3 an hour, with no tolerance.
    const RecreationActivity* a = recFind(sim, "Poker");
    REC_REQUIRE(a != nullptr);
    CHECK(sim.recreation().rules().gainPerHour(*a, Joy{}) * 1.3f == doctest::Approx(0.468f));
}

TEST_CASE("Recreation: television needs power, is watched from a few cells away and gives 1.2 times a game") {
    Sim sim = recSim();
    const Entity tv = sim.things().spawn("TubeTelevision", CellPos{20, 20, 0});
    REC_REQUIRE(tv != kNoEntity);
    Entity pawn = recPlayer(sim, CellPos{24, 20, 0}, 0.2f);
    recJoy(sim, pawn).addTolerance("Meditative", 0.9f, 1.0f);  // not a stroll
    sim.stepMany(300);
    CHECK(recActivity(sim, pawn) != "WatchTelevision");  // no power

    testFueled(sim, sim.things().spawn("WoodFiredGenerator", CellPos{22, 23, 0}));
    for (int y = 21; y <= 22; ++y) {
        sim.things().spawn("PowerConduit", CellPos{20, y, 0});
    }
    sim.things().spawn("PowerConduit", CellPos{21, 23, 0});
    sim.things().spawn("PowerConduit", CellPos{20, 23, 0});
    sim.stepMany(5);
    CHECK(sim.power().running(tv));
    // The first pawn is already stargazing; a second one sees the set is on.
    pawn = recPlayer(sim, CellPos{24, 21, 0}, 0.2f);
    recJoy(sim, pawn).addTolerance("Meditative", 0.9f, 1.0f);
    REC_REQUIRE(recRunUntil(sim, 3000, [&] { return recJob(sim, pawn).type == JobType::Recreate; }));
    CHECK(recActivity(sim, pawn) == "WatchTelevision");
    CHECK(recJob(sim, pawn).thing == tv);
    const CellPos at = recJob(sim, pawn).cell;
    CHECK(std::max(std::abs(at.x - 20), std::abs(at.y - 20)) <= 3);
    CHECK(at != CellPos{20, 20, 0});
}

TEST_CASE("Recreation: a telescope under a roof is not used, one under the sky is") {
    Sim roofed = recSim();
    for (int y = 10; y <= 16; ++y) {
        for (int x = 10; x <= 16; ++x) {
            if (x == 10 || x == 16 || y == 10 || y == 16) {
                roofed.things().spawn("Wall", CellPos{x, y, 0});
            }
        }
    }
    roofed.rooms().update();
    roofed.things().spawn("Telescope", CellPos{13, 13, 0});
    REC_REQUIRE(roofed.rooms().indoors(CellPos{13, 13, 0}));
    const Entity indoor = recPlayer(roofed, CellPos{20, 20, 0}, 0.2f);
    recJoy(roofed, indoor).addTolerance("Meditative", 0.9f, 1.0f);
    roofed.stepMany(300);
    CHECK(recActivity(roofed, indoor) != "UseTelescope");

    Sim open = recSim();
    const Entity scope = open.things().spawn("Telescope", CellPos{13, 13, 0});
    const Entity pawn = recPlayer(open, CellPos{20, 20, 0}, 0.2f);
    recJoy(open, pawn).addTolerance("Meditative", 0.9f, 1.0f);
    REC_REQUIRE(recRunUntil(open, 600, [&] { return recJob(open, pawn).type == JobType::Recreate; }));
    CHECK(recActivity(open, pawn) == "UseTelescope");
    CHECK(recJob(open, pawn).thing == scope);
}

TEST_CASE("Recreation: meditating happens on the spot itself, one at a time") {
    Sim sim = recSim();
    const Entity spot = sim.things().spawn("MeditationSpot", CellPos{12, 12, 0});
    REC_REQUIRE(spot != kNoEntity);
    const Entity a = recPlayer(sim, CellPos{14, 12, 0}, 0.2f);
    const Entity b = recPlayer(sim, CellPos{14, 14, 0}, 0.2f);
    for (const Entity p : {a, b}) {
        recJoy(sim, p).addTolerance("Meditative", 0.0f, 1.0f);
    }
    REC_REQUIRE(recRunUntil(sim, 600, [&] { return recJob(sim, a).type == JobType::Recreate; }));
    CHECK(recActivity(sim, a) == "Meditate");  // the nearest of the same joy
    CHECK(recJob(sim, a).cell == CellPos{12, 12, 0});
    sim.stepMany(300);
    CHECK(recActivity(sim, b) != "Meditate");
    REC_REQUIRE(recRunUntil(sim, 1500, [&] { return recJob(sim, a).step == 1 && recJob(sim, a).until != 0; }));
    CHECK(recCell(sim, a) == CellPos{12, 12, 0});
}

TEST_CASE("Recreation: gazing at the stars is for the night, watching clouds for the day") {
    Sim night = recSim();  // hour 0
    const Entity pawn = recPlayer(night, CellPos{20, 20, 0}, 0.2f);
    recJoy(night, pawn).addTolerance("Meditative", 0.0f, 1.0f);
    // Walks tie with the stars on joy; make the walk worn so the stars win.
    recJoy(night, pawn).tolerance.clear();
    const RecreationActivity* sky = recFind(night, "Skygaze");
    REC_REQUIRE(sky != nullptr);
    CHECK(sky->nightOnly);
    CHECK(sky->kind == recFind(night, "WatchClouds")->kind);
    REC_REQUIRE(recRunUntil(night, 600, [&] { return recJob(night, pawn).type == JobType::Recreate; }));
    const std::string chosen = recActivity(night, pawn);
    CHECK((chosen == "Skygaze" || chosen == "WalkNearby"));
    CHECK(chosen != "WatchClouds");

    Sim day = recSim();
    day.stepMany(10 * 3600);
    const Entity d = recPawn(day, CellPos{20, 20, 0}, ScheduleKind::Joy);
    recJoy(day, d).level = 0.2f;
    REC_REQUIRE(recRunUntil(day, 600, [&] { return recJob(day, d).type == JobType::Recreate; }));
    CHECK(recActivity(day, d) != "Skygaze");
}

TEST_CASE("Recreation: colonists relax socially in chairs next to each other") {
    Sim sim = recSim();
    // Three chairs and no table: two sit together, the third joins them rather than a far chair.
    const Entity c1 = sim.things().spawn("Stool", CellPos{20, 20, 0});
    const Entity c2 = sim.things().spawn("Stool", CellPos{21, 20, 0});
    sim.things().spawn("Stool", CellPos{40, 40, 0});
    REC_REQUIRE(c1 != kNoEntity);
    REC_REQUIRE(c2 != kNoEntity);
    const Entity a = recPlayer(sim, CellPos{18, 20, 0}, 0.2f);
    const Entity b = recPlayer(sim, CellPos{23, 20, 0}, 0.2f);
    for (const Entity p : {a, b}) {
        recJoy(sim, p).addTolerance("Meditative", 0.9f, 1.0f);  // not a stroll
    }
    REC_REQUIRE(recRunUntil(sim, 1500, [&] { return recJob(sim, a).type == JobType::Recreate && recJob(sim, b).type == JobType::Recreate; }));
    CHECK(recActivity(sim, a) == "SocialRelax");
    CHECK(recActivity(sim, b) == "SocialRelax");
    CHECK(recJob(sim, a).thing == recJob(sim, b).thing);  // one group, so they chat (C20)
    CHECK(((recJob(sim, a).chair == c1 && recJob(sim, b).chair == c2) || (recJob(sim, a).chair == c2 && recJob(sim, b).chair == c1)));
}

TEST_CASE("Recreation: tolerance and boredom are saved and loaded") {
    Sim sim = recSim();
    const Entity pawn = recPawn(sim, CellPos{5, 5, 0}, ScheduleKind::Work);
    recJoy(sim, pawn).addTolerance("Cerebral", 0.7f, 1.0f);
    sim.step();
    REC_REQUIRE(recJoy(sim, pawn).boredWith("Cerebral"));
    const uint64_t hash = sim.stateHash();
    SaveLoadResult result = loadSim(saveSim(sim), recDefs());
    REC_REQUIRE(result.error.empty());
    REC_REQUIRE(result.sim != nullptr);
    Sim& loaded = *result.sim;
    CHECK(loaded.stateHash() == hash);
    const auto& tolerance = loaded.things().registry().get<Joy>(loaded.things().registry().view<Joy>().front()).tolerance;
    REC_REQUIRE(tolerance.size() == 1);
    CHECK(tolerance[0].bored);
}
