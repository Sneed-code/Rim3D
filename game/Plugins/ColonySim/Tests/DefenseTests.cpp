#include "ColonySim/Apparel.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Cover.h"
#include "ColonySim/Defense.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fighters.h"
#include "FuelTestHelper.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Power.h"
#include "ColonySim/Raids.h"
#include "ColonySim/Random.h"
#include "ColonySim/Replay.h"
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

#define DEFN_REQUIRE(...)      \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// The game's real data. These tests check docs/design/defense.md.
std::shared_ptr<const DefDatabase> defnDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        for (const std::string& e : errors) {
            MESSAGE(e);
        }
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

std::unique_ptr<Sim> defnSim(uint64_t seed = 31) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = 96;
    settings.flatMap = true;
    return std::make_unique<Sim>(settings, defnDefs());
}

// A pawn of another side (it becomes a fighter at the next step).
Entity defnHostile(Sim& sim, int x, int y, uint32_t faction = 2) {
    const Entity pawn = sim.things().spawn("Human", CellPos{x, y, 0});
    sim.submit(SetFactionCommand{pawn, faction});
    sim.step();
    return pawn;
}

// A generator at (10,10), a wire along row 10 and a mini-turret at (15,10).
Entity defnPoweredTurret(Sim& sim) {
    testFueled(sim, sim.things().spawn("WoodFiredGenerator", CellPos{10, 10, 0}));
    for (int x = 12; x <= 14; ++x) {
        sim.things().spawn("PowerConduit", CellPos{x, 10, 0});
    }
    return sim.things().spawn("Turret_Mini", CellPos{15, 10, 0});
}

int defnCount(const Sim& sim, EventType type) {
    return static_cast<int>(std::count_if(sim.events().begin(), sim.events().end(), [&](const Event& e) { return e.type == type; }));
}

float defnHurt(Sim& sim, Entity pawn) {
    const Health& h = sim.things().registry().get<Health>(pawn);
    float total = h.dead ? 1000.0f : 0.0f;
    for (const Injury& i : h.injuries) {
        total += i.severity;
    }
    return total + static_cast<float>(h.missing.size()) * 50.0f;
}

// Steps and counts what the turret shot.
int defnShots(Sim& sim, Entity turret, int steps) {
    int shots = 0;
    for (int i = 0; i < steps; ++i) {
        sim.step();
        for (const Event& e : sim.events()) {
            shots += e.type == EventType::ShotFired && e.thing == turret ? 1 : 0;
        }
    }
    return shots;
}

void defnMoveTo(Sim& sim, Entity pawn, float x, float y) {
    Position& pos = sim.things().registry().get<Position>(pawn);
    pos.x = x;
    pos.y = y;
}

// True if the walk start -> route passes through the cell (with a hair of room).
bool defnWalkHits(float startX, float startY, const std::vector<CellPos>& route, CellPos cell) {
    float x0 = startX;
    float y0 = startY;
    for (const CellPos& c : route) {
        const float x1 = static_cast<float>(c.x) + 0.5f;
        const float y1 = static_cast<float>(c.y) + 0.5f;
        for (int i = 0; i <= 100; ++i) {
            const float t = static_cast<float>(i) / 100.0f;
            const float x = x0 + (x1 - x0) * t;
            const float y = y0 + (y1 - y0) * t;
            if (static_cast<int>(std::floor(x)) == cell.x && static_cast<int>(std::floor(y)) == cell.y) {
                return true;
            }
        }
        x0 = x1;
        y0 = y1;
    }
    return false;
}

}  // namespace

TEST_CASE("Defense: the mini-turret, spike trap and IED are defs with their numbers") {
    const DefDatabase& defs = *defnDefs();
    const Def* turret = defs.find("Things", "Turret_Mini");
    const Def* gun = defs.find("Things", "Gun_MiniTurret");
    const Def* spike = defs.find("Things", "TrapSpike");
    const Def* ied = defs.find("Things", "TrapIEDExplosive");
    DEFN_REQUIRE(turret != nullptr);
    DEFN_REQUIRE(gun != nullptr);
    DEFN_REQUIRE(spike != nullptr);
    DEFN_REQUIRE(ied != nullptr);
    CHECK(turret->number("power.consumption") == doctest::Approx(80.0));
    CHECK(turret->number("stats.maxHitPoints") == doctest::Approx(100.0));
    CHECK(turret->text("turret.gun") == "Gun_MiniTurret");
    CHECK(turret->number("explodeWhenDamaged.belowHealth") == doctest::Approx(0.2));
    CHECK(gun->number("verbs.0.range") == doctest::Approx(28.9));
    CHECK(gun->integer("verbs.0.burstShotCount") == 2);
    CHECK(spike->integer("trap.hits") == 5);
    CHECK(spike->number("trap.damage") == doctest::Approx(100.0));
    CHECK(ied->flag("trap.explodes"));
    CHECK(ied->flag("explodeWhenDestroyed"));
    Sim sim(SimSettings{}, defnDefs());
    CHECK(sim.defense().isTurret(defs.idOf("Things", "Turret_Mini")));
    CHECK(sim.defense().isTrap(defs.idOf("Things", "TrapSpike")));
    CHECK(sim.defense().isTrap(defs.idOf("Things", "TrapIEDExplosive")));
    CHECK_FALSE(sim.defense().isTurret(defs.idOf("Things", "Wall")));
}

