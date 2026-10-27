// Executing prisoners and harvesting organs: the work, the deaths, and what the colony feels (docs/design/prisoners.md).
#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Mood.h"
#include "ColonySim/Prisoners.h"
#include "ColonySim/Random.h"
#include "ColonySim/Raids.h"
#include "ColonySim/Save.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Trade.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

#define EXEC_REQUIRE(...)                \
    do {                                  \
        const bool execOk = (__VA_ARGS__); \
        CHECK(execOk);                    \
        if (!execOk) {                    \
            return;                       \
        }                                 \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> execDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

// A colony with a warden, three onlookers (plain, psychopath, bloodlust, cannibal) and one prisoner in a prison bed.
struct ExecFixture {
    Sim sim;
    Entity warden = kNoEntity;
    Entity prisoner = kNoEntity;
    std::vector<Event> events;

    ExecFixture() : sim(SimSettings{7, 64, 0, 0, true}, execDefs()) {
        sim.things().spawn("MealSurvivalPack", CellPos{14, 14, 0}, 60);  // so nobody starves in the day-long test
        warden = colonist(CellPos{10, 10, 0});
        // A prison cell must be a closed room: walls round x 17..23, y 17..23, a door at (20, 23).
        for (int y = 17; y <= 23; ++y) {
            for (int x = 17; x <= 23; ++x) {
                if (x == 17 || x == 23 || y == 17 || y == 23) {
                    sim.things().spawn(x == 20 && y == 23 ? "Door" : "Wall", CellPos{x, y, 0});
                }
            }
        }
        sim.rooms().update();
        const Entity bed = sim.things().spawn("Bed", CellPos{20, 20, 0});
        sim.submit(SetPrisonBedCommand{bed, true});
        prisoner = sim.things().spawn("Human", CellPos{30, 30, 0});
        std::string error;
        sim.fighters().setFaction(prisoner, 2, error);
        // Down it with bruises (they do not bleed).
        Rng rng(5);
        const std::vector<std::string> labels = sim.health().partLabels(sim.things().registry().get<Thing>(prisoner).def);
        for (const char* part : {"torso", "left leg", "right leg", "left arm"}) {
            const auto it = std::find(labels.begin(), labels.end(), part);
            sim.health().applyDamage(prisoner, sim.health().damageTypeId("Blunt"), 20.0f, static_cast<int>(it - labels.begin()), rng);
        }
        // A raider who was caught: it belongs to a raid squad, so it harmed the colony. The warden carries it to the bed.
        sim.things().registry().emplace<Raider>(prisoner);
        for (int i = 0; i < 4000 && !sim.prisoners().isPrisoner(prisoner); ++i) {
            sim.step();
        }
        CHECK(sim.prisoners().isPrisoner(prisoner));
        sim.stepMany(1);
    }
    entt::registry& reg() { return sim.things().registry(); }
    Entity colonist(CellPos cell, const char* trait = nullptr) {
        const Entity pawn = sim.things().spawn("Human", cell);
        // Mood needs a Colonist (traits live there); the Work tab row is made by addWorker.
        Colonist c;
        if (trait != nullptr) {
            c.traits.push_back(TraitRecord{trait, 0});
        }
        reg().emplace<Colonist>(pawn, c);
        sim.jobs().addWorker(pawn);
        sim.submit(SetWorkPriorityCommand{pawn, "Warden", pawn == warden || warden == kNoEntity ? 1 : 0});
        for (int h = 0; h < 24; ++h) {
            sim.submit(SetScheduleCommand{pawn, h, ScheduleKind::Work});
        }
        return pawn;
    }
    void run(int64_t seconds) {
        for (int64_t i = 0; i < seconds; ++i) {
            sim.step();
            events.insert(events.end(), sim.events().begin(), sim.events().end());
        }
    }
    const Event* event(EventType type) const {
        const auto it = std::find_if(events.begin(), events.end(), [&](const Event& e) { return e.type == type; });
        return it == events.end() ? nullptr : &*it;
    }
    bool remembers(Entity pawn, const char* thought) {
        const Mood* mood = reg().try_get<Mood>(pawn);
        return mood != nullptr && std::any_of(mood->memories.begin(), mood->memories.end(), [&](const Memory& m) { return m.def == thought; });
    }
    float offset(Entity pawn, const char* thought) {
        for (const ThoughtView& t : sim.mood().thoughts(pawn, sim.time().seconds)) {
            if (t.def == thought) {
                return t.offset;
            }
        }
        return 0.0f;
    }
};

}  // namespace

