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
#define SLV_REQUIRE(...)       \
    do {                       \
        CHECK((__VA_ARGS__));  \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// The game's real data: the numbers in docs/design/prisoners.md are what is tested.
std::shared_ptr<const DefDatabase> slvDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

Sim slvSim() { return Sim(SimSettings{7, 64, 0, 0, true}, slvDefs()); }

// A colonist who works all day; `warden` false takes the Warden work off so it is nobody's jailer.
Entity slvColonist(Sim& sim, CellPos cell, bool warden = true) {
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
Entity slvEnemy(Sim& sim, CellPos cell, bool down = true) {
    const Entity pawn = sim.things().spawn("Human", cell);
    std::string error;
    sim.fighters().setFaction(pawn, 2, error);
    if (down) {
        Rng rng(5);
        const std::vector<std::string> labels = sim.health().partLabels(sim.things().registry().get<Thing>(pawn).def);
        for (const char* part : {"torso", "left leg", "right leg", "left arm"}) {
            const auto it = std::find(labels.begin(), labels.end(), part);
            sim.health().applyDamage(pawn, sim.health().damageTypeId("Blunt"), std::string(part) == "left leg" ? 60.0f : 20.0f,
                                     static_cast<int>(it - labels.begin()), rng);
        }
    }
    return pawn;
}

// The prisoner stands up again at once (its wounds are gone), so talks and release can start.
void slvHeal(Sim& sim, Entity pawn) {
    Health& health = sim.things().registry().get<Health>(pawn);
    health.injuries.clear();
    health.dirty = true;
}

struct SlvLog {
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

bool slvRunUntil(Sim& sim, SlvLog& log, int64_t limit, const std::function<bool()>& done) {
    for (int64_t i = 0; i < limit; ++i) {
        log.step(sim);
        if (done()) {
            return true;
        }
    }
    return false;
}


bool slvAnyFailed(const SlvLog& log) { return log.type(EventType::CommandFailed) != nullptr; }

// A walled cell round x 17..23, y 17..23 with a door at (20, 23): the inside is a closed room.
void slvCellRoom(Sim& sim, bool door = true) {
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
struct SlvCamp {
    Sim sim = slvSim();
    Entity bed = kNoEntity;
    Entity warden = kNoEntity;
    Entity enemy = kNoEntity;
    SlvCamp() {
        slvCellRoom(sim);
        bed = sim.things().spawn("Bed", CellPos{20, 20, 0});
        sim.submit(SetPrisonBedCommand{bed, true});
        sim.things().spawn("MealSurvivalPack", CellPos{14, 14, 0}, 30);
        warden = slvColonist(sim, CellPos{10, 10, 0});
        enemy = slvEnemy(sim, CellPos{30, 30, 0});
        sim.step();
    }
    // Runs until the enemy is a prisoner; false if it never happened.
    bool capture(SlvLog& log) {
        return slvRunUntil(sim, log, 4000, [&] { return sim.prisoners().isPrisoner(enemy); });
    }
};

}  // namespace

TEST_CASE("Slaves: the new prisoner modes have names, and a captured prisoner has a will") {
    for (const PrisonerMode mode : {PrisonerMode::Enslave, PrisonerMode::ReduceWill}) {
        PrisonerMode back = PrisonerMode::NoInteraction;
        SLV_REQUIRE(prisonerModeFromName(prisonerModeName(mode), back));
        CHECK(back == mode);
    }
    SlvCamp camp;
    Sim& sim = camp.sim;
    SlvLog log;
    SLV_REQUIRE(camp.capture(log));
    const Prisoner& p = sim.things().registry().get<Prisoner>(camp.enemy);
    CHECK(p.will >= 1.0f);
    CHECK(p.will <= 3.0f);
    CHECK(p.will < p.resistance);  // the wiki: will starts much lower than resistance
}

TEST_CASE("Slaves: Enslave wears the will down, then makes the prisoner a slave who works within the slave limits") {
    SlvCamp camp;
    Sim& sim = camp.sim;
    SlvLog log;
    SLV_REQUIRE(camp.capture(log));
    slvHeal(sim, camp.enemy);
    sim.submit(SetPrisonerModeCommand{camp.enemy, "Enslave"});
    SLV_REQUIRE(slvRunUntil(sim, log, 10 * 86400, [&] { return log.type(EventType::PrisonerEnslaved) != nullptr; }));
    const entt::registry& reg = sim.things().registry();
    const Entity slave = camp.enemy;
    CHECK(log.type(EventType::PrisonerEnslaved)->target == camp.warden);
    CHECK(log.count(EventType::PrisonerChatted) >= 1);  // the will was worn down first
    CHECK_FALSE(sim.prisoners().isPrisoner(slave));
    CHECK(sim.prisoners().isSlave(slave));
    CHECK(sim.prisoners().slaveCount() == 1);
    CHECK(sim.fighters().factionOf(slave) == kPlayerFaction);
    CHECK(reg.get<Slave>(slave).homeFaction == 2);
    CHECK(reg.get<Slave>(slave).suppression == doctest::Approx(0.5f).epsilon(0.05));
    CHECK(sim.jobs().bedOf(slave) == kNoEntity);
    // It works, but not as a warden, hunter, artist or researcher, and the player can't turn those on.
    SLV_REQUIRE(reg.all_of<WorkSettings>(slave));
    sim.submit(SetWorkPriorityCommand{slave, "Warden", 1});
    sim.submit(SetWorkPriorityCommand{slave, "Research", 1});
    sim.submit(SetWorkPriorityCommand{slave, "Hauling", 1});
    sim.submit(DraftCommand{slave, true});
    SlvLog orders;
    orders.step(sim);
    CHECK(orders.count(EventType::CommandFailed) == 3);  // warden, research and the draft
    CHECK(sim.prisoners().workSpeedFactor(slave) == doctest::Approx(0.85f));
    CHECK(sim.prisoners().workSpeedFactor(camp.warden) == doctest::Approx(1.0f));

    Snapshot snapshot;
    sim.writeSnapshot(snapshot);
    const auto view = std::find_if(snapshot.things.begin(), snapshot.things.end(), [&](const ThingView& t) { return t.id == slave; });
    SLV_REQUIRE(view != snapshot.things.end());
    CHECK(view->slave);
    CHECK(view->suppression == doctest::Approx(reg.get<Slave>(slave).suppression));
}

TEST_CASE("Slaves: Reduce Will stops at 0 and never enslaves") {
    SlvCamp camp;
    Sim& sim = camp.sim;
    SlvLog log;
    SLV_REQUIRE(camp.capture(log));
    slvHeal(sim, camp.enemy);
    sim.submit(SetPrisonerModeCommand{camp.enemy, "ReduceWill"});
    SLV_REQUIRE(slvRunUntil(sim, log, 20 * 86400, [&] { return sim.things().registry().get<Prisoner>(camp.enemy).will <= 0.0f; }));
    CHECK_FALSE(sim.prisoners().chatDue(camp.enemy));
    sim.stepMany(2 * 86400);
    CHECK(sim.prisoners().isPrisoner(camp.enemy));
    CHECK_FALSE(sim.prisoners().isSlave(camp.enemy));
}

namespace {

// A captured prisoner made a slave at once.
Entity slvEnslaved(SlvCamp& camp, SlvLog& log) {
    if (!camp.capture(log)) {
        return kNoEntity;
    }
    slvHeal(camp.sim, camp.enemy);
    camp.sim.step();
    return camp.sim.prisoners().enslave(camp.enemy, camp.warden) ? camp.enemy : kNoEntity;
}

}  // namespace

TEST_CASE("Slaves: suppression falls by the wiki's brackets, and a warden suppresses a slave whose suppression is low") {
    SlvCamp camp;
    Sim& sim = camp.sim;
    SlvLog log;
    const Entity slave = slvEnslaved(camp, log);
    SLV_REQUIRE(slave != kNoEntity);
    entt::registry& reg = sim.things().registry();
    // 50% falls at 20% a day: a quarter of a day takes 5%. Keep the warden away for that.
    sim.submit(SetWorkPriorityCommand{camp.warden, "Warden", 0});
    sim.step();
    reg.get<Slave>(slave).suppression = 0.9f;
    const float before = reg.get<Slave>(slave).suppression;
    sim.stepMany(86400 / 4);
    CHECK(before - reg.get<Slave>(slave).suppression == doctest::Approx(0.05f).epsilon(0.02));
    reg.get<Slave>(slave).suppression = 0.1f;  // under 15%: 5% a day
    sim.stepMany(86400 / 4);
    CHECK(reg.get<Slave>(slave).suppression == doctest::Approx(0.1f - 0.0125f).epsilon(0.02));

    // The warden's power: Social 10 suppresses 28%, twice that at 0%.
    reg.get<Slave>(slave).suppression = 0.0f;
    const float amount = sim.prisoners().suppressAmount(camp.warden, slave);
    CHECK(amount > 0.0f);
    reg.get<Slave>(slave).suppression = 0.3f;
    CHECK(sim.prisoners().suppressDue(slave));
    sim.submit(SetWorkPriorityCommand{camp.warden, "Warden", 1});
    SLV_REQUIRE(slvRunUntil(sim, log, 86400, [&] { return log.type(EventType::SlaveSuppressed) != nullptr; }));
    const Event* suppressed = log.type(EventType::SlaveSuppressed);
    CHECK(suppressed->thing == camp.warden);
    CHECK(suppressed->target == slave);
    CHECK(reg.get<Slave>(slave).suppression > 0.3f);
    CHECK(sim.prisoners().isSlave(slave));
}

TEST_CASE("Slaves: the rebellion chance follows the wiki's factors, and a rebellion sends every slave who can walk off the map") {
    SlvCamp camp;
    Sim& sim = camp.sim;
    SlvLog log;
    const Entity slave = slvEnslaved(camp, log);
    SLV_REQUIRE(slave != kNoEntity);
    entt::registry& reg = sim.things().registry();
    reg.get<Slave>(slave).suppression = 1.0f;
    const float high = sim.prisoners().rebellionChance(slave, 86400);
    reg.get<Slave>(slave).suppression = 0.0f;
    const float low = sim.prisoners().rebellionChance(slave, 86400);
    CHECK(low > high * 15.0f);  // 5x against 0.25x
    // With 0% suppression and a normal mood: about one day in nine (45 days / 5).
    CHECK(low > 0.05f);
    CHECK(low < 0.2f);

    CHECK(sim.prisoners().rebel(slave) == 1);
    CHECK_FALSE(sim.prisoners().isSlave(slave));
    CHECK(sim.fighters().factionOf(slave) == 2);
    CHECK(reg.get<Fighter>(slave).mode == FightMode::Flee);
    bool rose = false;
    for (const Event& e : sim.events()) {
        rose = rose || (e.type == EventType::SlaveRebellion && e.count == 1);
    }
    CHECK(rose);
    SLV_REQUIRE(slvRunUntil(sim, log, 4000, [&] { return log.type(EventType::PawnLeftMap) != nullptr; }));
    CHECK_FALSE(sim.things().exists(slave));
}

TEST_CASE("Slaves: an unwatched slave with no suppression rebels by itself") {
    SlvCamp camp;
    Sim& sim = camp.sim;
    SlvLog log;
    const Entity slave = slvEnslaved(camp, log);
    SLV_REQUIRE(slave != kNoEntity);
    entt::registry& reg = sim.things().registry();
    // The warden is gone: nobody watches, so the chance is 20 times higher.
    sim.things().destroy(camp.warden);
    reg.get<Slave>(slave).suppression = 0.0f;
    SLV_REQUIRE(slvRunUntil(sim, log, 6 * 86400, [&] { return log.type(EventType::SlaveRebellion) != nullptr; }));
    CHECK_FALSE(sim.prisoners().isSlave(slave));
}

TEST_CASE("Slaves: a slave can be emancipated into a colonist, or released") {
    {
        SlvCamp camp;
        Sim& sim = camp.sim;
        SlvLog log;
        const Entity slave = slvEnslaved(camp, log);
        SLV_REQUIRE(slave != kNoEntity);
        sim.submit(SetPrisonerModeCommand{slave, "Torture"});
        sim.submit(SetPrisonerModeCommand{slave, "Emancipate"});
        SlvLog after;
        after.step(sim);
        CHECK(after.count(EventType::CommandFailed) == 1);
        CHECK(after.type(EventType::SlaveEmancipated) != nullptr);
        CHECK_FALSE(sim.prisoners().isSlave(slave));
        CHECK(sim.fighters().factionOf(slave) == kPlayerFaction);
        sim.submit(SetWorkPriorityCommand{slave, "Research", 1});
        sim.submit(DraftCommand{slave, true});
        SlvLog free;
        free.step(sim);
        CHECK_FALSE(slvAnyFailed(free));
    }
    {
        SlvCamp camp;
        Sim& sim = camp.sim;
        SlvLog log;
        const Entity slave = slvEnslaved(camp, log);
        SLV_REQUIRE(slave != kNoEntity);
        sim.submit(SetPrisonerModeCommand{slave, "Release"});
        SlvLog after;
        after.step(sim);
        CHECK_FALSE(slvAnyFailed(after));
        CHECK_FALSE(sim.prisoners().isSlave(slave));
        CHECK(sim.fighters().factionOf(slave) == 2);
        SLV_REQUIRE(slvRunUntil(sim, after, 4000, [&] { return after.type(EventType::PawnLeftMap) != nullptr; }));
    }
}

TEST_CASE("Slaves: a slave and a prisoner's will save and load, and the loaded game goes on the same") {
    SlvCamp camp;
    Sim& sim = camp.sim;
    SlvLog log;
    const Entity slave = slvEnslaved(camp, log);
    SLV_REQUIRE(slave != kNoEntity);
    sim.things().registry().get<Slave>(slave).suppression = 0.42f;
    const std::vector<uint8_t> bytes = saveSim(sim);
    SaveLoadResult loaded = loadSim(bytes, slvDefs());
    if (!loaded.error.empty()) MESSAGE(loaded.error);
    SLV_REQUIRE(loaded.error.empty());
    SLV_REQUIRE(loaded.sim != nullptr);
    Sim& copy = *loaded.sim;
    CHECK(copy.stateHash() == sim.stateHash());
    CHECK(copy.prisoners().isSlave(slave));
    CHECK(copy.things().registry().get<Slave>(slave).suppression == doctest::Approx(0.42f));
    CHECK(copy.things().registry().get<Slave>(slave).homeFaction == 2);
    CHECK(saveSim(copy) == bytes);
    sim.stepMany(20000);
    copy.stepMany(20000);
    CHECK(copy.stateHash() == sim.stateHash());
    CHECK(saveSim(copy) == saveSim(sim));
}