TEST_CASE("Defense: a powered turret shoots a hostile in range and leaves its own side alone") {
    auto sim = defnSim();
    const Entity turret = defnPoweredTurret(*sim);
    const Entity colonist = sim->things().spawn("Human", CellPos{32, 10, 0});
    sim->stepMany(5);
    DEFN_REQUIRE(sim->power().running(turret));
    CHECK(defnShots(*sim, turret, 300) == 0);  // only a colonist about
    CHECK(defnHurt(*sim, colonist) == 0.0f);

    const Entity raider = defnHostile(*sim, 36, 14);
    int shots = 0;
    bool aimed = false;
    for (int i = 0; i < 400 && defnHurt(*sim, raider) == 0.0f; ++i) {
        shots += defnShots(*sim, turret, 1);
        const Turret& t = sim->things().registry().get<Turret>(turret);
        aimed = aimed || (t.phase != TurretPhase::Idle && t.target == raider);
    }
    CHECK(aimed);
    CHECK(shots >= 3);  // a whole burst
    CHECK(defnHurt(*sim, raider) > 0.0f);
    CHECK(defnHurt(*sim, colonist) == 0.0f);
    CHECK(sim->defense().shotsFired() >= 3);
}

TEST_CASE("Defense: a turret with no power, out of range or behind a wall does not shoot") {
    {
        auto sim = defnSim();
        const Entity turret = sim->things().spawn("Turret_Mini", CellPos{15, 10, 0});  // no wire, no generator
        defnHostile(*sim, 25, 10);
        CHECK(defnShots(*sim, turret, 300) == 0);
    }
    {
        auto sim = defnSim();
        const Entity turret = defnPoweredTurret(*sim);
        defnHostile(*sim, 60, 10);  // 45 m: out of the 28.9 m range
        CHECK(defnShots(*sim, turret, 300) == 0);
    }
    {
        auto sim = defnSim();
        const Entity turret = defnPoweredTurret(*sim);
        for (int y = 6; y <= 14; ++y) {
            sim->things().spawn("Wall", CellPos{20, y, 0});
        }
        defnHostile(*sim, 26, 10);
        CHECK(defnShots(*sim, turret, 300) == 0);
    }
}

TEST_CASE("Defense: a raider with a gun shoots at a turret and wears it down; one without a gun leaves it be") {
    {
        auto sim = defnSim();
        // No power: the turret can't shoot back, so the raider lives to show what it does.
        const Entity turret = sim->things().spawn("Turret_Mini", CellPos{15, 10, 0});
        DEFN_REQUIRE(turret != kNoEntity);
        const int full = sim->things().registry().get<HitPoints>(turret).current;
        const Entity raider = defnHostile(*sim, 27, 10);
        sim->combat().equip(raider, sim->things().make(sim->defs().idOf("Things", "Gun_Revolver")));
        bool targeted = false;
        int shots = 0;
        for (int i = 0; i < 3000; ++i) {
            sim->step();
            if (!sim->things().registry().valid(turret) || sim->things().registry().get<HitPoints>(turret).current < full / 2) {
                break;
            }
            const Fighter* f = sim->things().registry().try_get<Fighter>(raider);
            targeted = targeted || (f != nullptr && f->target == turret);
            for (const Event& e : sim->events()) {
                shots += e.type == EventType::ShotFired && e.thing == raider ? 1 : 0;
            }
        }
        CHECK(targeted);
        CHECK(shots > 0);
        const bool gone = !sim->things().registry().valid(turret);
        CHECK((gone || sim->things().registry().get<HitPoints>(turret).current < full / 2));
    }
    {
        auto sim = defnSim();
        const Entity turret = sim->things().spawn("Turret_Mini", CellPos{15, 10, 0});
        const Entity raider = defnHostile(*sim, 22, 10);  // bare hands: it can't hurt a turret
        bool targeted = false;
        for (int i = 0; i < 600; ++i) {
            sim->step();
            const Fighter* f = sim->things().registry().try_get<Fighter>(raider);
            targeted = targeted || (f != nullptr && f->target == turret);
        }
        CHECK_FALSE(targeted);
        CHECK(sim->things().registry().get<HitPoints>(turret).current == 100);
    }
}

TEST_CASE("Defense: a switched-off turret stops mid-burst and starts again when switched on") {
    auto sim = defnSim();
    const Entity turret = defnPoweredTurret(*sim);
    defnHostile(*sim, 35, 10);
    sim->submit(FlickCommand{turret, false});
    CHECK(defnShots(*sim, turret, 200) == 0);
    sim->submit(FlickCommand{turret, true});
    CHECK(defnShots(*sim, turret, 200) >= 3);
}

