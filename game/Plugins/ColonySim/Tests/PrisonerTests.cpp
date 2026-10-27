#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Mood.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Prisoners.h"
#include "ColonySim/Raids.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"
#include "ColonySim/Trade.h"

#include <doctest.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define PRI_REQUIRE(...)       \
    do {                       \
        CHECK((__VA_ARGS__));  \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// The game's real data: the numbers in docs/design/prisoners.md are what is tested.
std::shared_ptr<const DefDatabase> priDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

Sim priSim() { return Sim(SimSettings{7, 64, 0, 0, true}, priDefs()); }

// A colonist who works all day; `warden` false takes the Warden work off so it is nobody's jailer.
Entity priColonist(Sim& sim, CellPos cell, bool warden = true) {
    const Entity pawn = sim.things().spawn("Human", cell);
    sim.jobs().addWorker(pawn);
    for (int h = 0; h < 24; ++h) {
        sim.submit(SetScheduleCommand{pawn, h, ScheduleKind::Work});
    }
    if (!warden) {
        sim.submit(SetWorkPriorityCommand{pawn, "Warden", 0});
    }
    return pawn;
}

// A pawn of another side, put down by bruises (they do not bleed).
Entity priEnemy(Sim& sim, CellPos cell, bool down = true) {
    const Entity pawn = sim.things().spawn("Human", cell);
    std::string error;
    sim.fighters().setFaction(pawn, 2, error);
    if (down) {
        Rng rng(5);
        const std::vector<std::string> labels = sim.health().partLabels(sim.things().registry().get<Thing>(pawn).def);
        for (const char* part : {"torso", "left leg", "right leg", "left arm"}) {
            const auto it = std::find(labels.begin(), labels.end(), part);
            sim.health().applyDamage(pawn, sim.health().damageTypeId("Blunt"), 20.0f, static_cast<int>(it - labels.begin()), rng);
        }
    }
    return pawn;
}

// The prisoner stands up again at once (its wounds are gone), so talks and release can start.
void priHeal(Sim& sim, Entity pawn) {
    Health& health = sim.things().registry().get<Health>(pawn);
    health.injuries.clear();
    health.dirty = true;
}

struct PriLog {
    std::vector<Event> events;
    void step(Sim& sim) {
        sim.step();
        events.insert(events.end(), sim.events().begin(), sim.events().end());
    }
    const Event* type(EventType type) const {
        const auto it = std::find_if(events.begin(), events.end(), [&](const Event& e) { return e.type == type; });
        return it == events.end() ? nullptr : &*it;
    }
    int count(EventType type) const {
        return static_cast<int>(std::count_if(events.begin(), events.end(), [&](const Event& e) { return e.type == type; }));
    }
};

bool priRunUntil(Sim& sim, PriLog& log, int64_t limit, const std::function<bool()>& done) {
    for (int64_t i = 0; i < limit; ++i) {
        log.step(sim);
        if (done()) {
            return true;
        }
    }
    return false;
}

CellPos priCell(const Sim& sim, Entity pawn) { return cellOf(sim.things().registry().get<Position>(pawn)); }

bool priAnyFailed(const PriLog& log) { return log.type(EventType::CommandFailed) != nullptr; }

// A walled cell round x 17..23, y 17..23 with a door at (20, 23): the inside is a closed room.
void priCell7(Sim& sim, bool door = true) {
    for (int y = 17; y <= 23; ++y) {
        for (int x = 17; x <= 23; ++x) {
            if (x == 17 || x == 23 || y == 17 || y == 23) {
                if (x == 20 && y == 23) {
                    if (door) {
                        sim.things().spawn("Door", CellPos{x, y, 0});
                    }
                } else {
                    sim.things().spawn("Wall", CellPos{x, y, 0});
                }
            }
        }
    }
    sim.rooms().update();
}

// A colony with a prison bed in a closed cell, a warden and a downed enemy; the capture has not happened yet.
struct PriCamp {
    Sim sim = priSim();
    Entity bed = kNoEntity;
    Entity warden = kNoEntity;
    Entity enemy = kNoEntity;
    PriCamp() {
        priCell7(sim);
        bed = sim.things().spawn("Bed", CellPos{20, 20, 0});
        sim.submit(SetPrisonBedCommand{bed, true});
        sim.things().spawn("MealSurvivalPack", CellPos{14, 14, 0}, 30);
        warden = priColonist(sim, CellPos{10, 10, 0});
        enemy = priEnemy(sim, CellPos{30, 30, 0});
        sim.step();
    }
    // Runs until the enemy is a prisoner; false if it never happened.
    bool capture(PriLog& log) {
        return priRunUntil(sim, log, 4000, [&] { return sim.prisoners().isPrisoner(enemy); });
    }
};

}  // namespace

