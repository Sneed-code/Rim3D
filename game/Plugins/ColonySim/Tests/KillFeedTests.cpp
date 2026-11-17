// The kill feed's sim side (docs/design/kill-feed.md): a PawnDied event names its killer, and each pawn has a side
// colour and a short name for the feed line.

#include "ColonySim/Animals.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Prisoners.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"
#include "ColonySim/Trade.h"

#include <doctest.h>

#include <memory>
#include <string>
#include <vector>

using namespace colony;

namespace {

std::shared_ptr<const DefDatabase> killFeedDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

struct KillFeedFixture {
    Sim sim;

    KillFeedFixture() : sim(settings(), killFeedDefs()) {
        for (int y = 0; y < sim.map().height(); ++y) {
            for (int x = 0; x < sim.map().width(); ++x) {
                sim.plants().remove(CellPos{x, y, 0});
            }
        }
        sim.stepMany(2);
    }
    static SimSettings settings() {
        SimSettings s;
        s.seed = 11;
        s.mapSize = 64;
        s.wildAnimals = 0;
        s.flatMap = true;
        s.checkpointEvery = 0;
        return s;
    }
    entt::registry& reg() { return sim.things().registry(); }
    Entity person(CellPos cell, const char* first, const char* nick, FactionId faction) {
        const Entity pawn = sim.things().spawn("Human", cell);
        Colonist c;
        c.firstName = first;
        c.nickName = nick;
        reg().emplace<Colonist>(pawn, c);
        if (faction != kPlayerFaction) {
            reg().emplace<Faction>(pawn, Faction{faction});
        }
        return pawn;
    }
    // Steps until `pawn`'s PawnDied event shows (at most `steps`), and returns its killer; kNoEntity if none (or no death).
    Entity killerOf(Entity pawn, bool& died, int steps = 5) {
        died = false;
        for (int i = 0; i < steps; ++i) {
            sim.step();
            for (const Event& e : sim.events()) {
                if (e.type == EventType::PawnDied && e.thing == pawn) {
                    died = true;
                    return e.target;
                }
            }
        }
        return kNoEntity;
    }
};

}  // namespace

TEST_CASE("Kill feed: a death names whoever last hurt the pawn") {
    KillFeedFixture f;
    const Entity shooter = f.person(CellPos{10, 10, 0}, "Ada", "Ace", kPlayerFaction);
    const Entity other = f.person(CellPos{12, 10, 0}, "Bo", "", kPlayerFaction);
    const Entity raider = f.person(CellPos{20, 10, 0}, "Cy", "", 2);
    f.sim.combat().noteHurt(raider, other);
    f.sim.step();
    f.sim.combat().noteHurt(raider, shooter);  // the last one gets the kill
    f.sim.submit(DamagePawnCommand{raider, "Bullet", 500.0f, 0});
    bool died = false;
    CHECK(f.killerOf(raider, died) == shooter);
    CHECK(died);
}

TEST_CASE("Kill feed: a blast credits whoever threw it, a death by itself credits nobody") {
    KillFeedFixture f;
    const Entity thrower = f.person(CellPos{10, 10, 0}, "Ada", "", kPlayerFaction);
    const Entity raider = f.person(CellPos{20, 10, 0}, "Cy", "", 2);
    f.sim.step();
    Explosion ex;
    ex.x = 20.5f;
    ex.y = 10.5f;
    ex.radius = 3.0f;
    ex.damage = 5000.0f;
    ex.damageType = f.sim.defs().idOf("DamageTypes", "Bomb");
    ex.instigator = thrower;
    ex.seed = 7;
    f.sim.combat().explode(ex);
    const Health* health = f.reg().try_get<Health>(raider);
    CHECK((health != nullptr && health->dead));
    if (health == nullptr || !health->dead) {
        return;
    }
    f.sim.creditKills();
    Entity killer = kNoEntity;
    for (const Event& e : f.sim.events()) {
        if (e.type == EventType::PawnDied && e.thing == raider) {
            killer = e.target;
        }
    }
    CHECK(killer == thrower);

    // Someone who dies with nobody's harm on them has no killer.
    const Entity lone = f.person(CellPos{30, 30, 0}, "Di", "", 2);
    f.sim.submit(DamagePawnCommand{lone, "Bullet", 500.0f, 0});
    bool died = false;
    CHECK(f.killerOf(lone, died) == kNoEntity);
    CHECK(died);
}

TEST_CASE("Kill feed: starving or an old wound credits nobody") {
    KillFeedFixture f;
    const Entity shooter = f.person(CellPos{10, 10, 0}, "Ada", "", kPlayerFaction);
    const Entity starved = f.person(CellPos{20, 10, 0}, "Cy", "", 2);
    f.sim.step();
    f.sim.combat().noteHurt(starved, shooter);
    f.sim.health().kill(starved, DeathCause::Starvation);
    f.sim.creditKills();
    bool found = false;
    for (const Event& e : f.sim.events()) {
        if (e.type == EventType::PawnDied && e.thing == starved) {
            found = true;
            CHECK(e.target == kNoEntity);
        }
    }
    CHECK(found);

    const Entity late = f.person(CellPos{20, 20, 0}, "Di", "", 2);
    f.sim.combat().noteHurt(late, shooter);
    f.sim.stepMany(static_cast<int>(Sim::kKillCreditSteps) + 5);
    f.sim.submit(DamagePawnCommand{late, "Bullet", 500.0f, 0});
    bool died = false;
    CHECK(f.killerOf(late, died) == kNoEntity);
    CHECK(died);
}

TEST_CASE("Kill feed: sides and names") {
    KillFeedFixture f;
    const Entity ours = f.person(CellPos{10, 10, 0}, "Ada", "Ace", kPlayerFaction);
    const Entity plain = f.person(CellPos{11, 10, 0}, "Bo", "", kPlayerFaction);
    const Entity raider = f.person(CellPos{20, 10, 0}, "Cy", "", 2);
    const Entity guest = f.person(CellPos{21, 10, 0}, "Di", "", 3);
    f.reg().emplace<Visitor>(guest);
    const Entity deer = f.sim.things().spawn("Deer", CellPos{30, 30, 0});
    f.sim.step();

    CHECK(f.sim.killSideOf(ours) == Sim::KillSide::Friendly);
    CHECK(f.sim.killSideOf(raider) == Sim::KillSide::Enemy);
    CHECK(f.sim.killSideOf(guest) == Sim::KillSide::Neutral);
    CHECK(f.sim.killSideOf(deer) == Sim::KillSide::Neutral);
    CHECK(f.sim.feedName(ours) == "Ace");
    CHECK(f.sim.feedName(plain) == "Bo");
    CHECK(f.sim.feedName(deer) == "Deer");

    // A wild animal on the attack is an enemy.
    f.reg().get<Animal>(deer).temper = AnimalTemper::Manhunter;
    CHECK(f.sim.killSideOf(deer) == Sim::KillSide::Enemy);
}
