#include "doctest.h"

#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/MentalBreaks.h"
#include "ColonySim/Mood.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define BREAK_REQUIRE(...)     \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// The game's real data: the break defs, rules, thoughts, bodies and pawns the break system reads.
std::shared_ptr<const DefDatabase> breakDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        for (const std::string& e : errors) MESSAGE(e);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

SimSettings breakSettings() {
    SimSettings settings{5, 48, 0, 0, true};
    settings.checkpointEvery = 100;
    return settings;
}

Sim breakSim() { return Sim(breakSettings(), breakDefs()); }

// A colonist made by hand: a worker with needs, a mood after the first step, and a Colonist record.
Entity breakColonist(Sim& sim, CellPos cell) {
    const Entity pawn = sim.things().spawn("Human", cell);
    sim.jobs().addWorker(pawn);
    sim.things().registry().emplace<Colonist>(pawn);
    return pawn;
}

Mood& breakMood(Sim& sim, Entity pawn) { return sim.things().registry().get<Mood>(pawn); }

bool breakHasMemory(Sim& sim, Entity pawn, const std::string& def) {
    const Mood& mood = breakMood(sim, pawn);
    return std::any_of(mood.memories.begin(), mood.memories.end(), [&](const Memory& m) { return m.def == def; });
}

CellPos breakCellOf(Sim& sim, Entity pawn) { return cellOf(sim.things().registry().get<Position>(pawn)); }

struct BreakLog {
    std::vector<Event> events;
    int count(EventType type) const {
        return static_cast<int>(std::count_if(events.begin(), events.end(), [&](const Event& e) { return e.type == type; }));
    }
    const Event* first(EventType type) const {
        const auto it = std::find_if(events.begin(), events.end(), [&](const Event& e) { return e.type == type; });
        return it == events.end() ? nullptr : &*it;
    }
};

void breakRun(Sim& sim, BreakLog& log, int64_t steps) {
    for (int64_t i = 0; i < steps; ++i) {
        sim.step();
        log.events.insert(log.events.end(), sim.events().begin(), sim.events().end());
    }
}

// Steps until `done` says so, at most `limit` steps.
template <typename F>
bool breakRunUntil(Sim& sim, BreakLog& log, int64_t limit, F done) {
    for (int64_t i = 0; i < limit && !done(); ++i) {
        breakRun(sim, log, 1);
    }
    return done();
}