TEST_CASE("Prisoners: the mode names, and the commands refuse what is no prisoner or no bed") {
    for (const PrisonerMode mode : {PrisonerMode::NoInteraction, PrisonerMode::ReduceResistance, PrisonerMode::AttemptRecruit,
                                    PrisonerMode::Release}) {
        PrisonerMode back = PrisonerMode::NoInteraction;
        PRI_REQUIRE(prisonerModeFromName(prisonerModeName(mode), back));
        CHECK(back == mode);
    }
    PrisonerMode ignored = PrisonerMode::NoInteraction;
    CHECK_FALSE(prisonerModeFromName("Torture", ignored));

    Sim sim = priSim();
    const Entity colonist = priColonist(sim, CellPos{10, 10, 0});
    const Entity table = sim.things().spawn("Steel", CellPos{12, 12, 0});
    sim.submit(SetPrisonerModeCommand{colonist, "Release"});
    sim.submit(SetPrisonerModeCommand{colonist, "Torture"});
    sim.submit(SetPrisonBedCommand{table, true});
    PriLog log;
    log.step(sim);
    CHECK(log.count(EventType::CommandFailed) == 3);
    CHECK_FALSE(sim.prisoners().isPrisoner(colonist));
}

TEST_CASE("Prisoners: a warden carries a downed enemy to the prison bed, where it becomes a prisoner") {
    PriCamp camp;
    Sim& sim = camp.sim;
    // The enemy is armed: its gun goes on the floor.
    const Entity gun = sim.things().spawn("Gun_AssaultRifle", CellPos{30, 31, 0});
    PRI_REQUIRE(sim.combat().equip(camp.enemy, gun));
    CHECK(sim.things().registry().get<Gear>(camp.enemy).weapon == gun);
    CHECK(sim.things().registry().all_of<Fighter>(camp.enemy));

    PriLog log;
    PRI_REQUIRE(camp.capture(log));
    const Entity enemy = camp.enemy;
    const entt::registry& reg = sim.things().registry();
    CHECK(log.type(EventType::PrisonerCaptured) != nullptr);
    CHECK(log.type(EventType::PrisonerCaptured)->thing == enemy);
    CHECK(log.type(EventType::PrisonerCaptured)->target == camp.warden);
    CHECK(priCell(sim, enemy) == CellPos{20, 20, 0});
    CHECK(sim.jobs().bedOf(enemy) == camp.bed);
    CHECK(sim.jobs().bedUnder(enemy) == camp.bed);
    CHECK(reg.get<Health>(enemy).rest == RestKind::Bed);
    CHECK(reg.get<Health>(enemy).downed);
    CHECK_FALSE(reg.all_of<Fighter>(enemy));  // it fights no more
    CHECK(reg.get<Faction>(enemy).id == 2);   // it keeps its own side
    CHECK(reg.get<Gear>(enemy).weapon == kNoEntity);
    CHECK(sim.things().spawned(gun));  // the rifle lies in the cell
    const Prisoner& prisoner = reg.get<Prisoner>(enemy);
    CHECK(prisoner.resistance >= 5.0f);
    CHECK(prisoner.resistance <= 9.0f);
    CHECK(prisoner.mode == PrisonerMode::ReduceResistance);
    CHECK(reg.all_of<Needs>(enemy));
    CHECK_FALSE(reg.all_of<WorkSettings>(enemy));
    CHECK(sim.prisoners().count() == 1);
    CHECK(log.type(EventType::PawnAction) != nullptr);
    CHECK_FALSE(priAnyFailed(log));

    // The snapshot says so.
    Snapshot snapshot;
    sim.writeSnapshot(snapshot);
    const auto view = std::find_if(snapshot.things.begin(), snapshot.things.end(), [&](const ThingView& t) { return t.id == enemy; });
    PRI_REQUIRE(view != snapshot.things.end());
    CHECK(view->prisoner);
    CHECK(view->resistance == doctest::Approx(prisoner.resistance));
    const auto bedView = std::find_if(snapshot.things.begin(), snapshot.things.end(), [&](const ThingView& t) { return t.id == camp.bed; });
    PRI_REQUIRE(bedView != snapshot.things.end());
    CHECK(bedView->prisonBed);
}

TEST_CASE("Prisoners: nothing is captured without a free prison bed, or while an armed enemy is close") {
    {
        Sim sim = priSim();
        sim.things().spawn("Bed", CellPos{20, 20, 0});  // an ordinary bed is not a prison
        priColonist(sim, CellPos{10, 10, 0});
        const Entity enemy = priEnemy(sim, CellPos{30, 30, 0});
        PriLog log;
        sim.step();
        for (int i = 0; i < 1500; ++i) {
            log.step(sim);
        }
        CHECK_FALSE(sim.prisoners().isPrisoner(enemy));
        CHECK(sim.prisoners().capturable(enemy));
    }
    {
        PriCamp camp;
        // Another enemy, on its feet, stands near the downed one.
        const Entity guard = priEnemy(camp.sim, CellPos{34, 30, 0}, false);
        PriLog log;
        for (int i = 0; i < 300; ++i) {
            log.step(camp.sim);
            if (camp.sim.things().registry().get<Health>(guard).downed || !camp.sim.things().spawned(guard)) {
                break;
            }
        }
        CHECK_FALSE(camp.sim.prisoners().isPrisoner(camp.enemy));
    }
}