TEST_CASE("Defense: a mini-turret below 20% of its hit points may spark and blow up 4 seconds later") {
    // Wiki (Mini-turret): below 20% health a 50% chance to spark for 240 ticks (120 steps), then 50 bomb damage in 3.9 m.
    int sparked = 0;
    bool checkedBlast = false;
    for (uint64_t seed = 1; seed <= 24; ++seed) {
        auto sim = defnSim(seed);
        const Entity turret = sim->things().spawn("Turret_Mini", CellPos{40, 40, 0});
        const Entity bystander = sim->things().spawn("Human", CellPos{40, 42, 0});  // 2 m off
        DEFN_REQUIRE(turret != kNoEntity);
        HitPoints& hp = sim->things().registry().get<HitPoints>(turret);
        hp.current = hp.max / 2;  // half: no roll
        sim->stepMany(3);
        CHECK_FALSE(sim->things().registry().get<Turret>(turret).fuseRolled);
        hp.current = hp.max / 10;
        sim->step();
        const Turret& t = sim->things().registry().get<Turret>(turret);
        CHECK(t.fuseRolled);
        if (t.fuse == 0) {
            // No luck this time: it never goes up, and it rolls again only after a repair.
            sim->stepMany(200);
            CHECK(sim->things().registry().valid(turret));
            continue;
        }
        ++sparked;
        CHECK(t.fuse == 120);  // 240 ticks
        if (checkedBlast) {
            continue;
        }
        checkedBlast = true;
        int blasts = 0;
        for (int k = 0; k < 125; ++k) {
            sim->step();
            blasts += defnCount(*sim, EventType::Explosion);
        }
        CHECK(blasts == 1);  // its own blast, which finishes it, and no second one
        CHECK_FALSE(sim->things().registry().valid(turret));
        CHECK(defnHurt(*sim, bystander) > 0.0f);
    }
    MESSAGE(sparked, " of 24 sparked");
    CHECK(sparked >= 6);
    CHECK(sparked <= 18);
    CHECK(checkedBlast);
}

TEST_CASE("Defense: a mini-turret shot to pieces at once or pulled down does not blow up") {
    auto sim = defnSim();
    const Entity turret = sim->things().spawn("Turret_Mini", CellPos{40, 40, 0});
    sim->submit(ExplodeCommand{40.5f, 40.5f, 0.5f, 500.0f, "Bomb", 0.1f});
    sim->step();
    CHECK_FALSE(sim->things().registry().valid(turret));
    sim->stepMany(150);
    CHECK(defnCount(*sim, EventType::Explosion) == 0);

    auto quiet = defnSim();
    const Entity other = quiet->things().spawn("Turret_Mini", CellPos{40, 40, 0});
    const Entity near = quiet->things().spawn("Human", CellPos{40, 43, 0});
    quiet->submit(DestroyThingCommand{other});
    quiet->stepMany(3);
    CHECK_FALSE(quiet->things().registry().valid(other));
    CHECK(defnCount(*quiet, EventType::Explosion) == 0);
    CHECK(defnHurt(*quiet, near) == 0.0f);
}

TEST_CASE("Defense: an enemy that steps on a spike trap is stabbed, the colony's own side never springs it") {
    auto sim = defnSim();
    const Entity trap = sim->things().spawn("TrapSpike", CellPos{20, 20, 0});
    const Entity own = sim->things().spawn("Human", CellPos{20, 20, 0});
    sim->stepMany(20);
    DEFN_REQUIRE(sim->things().registry().valid(trap));
    CHECK_FALSE(sim->things().registry().get<Trap>(trap).sprung);
    CHECK(defnHurt(*sim, own) == 0.0f);
    CHECK(sim->defense().knows(own, trap));

    defnMoveTo(*sim, own, 30.5f, 30.5f);
    const Entity raider = defnHostile(*sim, 25, 20);
    CHECK_FALSE(sim->defense().knows(raider, trap));
    defnMoveTo(*sim, raider, 20.5f, 20.5f);
    bool sprung = false;
    for (int i = 0; i < 3 && !sprung; ++i) {
        sim->step();
        sprung = defnCount(*sim, EventType::TrapSprung) > 0;
    }
    CHECK(sprung);
    CHECK(defnHurt(*sim, raider) > 0.0f);
    CHECK(sim->defense().trapsSprung() == 1);
    DEFN_REQUIRE(sim->things().registry().valid(trap));  // seed 31: this trap is not among the one in ten that break
    CHECK(sim->things().registry().get<Trap>(trap).sprung);
    Snapshot snapshot;
    sim->writeSnapshot(snapshot);
    bool shown = false;
    for (const ThingView& v : snapshot.things) {
        if (v.id == trap) {
            CHECK(v.trapState == 2);
            shown = true;
        }
    }
    CHECK(shown);

    // Rearmed, it waits for the next one; a spring trap springs only once.
    defnMoveTo(*sim, raider, 40.5f, 40.5f);
    sim->stepMany(5);
    CHECK(sim->defense().trapsSprung() == 1);
    sim->submit(RearmTrapCommand{trap});
    sim->step();
    CHECK(defnCount(*sim, EventType::CommandFailed) == 0);
    CHECK(defnCount(*sim, EventType::TrapRearmed) == 1);
    CHECK_FALSE(sim->things().registry().get<Trap>(trap).sprung);
    sim->submit(RearmTrapCommand{trap});  // armed already
    sim->step();
    CHECK(defnCount(*sim, EventType::CommandFailed) == 1);
}

