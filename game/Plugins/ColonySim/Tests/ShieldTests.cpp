// Shields (sneed, 2026-11-15, docs/design/shields.md).
#include "ColonySim/Apparel.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Health.h"
#include "ColonySim/Melee.h"
#include "ColonySim/Raids.h"
#include "ColonySim/Random.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <memory>
#include <string>
#include <vector>

using namespace colony;

#define SHIELD_REQUIRE(...)   \
    do {                      \
        CHECK(__VA_ARGS__);   \
        if (!(__VA_ARGS__)) { \
            return;           \
        }                     \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> shieldTestDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        for (const std::string& e : errors) {
            MESSAGE(e);
        }
        return db;
    }();
    return defs;
}

std::unique_ptr<Sim> shieldTestSim(uint64_t seed = 31) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = 64;
    settings.flatMap = true;
    return std::make_unique<Sim>(settings, shieldTestDefs());
}

DefId shieldDef(const Sim& sim, const char* name) {
    return sim.defs().idOf("Things", name);
}

Entity shieldMake(Sim& sim, const char* def, const char* stuff, CellPos cell) {
    return sim.apparel().make(shieldDef(sim, def), shieldDef(sim, stuff), -1, cell);
}

Entity shieldPawn(Sim& sim, int x, int y) {
    return sim.things().spawn("Human", CellPos{x, y, 0});
}

Entity shieldWear(Sim& sim, Entity pawn, const char* def, const char* stuff) {
    const Position& at = sim.things().registry().get<Position>(pawn);
    const Entity item = shieldMake(sim, def, stuff, cellOf(at));
    std::string why;
    if (item == kNoEntity || !sim.apparel().putOn(pawn, item, why)) {
        FAIL_CHECK("could not put on ", def, ": ", why);
    }
    return item;
}

Entity shieldHold(Sim& sim, Entity pawn, const char* weapon) {
    const Entity item = sim.things().make(shieldDef(sim, weapon));
    CHECK(sim.combat().equip(pawn, item));
    return item;
}

// Faces the pawn along +x.
void shieldFaceEast(Sim& sim, Entity pawn) {
    Posture& p = sim.things().registry().get<Posture>(pawn);
    p.facingX = 1.0f;
    p.facingY = 0.0f;
}

}  // namespace