TEST_CASE("Prisoners: colonists do not sleep in prison beds, and prisoners only in them") {
    PriCamp camp;
    Sim& sim = camp.sim;
    std::string error;
    CHECK_FALSE(sim.jobs().assignBed(camp.bed, camp.warden, error));
    CHECK_FALSE(error.empty());
    // A tired colonist with only a prison bed to go to sleeps on the ground.
    sim.things().registry().get<Needs>(camp.warden).rest = 0.05f;
    PriLog log;
    for (int i = 0; i < 200; ++i) {
        log.step(sim);
    }
    CHECK(sim.jobs().bedOf(camp.warden) == kNoEntity);
    // A bed with a colonist in it can't be marked, and one with a prisoner can't be unmarked.
    const Entity own = sim.things().spawn("Bed", CellPos{24, 20, 0});
    CHECK(sim.jobs().assignBed(own, camp.warden, error));
    sim.submit(SetPrisonBedCommand{own, true});
    log.step(sim);
    CHECK(log.count(EventType::CommandFailed) == 1);
    PRI_REQUIRE(camp.capture(log));
    sim.submit(SetPrisonBedCommand{camp.bed, false});
    log.step(sim);
    CHECK(log.count(EventType::CommandFailed) == 2);
    CHECK(sim.things().registry().all_of<PrisonBed>(camp.bed));
}

TEST_CASE("Prisoners: nobody shoots a prisoner") {
    PriCamp camp;
    Sim& sim = camp.sim;
    PriLog log;
    PRI_REQUIRE(camp.capture(log));
    priHeal(sim, camp.enemy);
    const Entity gunner = priColonist(sim, CellPos{21, 21, 0}, false);
    const Entity gun = sim.things().spawn("Gun_AssaultRifle", CellPos{21, 22, 0});
    sim.step();
    PRI_REQUIRE(sim.combat().equip(gunner, gun));
    sim.submit(DraftCommand{gunner, true});
    for (int i = 0; i < 600; ++i) {
        log.step(sim);
    }
    CHECK(sim.things().registry().all_of<Fighter>(gunner));
    CHECK(sim.things().registry().get<Fighter>(gunner).target != camp.enemy);
    CHECK(log.count(EventType::ShotFired) == 0);
    CHECK(sim.prisoners().isPrisoner(camp.enemy));
    // And the alarm does not count it as an enemy at the gates.
    CHECK_FALSE(sim.raids().state().alarm);
}

TEST_CASE("Prisoners: a hungry prisoner is fed by a warden") {
    PriCamp camp;
    Sim& sim = camp.sim;
    PriLog log;
    PRI_REQUIRE(camp.capture(log));
    priHeal(sim, camp.enemy);
    sim.things().registry().get<Needs>(camp.enemy).food = 0.1f;
    // It never goes looking for food itself: only the warden brings it.
    PRI_REQUIRE(priRunUntil(sim, log, 4000, [&] {
        return std::any_of(log.events.begin(), log.events.end(), [&](const Event& e) {
            return e.type == EventType::PawnAte && e.thing == camp.enemy;
        });
    }));
    const auto ate = std::find_if(log.events.begin(), log.events.end(), [&](const Event& e) {
        return e.type == EventType::PawnAte && e.thing == camp.enemy;
    });
    CHECK(ate->target == camp.warden);
    CHECK(ate->message == "fed");
    CHECK(sim.things().registry().get<Needs>(camp.enemy).food > 0.5f);
    CHECK(priCell(sim, camp.enemy) == CellPos{20, 20, 0});  // it did not leave its cell
    const auto fed = std::find_if(log.events.begin(), log.events.end(), [&](const Event& e) {
        return e.type == EventType::JobEnded && e.job == JobType::FeedPrisoner && e.thing == camp.warden;
    });
    CHECK(fed != log.events.end());
}

TEST_CASE("Prisoners: a prisoner left alone sleeps in its bed and stays in its cell") {
    PriCamp camp;
    Sim& sim = camp.sim;
    PriLog log;
    PRI_REQUIRE(camp.capture(log));
    priHeal(sim, camp.enemy);
    sim.submit(SetPrisonerModeCommand{camp.enemy, "NoInteraction"});
    sim.things().registry().get<Needs>(camp.enemy).rest = 0.1f;
    PRI_REQUIRE(priRunUntil(sim, log, 600, [&] { return jobIsLyingDown(sim.things().registry().get<Worker>(camp.enemy).job); }));
    CHECK(priCell(sim, camp.enemy) == CellPos{20, 20, 0});
    for (int i = 0; i < 3000; ++i) {
        log.step(sim);
        CHECK(priCell(sim, camp.enemy) == CellPos{20, 20, 0});
    }
    CHECK(sim.things().registry().get<Prisoner>(camp.enemy).resistance > 0.0f);  // nobody talked to it
}