TEST_CASE("Defense: a spike trap's stabs go through armor, so a flak vest and helmet take most of them") {
    auto sim = defnSim();
    const DefId stab = sim->defs().idOf("DamageTypes", "Stab");
    const DefId vest = sim->defs().idOf("Things", "Apparel_FlakVest");
    const DefId pants = sim->defs().idOf("Things", "Apparel_FlakPants");
    const DefId helmet = sim->defs().idOf("Things", "Apparel_SimpleHelmet");
    DEFN_REQUIRE(stab.valid());
    DEFN_REQUIRE(vest.valid());
    DEFN_REQUIRE(pants.valid());
    DEFN_REQUIRE(helmet.valid());
    CHECK(sim->defs().get(sim->defs().idOf("Things", "TrapSpike")).number("trap.armorPenetration") > 0.0);
    const DefId cloth = sim->defs().idOf("Things", "Cloth");
    float bare = 0.0f;
    float armoured = 0.0f;
    Rng rngBare(5);
    Rng rngArmoured(5);
    for (int i = 0; i < 20; ++i) {
        const Entity a = sim->things().spawn("Human", CellPos{10 + i, 10, 0});
        const Entity b = sim->things().spawn("Human", CellPos{10 + i, 14, 0});
        for (const DefId piece : {vest, pants, helmet}) {
            const bool stuffed = sim->defs().get(piece).flag("stuffRequired");
            const Entity item = sim->apparel().make(piece, stuffed ? cloth : DefId{}, -1, CellPos{10 + i, 14, 0});
            std::string why;
            CHECK(sim->apparel().putOn(b, item, why));
        }
        for (int hit = 0; hit < 5; ++hit) {
            sim->combat().strikePawn(a, stab, 20.0f, 0.5f, rngBare);
            sim->combat().strikePawn(b, stab, 20.0f, 0.5f, rngArmoured);
        }
        bare += defnHurt(*sim, a);
        armoured += defnHurt(*sim, b);
    }
    CHECK(bare > 0.0f);
    CHECK(armoured < bare * 0.8f);
}

TEST_CASE("Defense: a colonist with the Construction work type walks to a sprung spike trap and sets it again; an IED is left") {
    auto sim = defnSim();
    const Entity trap = sim->things().spawn("TrapSpike", CellPos{20, 20, 0});
    const Entity colonist = sim->things().spawn("Human", CellPos{26, 20, 0});
    sim->things().registry().emplace<Colonist>(colonist, Colonist{});
    sim->jobs().addWorker(colonist, {}, false);
    sim->submit(SetWorkPriorityCommand{colonist, "Construction", 1});
    sim->stepMany(120);
    CHECK_FALSE(sim->things().registry().get<Trap>(trap).sprung);
    CHECK(defnCount(*sim, EventType::TrapRearmed) == 0);  // nothing to do while it is armed

    sim->things().registry().get<Trap>(trap).sprung = true;
    const Entity ied = sim->things().spawn("TrapIEDExplosive", CellPos{20, 24, 0});
    sim->things().registry().get<Trap>(ied).sprung = true;
    int started = -1;
    int rearmed = -1;
    for (int i = 0; i < 600 && rearmed < 0; ++i) {
        sim->step();
        for (const Event& e : sim->events()) {
            if (e.type == EventType::PawnAction && e.action == PawnAction::Rearm && started < 0) {
                started = i;
                CHECK(e.thing == colonist);
            }
            if (e.type == EventType::TrapRearmed && e.thing == trap) {
                rearmed = i;
            }
        }
    }
    DEFN_REQUIRE(started >= 0);
    DEFN_REQUIRE(rearmed >= 0);
    CHECK(rearmed - started >= 12);  // the 13 seconds of work
    CHECK_FALSE(sim->things().registry().get<Trap>(trap).sprung);
    sim->stepMany(200);
    CHECK(sim->things().registry().get<Trap>(ied).sprung);  // an IED can't be used again
    CHECK(defnCount(*sim, EventType::TrapRearmed) == 0);
}

TEST_CASE("Defense: an IED blows up under the first enemy and is gone; a blast sets it off too") {
    auto sim = defnSim();
    const Entity trap = sim->things().spawn("TrapIEDExplosive", CellPos{20, 20, 0});
    const Entity friendly = sim->things().spawn("Human", CellPos{20, 24, 0});  // 4 m: outside the 3.9 m radius
    const Entity near = sim->things().spawn("Human", CellPos{20, 22, 0});      // 2 m: inside
    const Entity raider = defnHostile(*sim, 25, 20);
    DEFN_REQUIRE(sim->things().registry().valid(trap));
    defnMoveTo(*sim, raider, 20.5f, 20.5f);
    sim->step();
    CHECK(defnCount(*sim, EventType::TrapSprung) == 1);
    CHECK(defnCount(*sim, EventType::Explosion) == 1);  // and not a second one for the IED being destroyed
    CHECK_FALSE(sim->things().registry().valid(trap));
    CHECK(defnHurt(*sim, raider) > 0.0f);
    CHECK(defnHurt(*sim, near) > 0.0f);
    CHECK(defnHurt(*sim, friendly) == 0.0f);

    // One shot up by a stray blast goes off once, not twice.
    auto other = defnSim(32);
    const Entity ied = other->things().spawn("TrapIEDExplosive", CellPos{40, 40, 0});
    const Entity bystander = other->things().spawn("Human", CellPos{40, 43, 0});
    other->submit(ExplodeCommand{40.5f, 40.5f, 1.0f, 500.0f, "Bomb", 0.1f});
    other->step();
    CHECK_FALSE(other->things().registry().valid(ied));
    CHECK(defnCount(*other, EventType::Explosion) == 2);  // the stray blast, then the IED
    CHECK(defnHurt(*other, bystander) > 0.0f);
    other->stepMany(5);
    CHECK(defnCount(*other, EventType::Explosion) == 0);
}