bool breakStart(Sim& sim, Entity pawn, const std::string& def, std::string* error = nullptr) {
    sim.submit(StartMentalBreakCommand{pawn, def});
    sim.step();
    for (const Event& e : sim.events()) {
        if (e.type == EventType::CommandFailed) {
            if (error != nullptr) {
                *error = e.message;
            }
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("Mental breaks: the rules come from the defs") {
    Sim sim = breakSim();
    const MentalBreakRules& rules = sim.mentalBreaks().rules();
    CHECK(rules.present);
    CHECK(rules.meanDays[0] == doctest::Approx(4.0));
    CHECK(rules.meanDays[1] == doctest::Approx(1.5));
    CHECK(rules.meanDays[2] == doctest::Approx(0.5));
    BREAK_REQUIRE(rules.curve.size() == 3);
    CHECK(rules.curve[0].second == doctest::Approx(0.1f));
    CHECK(rules.curve[2].first == doctest::Approx(1.0f));
    CHECK(rules.minSecondsBetween == 24 * 3600);
    CHECK(rules.checkEvery == 216);
    CHECK(rules.recoveryMargin == doctest::Approx(0.1f));
}

TEST_CASE("Mental breaks: a colonist in a calm mood never snaps") {
    Sim sim = breakSim();
    const Entity pawn = breakColonist(sim, CellPos{24, 24, 0});
    sim.step();
    BreakLog log;
    for (int i = 0; i < 3 * 86400; ++i) {
        breakMood(sim, pawn).level = 60.0f;
        sim.things().registry().get<Needs>(pawn).food = 1.0f;
        breakRun(sim, log, 1);
    }
    CHECK(log.count(EventType::PawnMentalBreak) == 0);
}

TEST_CASE("Mental breaks: under a threshold colonists snap at the band's own breaks, and wait a day between") {
    Sim sim = breakSim();
    std::vector<Entity> pawns;
    for (int i = 0; i < 3; ++i) {
        pawns.push_back(breakColonist(sim, CellPos{10 + 8 * i, 24, 0}));
    }
    sim.things().spawn("Stool", CellPos{24, 30, 0});  // something to smash
    sim.step();
    BreakLog log;
    // Held at a major-break mood (20 to 5 on the scale); calm again as soon as a break begins, so
    // each one ends the moment its time is up.
    const int64_t days = 12;
    for (int64_t i = 0; i < days * 86400; ++i) {
        for (const Entity pawn : pawns) {
            breakMood(sim, pawn).level = sim.mentalBreaks().active(pawn) ? 90.0f : 15.0f;
            sim.things().registry().get<Needs>(pawn).food = 1.0f;  // no food on this map: keep it alive
        }
        breakRun(sim, log, 1);
    }
    std::set<std::string> kinds;
    for (const Event& e : log.events) {
        if (e.type == EventType::PawnMentalBreak) {
            kinds.insert(e.message);
        }
    }
    CHECK(log.count(EventType::PawnMentalBreak) >= 4);
    const std::set<std::string> major = {"WanderSadLong", "Tantrum", "InsultingSpree"};
    for (const std::string& kind : kinds) {
        CHECK_MESSAGE(major.count(kind) == 1, kind);
    }
    CHECK(kinds.size() >= 2);
}

TEST_CASE("Mental breaks: a pawn waits a day after one break before the next") {
    Sim sim = breakSim();
    const Entity pawn = breakColonist(sim, CellPos{24, 24, 0});
    sim.step();
    std::vector<int64_t> starts, ends;
    for (int64_t i = 0; i < 20 * 86400; ++i) {
        // Mood 22 is a minor-break mood (under 35, over 20); calm while a break lasts.
        breakMood(sim, pawn).level = sim.mentalBreaks().active(pawn) ? 90.0f : 22.0f;
        sim.things().registry().get<Needs>(pawn).food = 1.0f;  // no food on this map: keep it alive
        sim.step();
        for (const Event& e : sim.events()) {
            if (e.type == EventType::PawnMentalBreak) {
                starts.push_back(sim.time().seconds);

                CHECK(e.message == "WanderSad");  // no food and no roof: the only minor break that can happen
            } else if (e.type == EventType::PawnMentalBreakEnded) {
                ends.push_back(sim.time().seconds);

                CHECK(e.message == "recovered");
            }
        }
    }
    BREAK_REQUIRE(starts.size() >= 3);
    for (size_t i = 1; i < starts.size() && i <= ends.size(); ++i) {
        CHECK(starts[i] - ends[i - 1] >= 24 * 3600);
    }
}

TEST_CASE("Mental breaks: sad wander walks about, takes no jobs, and ends with catharsis") {
    Sim sim = breakSim();
    const Entity pawn = breakColonist(sim, CellPos{24, 24, 0});
    sim.step();
    BreakLog log;
    CHECK(breakStart(sim, pawn, "WanderSad"));
    CHECK(sim.mentalBreaks().active(pawn));
    breakMood(sim, pawn).level = 90.0f;
    std::set<std::pair<int, int>> cells;
    bool jobbed = false;
    for (int i = 0; i < 2 * 3600; ++i) {
        breakRun(sim, log, 1);
        const CellPos here = breakCellOf(sim, pawn);
        cells.insert({here.x, here.y});
        jobbed = jobbed || sim.things().registry().get<Worker>(pawn).job.type != JobType::None;
        breakMood(sim, pawn).level = 90.0f;
    }
    CHECK(cells.size() > 10);
    CHECK_FALSE(jobbed);
    // The snapshot names the break while it lasts.
    Snapshot snapshot;
    sim.writeSnapshot(snapshot);
    const auto view = std::find_if(snapshot.things.begin(), snapshot.things.end(), [&](const ThingView& v) { return v.id == pawn; });
    BREAK_REQUIRE(view != snapshot.things.end());
    CHECK((breakDefs()->get(view->mentalBreak).defName() == "WanderSad") == sim.mentalBreaks().active(pawn));
    BREAK_REQUIRE(breakRunUntil(sim, log, 9 * 3600, [&] { return !sim.mentalBreaks().active(pawn); }));
    const Event* ended = log.first(EventType::PawnMentalBreakEnded);
    BREAK_REQUIRE(ended != nullptr);
    CHECK(ended->message == "recovered");
    CHECK(ended->amount >= 3.0f * 3600.0f);  // the minimum length of a sad wander
    CHECK(breakHasMemory(sim, pawn, "Catharsis"));
    // Back to work: it takes a job again.
    breakRun(sim, log, 5);
    CHECK(sim.things().registry().get<Worker>(pawn).job.type != JobType::None);
    CHECK_FALSE(sim.mentalBreaks().breakOf(pawn).valid());
}

TEST_CASE("Mental breaks: a break stays on until the mood is back above the threshold") {
    Sim sim = breakSim();
    const Entity pawn = breakColonist(sim, CellPos{24, 24, 0});
    sim.step();
    BreakLog log;
    CHECK(breakStart(sim, pawn, "WanderSad"));
    // Mood held at 30: under the minor threshold (35) and the 10 above it that ends a break.
    for (int i = 0; i < 9 * 3600 && sim.mentalBreaks().active(pawn); ++i) {
        breakMood(sim, pawn).level = 30.0f;
        breakRun(sim, log, 1);
    }
    // Its hard stop is twice its longest length (but at least 12 hours): 16 hours for a sad wander.
    CHECK(sim.mentalBreaks().active(pawn));
    for (int i = 0; i < 8 * 3600 && sim.mentalBreaks().active(pawn); ++i) {
        breakMood(sim, pawn).level = 30.0f;
        breakRun(sim, log, 1);
    }
    CHECK_FALSE(sim.mentalBreaks().active(pawn));
}

TEST_CASE("Mental breaks: hiding goes to the colonist's own bed, and needs somewhere to hide") {
    Sim sim = breakSim();
    const Entity pawn = breakColonist(sim, CellPos{10, 10, 0});
    sim.step();
    std::string error;
    CHECK_FALSE(breakStart(sim, pawn, "HideInRoom", &error));  // open ground, no bed: nowhere to hide
    CHECK(error.find("nothing for the colonist to do") != std::string::npos);
    const Entity bed = sim.things().spawn("Bed", CellPos{30, 30, 0});
    BREAK_REQUIRE(bed != kNoEntity);
    CHECK(sim.jobs().assignBed(bed, pawn, error));
    CHECK(breakStart(sim, pawn, "HideInRoom"));
    BreakLog log;
    breakMood(sim, pawn).level = 90.0f;
    breakRun(sim, log, 600);
    const CellRect bedRect = sim.things().registry().get<Footprint>(bed).rect();
    CHECK(bedRect.contains(breakCellOf(sim, pawn)));
    breakRun(sim, log, 600);
    CHECK(bedRect.contains(breakCellOf(sim, pawn)));  // it stays
}

TEST_CASE("Mental breaks: a food binge eats several meals and then stops; with no food it can't start") {
    Sim sim = breakSim();
    const Entity pawn = breakColonist(sim, CellPos{24, 24, 0});
    sim.step();
    std::string error;
    CHECK_FALSE(breakStart(sim, pawn, "FoodBinge", &error));
    CHECK(error.find("nothing for the colonist to do") != std::string::npos);
    const Entity meals = sim.things().spawn("MealSimple", CellPos{30, 24, 0}, 6);
    BREAK_REQUIRE(meals != kNoEntity);
    sim.things().registry().get<Needs>(pawn).food = 0.3f;
    CHECK(breakStart(sim, pawn, "FoodBinge"));
    BreakLog log;
    breakMood(sim, pawn).level = 90.0f;
    BREAK_REQUIRE(breakRunUntil(sim, log, 3600, [&] { return !sim.mentalBreaks().active(pawn); }));
    int binged = 0;
    for (const Event& e : log.events) {
        binged += e.type == EventType::PawnAte && e.message == "binge" ? 1 : 0;
    }
    CHECK(binged == sim.mentalBreaks().rules().bingeMeals);
    CHECK(sim.things().registry().get<StackCount>(meals).count == 6 - sim.mentalBreaks().rules().bingeMeals);
    CHECK(sim.things().registry().get<Needs>(pawn).food == doctest::Approx(1.0f));
    CHECK(log.first(EventType::PawnMentalBreakEnded)->message == "recovered");
}

TEST_CASE("Mental breaks: a tantrum smashes a few things, but not the walls") {
    Sim sim = breakSim();
    const Entity pawn = breakColonist(sim, CellPos{24, 24, 0});
    const Entity wall = sim.things().spawn("Wall", CellPos{24, 26, 0});
    std::vector<Entity> stools;
    for (const CellPos cell : {CellPos{28, 24, 0}, CellPos{24, 29, 0}, CellPos{19, 24, 0}, CellPos{24, 19, 0}}) {
        stools.push_back(sim.things().spawn("Stool", cell));
    }
    BREAK_REQUIRE(wall != kNoEntity);
    sim.step();
    CHECK(breakStart(sim, pawn, "Tantrum"));
    BreakLog log;
    breakMood(sim, pawn).level = 90.0f;
    BREAK_REQUIRE(breakRunUntil(sim, log, 3600, [&] { return !sim.mentalBreaks().active(pawn); }));
    int smashed = 0;
    for (const Entity stool : stools) {
        smashed += sim.things().exists(stool) ? 0 : 1;
    }
    CHECK(smashed == sim.mentalBreaks().rules().tantrumMaxSmashes);
    CHECK(sim.things().exists(wall));
    CHECK(log.count(EventType::ThingDestroyed) >= smashed);
    CHECK(std::count_if(log.events.begin(), log.events.end(), [](const Event& e) {
              return e.type == EventType::PawnAction && e.action == PawnAction::Punch;
          }) >= smashed * 5);
}

TEST_CASE("Mental breaks: an insulting spree insults three colonists in turn and they take it to heart") {
    Sim sim = breakSim();
    const Entity pawn = breakColonist(sim, CellPos{10, 10, 0});
    const std::vector<Entity> others = {breakColonist(sim, CellPos{30, 10, 0}), breakColonist(sim, CellPos{10, 30, 0}),
                                        breakColonist(sim, CellPos{30, 30, 0}), breakColonist(sim, CellPos{20, 20, 0})};
    sim.step();
    CHECK(breakStart(sim, pawn, "InsultingSpree"));
    BreakLog log;
    for (int i = 0; i < 3600 && sim.mentalBreaks().active(pawn); ++i) {
        breakMood(sim, pawn).level = 90.0f;
        breakRun(sim, log, 1);
    }
    CHECK_FALSE(sim.mentalBreaks().active(pawn));
    std::set<uint32_t> insulted;
    for (const Event& e : log.events) {
        if (e.type == EventType::PawnInsulted) {
            CHECK(e.thing == pawn);
            insulted.insert(entt::to_integral(e.target));
        }
    }
    CHECK(insulted.size() == 3);
    CHECK(insulted.count(entt::to_integral(pawn)) == 0);
    for (const Entity other : others) {
        CHECK(breakHasMemory(sim, other, "Insulted") == (insulted.count(entt::to_integral(other)) == 1));
    }
    CHECK(log.first(EventType::PawnMentalBreakEnded)->message == "recovered");
}

TEST_CASE("Mental breaks: berserk hurts the nearest pawn until it is ended") {
    Sim sim = breakSim();
    const Entity pawn = breakColonist(sim, CellPos{20, 24, 0});
    const Entity victim = breakColonist(sim, CellPos{28, 24, 0});
    sim.step();
    CHECK(breakStart(sim, pawn, "Berserk"));
    BreakLog log;
    breakMood(sim, pawn).level = 5.0f;  // stays in the break
    breakRun(sim, log, 120);
    CHECK(sim.mentalBreaks().active(pawn));
    CHECK_FALSE(sim.things().registry().get<Health>(victim).injuries.empty());
    CHECK(sim.things().registry().get<Health>(pawn).injuries.empty());
    CHECK(std::count_if(log.events.begin(), log.events.end(), [&](const Event& e) {
              return e.type == EventType::PawnAction && e.action == PawnAction::Punch && e.target == victim;
          }) > 0);
    // Ended by hand: no catharsis for that.
    sim.submit(EndMentalBreakCommand{pawn});
    breakRun(sim, log, 2);
    CHECK_FALSE(sim.mentalBreaks().active(pawn));
    CHECK(log.first(EventType::PawnMentalBreakEnded)->message == "stopped");
    CHECK_FALSE(breakHasMemory(sim, pawn, "Catharsis"));
    sim.submit(EndMentalBreakCommand{pawn});
    sim.step();
    CHECK(sim.events().size() == 1);  // CommandFailed: it isn't in a break any more
}

TEST_CASE("Mental breaks: giving up walks to the map edge and leaves for good") {
    Sim sim = breakSim();
    const Entity pawn = breakColonist(sim, CellPos{24, 24, 0});
    const Entity other = breakColonist(sim, CellPos{20, 20, 0});
    const Entity bed = sim.things().spawn("Bed", CellPos{30, 30, 0});
    std::string error;
    CHECK(sim.jobs().assignBed(bed, pawn, error));
    sim.step();
    CHECK(breakStart(sim, pawn, "GiveUp"));
    BreakLog log;
    BREAK_REQUIRE(breakRunUntil(sim, log, 4000, [&] { return !sim.things().exists(pawn); }));
    const Event* left = log.first(EventType::PawnLeftColony);
    BREAK_REQUIRE(left != nullptr);
    CHECK(left->thing == pawn);
    CHECK(left->message == "GiveUp");
    // It left from the edge of the map.
    const CellPos at = left->cell;
    CHECK((at.x <= 1 || at.y <= 1 || at.x >= 46 || at.y >= 46));
    CHECK(sim.things().exists(other));
    CHECK(sim.things().registry().get<Bed>(bed).owners.empty());
    CHECK(sim.mentalBreaks().active(pawn) == false);
}

TEST_CASE("Mental breaks: fire-starting needs fire, which the game doesn't have yet") {
    Sim sim = breakSim();
    const Entity pawn = breakColonist(sim, CellPos{24, 24, 0});
    sim.step();
    std::string error;
    CHECK_FALSE(breakStart(sim, pawn, "FireSpree", &error));
    CHECK(error.find("needs fire") != std::string::npos);
    sim.mentalBreaks().provide("fire");
    CHECK(breakStart(sim, pawn, "FireSpree"));
    BreakLog log;
    breakMood(sim, pawn).level = 90.0f;
    breakRun(sim, log, 200);
    CHECK(sim.mentalBreaks().active(pawn));  // E4 gives it something to do; until then it waits it out
    CHECK_FALSE(breakStart(sim, pawn, "NoSuchBreak", &error));
}

TEST_CASE("Mental breaks: a colonist in a break takes no orders, and a drafted one is undrafted") {
    Sim sim = breakSim();
    const Entity pawn = breakColonist(sim, CellPos{24, 24, 0});
    sim.things().registry().emplace<Posture>(pawn);
    sim.step();
    sim.submit(DraftCommand{pawn, true});
    sim.step();
    BREAK_REQUIRE(sim.things().registry().all_of<Fighter>(pawn));
    CHECK(breakStart(sim, pawn, "WanderSad"));
    CHECK_FALSE(sim.things().registry().all_of<Fighter>(pawn));
    for (int i = 0; i < 3; ++i) {
        switch (i) {
            case 0: sim.submit(DraftCommand{pawn, true}); break;
            case 1: sim.submit(MoveOrderCommand{pawn, CellPos{5, 5, 0}}); break;
            default: sim.submit(ControlPawnCommand{pawn}); break;
        }
        sim.step();
        BREAK_REQUIRE(sim.events().size() == 1);
        CHECK(sim.events()[0].type == EventType::CommandFailed);
        CHECK(sim.events()[0].message.find("mental break") != std::string::npos);
    }
    // The player's own colonist isn't broken either.
    sim.submit(EndMentalBreakCommand{pawn});
    sim.step();
    sim.submit(ControlPawnCommand{pawn});
    sim.step();
    std::string error;
    CHECK_FALSE(breakStart(sim, pawn, "WanderSad", &error));
    CHECK(error.find("walking that colonist") != std::string::npos);
}

TEST_CASE("Mental breaks: a break in progress saves and loads, and the loaded game goes on the same") {
    Sim sim = breakSim();
    const Entity pawn = breakColonist(sim, CellPos{24, 24, 0});
    const Entity other = breakColonist(sim, CellPos{12, 12, 0});
    sim.things().spawn("Stool", CellPos{34, 24, 0});
    sim.step();
    CHECK(breakStart(sim, pawn, "Tantrum"));
    CHECK(breakStart(sim, other, "WanderSad"));
    BreakLog log;
    breakRun(sim, log, 25);  // part-way: a path may be queued, a blow not yet struck
    const std::vector<uint8_t> bytes = saveSim(sim);
    SaveLoadResult loaded = loadSim(bytes, breakDefs());
    BREAK_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.error.empty());
    CHECK(loaded.sim->stateHash() == sim.stateHash());
    CHECK(loaded.sim->mentalBreaks().active(pawn));
    CHECK(loaded.sim->mentalBreaks().active(other));
    BreakLog a, b;
    breakRun(sim, a, 3000);
    breakRun(*loaded.sim, b, 3000);
    CHECK(loaded.sim->stateHash() == sim.stateHash());
    CHECK(a.events.size() == b.events.size());
    // The readable dump shows the section.
    const std::string json = saveToJson(sim);
    CHECK(json.find("mentalBreaks") != std::string::npos);
    CHECK(json.find("mentalBreaker") != std::string::npos);
}

TEST_CASE("Mental breaks: the break commands replay to the same game") {
    Sim sim = breakSim();
    sim.submit(SpawnThingCommand{"Human", CellPos{24, 24, 0}, 1, Rot4::North});
    sim.submit(SpawnThingCommand{"Human", CellPos{30, 30, 0}, 1, Rot4::North});
    sim.submit(SpawnThingCommand{"MealSimple", CellPos{20, 20, 0}, 5, Rot4::North});
    sim.stepMany(10);
    // The pawns are the first two things in a fresh sim.
    Entity first = kNoEntity;
    for (const auto [e, thing] : sim.things().registry().view<Thing>().each()) {
        if (thing.category == ThingCategory::Pawn && first == kNoEntity) {
            first = e;
        }
    }
    BREAK_REQUIRE(first != kNoEntity);
    sim.submit(StartMentalBreakCommand{first, "FoodBinge"});
    sim.stepMany(600);
    sim.submit(EndMentalBreakCommand{first});
    sim.stepMany(100);
    const Replay recorded = recordReplay(sim);
    const std::string text = writeReplay(recorded);
    CHECK(text.find("StartMentalBreak") != std::string::npos);
    CHECK(text.find("EndMentalBreak") != std::string::npos);
    Replay loaded;
    std::string error;
    BREAK_REQUIRE(readReplay(text, loaded, error));
    Sim replayed(loaded.settings, breakDefs());
    playReplay(loaded, replayed, loaded.endStep);
    CHECK(replayed.stateHash() == sim.stateHash());
    CHECK(checkReplay(loaded, replayed).matched());
}