TEST_CASE("Prisoners: wardens talk resistance down, at the interval, with a mood and a Social factor, then stop") {
    PriCamp camp;
    Sim& sim = camp.sim;
    PriLog log;
    PRI_REQUIRE(camp.capture(log));
    priHeal(sim, camp.enemy);
    sim.step();
    Prisoner& prisoner = sim.things().registry().get<Prisoner>(camp.enemy);
    prisoner.resistance = 3.0f;
    // The reduction is 1.0 x mood factor x the warden's negotiation (0.5 + 0.05 x Social, 8 without skills).
    const float reduction = sim.prisoners().resistanceReduction(camp.warden, camp.enemy);
    CHECK(reduction > 0.5f);
    CHECK(reduction < 1.5f);
    CHECK(sim.prisoners().resistanceReduction(camp.warden, camp.enemy) == doctest::Approx(reduction));

    PRI_REQUIRE(priRunUntil(sim, log, 4000, [&] { return log.count(EventType::PrisonerChatted) >= 1; }));
    const Event* chat = log.type(EventType::PrisonerChatted);
    CHECK(chat->thing == camp.warden);
    CHECK(chat->target == camp.enemy);
    CHECK(chat->message == "reduced");
    CHECK(chat->amount == doctest::Approx(3.0f - reduction).epsilon(0.3));
    CHECK(sim.things().registry().get<Prisoner>(camp.enemy).resistance < 3.0f);
    const int64_t first = sim.time().seconds;
    // The next talk comes six hours later, not before.
    PRI_REQUIRE(priRunUntil(sim, log, 30000, [&] { return log.count(EventType::PrisonerChatted) >= 2; }));
    CHECK(sim.time().seconds - first >= 21000);
    // The captive thoughts are there.
    bool captive = false;
    for (const ThoughtView& t : sim.mood().thoughts(camp.enemy, sim.time().seconds)) {
        captive = captive || t.def == "Captive";
    }
    CHECK(captive);
    // Run it down: at 0 a Reduce Resistance prisoner is left alone.
    sim.things().registry().get<Prisoner>(camp.enemy).resistance = 0.0f;
    CHECK_FALSE(sim.prisoners().chatDue(camp.enemy));
    const int chats = log.count(EventType::PrisonerChatted);
    for (int i = 0; i < 30000; ++i) {
        log.step(sim);
    }
    CHECK(log.count(EventType::PrisonerChatted) == chats);
    CHECK(sim.things().registry().get<Prisoner>(camp.enemy).resistance == 0.0f);
}

TEST_CASE("Prisoners: a better warden talks resistance down faster") {
    PriCamp camp;
    Sim& sim = camp.sim;
    PriLog log;
    PRI_REQUIRE(camp.capture(log));
    const Entity good = priColonist(sim, CellPos{12, 12, 0});
    sim.step();
    const auto withSocial = [&](Entity pawn, int level) {
        Colonist colonist;
        SkillRecord skill;
        skill.def = "Social";
        skill.level = level;
        colonist.skills.push_back(skill);
        sim.things().registry().emplace_or_replace<Colonist>(pawn, colonist);
    };
    withSocial(good, 16);
    withSocial(camp.warden, 2);
    CHECK(sim.prisoners().resistanceReduction(good, camp.enemy) > 2.0f * sim.prisoners().resistanceReduction(camp.warden, camp.enemy));
}

TEST_CASE("Prisoners: Attempt Recruit wears the resistance down, then recruits it into the colony") {
    PriCamp camp;
    Sim& sim = camp.sim;
    PriLog log;
    PRI_REQUIRE(camp.capture(log));
    priHeal(sim, camp.enemy);
    sim.submit(SetPrisonerModeCommand{camp.enemy, "AttemptRecruit"});
    sim.step();
    sim.things().registry().get<Prisoner>(camp.enemy).resistance = 1.0f;
    CHECK(sim.prisoners().recruitChance(camp.enemy) >= 0.05f);
    CHECK(sim.prisoners().recruitChance(camp.enemy) <= 0.95f);
    PRI_REQUIRE(priRunUntil(sim, log, 10 * 86400, [&] { return log.type(EventType::PrisonerRecruited) != nullptr; }));
    const entt::registry& reg = sim.things().registry();
    const Event* recruited = log.type(EventType::PrisonerRecruited);
    CHECK(recruited->thing == camp.enemy);
    CHECK(recruited->target == camp.warden);
    CHECK_FALSE(sim.prisoners().isPrisoner(camp.enemy));
    CHECK(sim.fighters().factionOf(camp.enemy) == kPlayerFaction);
    CHECK(reg.all_of<WorkSettings>(reg.valid(camp.enemy) ? camp.enemy : kNoEntity));
    CHECK(sim.jobs().bedOf(camp.enemy) == kNoEntity);
    CHECK(sim.prisoners().count() == 0);
    // At least one talk wore it down before the attempt.
    CHECK(log.count(EventType::PrisonerChatted) >= 1);
    // It is a colonist now: it works and may be drafted.
    sim.submit(DraftCommand{camp.enemy, true});
    log.step(sim);
    CHECK_FALSE(priAnyFailed(log));
}