TEST_CASE("Shields: sizes double protection, a steel tower shield equals steel plate armour, materials scale it") {
    auto sim = shieldTestSim();
    const entt::registry& reg = sim->things().registry();
    const ApparelStats& stats = sim->apparel().stats();
    const Entity tower = shieldMake(*sim, "Apparel_ShieldTower", "Steel", CellPos{10, 10, 0});
    const Entity medium = shieldMake(*sim, "Apparel_ShieldMedium", "Steel", CellPos{11, 10, 0});
    const Entity small = shieldMake(*sim, "Apparel_ShieldSmall", "Steel", CellPos{12, 10, 0});
    const Entity plate = sim->apparel().make(shieldDef(*sim, "Apparel_PlateArmor"), shieldDef(*sim, "Steel"), -1, CellPos{13, 10, 0});
    SHIELD_REQUIRE((tower != kNoEntity && medium != kNoEntity && small != kNoEntity && plate != kNoEntity));
    for (int c = 0; c < kDamageClasses; ++c) {
        CHECK(stats.mitigation(reg, tower).pct[c] == doctest::Approx(stats.mitigation(reg, plate).pct[c]));
        CHECK(stats.mitigation(reg, medium).pct[c] == doctest::Approx(stats.mitigation(reg, tower).pct[c] / 2.0f));
        CHECK(stats.mitigation(reg, small).pct[c] == doctest::Approx(stats.mitigation(reg, tower).pct[c] / 4.0f));
    }
    CHECK(stats.mitigation(reg, small).pct[0] == doctest::Approx(5.0f));  // sneed: "if small did 5% tower is 20%"
    CHECK(stats.isShield(shieldDef(*sim, "Apparel_ShieldTower")));
    CHECK_FALSE(stats.isShield(shieldDef(*sim, "Apparel_PlateArmor")));
    CHECK(stats.catchChance(shieldDef(*sim, "Apparel_ShieldSmall")) == doctest::Approx(0.6f));
    CHECK(stats.catchChance(shieldDef(*sim, "Apparel_ShieldMedium")) == doctest::Approx(0.8f));
    CHECK(stats.catchChance(shieldDef(*sim, "Apparel_ShieldTower")) == doctest::Approx(0.95f));
    // Steel: 3 / 6 / 12 kg, flat block 2, 70 / 140 / 280 hit points.
    CHECK(stats.weightKg(reg, small) == doctest::Approx(3.0f));
    CHECK(stats.weightKg(reg, tower) == doctest::Approx(12.0f));
    CHECK(stats.shieldBlock(reg, tower) == doctest::Approx(2.0f));
    CHECK(reg.get<HitPoints>(tower).max == 280);

    // Wood: no elemental, 60% of the sharp, and the fire weakness by size; light, flat block 1.
    const Entity woodSmall = shieldMake(*sim, "Apparel_ShieldSmall", "WoodLog", CellPos{14, 10, 0});
    const Entity woodTower = shieldMake(*sim, "Apparel_ShieldTower", "WoodLog", CellPos{15, 10, 0});
    CHECK(stats.mitigation(reg, woodTower).pct[1] == doctest::Approx(18.0f));
    CHECK(stats.mitigation(reg, woodTower).pct[2] == doctest::Approx(0.0f));
    CHECK(stats.mitigation(reg, woodTower).pct[3] == doctest::Approx(-10.0f));
    CHECK(stats.mitigation(reg, woodSmall).pct[3] == doctest::Approx(-2.5f));
    CHECK(stats.weightKg(reg, woodTower) == doctest::Approx(9.6f));
    CHECK(stats.shieldBlock(reg, woodSmall) == doctest::Approx(1.0f));
    // Plasteel blocks 3 and is light; uranium is heavy.
    const Entity plasteel = shieldMake(*sim, "Apparel_ShieldTower", "Plasteel", CellPos{16, 10, 0});
    const Entity uranium = shieldMake(*sim, "Apparel_ShieldTower", "Uranium", CellPos{17, 10, 0});
    CHECK(stats.mitigation(reg, plasteel).pct[1] == doctest::Approx(37.5f));
    CHECK(stats.shieldBlock(reg, plasteel) == doctest::Approx(3.0f));
    CHECK(stats.weightKg(reg, plasteel) == doctest::Approx(8.4f));
    CHECK(stats.weightKg(reg, uranium) == doctest::Approx(24.0f));
    CHECK(stats.shieldBlock(reg, uranium) == doctest::Approx(2.5f));
}