TEST_CASE("Execution: the mode is set by command, and a warden executes the prisoner") {
    ExecFixture f;
    f.sim.submit(SetPrisonerModeCommand{f.prisoner, "Execute"});
    f.run(1);
    EXEC_REQUIRE(f.reg().get<Prisoner>(f.prisoner).mode == PrisonerMode::Execute);
    CHECK(f.event(EventType::CommandFailed) == nullptr);
    f.run(600);
    const Event* executed = f.event(EventType::PrisonerExecuted);
    EXEC_REQUIRE(executed != nullptr);
    CHECK(executed->thing == f.prisoner);
    CHECK(executed->target == f.warden);
    CHECK(f.reg().get<Health>(f.prisoner).dead);
    CHECK(f.reg().get<Health>(f.prisoner).cause == DeathCause::Executed);
    CHECK_FALSE(f.sim.prisoners().isPrisoner(f.prisoner));
    CHECK(f.sim.prisoners().count() == 0);
    CHECK(f.event(EventType::PawnDied) != nullptr);
    CHECK(std::string(deathCauseName(DeathCause::Executed)) == "executed");
}

TEST_CASE("Execution: the cut takes the work time, not an instant") {
    ExecFixture f;
    f.sim.submit(SetPrisonerModeCommand{f.prisoner, "Execute"});
    f.run(5);
    CHECK_FALSE(f.reg().get<Health>(f.prisoner).dead);
    f.run(600);
    CHECK(f.reg().get<Health>(f.prisoner).dead);
}

TEST_CASE("Execution: a raider just caught is guilty, one held for a day is not") {
    {
        ExecFixture f;
        CHECK(f.sim.prisoners().guilty(f.prisoner));
        f.sim.submit(SetPrisonerModeCommand{f.prisoner, "Execute"});
        f.run(600);
        const Event* executed = f.event(EventType::PrisonerExecuted);
        EXEC_REQUIRE(executed != nullptr);
        CHECK(executed->message == "guilty");
    }
    {
        ExecFixture f;
        f.sim.stepMany(25 * kSecondsPerHour);
        CHECK_FALSE(f.sim.prisoners().guilty(f.prisoner));
        f.sim.submit(SetPrisonerModeCommand{f.prisoner, "Execute"});
        // The warden may be asleep at this hour: give it until it wakes.
        for (int i = 0; i < 12 * 3600 && f.event(EventType::PrisonerExecuted) == nullptr; i += 60) {
            f.run(60);
        }
        const Event* executed = f.event(EventType::PrisonerExecuted);
        EXEC_REQUIRE(executed != nullptr);
        CHECK(executed->message == "innocent");
    }
}

TEST_CASE("Execution: the colony's moods, with the cold-hearted, the bloodthirsty and cannibals shrugging") {
    ExecFixture f;
    const Entity plain = f.colonist(CellPos{8, 8, 0});
    const Entity psycho = f.colonist(CellPos{8, 9, 0}, "Psychopath");
    const Entity bloody = f.colonist(CellPos{8, 10, 0}, "Bloodlust");
    const Entity cannibal = f.colonist(CellPos{8, 11, 0}, "Cannibal");
    f.sim.submit(SetPrisonerModeCommand{f.prisoner, "Execute"});
    f.run(600);
    EXEC_REQUIRE(f.event(EventType::PrisonerExecuted) != nullptr);
    // It was guilty: -2.
    CHECK(f.offset(plain, "KnowPrisonerExecutedGuilty") == doctest::Approx(-2.0f));
    CHECK(f.offset(f.warden, "KnowPrisonerExecutedGuilty") == doctest::Approx(-2.0f));
    for (const Entity cold : {psycho, bloody, cannibal}) {
        CHECK_FALSE(f.remembers(cold, "KnowPrisonerExecutedGuilty"));
        CHECK_FALSE(f.remembers(cold, "KnowPrisonerExecuted"));
    }
    CHECK_FALSE(f.remembers(plain, "KnowPrisonerExecuted"));
}

TEST_CASE("Execution: an innocent prisoner costs -5") {
    ExecFixture f;
    f.reg().get<Prisoner>(f.prisoner).guiltyUntil = 0;
    const Entity plain = f.colonist(CellPos{8, 8, 0});
    f.sim.submit(SetPrisonerModeCommand{f.prisoner, "Execute"});
    f.run(600);
    EXEC_REQUIRE(f.event(EventType::PrisonerExecuted) != nullptr);
    CHECK(f.offset(plain, "KnowPrisonerExecuted") == doctest::Approx(-5.0f));
    CHECK_FALSE(f.remembers(plain, "KnowPrisonerExecutedGuilty"));
}