TEST_CASE("Prisoners: a released prisoner walks off the map; one who can't walk can't be released") {
    PriCamp camp;
    Sim& sim = camp.sim;
    PriLog log;
    PRI_REQUIRE(camp.capture(log));
    sim.submit(SetPrisonerModeCommand{camp.enemy, "Release"});  // still downed
    log.step(sim);
    CHECK(priAnyFailed(log));
    CHECK(sim.things().registry().get<Prisoner>(camp.enemy).mode != PrisonerMode::Release);

    priHeal(sim, camp.enemy);
    sim.step();
    sim.submit(SetPrisonerModeCommand{camp.enemy, "Release"});
    PRI_REQUIRE(priRunUntil(sim, log, 6 * 3600, [&] { return log.type(EventType::PrisonerReleased) != nullptr; }));
    CHECK(log.type(EventType::PrisonerReleased)->thing == camp.enemy);
    CHECK_FALSE(sim.prisoners().isPrisoner(camp.enemy));
    CHECK(sim.jobs().bedOf(camp.enemy) == kNoEntity);
    CHECK(sim.things().registry().get<Fighter>(camp.enemy).mode == FightMode::Flee);
    PRI_REQUIRE(priRunUntil(sim, log, 4000, [&] { return log.type(EventType::PawnLeftMap) != nullptr; }));
    CHECK(log.type(EventType::PawnLeftMap)->thing == camp.enemy);
    CHECK_FALSE(sim.things().exists(camp.enemy));
}

TEST_CASE("Prisoners: breakOut frees every prisoner who can walk, for the prison break incident") {
    PriCamp camp;
    Sim& sim = camp.sim;
    const Entity bed2 = sim.things().spawn("Bed", CellPos{22, 20, 0});
    sim.submit(SetPrisonBedCommand{bed2, true});
    const Entity second = priEnemy(sim, CellPos{32, 30, 0});
    PriLog log;
    PRI_REQUIRE(priRunUntil(sim, log, 8000, [&] { return sim.prisoners().count() == 2; }));
    priHeal(sim, camp.enemy);  // only this one can walk
    sim.step();
    CHECK(sim.prisoners().breakOut("test") == 1);
    CHECK_FALSE(sim.prisoners().isPrisoner(camp.enemy));
    CHECK(sim.prisoners().isPrisoner(second));
    CHECK(sim.prisoners().count() == 1);
    bool broke = false;  // (the events of the step stand until the next one starts)
    for (const Event& e : sim.events()) {
        broke = broke || (e.type == EventType::PrisonBreak && e.count == 1 && e.message == "test");
    }
    CHECK(broke);
}

TEST_CASE("Prisoners: doctors tend a prisoner's wounds") {
    PriCamp camp;
    Sim& sim = camp.sim;
    PriLog log;
    PRI_REQUIRE(camp.capture(log));
    sim.things().spawn("MedicineIndustrial", CellPos{12, 12, 0}, 3);
    // The prisoner is bleeding from a cut.
    Rng rng(3);
    const std::vector<std::string> labels = sim.health().partLabels(sim.things().registry().get<Thing>(camp.enemy).def);
    const auto arm = std::find(labels.begin(), labels.end(), "right arm");
    sim.health().applyDamage(camp.enemy, sim.health().damageTypeId("Cut"), 6.0f, static_cast<int>(arm - labels.begin()), rng);
    PRI_REQUIRE(priRunUntil(sim, log, 4000, [&] {
        return std::any_of(log.events.begin(), log.events.end(), [&](const Event& e) {
            return e.type == EventType::PawnTended && e.target == camp.enemy;
        });
    }));
    CHECK(sim.prisoners().isPrisoner(camp.enemy));
    CHECK(priCell(sim, camp.enemy) == CellPos{20, 20, 0});
}