TEST_CASE("Shields: a shield works like armour, weighs on walking and attacks, and a two-handed weapon slings it") {
    auto sim = shieldTestSim();
    const entt::registry& reg = sim->things().registry();
    const ApparelStats& stats = sim->apparel().stats();
    const Entity pawn = shieldPawn(*sim, 20, 20);
    const DefId blunt = sim->defs().idOf("DamageTypes", "Blunt");
    const float bare = sim->combat().mitigate(pawn, blunt, 10.0f);
    const Entity shield = shieldWear(*sim, pawn, "Apparel_ShieldTower", "Steel");
    // A steel tower shield: blunt 20%, on every hit like a vest.
    CHECK(sim->combat().mitigate(pawn, blunt, 10.0f) == doctest::Approx(bare / 1.2f));
    CHECK(stats.activeShield(reg, pawn) == shield);
    CHECK(stats.armourKg(reg, pawn) == doctest::Approx(12.0f));
    CHECK(stats.moveFactor(reg, pawn) == doctest::Approx(1.0f / 1.12f));
    CHECK(stats.attackTimeFactor(reg, pawn) == doctest::Approx(1.12f));
    // A one-handed weapon keeps it on the arm.
    shieldHold(*sim, pawn, "Gun_Revolver");
    CHECK(stats.activeShield(reg, pawn) == shield);
    // A bow or rifle needs both hands: the shield hangs on the back, still weighs, protects nothing, can't block.
    shieldHold(*sim, pawn, "Gun_RecurveBow");
    CHECK(stats.shieldWorn(reg, pawn) == shield);
    CHECK(stats.activeShield(reg, pawn) == kNoEntity);
    CHECK(sim->combat().mitigate(pawn, blunt, 10.0f) == doctest::Approx(bare));
    CHECK(stats.armourKg(reg, pawn) == doctest::Approx(12.0f));
    std::string why;
    CHECK_FALSE(sim->combat().setBlocking(pawn, true, &why));
    CHECK(why.find("two-handed") != std::string::npos);
    sim->step();
    Snapshot snap;
    sim->writeSnapshot(snap);
    bool seen = false;
    for (const ThingView& v : snap.things) {
        if (v.id == pawn) {
            seen = true;
            CHECK(v.shieldSlung);
            CHECK_FALSE(v.shieldUp);
        }
    }
    CHECK(seen);
    // Every two-handed weapon in the data.
    for (const char* name : {"Gun_BoltActionRifle", "Gun_AssaultRifle", "Gun_PumpShotgun", "Gun_LMG", "Gun_ShortBow", "Gun_Greatbow",
                             "Gun_TripleRocketLauncher", "Melee_Longsword"}) {
        CHECK_MESSAGE(stats.twoHanded(shieldDef(*sim, name)), name);
    }
    for (const char* name : {"Gun_Revolver", "Gun_Autopistol", "Gun_MachinePistol", "Melee_Knife", "Melee_Club", "Melee_Mace",
                             "Melee_Spear", "Melee_Gladius", "Gun_Pila"}) {
        CHECK_FALSE_MESSAGE(stats.twoHanded(shieldDef(*sim, name)), name);
    }
}

TEST_CASE("Shields: a hit on a raised shield loses 40% and the flat block, never below 1 from 6 up; only from the front") {
    auto sim = shieldTestSim();
    const entt::registry& reg = sim->things().registry();
    const DefId blunt = sim->defs().idOf("DamageTypes", "Blunt");
    const Entity pawn = shieldPawn(*sim, 20, 20);
    const Entity shield = shieldWear(*sim, pawn, "Apparel_ShieldTower", "Steel");
    shieldFaceEast(*sim, pawn);
    // Lowered: no block roll at all.
    const Position& at = reg.get<Position>(pawn);
    const HitFrom front{true, at.x + 5.0f, at.y};
    const PawnHitResult lowered = sim->combat().hitPawn(pawn, 0, blunt, 12.0f, 0.0f, 7, front);
    CHECK_FALSE(lowered.shieldBlocked);
    SHIELD_REQUIRE(sim->combat().setBlocking(pawn, true, nullptr));
    shieldFaceEast(*sim, pawn);

    // 12 blunt on steel: 12 x 0.6 - 2 = 5.2 reaches the armour (the shield's own 20% then takes 5.2 to 4.33).
    int caught = 0;
    const int tries = 400;
    for (int i = 0; i < tries; ++i) {
        const PawnHitResult r = sim->combat().hitPawn(pawn, 0, blunt, 12.0f, 0.0f, 1000 + static_cast<uint64_t>(i), front);
        if (!r.shieldBlocked) {
            CHECK(r.amount == doctest::Approx(12.0f / 1.2f));
            continue;
        }
        ++caught;
        CHECK(r.shield == shield);
        CHECK(r.shieldTook == doctest::Approx(6.8f));
        CHECK(r.amount == doctest::Approx(5.2f / 1.2f));
        CHECK_FALSE(r.deflected);
    }
    // A tower shield catches 95% of hits from the front.
    CHECK(caught > tries * 90 / 100);
    CHECK(caught < tries);

    // From behind or the side (more than 60 degrees off), never.
    for (const HitFrom from : {HitFrom{true, at.x - 5.0f, at.y}, HitFrom{true, at.x, at.y + 5.0f}, HitFrom{}}) {
        for (int i = 0; i < 50; ++i) {
            CHECK_FALSE(sim->combat().hitPawn(pawn, 0, blunt, 12.0f, 0.0f, 50 + static_cast<uint64_t>(i), from).shieldBlocked);
        }
    }
    // Just inside the arc (55 degrees), it can.
    bool inside = false;
    for (int i = 0; i < 50 && !inside; ++i) {
        const HitFrom slant{true, at.x + 5.0f * 0.5736f, at.y + 5.0f * 0.8192f};
        inside = sim->combat().hitPawn(pawn, 0, blunt, 12.0f, 0.0f, 90 + static_cast<uint64_t>(i), slant).shieldBlocked;
    }
    CHECK(inside);

    // Plasteel blocks 3: a hit of 5 is stopped (5 x 0.6 - 3 = 0), a hit of 6 keeps 1 (6 x 0.6 - 3 = 0.6, floored).
    auto sim2 = shieldTestSim();
    const Entity p2 = shieldPawn(*sim2, 20, 20);
    shieldWear(*sim2, p2, "Apparel_ShieldTower", "Plasteel");
    SHIELD_REQUIRE(sim2->combat().setBlocking(p2, true, nullptr));
    shieldFaceEast(*sim2, p2);
    const Position& at2 = sim2->things().registry().get<Position>(p2);
    const HitFrom front2{true, at2.x + 5.0f, at2.y};
    bool sawFive = false;
    bool sawSix = false;
    for (int i = 0; i < 40; ++i) {
        const PawnHitResult five = sim2->combat().hitPawn(p2, 0, blunt, 5.0f, 0.0f, 300 + static_cast<uint64_t>(i), front2);
        if (five.shieldBlocked) {
            sawFive = true;
            CHECK(five.deflected);
            CHECK(five.amount == doctest::Approx(0.0f));
        }
        const PawnHitResult six = sim2->combat().hitPawn(p2, 0, blunt, 6.0f, 0.0f, 400 + static_cast<uint64_t>(i), front2);
        if (six.shieldBlocked) {
            sawSix = true;
            CHECK_FALSE(six.deflected);
            CHECK(six.shieldTook == doctest::Approx(5.0f));
            CHECK(six.amount == doctest::Approx(1.0f / 1.24f));  // the plasteel shield's own 24% blunt after
        }
    }
    CHECK(sawFive);
    CHECK(sawSix);
}

