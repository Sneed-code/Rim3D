#include "ColonySim/BodyMotion.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Melee.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Souls.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// The running lunge (sneed 2026-11-28, docs/design/souls-combat.md "Running lunges"): "enemies chasing you while you
// back pedal never melee you, they have to stop before attacking. Make it so they run into you and add a lunging attack
// while running, all weapons; they don't stop running until you do. A lunging attack is -6 poise."

#define LUNGE_REQUIRE(...)                                  \
    do {                                                    \
        const bool lungeOk_ = static_cast<bool>(__VA_ARGS__); \
        CHECK_MESSAGE(lungeOk_, #__VA_ARGS__);              \
        if (!lungeOk_) {                                    \
            return;                                         \
        }                                                   \
    } while (0)

namespace {

// A player backing off while locked on walks backwards at about this pace (m/s).
constexpr float kLungeTestBackPace = 2.5f;

std::shared_ptr<const DefDatabase> lungeTestDefs() {
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

std::unique_ptr<Sim> lungeTestSim(uint64_t seed) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = 64;
    settings.flatMap = true;
    return std::make_unique<Sim>(settings, lungeTestDefs());
}

Entity lungeTestPawn(Sim& sim, float x, float y, const char* weapon) {
    const Entity pawn = sim.things().spawn("Human", CellPos{static_cast<int>(x), static_cast<int>(y), 0});
    Position& pos = sim.things().registry().get<Position>(pawn);
    pos.x = x;
    pos.y = y;
    if (weapon != nullptr) {
        sim.combat().equip(pawn, sim.things().make(sim.defs().idOf("Things", weapon)));
    }
    return pawn;
}

Entity lungeTestHero(Sim& sim, float x, float y, const char* weapon) {
    const Entity hero = lungeTestPawn(sim, x, y, weapon);
    sim.things().registry().emplace<Colonist>(hero);
    sim.jobs().addWorker(hero);
    sim.submit(ControlPawnCommand{hero});
    sim.step();
    return hero;
}

void lungeTestFace(Sim& sim, Entity pawn, float x, float y) {
    Posture& p = sim.things().registry().get<Posture>(pawn);
    p.facingX = x;
    p.facingY = y;
}

struct LungeChase {
    int lunges = 0;
    int hits = 0;           // blows by the chaser that met the hero (hit or blocked)
    int firstHitStep = -1;
    float slowestInLunge = 1e9f;  // the chaser's slowest pace (m/s) through a lunge's wind-up
    int lungeWindupSteps = 0;
};

// The hero backs straight off from a raider running at it with `weapon` (steps toward -x each step); counts the raider's
// lunges and blows, and how slow the raider got while winding up a lunge.
LungeChase lungeTestChase(uint64_t seed, const char* weapon, int steps) {
    LungeChase out;
    auto sim = lungeTestSim(seed);
    const Entity hero = lungeTestHero(*sim, 40.5f, 30.5f, nullptr);
    const Entity raider = lungeTestPawn(*sim, 47.5f, 30.5f, weapon);
    sim->submit(SetFactionCommand{raider, 2});
    entt::registry& reg = sim->things().registry();
    Position last = reg.get<Position>(raider);
    for (int s = 0; s < steps; ++s) {
        const Position& h = reg.get<Position>(hero);
        sim->submit(DirectMoveCommand{hero, h.x - kLungeTestBackPace / static_cast<float>(kStepsPerRealSecondAt1x), h.y});
        lungeTestFace(*sim, hero, 1.0f, 0.0f);  // backpedalling: facing the raider
        // Kept alive and on its feet so the chase goes on.
        reg.replace<Health>(hero, Health{});
        sim->health().refresh(hero);
        sim->step();
        for (const Event& e : sim->events()) {
            if (e.type == EventType::MeleeHit && e.thing == raider && e.target == hero) {
                ++out.hits;
                out.firstHitStep = out.firstHitStep < 0 ? s : out.firstHitStep;
            }
        }
        const Position& r = reg.get<Position>(raider);
        const float pace = std::hypot(r.x - last.x, r.y - last.y) * static_cast<float>(kStepsPerRealSecondAt1x);
        last = r;
        const Melee* m = reg.try_get<Melee>(raider);
        if (m != nullptr && m->kind == SoulsAttack::Running && m->phase == MeleePhase::Windup && !m->committed) {
            out.slowestInLunge = std::min(out.slowestInLunge, pace);
            ++out.lungeWindupSteps;
        }
        if (reg.get<Position>(hero).x < 6.0f) {
            break;  // near the map's edge
        }
    }
    out.lunges = static_cast<int>(sim->melee().lunges());
    return out;
}

}  // namespace

TEST_CASE("Lunge: every weapon kind and the bare fists have a running lunge, and it takes 6 poise") {
    auto sim = lungeTestSim(1);
    const SoulsRules& r = sim->melee().souls();
    for (const char* weapon : {"Melee_Gladius", "Melee_Knife", "Melee_Club", "Melee_Mace", "Melee_Longsword", "Melee_BreachAxe",
                               "Melee_Spear", "Melee_Monosword", "Melee_Zeushammer"}) {
        const Entity pawn = lungeTestPawn(*sim, 10.5f, 10.5f, weapon);
        CHECK_MESSAGE(sim->melee().hasLunge(pawn), std::string(weapon));
        sim->things().despawn(pawn);
    }
    const Entity fists = lungeTestPawn(*sim, 12.5f, 10.5f, nullptr);
    CHECK(sim->melee().hasLunge(fists));
    CHECK_FALSE(sim->melee().hasMoveset(fists));  // only its lunge is new: the punches stay as they were
    for (const SoulsMoveset& ms : r.movesets) {
        CHECK_MESSAGE(ms.weaponPoise * r.kinds[static_cast<size_t>(SoulsAttack::Running)].poise == doctest::Approx(6.0f), ms.key);
    }
}

TEST_CASE("Lunge: each weapon kind lunges with its own clip (one hand, two hand, the spear's impale, fists)") {
    auto sim = lungeTestSim(2);
    const BodyMotionSystem& motion = sim->combat().motion();
    const struct {
        const char* weapon;
        const char* clip;
    } expected[] = {{"Melee_Club", "Motion_SS_Attack3"},
                    {"Melee_Gladius", "Motion_SS_Attack3"},
                    {"Melee_Longsword", "Motion_TwoHand_Slash2"},
                    {"Melee_Spear", "Motion_SS_Riposte"},
                    {nullptr, "Motion_Unarmed_Jab"}};
    float x = 10.5f;
    for (const auto& e : expected) {
        const Entity hero = lungeTestHero(*sim, x, 20.5f, e.weapon);
        const Entity foe = lungeTestPawn(*sim, x + 2.5f, 20.5f, nullptr);
        lungeTestFace(*sim, hero, 1.0f, 0.0f);
        lungeTestFace(*sim, foe, -1.0f, 0.0f);
        SoulsPress press;
        press.running = true;
        std::string why;
        CHECK_MESSAGE(sim->melee().soulsAttack(hero, foe, true, press, &why), why);
        CHECK(sim->melee().attackKind(hero) == SoulsAttack::Running);
        const BodyMotion* bm = sim->things().registry().try_get<BodyMotion>(hero);
        LUNGE_REQUIRE(bm != nullptr);
        CHECK_MESSAGE(bm->clip == motion.clipId(e.clip), e.clip);
        x += 8.0f;
    }
}

TEST_CASE("Lunge: a club's lunge breaks 6 of a man's 10 poise, and a standing club blow is still the old swing") {
    auto sim = lungeTestSim(3);
    const Entity hero = lungeTestHero(*sim, 30.5f, 30.5f, "Melee_Club");
    const Entity foe = lungeTestPawn(*sim, 32.3f, 30.5f, nullptr);
    lungeTestFace(*sim, hero, 1.0f, 0.0f);
    lungeTestFace(*sim, foe, -1.0f, 0.0f);
    entt::registry& reg = sim->things().registry();
    SoulsPress press;
    press.running = true;
    std::string why;
    LUNGE_REQUIRE(sim->melee().soulsAttack(hero, foe, true, press, &why));
    bool landed = false;
    for (int s = 0; s < 90 && !landed; ++s) {
        sim->step();
        for (const Event& e : sim->events()) {
            landed = landed || (e.type == EventType::MeleeHit && e.thing == hero && e.target == foe && e.message == "hit");
        }
    }
    LUNGE_REQUIRE(landed);
    const PoiseMeter* p = reg.try_get<PoiseMeter>(foe);
    LUNGE_REQUIRE(p != nullptr);
    CHECK(p->value == doctest::Approx(4.0f));
    // A press standing still is the club's old swing (no moveset for it yet).
    for (int s = 0; s < 120 && (!sim->melee().ready(hero) || sim->melee().inOpening(hero)); ++s) {
        sim->step();
    }
    LUNGE_REQUIRE(sim->melee().soulsAttack(hero, foe, true, SoulsPress{}, &why));
    CHECK(sim->melee().attackKind(hero) == SoulsAttack::None);
}

TEST_CASE("Lunge: the lunge keeps the pace it ran at and slows into the blow, it doesn't stop dead") {
    auto sim = lungeTestSim(4);
    const Entity hero = lungeTestHero(*sim, 30.5f, 30.5f, "Melee_Club");
    const Entity foe = lungeTestPawn(*sim, 34.5f, 30.5f, nullptr);
    lungeTestFace(*sim, hero, 1.0f, 0.0f);
    lungeTestFace(*sim, foe, -1.0f, 0.0f);
    entt::registry& reg = sim->things().registry();
    // Running at 4.5 m/s when it presses.
    Mover& mv = reg.get<Mover>(hero);
    mv.vx = 4.5f / static_cast<float>(kStepsPerRealSecondAt1x);
    mv.vy = 0.0f;
    SoulsPress press;
    press.running = true;
    std::string why;
    LUNGE_REQUIRE(sim->melee().soulsAttack(hero, foe, true, press, &why));
    float lastX = reg.get<Position>(hero).x;
    float firstPace = -1.0f;
    float biggestChange = 0.0f;
    float lastPace = -1.0f;
    for (int s = 0; s < 90 && reg.get<Melee>(hero).phase != MeleePhase::Ready; ++s) {
        sim->step();
        const float x = reg.get<Position>(hero).x;
        const float pace = (x - lastX) * static_cast<float>(kStepsPerRealSecondAt1x);
        lastX = x;
        firstPace = firstPace < 0.0f ? pace : firstPace;
        if (lastPace >= 0.0f && reg.get<Melee>(hero).phase == MeleePhase::Windup) {
            biggestChange = std::max(biggestChange, std::fabs(pace - lastPace));
        }
        lastPace = pace;
    }
    MESSAGE("first step " << firstPace << " m/s, biggest change in a step " << biggestChange << " m/s");
    CHECK(firstPace > 3.0f);  // still running as the lunge begins
    CHECK(biggestChange < 1.6f);  // no stop dead or jerk (the clip's own steps vary a little)
    const Position& a = reg.get<Position>(hero);
    const Position& b = reg.get<Position>(foe);
    CHECK(std::hypot(a.x - b.x, a.y - b.y) > 0.4f);  // not run through it
}

TEST_CASE("Lunge: raiders chasing a colonist who backs off run into it and land blows, with every weapon kind") {
    for (const char* weapon : {"Melee_Club", "Melee_Gladius", "Melee_Longsword", "Melee_Spear"}) {
        const LungeChase c = lungeTestChase(7, weapon, 600);
        MESSAGE(std::string(weapon) << ": lunges " << c.lunges << ", blows " << c.hits << " (first at step " << c.firstHitStep
                       << "), slowest in a lunge " << c.slowestInLunge << " m/s over " << c.lungeWindupSteps << " steps");
        CHECK_MESSAGE(c.lunges >= 1, std::string(weapon));
        CHECK_MESSAGE(c.hits >= 1, std::string(weapon));
        if (c.lungeWindupSteps > 0) {
            CHECK_MESSAGE(c.slowestInLunge > 0.5f, std::string(weapon));  // it never stood still winding up a lunge
        }
    }
}

TEST_CASE("Lunge: an unarmed brawler chases and lunges with its fists the same way") {
    auto sim = lungeTestSim(8);
    const Entity hero = lungeTestHero(*sim, 40.5f, 30.5f, nullptr);
    const Entity raider = lungeTestPawn(*sim, 46.5f, 30.5f, nullptr);
    sim->submit(SetFactionCommand{raider, 2});
    sim->submit(SetFightResponseCommand{raider, FightResponse::Fists});
    entt::registry& reg = sim->things().registry();
    int hits = 0;
    for (int s = 0; s < 600 && reg.get<Position>(hero).x > 6.0f; ++s) {
        const Position& h = reg.get<Position>(hero);
        sim->submit(DirectMoveCommand{hero, h.x - kLungeTestBackPace / static_cast<float>(kStepsPerRealSecondAt1x), h.y});
        lungeTestFace(*sim, hero, 1.0f, 0.0f);
        reg.replace<Health>(hero, Health{});
        sim->health().refresh(hero);
        sim->step();
        for (const Event& e : sim->events()) {
            hits += e.type == EventType::MeleeHit && e.thing == raider && e.target == hero ? 1 : 0;
        }
    }
    MESSAGE("lunges " << sim->melee().lunges() << ", blows " << hits);
    CHECK(sim->melee().lunges() >= 1);
    CHECK(hits >= 1);
}

TEST_CASE("Lunge: a lunge under way comes back from a save as it was, and goes on the same") {
    auto sim = lungeTestSim(9);
    const Entity hero = lungeTestHero(*sim, 30.5f, 30.5f, "Melee_Mace");
    const Entity foe = lungeTestPawn(*sim, 33.5f, 30.5f, nullptr);
    lungeTestFace(*sim, hero, 1.0f, 0.0f);
    lungeTestFace(*sim, foe, -1.0f, 0.0f);
    entt::registry& reg = sim->things().registry();
    reg.get<Mover>(hero).vx = 4.0f / static_cast<float>(kStepsPerRealSecondAt1x);
    SoulsPress press;
    press.running = true;
    std::string why;
    LUNGE_REQUIRE(sim->melee().soulsAttack(hero, foe, true, press, &why));
    for (int s = 0; s < 5; ++s) {
        sim->step();
    }
    CHECK(reg.get<Melee>(hero).lungeSpeed > 0.0f);
    SaveLoadResult result = loadSim(saveSim(*sim), lungeTestDefs());
    LUNGE_REQUIRE(result.sim != nullptr);
    Sim* loaded = result.sim.get();
    CHECK(loaded->things().registry().get<Melee>(hero).lungeSpeed == doctest::Approx(reg.get<Melee>(hero).lungeSpeed));
    CHECK(loaded->stateHash() == sim->stateHash());
    for (int s = 0; s < 40; ++s) {
        sim->step();
        loaded->step();
    }
    CHECK(loaded->stateHash() == sim->stateHash());
}