TEST_CASE("Prisoners: a prisoner, its mode and a prison bed save and load, and the loaded game goes on the same") {
    PriCamp camp;
    Sim& sim = camp.sim;
    PriLog log;
    PRI_REQUIRE(camp.capture(log));
    priHeal(sim, camp.enemy);
    sim.submit(SetPrisonerModeCommand{camp.enemy, "AttemptRecruit"});
    sim.things().registry().get<Needs>(camp.enemy).food = 0.1f;
    // Stop in the middle of feeding.
    PRI_REQUIRE(priRunUntil(sim, log, 4000, [&] {
        const Job& job = sim.things().registry().get<Worker>(camp.warden).job;
        return job.type == JobType::FeedPrisoner && job.step >= 2;
    }));
    const std::vector<uint8_t> bytes = saveSim(sim);
    SaveLoadResult loaded = loadSim(bytes, priDefs());
    if (!loaded.error.empty()) MESSAGE(loaded.error);
    PRI_REQUIRE(loaded.error.empty());
    PRI_REQUIRE(loaded.sim != nullptr);
    Sim& copy = *loaded.sim;
    CHECK(copy.stateHash() == sim.stateHash());
    CHECK(copy.prisoners().isPrisoner(camp.enemy));
    CHECK(copy.things().registry().get<Prisoner>(camp.enemy).mode == PrisonerMode::AttemptRecruit);
    CHECK(copy.things().registry().get<Prisoner>(camp.enemy).resistance == sim.things().registry().get<Prisoner>(camp.enemy).resistance);
    CHECK(copy.prisoners().isPrisonBed(camp.bed));
    CHECK(copy.things().registry().get<Worker>(camp.warden).job.type == JobType::FeedPrisoner);
    CHECK(saveSim(copy) == bytes);
    sim.stepMany(30000);
    copy.stepMany(30000);
    CHECK(copy.stateHash() == sim.stateHash());
    CHECK(saveSim(copy) == saveSim(sim));
}

TEST_CASE("Prisoners: the commands are written into a replay and read back") {
    SimSettings settings;
    settings.seed = 7;
    settings.mapSize = 128;
    settings.flatMap = true;
    settings.scenario = "Crashlanded";
    settings.checkpointEvery = 600;
    Sim recorded(settings, priDefs());
    Entity colonist = kNoEntity;
    for (const auto [pawn, c] : recorded.things().registry().view<const Colonist>().each()) {
        (void)c;
        colonist = colonist == kNoEntity || pawn < colonist ? pawn : colonist;
    }
    PRI_REQUIRE(colonist != kNoEntity);
    recorded.stepMany(5);
    recorded.submit(SetPrisonerModeCommand{colonist, "Release"});
    recorded.submit(SetPrisonBedCommand{colonist, true});
    recorded.stepMany(600);
    const Replay replay = recordReplay(recorded);
    const std::string text = writeReplay(replay);
    CHECK(text.find("SetPrisonerMode") != std::string::npos);
    CHECK(text.find("SetPrisonBed") != std::string::npos);
    Replay parsed;
    std::string error;
    PRI_REQUIRE(readReplay(text, parsed, error));
    Sim again(parsed.settings, priDefs());
    playReplay(parsed, again, replay.endStep);
    const ReplayCheck check = checkReplay(parsed, again);
    CHECK(check.matched());
    CHECK(again.stateHash() == recorded.stateHash());
}

TEST_CASE("Prisoners: a prison cell must be fully enclosed") {
    for (const int variant : {0, 1}) {  // 0: no walls at all, 1: walls with the door left out
        Sim sim = priSim();
        if (variant == 1) {
            priCell7(sim, false);
        }
        const Entity bed = sim.things().spawn("Bed", CellPos{20, 20, 0});
        sim.submit(SetPrisonBedCommand{bed, true});
        priColonist(sim, CellPos{10, 10, 0});
        const Entity enemy = priEnemy(sim, CellPos{30, 30, 0});
        sim.step();
        CHECK_FALSE(sim.prisoners().cellEnclosed(CellPos{20, 20, 0}));
        PriLog log;
        for (int i = 0; i < 1500; ++i) {
            log.step(sim);
        }
        CHECK_FALSE(sim.prisoners().isPrisoner(enemy));
    }
    // Close the cell (a door in the gap) and the same bed takes the prisoner.
    PriCamp camp;
    CHECK(camp.sim.prisoners().cellEnclosed(CellPos{20, 20, 0}));
    CHECK_FALSE(camp.sim.prisoners().cellEnclosed(CellPos{40, 40, 0}));
    PriLog log;
    CHECK(camp.capture(log));
}