TEST_CASE("Defense: the side of a pawn that sprang a trap knows it, and enemy fighters near one may notice it") {
    auto sim = defnSim();
    const Entity trap = sim->things().spawn("TrapSpike", CellPos{20, 20, 0});
    const Entity raider = defnHostile(*sim, 25, 20);
    defnMoveTo(*sim, raider, 20.5f, 20.5f);
    sim->stepMany(2);
    DEFN_REQUIRE(sim->things().registry().valid(trap));
    DEFN_REQUIRE(sim->things().registry().get<Trap>(trap).sprung);
    defnMoveTo(*sim, raider, 60.5f, 60.5f);
    sim->submit(RearmTrapCommand{trap});
    sim->step();
    CHECK(sim->defense().avoidsCell(2, CellPos{20, 20, 0}));
    CHECK_FALSE(sim->defense().avoidsCell(3, CellPos{20, 20, 0}));
    CHECK_FALSE(sim->defense().avoidsCell(1, CellPos{20, 20, 0}));
    CHECK_FALSE(sim->defense().avoidsCell(2, CellPos{21, 20, 0}));
    CHECK(sim->defense().knows(raider, trap));

    // Another side's fighter, a couple of metres off, notices it in time (35% every quarter minute).
    const Entity scout = defnHostile(*sim, 22, 20, 3);
    DEFN_REQUIRE(sim->things().registry().valid(scout));
    bool noticed = false;
    for (int i = 0; i < 1200 && !noticed; ++i) {
        sim->step();
        noticed = sim->defense().avoidsCell(3, CellPos{20, 20, 0});
    }
    CHECK(noticed);
    // Far from it, nobody notices.
    const Entity far = sim->things().spawn("TrapSpike", CellPos{70, 70, 0});
    sim->stepMany(900);
    CHECK_FALSE(sim->defense().avoidsCell(3, CellPos{70, 70, 0}));
    CHECK_FALSE(sim->defense().avoidsCell(2, CellPos{70, 70, 0}));
    CHECK(sim->things().registry().valid(far));
}

TEST_CASE("Defense: a walk is bent round the traps a side knows and no others") {
    auto sim = defnSim();
    const Entity trap = sim->things().spawn("TrapSpike", CellPos{30, 48, 0});
    sim->things().registry().get<Trap>(trap).known = {2};
    sim->step();
    std::vector<CellPos> straight{CellPos{50, 48, 0}};
    CHECK(defnWalkHits(10.5f, 48.5f, straight, CellPos{30, 48, 0}));

    std::vector<CellPos> bent = straight;
    sim->defense().detour(2, 10.5f, 48.5f, bent);
    DEFN_REQUIRE(bent.size() == 2);
    CHECK(bent.back() == CellPos{50, 48, 0});
    CHECK(std::abs(bent[0].x - 30) <= 2);
    CHECK(std::abs(bent[0].y - 48) <= 2);
    CHECK_FALSE(defnWalkHits(10.5f, 48.5f, bent, CellPos{30, 48, 0}));

    std::vector<CellPos> other = straight;
    sim->defense().detour(3, 10.5f, 48.5f, other);  // a side that doesn't know it
    CHECK(other == straight);
    other = straight;
    sim->defense().detour(1, 10.5f, 48.5f, other);  // the colony's own
    CHECK(other == straight);

    // A trap on the very last cell is left alone: the fighter's planning keeps its spot off it.
    std::vector<CellPos> onTrap{CellPos{30, 48, 0}};
    sim->defense().detour(2, 10.5f, 48.5f, onTrap);
    CHECK(onTrap.back() == CellPos{30, 48, 0});
}

TEST_CASE("Defense: a raider that knows of a trap walks round it, one that doesn't walks onto it") {
    for (const bool known : {true, false}) {
        auto sim = defnSim();
        DefenseNumbers blind = sim->defense().numbers();
        blind.noticeChance = 0.0f;  // nobody notices it on the way: only what the side knew beforehand counts
        sim->defense().setNumbers(blind);
        const Entity trap = sim->things().spawn("TrapSpike", CellPos{30, 48, 0});
        if (known) {
            sim->things().registry().get<Trap>(trap).known = {2};
        }
        const Entity raider = defnHostile(*sim, 10, 48);
        DEFN_REQUIRE(sim->things().registry().all_of<Fighter>(raider));
        Fighter& f = sim->things().registry().get<Fighter>(raider);
        f.hasGoal = true;
        f.goal = CellPos{50, 48, 0};
        sim->fighters().replan(raider);
        sim->stepMany(700);
        const Position& pos = sim->things().registry().get<Position>(raider);
        if (known) {
            CHECK(pos.x > 45.0f);  // it got there
            CHECK(sim->defense().trapsSprung() == 0);
            CHECK(defnHurt(*sim, raider) == 0.0f);
        } else {
            CHECK(sim->defense().trapsSprung() == 1);  // and the stabs stopped it there
            CHECK(defnHurt(*sim, raider) > 0.0f);
        }
    }
}