TEST_CASE("Execution: the prisoner's faction loses 10 goodwill") {
    ExecFixture f;
    std::string error;
    EXEC_REQUIRE(f.sim.fighters().setFaction(f.prisoner, f.sim.raids().factionId("FrayCompact"), error));
    EXEC_REQUIRE(f.sim.trade().adjustGoodwill("FrayCompact", -f.sim.trade().goodwill("FrayCompact"), error));
    EXEC_REQUIRE(f.sim.prisoners().execute(f.prisoner, f.warden));
    CHECK(f.sim.trade().goodwill("FrayCompact") == doctest::Approx(-10.0f));
}

TEST_CASE("Execution: switching the mode away before the cut saves the prisoner") {
    ExecFixture f;
    f.sim.submit(SetPrisonerModeCommand{f.prisoner, "Execute"});
    f.run(10);
    f.sim.submit(SetPrisonerModeCommand{f.prisoner, "NoInteraction"});
    f.run(600);
    CHECK(f.event(EventType::PrisonerExecuted) == nullptr);
    CHECK_FALSE(f.reg().get<Health>(f.prisoner).dead);
}

TEST_CASE("Organ harvesting: the event, the victim's scar and what the others feel") {
    ExecFixture f;
    const Entity plain = f.colonist(CellPos{8, 8, 0});
    const Entity psycho = f.colonist(CellPos{8, 9, 0}, "Psychopath");
    const Entity bloody = f.colonist(CellPos{8, 10, 0}, "Bloodlust");
    const Entity cannibal = f.colonist(CellPos{8, 11, 0}, "Cannibal");
    const Entity victim = f.colonist(CellPos{8, 12, 0});

    // A prisoner's organ: strangers' -6, the cannibal and psychopath shrug, the bloodthirsty enjoy it.
    // The surgery calls noteOrganHarvested during a step; here the event is handed to the mood system by hand.
    std::vector<Event> log;
    f.sim.prisoners().setEventLog(&log);
    f.sim.prisoners().noteOrganHarvested(f.prisoner, f.warden, 3);
    EXEC_REQUIRE(log.size() == 1);
    CHECK(log[0].type == EventType::OrganHarvested);
    CHECK(log[0].message == "prisoner");
    CHECK(log[0].part == 3);
    f.sim.mood().step(f.sim.time().seconds, log);
    CHECK(f.offset(plain, "KnowGuestOrganHarvested") == doctest::Approx(-6.0f));
    CHECK(f.offset(f.warden, "KnowGuestOrganHarvested") == doctest::Approx(-6.0f));
    CHECK_FALSE(f.remembers(psycho, "KnowGuestOrganHarvested"));
    CHECK_FALSE(f.remembers(cannibal, "KnowGuestOrganHarvested"));
    CHECK_FALSE(f.remembers(bloody, "KnowGuestOrganHarvested"));
    CHECK(f.offset(bloody, "BloodlustOrganHarvested") == doctest::Approx(4.0f));

    // A colonist's organ: -30 for the victim, -6 for the others (cannibals too), none for the cold-hearted.
    log.clear();
    f.sim.prisoners().noteOrganHarvested(victim, f.warden, 3);
    EXEC_REQUIRE(log.size() == 1);
    CHECK(log[0].message == "colonist");
    f.sim.mood().step(f.sim.time().seconds, log);
    CHECK(f.offset(victim, "MyOrganHarvested") == doctest::Approx(-30.0f));
    CHECK_FALSE(f.remembers(victim, "KnowColonistOrganHarvested"));
    CHECK(f.offset(plain, "KnowColonistOrganHarvested") == doctest::Approx(-6.0f));
    CHECK(f.offset(cannibal, "KnowColonistOrganHarvested") == doctest::Approx(-6.0f));
    CHECK_FALSE(f.remembers(psycho, "KnowColonistOrganHarvested"));
}

TEST_CASE("Execution: the guilt time saves and loads, and the loaded game goes on the same") {
    ExecFixture f;
    f.sim.submit(SetPrisonerModeCommand{f.prisoner, "Execute"});
    f.sim.stepMany(3);
    const std::vector<uint8_t> bytes = saveSim(f.sim);
    SaveLoadResult loadedGame = loadSim(bytes, execDefs());
    EXEC_REQUIRE(loadedGame.error.empty());
    EXEC_REQUIRE(loadedGame.sim != nullptr);
    std::unique_ptr<Sim> loaded = std::move(loadedGame.sim);
    const Prisoner& was = f.reg().get<Prisoner>(f.prisoner);
    const Prisoner& now = loaded->things().registry().get<Prisoner>(f.prisoner);
    CHECK(now.mode == PrisonerMode::Execute);
    CHECK(now.guiltyUntil == was.guiltyUntil);
    CHECK(now.guiltyUntil > 0);
    for (int i = 0; i < 700; ++i) {
        f.sim.step();
        loaded->step();
    }
    CHECK(f.sim.stateHash() == loaded->stateHash());
    CHECK(loaded->things().registry().get<Health>(f.prisoner).dead);
}