TEST_CASE("Prisoners: a colonist is arrested, locked in the cell, and let out again") {
    PriCamp camp;
    Sim& sim = camp.sim;
    const Entity colonist = priColonist(sim, CellPos{12, 12, 0}, false);  // no warden
    PriLog log;
    CHECK(sim.prisoners().arrestable(colonist));
    sim.submit(ArrestCommand{colonist, true});
    sim.submit(ArrestCommand{kNoEntity, true});  // fails
    // The downed enemy is the warden's first job too; remove it from the picture.
    sim.things().destroy(camp.enemy);
    log.step(sim);
    CHECK(log.count(EventType::CommandFailed) == 1);
    PRI_REQUIRE(priRunUntil(sim, log, 4000, [&] { return sim.prisoners().isPrisoner(colonist); }));
    const entt::registry& reg = sim.things().registry();
    CHECK(priCell(sim, colonist) == CellPos{20, 20, 0});
    CHECK(sim.fighters().factionOf(colonist) == kPlayerFaction);
    CHECK_FALSE(reg.all_of<ArrestOrder>(colonist));
    CHECK_FALSE(reg.all_of<WorkSettings>(colonist));
    const Prisoner& prisoner = reg.get<Prisoner>(colonist);
    CHECK(prisoner.mode == PrisonerMode::NoInteraction);
    CHECK(prisoner.resistance == 0.0f);
    CHECK(sim.jobs().bedOf(colonist) == camp.bed);
    PRI_REQUIRE(log.type(EventType::PrisonerCaptured) != nullptr);
    CHECK(log.type(EventType::PrisonerCaptured)->message == "arrested");
    // It stays put, and its own people are not allowed to recruit it.
    sim.submit(SetPrisonerModeCommand{colonist, "AttemptRecruit"});
    log.step(sim);
    CHECK(log.count(EventType::CommandFailed) == 2);
    for (int i = 0; i < 3000; ++i) {
        log.step(sim);
    }
    CHECK(priCell(sim, colonist) == CellPos{20, 20, 0});
    CHECK(sim.prisoners().isPrisoner(colonist));
    // The player lets it out: a warden talks, it goes back to work and stays on the map.
    sim.submit(SetPrisonerModeCommand{colonist, "Release"});
    PRI_REQUIRE(priRunUntil(sim, log, 6000, [&] { return !sim.prisoners().isPrisoner(colonist); }));
    CHECK(log.type(EventType::PrisonerReleased) != nullptr);
    CHECK(reg.all_of<WorkSettings>(colonist));
    CHECK(sim.things().spawned(colonist));
    CHECK_FALSE(reg.all_of<Fighter>(colonist));
}

TEST_CASE("Prisoners: an arrest can be cancelled; the protagonist, raiders and other sides can't be arrested") {
    PriCamp camp;
    Sim& sim = camp.sim;
    sim.things().destroy(camp.enemy);
    const Entity colonist = priColonist(sim, CellPos{12, 12, 0}, false);
    PriLog log;
    sim.submit(ArrestCommand{colonist, true});
    log.step(sim);
    CHECK(sim.things().registry().all_of<ArrestOrder>(colonist));
    sim.submit(ArrestCommand{colonist, false});
    log.step(sim);
    CHECK_FALSE(sim.things().registry().all_of<ArrestOrder>(colonist));
    for (int i = 0; i < 1500; ++i) {
        log.step(sim);
    }
    CHECK_FALSE(sim.prisoners().isPrisoner(colonist));
    CHECK(sim.prisoners().count() == 0);
    // An enemy raider is captured only once it is down; a pawn of another side is never arrested.
    const Entity raider = priEnemy(sim, CellPos{40, 40, 0}, false);
    sim.things().registry().emplace<Raider>(raider);
    CHECK_FALSE(sim.prisoners().arrestable(raider));
    sim.submit(ArrestCommand{raider, true});
    log.step(sim);
    CHECK(log.count(EventType::CommandFailed) == 1);
    const Entity stranger = priEnemy(sim, CellPos{44, 40, 0}, false);
    CHECK_FALSE(sim.prisoners().arrestable(stranger));
    sim.things().registry().emplace_or_replace<Colonist>(colonist).protagonist = true;
    CHECK_FALSE(sim.prisoners().arrestable(colonist));
}

TEST_CASE("Prisoners: a visitor can be arrested; its faction turns hostile and the rest of its visit leaves") {
    PriCamp camp;
    Sim& sim = camp.sim;
    sim.things().destroy(camp.enemy);
    uint32_t id = 0;
    std::string error;
    PRI_REQUIRE(sim.trade().startVisit("FrayCompact", VisitKind::Visitors, "", -1, id, error));
    PRI_REQUIRE(sim.trade().find(id) != nullptr);
    const Entity visitor = sim.trade().find(id)->members.front();
    const size_t members = sim.trade().find(id)->members.size();
    CHECK(sim.prisoners().arrestable(visitor));
    CHECK_FALSE(sim.trade().hostile("FrayCompact"));
    PriLog log;
    sim.submit(ArrestCommand{visitor, true});
    PRI_REQUIRE(priRunUntil(sim, log, 8000, [&] { return sim.prisoners().isPrisoner(visitor); }));
    const entt::registry& reg = sim.things().registry();
    CHECK(priCell(sim, visitor) == CellPos{20, 20, 0});
    CHECK_FALSE(reg.all_of<Visitor>(visitor));
    CHECK(sim.trade().hostile("FrayCompact"));
    CHECK(reg.get<Prisoner>(visitor).resistance > 0.0f);  // not one of ours: it can be talked round and recruited
    CHECK(log.type(EventType::PrisonerCaptured)->message == "arrested");
    const Visit* visit = sim.trade().find(id);
    if (visit != nullptr) {
        CHECK(visit->members.size() == members - 1);
        CHECK(visit->phase == VisitPhase::Leaving);
    }
}