TEST_CASE("Defense: a raider that notices a trap on its way plans again and walks round it") {
    auto sim = defnSim();
    DefenseNumbers sharp = sim->defense().numbers();
    sharp.noticeChance = 1.0f;
    sim->defense().setNumbers(sharp);
    const Entity trap = sim->things().spawn("TrapSpike", CellPos{30, 48, 0});
    const Entity raider = defnHostile(*sim, 10, 48);
    const Entity mate = defnHostile(*sim, 8, 50);
    DEFN_REQUIRE(sim->things().registry().all_of<Fighter>(raider));
    for (const Entity e : {raider, mate}) {
        Fighter& f = sim->things().registry().get<Fighter>(e);
        f.hasGoal = true;
        f.goal = CellPos{50, 48, 0};
        sim->fighters().replan(e);
    }
    CHECK_FALSE(sim->defense().avoidsCell(2, CellPos{30, 48, 0}));
    sim->stepMany(900);
    CHECK(sim->defense().avoidsCell(2, CellPos{30, 48, 0}));  // learned on the way
    CHECK(sim->defense().trapsSprung() == 0);
    CHECK(defnHurt(*sim, raider) == 0.0f);
    CHECK(defnHurt(*sim, mate) == 0.0f);
    CHECK(sim->things().registry().get<Position>(raider).x > 45.0f);
    CHECK(sim->things().registry().get<Position>(mate).x > 45.0f);
    CHECK_FALSE(sim->things().registry().get<Trap>(trap).sprung);
}

TEST_CASE("Defense: turrets, traps, what sides know and a rearm survive a save, load and replay") {
    auto sim = defnSim();
    const Entity turret = defnPoweredTurret(*sim);
    const Entity trap = sim->things().spawn("TrapSpike", CellPos{40, 40, 0});
    const Entity ied = sim->things().spawn("TrapIEDExplosive", CellPos{44, 40, 0});
    sim->things().registry().get<Trap>(trap).known = {2, 5};
    defnHostile(*sim, 35, 10);
    // Mid-burst.
    for (int i = 0; i < 400 && sim->things().registry().get<Turret>(turret).phase != TurretPhase::Firing; ++i) {
        sim->step();
    }
    DEFN_REQUIRE(sim->things().registry().get<Turret>(turret).phase == TurretPhase::Firing);
    {
        // Sparking (below 20% of its hit points, the roll lost): the fuse is saved too.
        sim->things().registry().get<HitPoints>(turret).current = 10;
        Turret& t = sim->things().registry().get<Turret>(turret);
        t.fuseRolled = true;
        t.fuse = 300;
    }
    const std::string json = saveToJson(*sim);
    CHECK(json.find("\"defense\"") != std::string::npos);
    CHECK(json.find("\"turret\"") != std::string::npos);
    CHECK(json.find("\"trap\"") != std::string::npos);
    SaveLoadResult loaded = loadSim(saveSim(*sim), defnDefs());
    DEFN_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->stateHash() == sim->stateHash());
    CHECK(loaded.sim->things().registry().get<Trap>(trap).known == std::vector<FactionId>{2, 5});
    CHECK(loaded.sim->things().registry().get<Turret>(turret).fuse == 300);
    CHECK(loaded.sim->things().registry().get<Turret>(turret).fuseRolled);
    CHECK(loaded.sim->things().registry().valid(ied));
    for (int i = 0; i < 200; ++i) {
        sim->step();
        loaded.sim->step();
        DEFN_REQUIRE(loaded.sim->stateHash() == sim->stateHash());
    }

    SimSettings settings;
    settings.seed = 33;
    settings.mapSize = 96;
    settings.flatMap = true;
    settings.checkpointEvery = 100;
    Sim original(settings, defnDefs());
    original.submit(SpawnThingCommand{"TrapSpike", CellPos{20, 20, 0}});
    original.submit(SpawnThingCommand{"Human", CellPos{20, 20, 0}});
    original.step();
    std::vector<Entity> spawned;
    for (const Event& e : original.events()) {
        if (e.type == EventType::ThingSpawned) {
            spawned.push_back(e.thing);
        }
    }
    DEFN_REQUIRE(spawned.size() == 2);
    original.submit(SetFactionCommand{spawned[1], 2});
    original.stepMany(10);
    original.submit(RearmTrapCommand{spawned[0]});
    original.stepMany(20);
    const Replay recorded = recordReplay(original);
    Replay again;
    std::string error;
    DEFN_REQUIRE(readReplay(writeReplay(recorded), again, error));
    CHECK(writeReplay(again) == writeReplay(recorded));
    CHECK(writeReplay(again).find("\"RearmTrap\"") != std::string::npos);
    Sim replayed(again.settings, defnDefs());
    playReplay(again, replayed, again.endStep);
    CHECK(replayed.stateHash() == original.stateHash());
    CHECK(checkReplay(again, replayed).matched());
}

