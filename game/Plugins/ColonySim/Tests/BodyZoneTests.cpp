// Body zones (docs/design/body-health.md): sneed's head, torso, arm and leg hit points, downing, unconsciousness,
// bleeding by zone and the 3000 blood store, on the game's real Human body.
#include "ColonySim/Apparel.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Melee.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Events.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Patch.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Save.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

namespace {

std::shared_ptr<const DefDatabase> zoneTestDefs() {
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

std::unique_ptr<Sim> zoneTestSim() {
    SimSettings settings;
    settings.seed = 11;
    settings.mapSize = 64;
    settings.flatMap = true;
    settings.pawns = 0;
    settings.items = 0;
    return std::make_unique<Sim>(settings, zoneTestDefs());
}

// A parka keeps the weather off (C15).
// `held`: under the player's hand, so the AI doesn't patch it (part B) and a test can watch it bleed.
Entity zoneTestPawn(Sim& sim, int x = 10, int y = 10, bool held = false) {
    const Entity pawn = sim.things().spawn("Human", CellPos{x, y, 0});
    if (held) {
        sim.things().registry().emplace<Colonist>(pawn);
        sim.jobs().addWorker(pawn);
        sim.things().registry().emplace<PlayerControl>(pawn);
    }
    std::string why;
    const Entity parka =
        sim.apparel().make(sim.defs().idOf("Things", "Apparel_Parka"), sim.defs().idOf("Things", "Cloth"), -1, CellPos{x, y, 0}, &why);
    sim.apparel().putOn(pawn, parka, why);
    return pawn;
}

int zoneTestPart(const Sim& sim, Entity pawn, const std::string& label) {
    const std::vector<std::string> labels = sim.health().partLabels(sim.things().registry().get<Thing>(pawn).def);
    for (size_t i = 0; i < labels.size(); ++i) {
        if (labels[i] == label) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool zoneTestHit(Sim& sim, Entity pawn, const char* type, float amount, const std::string& label) {
    Rng rng(5);
    const int part = zoneTestPart(sim, pawn, label);
    return part >= 0 && sim.health().applyDamage(pawn, sim.health().damageTypeId(type), amount, part, rng);
}

// The zone of that name, the first if there are two (the left arm, the left leg).
int zoneTestZone(const Sim& sim, Entity pawn, const std::string& name, int nth = 0) {
    const std::vector<std::string> names = sim.health().zoneNames(pawn);
    for (size_t i = 0; i < names.size(); ++i) {
        if (names[i] == name && nth-- == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

const Health& zoneTestHealth(const Sim& sim, Entity pawn) { return sim.things().registry().get<Health>(pawn); }

void zoneTestRun(Sim& sim, int steps) {
    for (int i = 0; i < steps; ++i) {
        sim.step();
    }
}

}  // namespace

TEST_CASE("Body zones: a person has a head, a torso, two arms and two legs, with sneed's hit points") {
    auto sim = zoneTestSim();
    const Entity pawn = zoneTestPawn(*sim);
    std::vector<std::string> names = sim->health().zoneNames(pawn);
    std::sort(names.begin(), names.end());
    CHECK(names == std::vector<std::string>{"Arm", "Arm", "Head", "Leg", "Leg", "Torso"});
    // Each zone's root has the zone's hit points; the parts in it are scaled the same way, organs too.
    CHECK(sim->health().partMaxHitPoints(pawn, zoneTestPart(*sim, pawn, "torso")) == doctest::Approx(100.0f));
    CHECK(sim->health().partMaxHitPoints(pawn, zoneTestPart(*sim, pawn, "neck")) == doctest::Approx(50.0f));
    CHECK(sim->health().partMaxHitPoints(pawn, zoneTestPart(*sim, pawn, "left shoulder")) == doctest::Approx(50.0f));
    CHECK(sim->health().partMaxHitPoints(pawn, zoneTestPart(*sim, pawn, "left leg")) == doctest::Approx(75.0f));
    CHECK(sim->health().partMaxHitPoints(pawn, zoneTestPart(*sim, pawn, "heart")) == doctest::Approx(15.0f * 2.5f));
    for (size_t z = 0; z < names.size(); ++z) {
        CHECK(sim->health().zoneHealth(pawn, static_cast<int>(z)) == 1.0f);
    }
    CHECK(sim->health().blood(pawn) == doctest::Approx(3000.0f));
    CHECK_FALSE(sim->health().unconscious(pawn));
    // Each part knows its zone, the left arm and leg before the right (Unreal shows a hit on the zone it landed in).
    const std::vector<std::string> order = sim->health().zoneNames(pawn);
    const int leftArm = sim->health().partZone(pawn, zoneTestPart(*sim, pawn, "left shoulder"));
    const int rightArm = sim->health().partZone(pawn, zoneTestPart(*sim, pawn, "right shoulder"));
    const int leftLeg = sim->health().partZone(pawn, zoneTestPart(*sim, pawn, "left leg"));
    const int rightLeg = sim->health().partZone(pawn, zoneTestPart(*sim, pawn, "right leg"));
    CHECK(leftArm >= 0);
    CHECK(leftArm < rightArm);
    CHECK(leftLeg >= 0);
    CHECK(leftLeg < rightLeg);
    if (leftArm >= 0 && rightLeg >= 0 && static_cast<size_t>(rightLeg) < order.size()) {
        CHECK(order[static_cast<size_t>(leftArm)] == "Arm");
        CHECK(order[static_cast<size_t>(rightLeg)] == "Leg");
    }
    CHECK(order[static_cast<size_t>(sim->health().partZone(pawn, zoneTestPart(*sim, pawn, "heart")))] == "Torso");
    CHECK(sim->health().partZone(pawn, -1) == -1);

    // Animals keep RimWorld's rules.
    const Entity muffalo = sim->things().spawn("Muffalo", CellPos{20, 20, 0});
    if (muffalo != kNoEntity && sim->health().tracks(muffalo)) {
        CHECK(sim->health().zoneNames(muffalo).empty());
    }
}

TEST_CASE("Body zones: a hurt zone bleeds until it is patched, then the blood comes back") {
    auto sim = zoneTestSim();
    const Entity pawn = zoneTestPawn(*sim, 10, 10, true);
    const int arm = zoneTestZone(*sim, pawn, "Arm");
    // 10 of 50 on the arm: 80%, no bleeding.
    CHECK(zoneTestHit(*sim, pawn, "Cut", 10.0f, "left hand"));
    const float handHit = 50.0f - 50.0f * sim->health().zoneHealth(pawn, arm);
    CHECK(handHit == doctest::Approx(10.0f));
    CHECK(zoneTestHealth(*sim, pawn).bleedPerDay == 0.0f);
    zoneTestRun(*sim, 30);
    CHECK(sim->health().blood(pawn) == doctest::Approx(3000.0f));

    // 10 more on the arm: 60% (less the little it healed meanwhile), 1 blood a real second (30 steps).
    CHECK(zoneTestHit(*sim, pawn, "Cut", 10.0f, "left arm"));
    CHECK(sim->health().zoneHealth(pawn, arm) == doctest::Approx(0.6f).epsilon(0.01));
    zoneTestRun(*sim, 300);
    CHECK(sim->health().blood(pawn) == doctest::Approx(2990.0f).epsilon(0.0005));

    // 20 more: 20%, 3 a second.
    CHECK(zoneTestHit(*sim, pawn, "Cut", 20.0f, "left arm"));
    CHECK(sim->health().zoneHealth(pawn, arm) == doctest::Approx(0.2f).epsilon(0.03));
    const float before = sim->health().blood(pawn);
    zoneTestRun(*sim, 300);
    CHECK(before - sim->health().blood(pawn) == doctest::Approx(30.0f).epsilon(0.01));
    // An arm doesn't down anybody by itself.
    CHECK_FALSE(zoneTestHealth(*sim, pawn).downed);

    // Patched: the bleeding stops; the blood comes back at 10 a second from 30 seconds on.
    CHECK(sim->health().tendAll(pawn, 0.5f) > 0);
    zoneTestRun(*sim, 1);
    const float patched = sim->health().blood(pawn);
    CHECK(zoneTestHealth(*sim, pawn).bleedPerDay == 0.0f);
    zoneTestRun(*sim, 29 * 30);
    CHECK(sim->health().blood(pawn) == doctest::Approx(patched));
    zoneTestRun(*sim, 31 * 30);
    CHECK(sim->health().blood(pawn) > patched + 5.0f);
    zoneTestRun(*sim, 60 * 30);
    CHECK(sim->health().blood(pawn) == doctest::Approx(3000.0f));
}

TEST_CASE("Body zones: a leg, the torso or the head under a quarter downs a person, so do three zones under half") {
    SUBCASE("a leg") {
        auto sim = zoneTestSim();
        const Entity pawn = zoneTestPawn(*sim);
        CHECK(zoneTestHit(*sim, pawn, "Blunt", 50.0f, "left leg"));
        CHECK_FALSE(zoneTestHealth(*sim, pawn).downed);  // a third left
        CHECK(zoneTestHit(*sim, pawn, "Blunt", 10.0f, "left leg"));
        CHECK(zoneTestHealth(*sim, pawn).downed);  // a fifth
        CHECK_FALSE(sim->health().unconscious(pawn));
    }
    SUBCASE("the torso") {
        auto sim = zoneTestSim();
        const Entity pawn = zoneTestPawn(*sim);
        CHECK(zoneTestHit(*sim, pawn, "Blunt", 70.0f, "torso"));
        CHECK_FALSE(zoneTestHealth(*sim, pawn).downed);
        CHECK(zoneTestHit(*sim, pawn, "Blunt", 10.0f, "torso"));
        CHECK(zoneTestHealth(*sim, pawn).downed);
        CHECK_FALSE(zoneTestHealth(*sim, pawn).dead);
        CHECK_FALSE(sim->health().unconscious(pawn));
    }
    SUBCASE("the head knocks out") {
        auto sim = zoneTestSim();
        const Entity pawn = zoneTestPawn(*sim);
        CHECK(zoneTestHit(*sim, pawn, "Blunt", 20.0f, "neck"));
        CHECK(zoneTestHit(*sim, pawn, "Blunt", 20.0f, "jaw"));
        CHECK(zoneTestHealth(*sim, pawn).downed);
        CHECK(sim->health().unconscious(pawn));
        CHECK_FALSE(zoneTestHealth(*sim, pawn).dead);
    }
    SUBCASE("three zones under half") {
        auto sim = zoneTestSim();
        const Entity pawn = zoneTestPawn(*sim);
        CHECK(zoneTestHit(*sim, pawn, "Blunt", 26.0f, "left arm"));
        CHECK(zoneTestHit(*sim, pawn, "Blunt", 26.0f, "right arm"));
        CHECK_FALSE(zoneTestHealth(*sim, pawn).downed);
        CHECK(zoneTestHit(*sim, pawn, "Blunt", 40.0f, "right leg"));
        CHECK(zoneTestHealth(*sim, pawn).downed);
        CHECK_FALSE(sim->health().unconscious(pawn));
    }
}

TEST_CASE("Body zones: bleeding out knocks a person out at 1000 blood, kills at 0, and they wake above 1000") {
    auto sim = zoneTestSim();
    const Entity pawn = zoneTestPawn(*sim, 10, 10, true);
    Health& health = sim->things().registry().get<Health>(pawn);
    CHECK(zoneTestHit(*sim, pawn, "Cut", 20.0f, "left arm"));  // 60%: 1 a second
    health.bloodLoss = 1.0f - 1003.0f / 3000.0f;
    zoneTestRun(*sim, 30);
    CHECK_FALSE(sim->health().unconscious(pawn));
    zoneTestRun(*sim, 90);
    CHECK(sim->health().blood(pawn) <= 1000.0f);
    CHECK(sim->health().unconscious(pawn));
    CHECK(health.downed);

    // Patched, it wakes once the blood is back over 1000.
    sim->health().tendAll(pawn, 0.5f);
    zoneTestRun(*sim, 31 * 30);
    CHECK_FALSE(sim->health().unconscious(pawn));
    CHECK_FALSE(health.downed);

    // Not patched, it bleeds to death.
    auto sim2 = zoneTestSim();
    const Entity other = zoneTestPawn(*sim2, 10, 10, true);
    CHECK(zoneTestHit(*sim2, other, "Cut", 45.0f, "left leg"));  // 40%: 2 a second
    sim2->things().registry().get<Health>(other).bloodLoss = 1.0f - 60.0f / 3000.0f;
    zoneTestRun(*sim2, 31 * 30);
    CHECK(zoneTestHealth(*sim2, other).dead);
    CHECK(zoneTestHealth(*sim2, other).cause == DeathCause::BloodLoss);
}

TEST_CASE("Body zones: the protagonist is never downed and dies when 100 hit points are gone") {
    auto sim = zoneTestSim();
    const Entity hero = zoneTestPawn(*sim);
    sim->things().registry().get<Health>(hero).protagonist = true;
    sim->health().refresh(hero);
    CHECK(sim->health().partMaxHitPoints(hero, zoneTestPart(*sim, hero, "torso")) == doctest::Approx(100.0f));
    CHECK(zoneTestHit(*sim, hero, "Blunt", 60.0f, "left leg"));  // would down anybody else
    CHECK(zoneTestHit(*sim, hero, "Blunt", 30.0f, "torso"));
    CHECK_FALSE(zoneTestHealth(*sim, hero).downed);
    CHECK_FALSE(zoneTestHealth(*sim, hero).dead);
    CHECK(zoneTestHit(*sim, hero, "Blunt", 12.0f, "right arm"));
    CHECK(zoneTestHealth(*sim, hero).dead);
    CHECK(zoneTestHealth(*sim, hero).cause == DeathCause::Wounds);
}

TEST_CASE("Body zones: when the bleeding stopped is saved, so the blood comes back on time after a load") {
    auto sim = zoneTestSim();
    const Entity pawn = zoneTestPawn(*sim, 10, 10, true);
    CHECK(zoneTestHit(*sim, pawn, "Cut", 30.0f, "left arm"));
    zoneTestRun(*sim, 600);
    sim->health().tendAll(pawn, 0.5f);
    zoneTestRun(*sim, 300);
    const int64_t stopped = zoneTestHealth(*sim, pawn).bleedStoppedAt;
    CHECK(stopped >= 0);
    SaveLoadResult loaded = loadSim(saveSim(*sim), zoneTestDefs());
    CHECK(loaded.error.empty());
    if (loaded.sim == nullptr) {
        return;
    }
    CHECK(zoneTestHealth(*loaded.sim, pawn).bleedStoppedAt == stopped);
    CHECK(loaded.sim->stateHash() == sim->stateHash());
    zoneTestRun(*sim, 1200);
    zoneTestRun(*loaded.sim, 1200);
    CHECK(loaded.sim->health().blood(pawn) == sim->health().blood(pawn));
    CHECK(loaded.sim->stateHash() == sim->stateHash());
}

// ---- Part B: patching ------------------------------------------------------------------------------

TEST_CASE("Patching: the player's pawn patches its worst bleeding zone on command, in 1 to 3 seconds") {
    auto sim = zoneTestSim();
    const Entity pawn = zoneTestPawn(*sim);
    sim->things().registry().emplace<Colonist>(pawn);
    sim->jobs().addWorker(pawn);
    sim->things().registry().emplace<PlayerControl>(pawn);
    // Nothing bleeds yet.
    sim->submit(PatchCommand{pawn});
    sim->step();
    bool failed = false;
    for (const Event& e : sim->events()) {
        failed = failed || e.type == EventType::CommandFailed;
    }
    CHECK(failed);

    CHECK(zoneTestHit(*sim, pawn, "Cut", 10.0f, "left hand"));  // 80%: the arm doesn't bleed
    CHECK(zoneTestHit(*sim, pawn, "Cut", 33.0f, "left leg"));   // 56%: light
    CHECK(zoneTestHit(*sim, pawn, "Cut", 20.0f, "right arm"));  // 60%: light
    CHECK(zoneTestHit(*sim, pawn, "Cut", 20.0f, "right arm"));  // 20%: heavy
    const int arm = zoneTestZone(*sim, pawn, "Arm", 1);
    const int leg = zoneTestZone(*sim, pawn, "Leg");
    CHECK(sim->health().zoneBleedLevel(pawn, arm) == 3);
    CHECK(sim->health().zoneBleedLevel(pawn, leg) == 1);
    CHECK(sim->health().worstBleedingZone(pawn) == arm);
    // The AI leaves the player's pawn alone.
    zoneTestRun(*sim, 40);
    CHECK_FALSE(sim->patches().patching(pawn));

    sim->submit(PatchCommand{pawn});
    sim->step();
    CHECK(sim->patches().patching(pawn));
    bool started = false;
    for (const Event& e : sim->events()) {
        started = started || (e.type == EventType::PawnAction && e.action == PawnAction::Patch && e.part == arm && e.amount == 90.0f);
    }
    CHECK(started);
    zoneTestRun(*sim, 88);
    CHECK(sim->health().zoneBleedLevel(pawn, arm) == 3);  // 3 seconds for a heavy bleed
    zoneTestRun(*sim, 2);
    CHECK_FALSE(sim->patches().patching(pawn));
    CHECK(sim->health().zoneBleedLevel(pawn, arm) == 0);
    CHECK(sim->health().zoneBleedLevel(pawn, leg) == 1);  // one zone at a time
    sim->submit(PatchCommand{pawn});
    zoneTestRun(*sim, 32);
    CHECK(sim->health().worstBleedingZone(pawn) < 0);
    CHECK(zoneTestHealth(*sim, pawn).bleedPerDay == 0.0f);
}

TEST_CASE("Patching: a colonist going about its work patches itself; a fighter only with no enemy seen near") {
    auto sim = zoneTestSim();
    const Entity worker = zoneTestPawn(*sim);
    CHECK(zoneTestHit(*sim, worker, "Cut", 30.0f, "left arm"));  // 40%: moderate, 2 seconds
    zoneTestRun(*sim, 15 + 60 + 2);
    CHECK(sim->health().worstBleedingZone(worker) < 0);

    // A fighter with an enemy in sight 10 m off waits; once it has seen none for 2 seconds it patches.
    const Entity fighter = zoneTestPawn(*sim, 20, 20);
    const Entity enemy = zoneTestPawn(*sim, 30, 20);
    entt::registry& reg = sim->things().registry();
    Fighter& f = reg.emplace<Fighter>(fighter);
    f.known.push_back(KnownEnemy{enemy, 30.5f, 20.5f, 0, sim->time().steps});
    CHECK(zoneTestHit(*sim, fighter, "Cut", 30.0f, "left arm"));
    CHECK_FALSE(sim->patches().safe(fighter));
    // An enemy seen far off (over 30 m) doesn't count.
    reg.get<Fighter>(fighter).known[0].x = 60.5f;
    CHECK(sim->patches().safe(fighter));
    reg.get<Fighter>(fighter).known[0].x = 30.5f;
    reg.get<Fighter>(fighter).known[0].seenAt = sim->time().steps - 61;
    CHECK(sim->patches().safe(fighter));
}

TEST_CASE("Patching: a fighter bleeding under 1500 blood falls back to patch; above it, it fights on") {
    auto sim = zoneTestSim();
    const Entity pawn = zoneTestPawn(*sim);
    Health& health = sim->things().registry().get<Health>(pawn);
    CHECK(zoneTestHit(*sim, pawn, "Cut", 20.0f, "left arm"));  // bleeding, light
    health.bloodLoss = 1.0f - 2200.0f / 3000.0f;
    sim->health().refresh(pawn);
    CHECK_FALSE(sim->fighters().badlyHurt(pawn));
    health.bloodLoss = 1.0f - 1400.0f / 3000.0f;
    sim->health().refresh(pawn);
    CHECK(sim->fighters().badlyHurt(pawn));
    sim->health().patchZone(pawn, sim->health().worstBleedingZone(pawn), 0.3f);
    health.bloodLoss = 1.0f - 2200.0f / 3000.0f;
    sim->health().refresh(pawn);
    CHECK_FALSE(sim->fighters().badlyHurt(pawn));  // patched
}

TEST_CASE("Patching: a patch under way saves and loads, and finishes the same") {
    auto sim = zoneTestSim();
    const Entity pawn = zoneTestPawn(*sim);
    sim->things().registry().emplace<Colonist>(pawn);
    sim->jobs().addWorker(pawn);
    sim->things().registry().emplace<PlayerControl>(pawn);
    CHECK(zoneTestHit(*sim, pawn, "Cut", 40.0f, "left arm"));  // 20%: heavy
    sim->submit(PatchCommand{pawn});
    zoneTestRun(*sim, 30);
    CHECK(sim->patches().patching(pawn));
    SaveLoadResult loaded = loadSim(saveSim(*sim), zoneTestDefs());
    CHECK_MESSAGE(loaded.error.empty(), loaded.error);
    if (loaded.sim == nullptr) {
        return;
    }
    CHECK(loaded.sim->patches().patching(pawn));
    CHECK(loaded.sim->stateHash() == sim->stateHash());
    zoneTestRun(*sim, 100);
    zoneTestRun(*loaded.sim, 100);
    CHECK_FALSE(loaded.sim->patches().patching(pawn));
    CHECK(loaded.sim->health().worstBleedingZone(pawn) < 0);
    CHECK(loaded.sim->stateHash() == sim->stateHash());
}

// ---- Part C: poise, staggers and sweeping blows (docs/design/body-health.md "Part C as built").

TEST_CASE("Poise: a blow harder than 50 knocks a person down, then nothing can for 30 seconds") {
    auto sim = zoneTestSim();
    const Entity pawn = zoneTestPawn(*sim, 10, 10, true);
    sim->step();
    const int torso = zoneTestPart(*sim, pawn, "torso");
    CHECK(torso >= 0);
    if (!(torso >= 0)) {
        return;
    }
    const DefId blunt = sim->defs().idOf("DamageTypes", "Blunt");
    const DefId cut = sim->defs().idOf("DamageTypes", "Cut");
    // Under the poise: no knock-down. A blunt blow counts half again (35 blunt is 52.5).
    CHECK(sim->combat().jolt(pawn, torso, cut, 45.0f, 1.0f));
    CHECK(sim->combat().knockedDownFor(pawn) == 0);
    sim->combat().jolt(pawn, torso, blunt, 35.0f, 1.0f);
    CHECK(sim->combat().knockedDownFor(pawn) > 0);
    CHECK(sim->combat().poiseImmuneFor(pawn) == 900);
    CHECK(std::any_of(sim->events().begin(), sim->events().end(), [](const Event& e) { return e.type == EventType::PawnKnockedDown; }));
    // It gets up, and for the rest of the 30 seconds an even harder blow doesn't knock it down again.
    while (sim->combat().knockedDownFor(pawn) > 0) {
        sim->step();
    }
    sim->combat().jolt(pawn, torso, cut, 200.0f, 1.0f);
    CHECK(sim->combat().knockedDownFor(pawn) == 0);
    while (sim->combat().poiseImmuneFor(pawn) > 0) {
        sim->step();
    }
    sim->combat().jolt(pawn, torso, cut, 60.0f, 1.0f);
    CHECK(sim->combat().knockedDownFor(pawn) > 0);
}

TEST_CASE("Poise: a hit staggers only if it took over a tenth of the zone, and at most once in 5 seconds") {
    auto sim = zoneTestSim();
    const Entity pawn = zoneTestPawn(*sim, 10, 10, true);
    sim->step();
    const int torso = zoneTestPart(*sim, pawn, "torso");
    const int leg = zoneTestPart(*sim, pawn, "left leg");
    CHECK(torso >= 0);
    if (!(torso >= 0)) {
        return;
    }
    CHECK(leg >= 0);
    if (!(leg >= 0)) {
        return;
    }
    const DefId cut = sim->defs().idOf("DamageTypes", "Cut");
    // Torso 100: 10 is not over a tenth, 11 is. Leg 75: 8 is.
    sim->combat().jolt(pawn, torso, cut, 10.0f, 10.0f);
    CHECK(sim->combat().staggeredFor(pawn) == 0);
    sim->combat().jolt(pawn, leg, cut, 8.0f, 8.0f);
    CHECK(sim->combat().staggeredFor(pawn) > 0);
    CHECK(sim->combat().staggerWaitFor(pawn) == 150);
    while (sim->combat().staggeredFor(pawn) > 0) {
        sim->step();
    }
    sim->combat().jolt(pawn, torso, cut, 30.0f, 30.0f);
    CHECK(sim->combat().staggeredFor(pawn) == 0);  // too soon
    while (sim->combat().staggerWaitFor(pawn) > 0) {
        sim->step();
    }
    sim->combat().jolt(pawn, torso, cut, 30.0f, 30.0f);
    CHECK(sim->combat().staggeredFor(pawn) > 0);
}

TEST_CASE("Poise: the knock-down and the stagger wait save and load") {
    auto sim = zoneTestSim();
    const Entity pawn = zoneTestPawn(*sim, 10, 10, true);
    sim->step();
    const int torso = zoneTestPart(*sim, pawn, "torso");
    sim->combat().jolt(pawn, torso, sim->defs().idOf("DamageTypes", "Blunt"), 60.0f, 20.0f);
    sim->stepMany(5);
    const int immune = sim->combat().poiseImmuneFor(pawn);
    CHECK(immune > 0);
    if (!(immune > 0)) {
        return;
    }
    SaveLoadResult loaded = loadSim(saveSim(*sim), zoneTestDefs());
    CHECK_MESSAGE(loaded.error.empty(), loaded.error);
    CHECK(loaded.sim != nullptr);
    if (!(loaded.sim != nullptr)) {
        return;
    }
    CHECK(loaded.sim->combat().poiseImmuneFor(pawn) == immune);
    CHECK(loaded.sim->combat().knockedDownFor(pawn) == sim->combat().knockedDownFor(pawn));
    CHECK(loaded.sim->stateHash() == sim->stateHash());
}

TEST_CASE("Sweeping blows: a hammer or a mace meets a man beside its aimed line") {
    const auto swing = [](const char* weapon) {
        auto sim = zoneTestSim();
        const Entity hero = sim->things().spawn("Human", CellPos{30, 30, 0});
        const Entity dummy = sim->things().spawn("Human", CellPos{31, 30, 0});
        entt::registry& reg = sim->things().registry();
        reg.get<Position>(hero).x = 30.5f;
        reg.get<Position>(hero).y = 30.5f;
        reg.get<Position>(dummy).x = 31.3f;   // 0.9 m away, 27 degrees off the hero's facing
        reg.get<Position>(dummy).y = 30.91f;
        sim->combat().equip(hero, sim->things().make(sim->defs().idOf("Things", weapon)));
        reg.emplace<Colonist>(hero);
        sim->jobs().addWorker(hero);
        sim->submit(ControlPawnCommand{hero});
        sim->step();
        reg.get<Posture>(hero).facingX = 1.0f;
        reg.get<Posture>(hero).facingY = 0.0f;
        // Both tools of each are blunt, so every swing sweeps.
        sim->submit(MeleeCommand{hero, kNoEntity, MeleeMode::Attack});
        std::string outcome;
        for (int s = 0; s < 60 && outcome.empty(); ++s) {
            sim->step();
            for (const Event& e : sim->events()) {
                if (e.type == EventType::MeleeHit && e.thing == hero) {
                    outcome = e.message;
                }
            }
        }
        return outcome;
    };
    CHECK(swing("Melee_Zeushammer") == "hit");
    CHECK(swing("Melee_Mace") == "hit");
}

TEST_CASE("Body zones: a person on their feet limps at 40% or better, however much it hurts") {
    auto sim = zoneTestSim();
    const Entity pawn = zoneTestPawn(*sim, 10, 10, true);
    sim->step();
    // Bruises all over: lots of pain, nothing under a quarter, so still on their feet.
    for (int round = 0; round < 3; ++round) {
        for (const char* part : {"left leg", "right leg", "torso"}) {
            CHECK(zoneTestHit(*sim, pawn, "Blunt", 14.0f, part));
        }
    }
    const Health& h = sim->things().registry().get<Health>(pawn);
    MESSAGE("pain " << h.pain << ", moving " << h.movingFactor);
    CHECK_FALSE(sim->health().incapacitated(pawn));
    CHECK(h.movingFactor >= 0.4f - 1e-4f);
}

// ---- Part D: crawling away.

TEST_CASE("Crawling: a person downed by the torso crawls away from an enemy near; by a leg, or with nobody near, lies still") {
    const auto run = [](const char* part, float amount, bool enemy) {
        auto sim = zoneTestSim();
        const Entity pawn = zoneTestPawn(*sim, 20, 20);
        Entity raider = kNoEntity;
        if (enemy) {
            raider = sim->things().spawn("Human", CellPos{15, 20, 0});
            sim->submit(SetFactionCommand{raider, 2});
        }
        sim->step();
        if (raider != kNoEntity) {
            sim->things().registry().emplace<Stunned>(raider).stepsLeft = 100000;  // stands where it is
        }
        CHECK(zoneTestHit(*sim, pawn, "Blunt", amount, part));
        sim->step();
        CHECK(sim->health().incapacitated(pawn));
        const Position start = sim->things().registry().get<Position>(pawn);
        sim->stepMany(600);
        const Position& end = sim->things().registry().get<Position>(pawn);
        return std::hypot(end.x - start.x, end.y - start.y) * (end.x >= start.x ? 1.0f : -1.0f);
    };
    const float crawled = run("torso", 80.0f, true);
    MESSAGE("crawled " << crawled << " m in 20 s");
    CHECK(crawled > 1.5f);   // away from the raider (it stands to the west)
    CHECK(crawled < 9.0f);   // and slowly (a tenth of its walk, a cell at a time)
    CHECK(std::abs(run("torso", 80.0f, false)) < 0.01f);
    CHECK(std::abs(run("left leg", 60.0f, true)) < 0.01f);
}

TEST_CASE("Crawling: the snapshot tells Unreal who crawls") {
    auto sim = zoneTestSim();
    const Entity pawn = zoneTestPawn(*sim, 20, 20);
    sim->step();
    CHECK(zoneTestHit(*sim, pawn, "Blunt", 80.0f, "torso"));
    sim->step();
    Snapshot snap;
    sim->writeSnapshot(snap);
    const ThingView* view = snap.find(pawn);
    CHECK(view != nullptr);
    CHECK((view != nullptr && view->crawling));
}