TEST_CASE("Prisoners: a visitor let go after it was seized walks off the map") {
    PriCamp camp;
    Sim& sim = camp.sim;
    sim.things().destroy(camp.enemy);
    uint32_t id = 0;
    std::string error;
    PRI_REQUIRE(sim.trade().startVisit("FrayCompact", VisitKind::Visitors, "", -1, id, error));
    const Entity visitor = sim.trade().find(id)->members.front();
    PriLog log;
    sim.submit(ArrestCommand{visitor, true});
    PRI_REQUIRE(priRunUntil(sim, log, 8000, [&] {
        const ArrestOrder* order = sim.things().registry().try_get<ArrestOrder>(visitor);
        return order != nullptr && order->by == camp.warden;
    }));
    CHECK(sim.trade().hostile("FrayCompact"));
    sim.submit(ArrestCommand{visitor, false});
    log.step(sim);
    const entt::registry& reg = sim.things().registry();
    CHECK_FALSE(reg.all_of<ArrestOrder>(visitor));
    CHECK_FALSE(reg.all_of<Visitor>(visitor));
    PRI_REQUIRE(reg.all_of<Fighter>(visitor));
    CHECK(reg.get<Fighter>(visitor).mode == FightMode::Flee);
    PRI_REQUIRE(priRunUntil(sim, log, 6000, [&] { return !sim.things().spawned(visitor) || !reg.valid(visitor); }));
    CHECK_FALSE(sim.prisoners().isPrisoner(visitor));
}

TEST_CASE("Prisoners: an arrested colonist can be executed, and an arrest in progress saves and loads") {
    PriCamp camp;
    Sim& sim = camp.sim;
    sim.things().destroy(camp.enemy);
    const Entity colonist = priColonist(sim, CellPos{12, 12, 0}, false);
    PriLog log;
    sim.submit(ArrestCommand{colonist, true});
    // Stop while the colonist is being carried.
    PRI_REQUIRE(priRunUntil(sim, log, 4000, [&] {
        const ArrestOrder* order = sim.things().registry().try_get<ArrestOrder>(colonist);
        return order != nullptr && order->by == camp.warden;
    }));
    const std::vector<uint8_t> bytes = saveSim(sim);
    SaveLoadResult loaded = loadSim(bytes, priDefs());
    if (!loaded.error.empty()) MESSAGE(loaded.error);
    PRI_REQUIRE(loaded.error.empty());
    PRI_REQUIRE(loaded.sim != nullptr);
    Sim& copy = *loaded.sim;
    CHECK(copy.stateHash() == sim.stateHash());
    sim.stepMany(3000);
    copy.stepMany(3000);
    CHECK(copy.stateHash() == sim.stateHash());
    PRI_REQUIRE(sim.prisoners().isPrisoner(colonist));
    CHECK(copy.prisoners().isPrisoner(colonist));

    // Execute mode works on one's own: an innocent prisoner, as main's executions have it.
    sim.things().registry().emplace_or_replace<Colonist>(camp.warden);  // so the warden's mood hears of it
    sim.submit(SetPrisonerModeCommand{colonist, "Execute"});
    PRI_REQUIRE(priRunUntil(sim, log, 4000, [&] { return log.type(EventType::PrisonerExecuted) != nullptr; }));
    CHECK(log.type(EventType::PrisonerExecuted)->message == "innocent");
    CHECK(sim.things().registry().get<Health>(colonist).dead);
    // The colony knows one of its own was put to death: "colonist executed", not the stranger's thought.
    const Mood* mood = sim.things().registry().try_get<Mood>(camp.warden);
    PRI_REQUIRE(mood != nullptr);
    const auto remembers = [&](const char* thought) {
        return std::any_of(mood->memories.begin(), mood->memories.end(), [&](const Memory& m) { return m.def == thought; });
    };
    CHECK(remembers("KnowColonistExecuted"));
    CHECK_FALSE(remembers("KnowPrisonerExecuted"));
}

TEST_CASE("Prisoners: an arrest is written into a replay and read back") {
    SimSettings settings;
    settings.seed = 7;
    settings.mapSize = 128;
    settings.flatMap = true;
    settings.scenario = "Crashlanded";
    settings.checkpointEvery = 600;
    Sim recorded(settings, priDefs());
    Entity colonist = kNoEntity;
    for (const auto [pawn, c] : recorded.things().registry().view<const Colonist>().each()) {
        if (!c.protagonist) {
            colonist = colonist == kNoEntity || pawn < colonist ? pawn : colonist;
        }
    }
    PRI_REQUIRE(colonist != kNoEntity);
    recorded.stepMany(5);
    recorded.submit(ArrestCommand{colonist, true});
    recorded.stepMany(5);
    recorded.submit(ArrestCommand{colonist, false});
    recorded.stepMany(600);
    const Replay replay = recordReplay(recorded);
    const std::string text = writeReplay(replay);
    CHECK(text.find("\"Arrest\"") != std::string::npos);
    Replay parsed;
    std::string error;
    PRI_REQUIRE(readReplay(text, parsed, error));
    Sim again(parsed.settings, priDefs());
    playReplay(parsed, again, replay.endStep);
    CHECK(checkReplay(parsed, again).matched());
    CHECK(again.stateHash() == recorded.stateHash());
}