TEST_CASE("Defense: a wood barricade is 1.1 m of cover a pawn walks through, with the wiki's numbers") {
    const DefDatabase& defs = *defnDefs();
    const Def* barricade = defs.find("Things", "Barricade");
    DEFN_REQUIRE(barricade != nullptr);
    CHECK(barricade->integer("costStuffCount") == 5);
    CHECK(barricade->number("stats.maxHitPoints") == doctest::Approx(300.0));
    CHECK(barricade->number("stats.workToBuild") == doctest::Approx(320.0 * 1.44));
    CHECK(barricade->number("fillPercent") == doctest::Approx(0.55));
    CHECK(barricade->text("passability") == "PassThroughOnly");

    auto sim = defnSim();
    // No research needed: a blueprint in wood is taken.
    sim->submit(PlaceBlueprintCommand{"Barricade", "WoodLog", CellPos{20, 20, 0}, Rot4::North});
    sim->step();
    CHECK(defnCount(*sim, EventType::CommandFailed) == 0);
    CHECK(sim->construction().siteAt(CellPos{20, 20, 0}) != kNoEntity);

    const Entity wood = sim->things().spawn("Barricade", CellPos{30, 20, 0});  // (the macro runs its argument twice)
    DEFN_REQUIRE(wood != kNoEntity);
    CHECK(sim->map().walkable(CellPos{30, 20, 0}));
    CHECK(sim->map().obstacleDm(CellPos{30, 20, 0}) == 11);
    CHECK(sim->cover().coverHeight(CellPos{29, 20, 0}, Dir8::E) == doctest::Approx(1.1f));
}

TEST_CASE("Defense: the year run's gun post (solar panel, battery, two mini-turrets) keeps its turrets on through the night") {
    auto sim = defnSim();
    // The layout of docs/design/colony-scenario.md: conduits along y = 10, the solar panel at x 13..16, y 11..14, the
    // battery at x 11, y 11..12 and the turrets at the two ends.
    for (int x = 10; x < 20; ++x) {
        sim->things().spawn("PowerConduit", CellPos{x, 10, 0});
    }
    const Entity solar = sim->things().spawn("SolarGenerator", CellPos{13, 11, 0});  // (the macro runs its argument twice)
    const Entity battery = sim->things().spawn("Battery", CellPos{11, 11, 0});
    DEFN_REQUIRE(solar != kNoEntity);
    DEFN_REQUIRE(battery != kNoEntity);
    const Entity west = sim->things().spawn("Turret_Mini", CellPos{10, 11, 0});
    const Entity east = sim->things().spawn("Turret_Mini", CellPos{19, 11, 0});
    DEFN_REQUIRE(west != kNoEntity);
    DEFN_REQUIRE(east != kNoEntity);
    // The first day charges the battery; through the second, night included, both turrets stay powered.
    while (sim->time().seconds < kSecondsPerDay) {
        sim->step();
    }
    int dark = 0;
    while (sim->time().seconds < 2 * kSecondsPerDay) {
        sim->step();
        if (sim->time().seconds % 600 == 0) {
            dark += sim->power().running(west) && sim->power().running(east) ? 0 : 1;
        }
    }
    CHECK(dark == 0);
}

TEST_CASE("Defense: the colony's fighters take no cover within the blast of their own turret") {
    auto sim = defnSim();
    const Entity turret = sim->things().spawn("Turret_Mini", CellPos{30, 30, 0});
    DEFN_REQUIRE(turret != kNoEntity);
    // The mini-turret's blast reaches 3.9 m.
    CHECK(sim->defense().avoidsCell(kPlayerFaction, CellPos{31, 30, 0}));
    CHECK(sim->defense().avoidsCell(kPlayerFaction, CellPos{32, 31, 0}));
    CHECK_FALSE(sim->defense().avoidsCell(kPlayerFaction, CellPos{34, 30, 0}));
    CHECK_FALSE(sim->defense().avoidsCell(2, CellPos{31, 30, 0}));  // raiders don't mind it

    // A drafted rifleman with sandbags 5 m north and an unpowered turret just past their middle takes the sandbags
    // at one end, out of the blast (right behind the middle is the best cover against the enemy 30 m north).
    for (int x = 36; x <= 44; ++x) {
        sim->things().spawn("Sandbags", CellPos{x, 45, 0});
    }
    const Entity guard = sim->things().spawn("Turret_Mini", CellPos{40, 46, 0});
    DEFN_REQUIRE(guard != kNoEntity);
    const Entity rifleman = sim->things().spawn("Human", CellPos{40, 40, 0});
    DEFN_REQUIRE(rifleman != kNoEntity);
    sim->things().registry().emplace<Colonist>(rifleman);
    sim->combat().equip(rifleman, sim->things().make(sim->defs().idOf("Things", "Gun_BoltActionRifle")));
    const Entity enemy = defnHostile(*sim, 41, 72);
    sim->combat().equip(enemy, sim->things().make(sim->defs().idOf("Things", "Gun_Revolver")));
    sim->submit(DraftCommand{rifleman, true});
    for (int i = 0; i < 400; ++i) {
        sim->things().registry().replace<Health>(rifleman, Health{});
        sim->things().registry().replace<Health>(enemy, Health{});
        sim->step();
    }
    const Fighter* f = sim->things().registry().try_get<Fighter>(rifleman);
    DEFN_REQUIRE(f != nullptr);
    CHECK(f->hasSpot);
    MESSAGE("spot ", f->spot.x, ",", f->spot.y);
    CHECK(f->spot.y == 44);  // behind the sandbags
    CHECK(std::hypot(static_cast<float>(f->spot.x) + 0.5f - 40.5f, static_cast<float>(f->spot.y) + 0.5f - 46.5f) > 3.9f);
}