TEST_CASE("Shields: small shields catch fewer hits; blocked hits wear the shield and send ShieldBlocked") {
    auto sim = shieldTestSim();
    const DefId blunt = sim->defs().idOf("DamageTypes", "Blunt");
    const Entity pawn = shieldPawn(*sim, 20, 20);
    const Entity shield = shieldWear(*sim, pawn, "Apparel_ShieldSmall", "WoodLog");
    SHIELD_REQUIRE(sim->combat().setBlocking(pawn, true, nullptr));
    shieldFaceEast(*sim, pawn);
    const Position& at = sim->things().registry().get<Position>(pawn);
    const HitFrom front{true, at.x + 5.0f, at.y};
    int caught = 0;
    const int tries = 1000;
    for (int i = 0; i < tries; ++i) {
        caught += sim->combat().hitPawn(pawn, 0, blunt, 10.0f, 0.0f, 5000 + static_cast<uint64_t>(i), front).shieldBlocked ? 1 : 0;
    }
    CHECK(caught > tries * 55 / 100);  // 60%
    CHECK(caught < tries * 65 / 100);

    // A real blow: the shield stops 40 x 0.4 + 1 = 17 and wears for it.
    Rng rng(9);
    const float before = ApparelStats::condition(sim->things().registry(), shield);
    sim->step();  // a fresh event list
    bool event = false;
    for (int i = 0; i < 20 && !event; ++i) {
        sim->combat().strikePawn(pawn, blunt, 40.0f, 0.0f, rng, front);
        for (const Event& e : sim->events()) {
            if (e.type == EventType::ShieldBlocked && e.thing == pawn) {
                event = true;
                CHECK(e.target == shield);
                CHECK(e.amount == doctest::Approx(17.0f));
                CHECK(e.message == "softened");
            }
        }
    }
    CHECK(event);
    CHECK(ApparelStats::condition(sim->things().registry(), shield) < before);
}

