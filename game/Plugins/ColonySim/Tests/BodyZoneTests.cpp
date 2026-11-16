// Body zones (docs/design/body-health.md): sneed's head, torso, arm and leg hit points, downing, unconsciousness,
// bleeding by zone and the 3000 blood store, on the game's real Human body.
#include "ColonySim/Apparel.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Health.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
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
Entity zoneTestPawn(Sim& sim, int x = 10, int y = 10) {
    const Entity pawn = sim.things().spawn("Human", CellPos{x, y, 0});
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

    // Animals keep RimWorld's rules.
    const Entity muffalo = sim->things().spawn("Muffalo", CellPos{20, 20, 0});
    if (muffalo != kNoEntity && sim->health().tracks(muffalo)) {
        CHECK(sim->health().zoneNames(muffalo).empty());
    }
}

TEST_CASE("Body zones: a hurt zone bleeds until it is patched, then the blood comes back") {
    auto sim = zoneTestSim();
    const Entity pawn = zoneTestPawn(*sim);
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
    const Entity pawn = zoneTestPawn(*sim);
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
    const Entity other = zoneTestPawn(*sim2);
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
    const Entity pawn = zoneTestPawn(*sim);
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