TEST_CASE("Defense: a lancer outranges the mini-turret (32.9 m against 28.9 m) and shoots it to pieces, as in RimWorld") {
    // RimWorld's numbers (wiki): the turret can't hold off a lancer by itself; the colonists have to go out to it.
    auto sim = defnSim();
    const Entity turret = defnPoweredTurret(*sim);  // at 15,10
    const Entity mech = sim->things().spawn("Mech_Lancer", CellPos{15, 43, 0});
    DEFN_REQUIRE(mech != kNoEntity);
    sim->combat().equip(mech, sim->things().make(sim->defs().idOf("Things", "Gun_ChargeLance")));
    sim->submit(SetFactionCommand{mech, 3});
    int turretShots = 0;
    int mechShots = 0;
    for (int i = 0; i < 30 * 120 && sim->things().registry().valid(turret); ++i) {
        sim->step();
        for (const Event& e : sim->events()) {
            turretShots += e.type == EventType::ShotFired && e.thing == turret ? 1 : 0;
            mechShots += e.type == EventType::ShotFired && e.thing == mech ? 1 : 0;
        }
    }
    const Position& p = sim->things().registry().get<Position>(mech);
    const float apart = std::hypot(p.x - 15.5f, p.y - 10.5f);
    MESSAGE("turret shots ", turretShots, ", lancer shots ", mechShots, ", lancer ", apart, " m off");
    CHECK_FALSE(sim->things().registry().valid(turret));
    CHECK(mechShots > 0);
    CHECK(apart > 28.9f);
}

namespace {
// A powered mini-turret at 15,10 with a barricade in every free cell round it (7: the conduit west of it is taken),
// and a lancer `away` metres north of it, hostile. Steps up to two minutes; counts both sides' shots.
struct DefnBarricadeFight {
    int turretShots = 0;
    int mechShots = 0;
    int standing = 0;
    int turretHp = 0;
    bool mechAble = true;
};

DefnBarricadeFight defnBarricadeFight(int away) {
    DefnBarricadeFight out;
    auto sim = defnSim();
    const Entity turret = defnPoweredTurret(*sim);
    std::vector<Entity> barricades;
    for (int dx = -1; dx <= 1; ++dx) {
        for (int dy = -1; dy <= 1; ++dy) {
            if ((dx != 0 || dy != 0) && sim->map().buildingAt(CellPos{15 + dx, 10 + dy, 0}) == kNoEntity) {
                barricades.push_back(sim->things().spawn("Barricade", CellPos{15 + dx, 10 + dy, 0}));
            }
        }
    }
    CHECK(barricades.size() == 7u);
    const Entity mech = sim->things().spawn("Mech_Lancer", CellPos{15, 10 + away, 0});
    CHECK(mech != kNoEntity);
    sim->combat().equip(mech, sim->things().make(sim->defs().idOf("Things", "Gun_ChargeLance")));
    sim->submit(SetFactionCommand{mech, 3});
    for (int i = 0; i < 30 * 120 && sim->things().registry().valid(turret); ++i) {
        sim->step();
        for (const Event& e : sim->events()) {
            out.turretShots += e.type == EventType::ShotFired && e.thing == turret ? 1 : 0;
            out.mechShots += e.type == EventType::ShotFired && e.thing == mech ? 1 : 0;
        }
    }
    out.standing = static_cast<int>(std::count_if(barricades.begin(), barricades.end(), [&](Entity b) { return sim->things().registry().valid(b); }));
    out.turretHp = sim->things().registry().valid(turret) ? sim->things().registry().get<HitPoints>(turret).current : 0;
    const entt::registry& reg = sim->things().registry();
    out.mechAble = reg.valid(mech) && !reg.get<Health>(mech).dead && reg.get<Posture>(mech).stance != Stance::Prone;
    MESSAGE("lancer ", away, " m off: turret shots ", out.turretShots, ", lancer shots ", out.mechShots, ", barricades standing ",
            out.standing, ", turret hp ", out.turretHp, ", lancer ", out.mechAble ? "able" : "down");
    return out;
}
}  // namespace

TEST_CASE("Defense: barricades round the mini-turret hide it from a lancer, and it fires over them") {
    // The player's answer to the lancer's longer reach (RimWorld's too): cover round the turret. The barricades
    // (1.1 m) hide the turret's 0.8 m body from level ground, and its gun (muzzle 1.3 m) fires over them.
    SUBCASE("out of the turret's reach the lancer has nothing to shoot") {
        const DefnBarricadeFight f = defnBarricadeFight(33);
        CHECK(f.mechShots == 0);
        CHECK(f.turretHp == 100);
    }
    SUBCASE("inside it the turret shoots the lancer, which can't hit back") {
        const DefnBarricadeFight f = defnBarricadeFight(26);
        CHECK(f.turretShots > 0);
        CHECK(f.mechShots == 0);
        CHECK(f.turretHp == 100);
        CHECK(f.standing == 7);
    }
}