TEST_CASE("Shields: held up, a pawn can't shoot or swing but can reload; the command needs a shield on the arm") {
    auto sim = shieldTestSim();
    const Entity pawn = shieldPawn(*sim, 20, 20);
    const Entity target = shieldPawn(*sim, 30, 20);
    shieldHold(*sim, pawn, "Gun_Revolver");
    sim->submit(ShieldCommand{pawn, true});
    sim->step();
    bool failed = false;
    for (const Event& e : sim->events()) {
        failed = failed || (e.type == EventType::CommandFailed && e.message == "that pawn has no shield");
    }
    CHECK(failed);
    shieldWear(*sim, pawn, "Apparel_ShieldMedium", "Steel");
    sim->submit(ShieldCommand{pawn, true, true, 0.0f, 1.0f});
    sim->step();
    CHECK(sim->combat().shieldUp(pawn));
    // It faces where the command said.
    CHECK(sim->things().registry().get<Posture>(pawn).facingY == doctest::Approx(1.0f));
    std::string why;
    CHECK_FALSE(sim->combat().shoot(pawn, target, 0.0f, 0.0f, 0.0f, true, &why));
    CHECK(why == "holding up a shield");
    CHECK_FALSE(sim->melee().ready(pawn));
    CHECK_FALSE(sim->melee().attack(pawn, target, true, &why));
    CHECK(why == "holding up a shield");
    // One shot fired before raising it leaves the gun to reload while blocking.
    sim->submit(ShieldCommand{pawn, false});
    sim->step();
    CHECK_FALSE(sim->combat().shieldUp(pawn));
    SHIELD_REQUIRE(sim->combat().shoot(pawn, target, 0.0f, 0.0f, 0.0f, true, &why));
    for (int i = 0; i < 300 && sim->combat().shotReadyIn(pawn) > 0; ++i) {
        sim->step();
    }
    SHIELD_REQUIRE(sim->combat().setBlocking(pawn, true, &why));
    CHECK(sim->combat().reload(pawn, &why));
    // Taking up a two-handed weapon lowers it.
    shieldHold(*sim, pawn, "Gun_AssaultRifle");
    sim->step();
    CHECK_FALSE(sim->combat().shieldUp(pawn));
}

TEST_CASE("Shields: a fighter raises its shield at a shot aimed at it and lowers it to swing") {
    auto sim = shieldTestSim(17);
    entt::registry& reg = sim->things().registry();
    const Entity colonist = shieldPawn(*sim, 20, 20);
    shieldHold(*sim, colonist, "Melee_Mace");
    shieldWear(*sim, colonist, "Apparel_ShieldTower", "Steel");
    const Entity raider = shieldPawn(*sim, 32, 20);
    shieldHold(*sim, raider, "Gun_Revolver");
    sim->submit(DraftCommand{colonist, true});
    sim->submit(SetFactionCommand{raider, 2});
    sim->step();
    reg.get<Fighter>(colonist).defending = true;
    int aimedSteps = 0;
    int raisedWhileAimed = 0;
    int blocks = 0;
    for (int i = 0; i < 600; ++i) {
        sim->step();
        const Shooter* s = reg.try_get<Shooter>(raider);
        if (s != nullptr && s->target == colonist && (s->phase == ShotPhase::Aiming || s->phase == ShotPhase::Firing)) {
            ++aimedSteps;
            if (const Blocking* b = reg.try_get<Blocking>(colonist)) {
                ++raisedWhileAimed;
                CHECK(b->ai);
                CHECK(b->threat == raider);
            }
        }
        for (const Event& e : sim->events()) {
            blocks += e.type == EventType::ShieldBlocked && e.thing == colonist ? 1 : 0;
        }
    }
    SHIELD_REQUIRE(aimedSteps > 0);
    // Raised within a step of seeing the aim, and kept up through it.
    CHECK(raisedWhileAimed >= aimedSteps * 8 / 10);
    MESSAGE(aimedSteps << " steps aimed at, " << raisedWhileAimed << " with the shield up, " << blocks << " shots on the shield");

    // Toe to toe with a knifeman: it still swings (it lowers the shield whenever its mace is ready).
    auto duel = shieldTestSim(18);
    entt::registry& dreg = duel->things().registry();
    const Entity guard = shieldPawn(*duel, 20, 20);
    shieldHold(*duel, guard, "Melee_Mace");
    shieldWear(*duel, guard, "Apparel_ShieldMedium", "Steel");
    const Entity knife = shieldPawn(*duel, 21, 20);
    shieldHold(*duel, knife, "Melee_Knife");
    duel->submit(DraftCommand{guard, true});
    duel->submit(SetFactionCommand{knife, 2});
    duel->step();
    dreg.get<Fighter>(guard).defending = true;
    int swings = 0;
    int raised = 0;
    for (int i = 0; i < 400; ++i) {
        duel->step();
        raised += dreg.all_of<Blocking>(guard) ? 1 : 0;
        for (const Event& e : duel->events()) {
            swings += e.type == EventType::MeleeSwing && e.thing == guard ? 1 : 0;
        }
        if (duel->health().incapacitated(knife) || duel->health().incapacitated(guard)) {
            break;
        }
    }
    MESSAGE(swings << " swings, shield up " << raised << " steps");
    CHECK(swings > 0);
    CHECK(raised > 0);
}

TEST_CASE("Shields: a raised shield saves and loads, and the command records into a replay") {
    SimSettings settings;
    settings.seed = 5;
    settings.mapSize = 64;
    settings.flatMap = true;
    Sim original(settings, shieldTestDefs());
    const Entity pawn = shieldPawn(original, 20, 20);
    shieldWear(original, pawn, "Apparel_ShieldSmall", "Steel");
    original.submit(ShieldCommand{pawn, true, true, 1.0f, 1.0f});
    original.step();
    SHIELD_REQUIRE(original.combat().shieldUp(pawn));
    const std::vector<uint8_t> bytes = saveSim(original);
    SaveLoadResult loaded = loadSim(bytes, shieldTestDefs());
    SHIELD_REQUIRE(loaded.error.empty());
    SHIELD_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->stateHash() == original.stateHash());
    CHECK(loaded.sim->combat().shieldUp(pawn));
    const Blocking& b = loaded.sim->things().registry().get<Blocking>(pawn);
    CHECK(b.hasDir);
    CHECK(b.dirX == doctest::Approx(0.7071f));
    original.submit(ShieldCommand{pawn, false});
    for (int i = 0; i < 30; ++i) {
        original.step();
    }

    const Replay recorded = recordReplay(original);
    std::string error;
    Replay parsed;
    SHIELD_REQUIRE(readReplay(writeReplay(recorded), parsed, error));
    CHECK(parsed.commands.size() == recorded.commands.size());
    Command command;
    CHECK(commandFromName("Shield", command));
    CHECK(std::string(commandName(command)) == "Shield");
}

TEST_CASE("Shields: clan warriors raid with wooden shields") {
    auto sim = shieldTestSim(23);
    sim->submit(StartRaidCommand{"ClanSavage", 3000.0f, 0});
    sim->step();
    entt::registry& reg = sim->things().registry();
    const ApparelStats& stats = sim->apparel().stats();
    int raiders = 0;
    int shields = 0;
    for (const auto [pawn, raider] : reg.view<const Raider>().each()) {
        (void)raider;
        ++raiders;
        const Entity shield = stats.shieldWorn(reg, pawn);
        if (shield == kNoEntity) {
            continue;
        }
        ++shields;
        CHECK(reg.get<MadeOf>(shield).stuff == shieldDef(*sim, "WoodLog"));
        CHECK(reg.get<HitPoints>(shield).max == stats.maxHitPoints(reg.get<Thing>(shield).def, shieldDef(*sim, "WoodLog")));
        CHECK(stats.mitigation(reg, shield).pct[0] == doctest::Approx(10.0f));
    }
    MESSAGE(raiders << " clan raiders, " << shields << " with shields");
    CHECK(raiders > 4);
    CHECK(shields > 0);
    CHECK(shields < raiders);
}
