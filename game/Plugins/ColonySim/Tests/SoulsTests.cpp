#include "ColonySim/Apparel.h"
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
#include "ColonySim/Snapshot.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Souls.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Elden Ring style melee (sneed 2026-11-27, docs/design/souls-combat.md): stamina, poise, light and heavy combos,
// charged heavies, the guard and its break, staggers and knockdowns that can't be chained.

#define SOULS_REQUIRE(...)                                  \
    do {                                                    \
        const bool soulsOk_ = static_cast<bool>(__VA_ARGS__); \
        CHECK_MESSAGE(soulsOk_, #__VA_ARGS__);              \
        if (!soulsOk_) {                                    \
            return;                                         \
        }                                                   \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> soulsTestDefs() {
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

std::unique_ptr<Sim> soulsTestSim(uint64_t seed) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = 64;
    settings.flatMap = true;
    return std::make_unique<Sim>(settings, soulsTestDefs());
}

Entity soulsTestPawn(Sim& sim, float x, float y, const char* weapon) {
    const Entity pawn = sim.things().spawn("Human", CellPos{static_cast<int>(x), static_cast<int>(y), 0});
    Position& pos = sim.things().registry().get<Position>(pawn);
    pos.x = x;
    pos.y = y;
    if (weapon != nullptr) {
        sim.combat().equip(pawn, sim.things().make(sim.defs().idOf("Things", weapon)));
    }
    return pawn;
}

Entity soulsTestHero(Sim& sim, float x, float y, const char* weapon) {
    const Entity hero = soulsTestPawn(sim, x, y, weapon);
    sim.things().registry().emplace<Colonist>(hero);
    sim.jobs().addWorker(hero);
    sim.submit(ControlPawnCommand{hero});
    sim.step();
    return hero;
}

Entity soulsTestShield(Sim& sim, Entity pawn) {
    const Position& at = sim.things().registry().get<Position>(pawn);
    const Entity item = sim.apparel().make(sim.defs().idOf("Things", "Apparel_ShieldMedium"), sim.defs().idOf("Things", "Steel"), -1, cellOf(at));
    std::string why;
    if (item == kNoEntity || !sim.apparel().putOn(pawn, item, why)) {
        FAIL_CHECK("could not put on a shield: ", why);
    }
    return item;
}

void soulsTestFace(Sim& sim, Entity pawn, float x, float y) {
    Posture& p = sim.things().registry().get<Posture>(pawn);
    p.facingX = x;
    p.facingY = y;
}

void soulsTestHeal(Sim& sim, Entity pawn) {
    sim.things().registry().replace<Health>(pawn, Health{});
    sim.health().refresh(pawn);
}

// Steps until the pawn's attack (and any combo after it, and the opening after that) is over, at most `most`.
int soulsTestRunOut(Sim& sim, Entity pawn, int most = 200) {
    int steps = 0;
    while (steps < most && (!sim.melee().ready(pawn) || sim.melee().inOpening(pawn))) {  // and its opening (sneed 2026-11-28)
        sim.step();
        ++steps;
    }
    return steps;
}

}  // namespace

TEST_CASE("Souls: the rules come from the data, and the sword has a moveset") {
    auto sim = soulsTestSim(1);
    const SoulsRules& r = sim->melee().souls();
    CHECK(r.enabled);
    CHECK(r.kinds[static_cast<size_t>(SoulsAttack::Light)].stamina == doctest::Approx(10.0f));  // sneed's numbers
    CHECK(r.kinds[static_cast<size_t>(SoulsAttack::Heavy)].stamina == doctest::Approx(20.0f));
    CHECK(r.rollCost == doctest::Approx(30.0f));
    CHECK(r.blockShare == doctest::Approx(0.5f));
    CHECK(r.regenSeconds == doctest::Approx(2.0f));
    const Entity swordsman = soulsTestPawn(*sim, 20.5f, 20.5f, "Melee_Gladius");
    const Entity spearman = soulsTestPawn(*sim, 24.5f, 20.5f, "Melee_Spear");
    CHECK(sim->melee().hasMoveset(swordsman));
    CHECK_FALSE(sim->melee().hasMoveset(spearman));  // one weapon first (sneed): the rest swing as before, for stamina
    CHECK(sim->melee().staminaShare(swordsman) == doctest::Approx(1.0f));
    CHECK(sim->melee().poiseShare(swordsman) == doctest::Approx(1.0f));
    CHECK(sim->melee().poiseMax(swordsman) == doctest::Approx(r.personPoise));
}

TEST_CASE("Souls: a light attack costs 10, a heavy 20, a roll 30, and the bar refills in 2 s of rest") {
    auto sim = soulsTestSim(2);
    const Entity hero = soulsTestHero(*sim, 30.5f, 30.5f, "Melee_Gladius");
    soulsTestFace(*sim, hero, 1.0f, 0.0f);
    std::string why;
    SOULS_REQUIRE(sim->melee().soulsAttack(hero, kNoEntity, true, SoulsPress{}, &why));
    CHECK(sim->melee().stamina(hero) == doctest::Approx(90.0f));
    CHECK(sim->melee().attackKind(hero) == SoulsAttack::Light);
    // Nothing comes back while it swings.
    for (int s = 0; s < 6; ++s) {
        sim->step();
    }
    CHECK(sim->melee().stamina(hero) == doctest::Approx(90.0f));
    soulsTestRunOut(*sim, hero);
    // About 2 s for the whole bar: 10 points in about 0.2 s.
    int refill = 0;
    while (sim->melee().stamina(hero) < 99.99f && refill < 200) {
        sim->step();
        ++refill;
    }
    CHECK(refill <= 8);
    SoulsPress heavy;
    heavy.heavy = true;
    SOULS_REQUIRE(sim->melee().soulsAttack(hero, kNoEntity, true, heavy, &why));
    CHECK(sim->melee().stamina(hero) == doctest::Approx(80.0f));
    CHECK(sim->melee().attackKind(hero) == SoulsAttack::Heavy);
    soulsTestRunOut(*sim, hero);
    sim->things().registry().remove<Stamina>(hero);
    SOULS_REQUIRE(sim->combat().dodgeRoll(hero, 0.0f, 1.0f, &why));
    CHECK(sim->melee().stamina(hero) == doctest::Approx(70.0f));
    // From empty, the whole bar comes back in 2 s.
    for (int s = 0; s < 120 && sim->combat().rollLocked(hero); ++s) {
        sim->step();
    }
    sim->things().registry().emplace_or_replace<Stamina>(hero, Stamina{0.0f, sim->time().steps});
    int full = 0;
    while (sim->melee().stamina(hero) < 99.99f && full < 200) {
        sim->step();
        ++full;
    }
    CHECK(full >= 58);
    CHECK(full <= 62);
}

TEST_CASE("Souls: out of stamina nothing starts, and a raised guard stops the refill") {
    auto sim = soulsTestSim(3);
    const Entity hero = soulsTestHero(*sim, 30.5f, 30.5f, "Melee_Gladius");
    soulsTestShield(*sim, hero);
    sim->things().registry().emplace_or_replace<Stamina>(hero, Stamina{0.0f, sim->time().steps});
    std::string why;
    CHECK_FALSE(sim->melee().soulsAttack(hero, kNoEntity, true, SoulsPress{}, &why));
    CHECK(why == "out of stamina");
    CHECK_FALSE(sim->combat().dodgeRoll(hero, 1.0f, 0.0f, &why));
    CHECK(why == "out of stamina");
    // With 1 point an action still goes (Elden Ring: anything above zero).
    sim->things().registry().replace<Stamina>(hero, Stamina{1.0f, sim->time().steps});
    sim->submit(ShieldCommand{hero, true});
    sim->step();
    SOULS_REQUIRE(sim->combat().shieldUp(hero));
    const float before = sim->melee().stamina(hero);
    for (int s = 0; s < 60; ++s) {
        sim->step();
    }
    CHECK(sim->melee().stamina(hero) == doctest::Approx(before));  // guard up: no refill (sneed, for now)
    sim->submit(ShieldCommand{hero, false});
    for (int s = 0; s < 30; ++s) {
        sim->step();
    }
    CHECK(sim->melee().stamina(hero) > before + 30.0f);
}

TEST_CASE("Souls: presses during a light attack chain the combo, each blow the next clip of the chain") {
    auto sim = soulsTestSim(4);
    const Entity hero = soulsTestHero(*sim, 30.5f, 30.5f, "Melee_Gladius");
    soulsTestFace(*sim, hero, 1.0f, 0.0f);
    std::string why;
    SOULS_REQUIRE(sim->melee().soulsAttack(hero, kNoEntity, true, SoulsPress{}, &why));
    std::vector<int> combos{static_cast<int>(sim->things().registry().get<Melee>(hero).combo)};
    uint64_t seen = sim->melee().swings();
    const auto note = [&] {
        if (sim->melee().swings() > seen) {
            seen = sim->melee().swings();
            combos.push_back(sim->things().registry().get<Melee>(hero).combo);
        }
    };
    for (int s = 0; s < 200 && combos.size() < 3; ++s) {
        // The button pressed again every few steps, as a player mashing it does: kept until the chain opens.
        if (s % 5 == 0) {
            CHECK(sim->melee().soulsAttack(hero, kNoEntity, true, SoulsPress{}, &why));
            note();
        }
        sim->step();
        note();
    }
    SOULS_REQUIRE(combos.size() == 3);
    CHECK(combos[0] == 0);
    CHECK(combos[1] == 1);
    CHECK(combos[2] == 2);
    CHECK(sim->melee().stamina(hero) <= 70.5f);  // 10 each (a press kept from the mashing may add a fourth)
    // After the chain lets go, the next press starts over.
    soulsTestRunOut(*sim, hero);
    for (int s = 0; s < 30; ++s) {
        sim->step();
    }
    SOULS_REQUIRE(sim->melee().soulsAttack(hero, kNoEntity, true, SoulsPress{}, &why));
    CHECK(sim->things().registry().get<Melee>(hero).combo == 0);
}

TEST_CASE("Souls: an attack carries the whole body: the walking keys don't move it until the cancel point") {
    auto sim = soulsTestSim(5);
    const Entity hero = soulsTestHero(*sim, 30.5f, 30.5f, "Melee_Gladius");
    soulsTestFace(*sim, hero, 1.0f, 0.0f);
    std::string why;
    SOULS_REQUIRE(sim->melee().soulsAttack(hero, kNoEntity, true, SoulsPress{}, &why));
    const Melee& m = sim->things().registry().get<Melee>(hero);
    const int windup = m.windupSteps;
    // Walking sideways (+y) the whole wind-up: the body goes only where the clip takes it (forward, +x).
    const Position& pos = sim->things().registry().get<Position>(hero);
    const float startY = pos.y;
    for (int s = 0; s < windup; ++s) {
        sim->submit(DirectMoveCommand{hero, pos.x, pos.y + 0.1f});
        sim->step();
    }
    CHECK(std::fabs(pos.y - startY) < 0.05f);
    const BodyMotion* bodyClip = sim->things().registry().try_get<BodyMotion>(hero);
    CHECK((bodyClip == nullptr || !bodyClip->freeFeet));
    // A roll is refused mid-swing...
    CHECK_FALSE(sim->combat().dodgeRoll(hero, 0.0f, 1.0f, &why));
    CHECK(why == "mid-attack");
    // ...but past the cancel point it cuts the recovery short.
    int s = 0;
    while (!sim->combat().dodgeRoll(hero, 0.0f, 1.0f, &why) && s < 120) {
        sim->step();
        ++s;
    }
    CHECK(sim->combat().rollLocked(hero));
}

TEST_CASE("Souls: a held heavy stops at its charge point and lets go as a charged heavy") {
    auto sim = soulsTestSim(6);
    const Entity hero = soulsTestHero(*sim, 30.5f, 30.5f, "Melee_Gladius");
    soulsTestFace(*sim, hero, 1.0f, 0.0f);
    std::string why;
    SoulsPress press;
    press.heavy = true;
    press.hold = true;
    SOULS_REQUIRE(sim->melee().soulsAttack(hero, kNoEntity, true, press, &why));
    int s = 0;
    while (!sim->melee().charging(hero) && s < 60) {
        sim->step();
        ++s;
    }
    SOULS_REQUIRE(sim->melee().charging(hero));
    const int left = sim->things().registry().get<Melee>(hero).stepsLeft;
    for (int i = 0; i < 20; ++i) {
        sim->step();
    }
    CHECK(sim->things().registry().get<Melee>(hero).stepsLeft == left);  // held
    if (const BodyMotion* bm = sim->things().registry().try_get<BodyMotion>(hero)) {
        CHECK(bm->rate == doctest::Approx(0.0f));  // the body holds the blow drawn back
    }
    sim->submit(MeleeCommand{hero, kNoEntity, MeleeMode::Release});
    sim->step();
    sim->step();
    CHECK_FALSE(sim->melee().charging(hero));
    CHECK(sim->melee().attackKind(hero) == SoulsAttack::Charged);
    CHECK(sim->melee().stamina(hero) < 80.0f);  // a charged heavy costs more than a heavy
}

TEST_CASE("Souls: a raised shield takes a blow for half its stamina cost, and breaks when the bar runs out") {
    auto sim = soulsTestSim(7);
    const Entity hero = soulsTestHero(*sim, 30.5f, 30.5f, "Melee_Gladius");
    soulsTestShield(*sim, hero);
    soulsTestFace(*sim, hero, 1.0f, 0.0f);
    const Entity foe = soulsTestPawn(*sim, 31.6f, 30.5f, "Melee_Gladius");
    soulsTestFace(*sim, foe, -1.0f, 0.0f);
    sim->submit(ShieldCommand{hero, true, true, 1.0f, 0.0f});
    sim->step();
    SOULS_REQUIRE(sim->combat().shieldUp(hero));
    std::string why;
    SOULS_REQUIRE(sim->melee().soulsAttack(foe, hero, false, SoulsPress{}, &why));
    int blocked = 0;
    for (int s = 0; s < 60 && blocked == 0; ++s) {
        sim->submit(ShieldCommand{hero, true, true, 1.0f, 0.0f});
        sim->step();
        for (const Event& e : sim->events()) {
            blocked += e.type == EventType::MeleeHit && e.thing == foe && e.message == "blocked" ? 1 : 0;
        }
    }
    SOULS_REQUIRE(blocked == 1);
    CHECK(sim->melee().stamina(hero) == doctest::Approx(95.0f));  // half the light attack's 10
    HealthReport report;
    sim->health().report(hero, report);
    CHECK(report.injuries.empty());  // a steel shield stops all of it
    CHECK(sim->melee().counterOpen(hero));  // a heavy now is a guard counter
    // Nearly out of stamina: the next blow breaks the guard.
    soulsTestRunOut(*sim, foe);
    sim->things().registry().emplace_or_replace<Stamina>(hero, Stamina{3.0f, sim->time().steps});
    sim->things().registry().remove<Stamina>(foe);
    for (int s = 0; s < 40; ++s) {
        sim->submit(ShieldCommand{hero, true, true, 1.0f, 0.0f});
        sim->step();
    }
    SOULS_REQUIRE(sim->melee().soulsAttack(foe, hero, false, SoulsPress{}, &why));
    int broke = 0;
    for (int s = 0; s < 60 && broke == 0; ++s) {
        sim->step();
        for (const Event& e : sim->events()) {
            broke += e.type == EventType::MeleeHit && e.thing == foe && e.message == "blocked" && e.count == 1 ? 1 : 0;
        }
    }
    CHECK(broke == 1);
    CHECK_FALSE(sim->combat().shieldUp(hero));
    CHECK(sim->melee().soulsStunned(hero));
    // Reeling, the guard can't come back up.
    sim->submit(ShieldCommand{hero, true, true, 1.0f, 0.0f});
    sim->step();
    CHECK_FALSE(sim->combat().shieldUp(hero));
}

TEST_CASE("Souls: three light hits break a man's poise into a stagger that stops his swing; knockdowns can't be chained") {
    auto sim = soulsTestSim(8);
    const Entity hero = soulsTestHero(*sim, 30.5f, 30.5f, "Melee_Gladius");
    soulsTestFace(*sim, hero, 1.0f, 0.0f);
    const Entity foe = soulsTestPawn(*sim, 31.5f, 30.5f, "Melee_Gladius");
    soulsTestFace(*sim, foe, -1.0f, 0.0f);
    entt::registry& reg = sim->things().registry();
    std::string why;
    int hits = 0;
    bool staggered = false;
    for (int i = 0; i < 6 && !staggered; ++i) {
        reg.get<Position>(hero).x = 30.5f;
        reg.get<Position>(hero).y = 30.5f;
        reg.get<Position>(foe).x = 31.5f;
        reg.get<Position>(foe).y = 30.5f;
        soulsTestHeal(*sim, foe);
        reg.remove<Stamina>(hero);
        soulsTestRunOut(*sim, hero);
        SOULS_REQUIRE(sim->melee().soulsAttack(hero, foe, true, SoulsPress{}, &why));
        for (int s = 0; s < 40; ++s) {
            sim->step();
            for (const Event& e : sim->events()) {
                hits += e.type == EventType::MeleeHit && e.thing == hero && e.message == "hit" ? 1 : 0;
                staggered = staggered || (e.type == EventType::PawnStaggered && e.thing == foe);
            }
            if (staggered) {
                break;
            }
        }
    }
    MESSAGE(hits << " hits to the stagger");
    CHECK(staggered);
    CHECK(hits == 3);  // 12 poise a light hit against a man's 30
    CHECK(sim->melee().soulsStunned(foe));
    CHECK_FALSE(sim->melee().ready(foe));
    CHECK(sim->melee().poiseShare(foe) == doctest::Approx(1.0f));  // a break refills it
    // A second break soon after the first knocks down; a third right after can't (knockdown immunity). The stagger is
    // cut short first: a light press at a reeling man is a riposte, not a poise blow.
    reg.get<PoiseMeter>(foe).value = 1.0f;
    reg.get<Melee>(foe).stunUntil = 0;
    for (int i = 0; i < 40 && !reg.all_of<KnockedDown>(foe); ++i) {
        reg.remove<Stamina>(hero);
        soulsTestRunOut(*sim, hero);
        reg.get<Position>(hero).x = reg.get<Position>(foe).x - 1.0f;
        reg.get<Position>(hero).y = reg.get<Position>(foe).y;
        soulsTestHeal(*sim, foe);
        reg.get<Melee>(foe).stunUntil = 0;  // not reeling: a poise blow, not a riposte
        soulsTestFace(*sim, foe, -1.0f, 0.0f);
        if (sim->melee().soulsAttack(hero, foe, true, SoulsPress{}, &why)) {
            for (int s = 0; s < 30 && !reg.all_of<KnockedDown>(foe); ++s) {
                sim->step();
            }
        } else {
            sim->step();
        }
    }
    SOULS_REQUIRE(reg.all_of<KnockedDown>(foe));
    const int64_t immune = reg.get<PoiseMeter>(foe).knockdownImmuneUntil;
    CHECK(immune > sim->time().steps);
    // Up again, broken again at once: a stagger, not another knockdown.
    reg.remove<KnockedDown>(foe);
    reg.get<Posture>(foe).stance = Stance::Standing;
    reg.get<PoiseMeter>(foe).value = 1.0f;
    reg.get<PoiseMeter>(foe).lastBreak = sim->time().steps;
    Melee& fm = reg.get<Melee>(foe);
    fm.stunUntil = 0;
    bool knocked = false;
    bool stagger2 = false;
    for (int i = 0; i < 20 && !stagger2 && !knocked; ++i) {
        reg.remove<Stamina>(hero);
        soulsTestRunOut(*sim, hero);
        reg.get<Position>(hero).x = reg.get<Position>(foe).x - 1.0f;
        reg.get<Position>(hero).y = reg.get<Position>(foe).y;
        soulsTestHeal(*sim, foe);
        reg.get<Melee>(foe).stunUntil = 0;  // not reeling: a poise blow, not a riposte
        soulsTestFace(*sim, foe, -1.0f, 0.0f);
        if (sim->melee().soulsAttack(hero, foe, true, SoulsPress{}, &why)) {
            for (int s = 0; s < 30; ++s) {
                sim->step();
                for (const Event& e : sim->events()) {
                    stagger2 = stagger2 || (e.type == EventType::PawnStaggered && e.thing == foe);
                    knocked = knocked || (e.type == EventType::PawnKnockedDown && e.thing == foe);
                }
            }
        } else {
            sim->step();
        }
    }
    CHECK(stagger2);
    CHECK_FALSE(knocked);
}

TEST_CASE("Souls: a light press at a man reeling from a broken guard is a riposte: unblockable, heavy, and it throws him down") {
    auto sim = soulsTestSim(21);
    const Entity hero = soulsTestHero(*sim, 30.5f, 30.5f, "Melee_Gladius");
    soulsTestFace(*sim, hero, 1.0f, 0.0f);
    const Entity foe = soulsTestPawn(*sim, 31.7f, 30.5f, "Melee_Gladius");
    soulsTestShield(*sim, foe);
    soulsTestFace(*sim, foe, -1.0f, 0.0f);
    entt::registry& reg = sim->things().registry();
    // Not reeling: a plain light attack.
    CHECK(sim->melee().criticalFor(hero, foe) == SoulsAttack::None);
    // Reeling from a guard break (or a poise break): open to a riposte.
    Melee& fm = reg.get_or_emplace<Melee>(foe);
    fm.stunUntil = sim->time().steps + 45;
    fm.guardStun = false;
    CHECK(sim->melee().exposed(foe));
    CHECK(sim->melee().criticalFor(hero, foe) == SoulsAttack::Riposte);
    // A block's own short stun is not an opening.
    fm.guardStun = true;
    CHECK(sim->melee().criticalFor(hero, foe) == SoulsAttack::None);
    fm.guardStun = false;
    std::string why;
    SOULS_REQUIRE(sim->melee().soulsAttack(hero, foe, true, SoulsPress{}, &why));
    CHECK(sim->melee().attackKind(hero) == SoulsAttack::Riposte);
    // Caught: he can't raise his shield while the blade comes.
    sim->submit(ShieldCommand{foe, true, true, -1.0f, 0.0f});
    sim->step();
    CHECK_FALSE(sim->combat().shieldUp(foe));
    bool hit = false;
    float amount = 0.0f;
    for (int s = 0; s < 90 && !hit; ++s) {
        sim->step();
        for (const Event& e : sim->events()) {
            if (e.type == EventType::MeleeHit && e.thing == hero && e.message == "hit") {
                hit = true;
                amount = e.amount;
            }
        }
    }
    CHECK(hit);
    MESSAGE("riposte took " << amount);
    CHECK(reg.all_of<KnockedDown>(foe));  // thrown to the ground
}

TEST_CASE("Souls: from behind a turned back, a light press is a backstab that always lands and knocks down") {
    auto sim = soulsTestSim(22);
    const Entity hero = soulsTestHero(*sim, 30.5f, 30.5f, "Melee_Gladius");
    soulsTestFace(*sim, hero, 1.0f, 0.0f);
    const Entity foe = soulsTestPawn(*sim, 31.6f, 30.5f, "Melee_Gladius");
    entt::registry& reg = sim->things().registry();
    soulsTestFace(*sim, foe, -1.0f, 0.0f);  // facing the hero: no backstab
    CHECK(sim->melee().criticalFor(hero, foe) == SoulsAttack::None);
    soulsTestFace(*sim, foe, 0.0f, 1.0f);  // side on: no backstab
    CHECK(sim->melee().criticalFor(hero, foe) == SoulsAttack::None);
    soulsTestFace(*sim, foe, 1.0f, 0.0f);  // back turned
    CHECK(sim->melee().criticalFor(hero, foe) == SoulsAttack::Backstab);
    // A heavy press from behind stays a heavy (Elden Ring: only the light attack crits).
    std::string why;
    SoulsPress heavy;
    heavy.heavy = true;
    SOULS_REQUIRE(sim->melee().soulsAttack(hero, foe, true, heavy, &why));
    CHECK(sim->melee().attackKind(hero) == SoulsAttack::Heavy);
    soulsTestRunOut(*sim, hero);
    reg.remove<Stamina>(hero);
    reg.get<Position>(hero).x = 30.5f;
    reg.get<Position>(hero).y = 30.5f;
    reg.get<Position>(foe).x = 31.6f;
    reg.get<Position>(foe).y = 30.5f;
    soulsTestFace(*sim, foe, 1.0f, 0.0f);
    soulsTestHeal(*sim, foe);
    reg.remove<KnockedDown>(foe);
    reg.get<Posture>(foe).stance = Stance::Standing;
    if (Melee* m = reg.try_get<Melee>(foe)) {
        m->stunUntil = 0;
    }
    SOULS_REQUIRE(sim->melee().criticalFor(hero, foe) == SoulsAttack::Backstab);
    SOULS_REQUIRE(sim->melee().soulsAttack(hero, foe, true, SoulsPress{}, &why));
    CHECK(sim->melee().attackKind(hero) == SoulsAttack::Backstab);
    bool hit = false;
    for (int s = 0; s < 90 && !hit; ++s) {
        sim->step();
        for (const Event& e : sim->events()) {
            hit = hit || (e.type == EventType::MeleeHit && e.thing == hero && e.message == "hit");
        }
    }
    CHECK(hit);
    CHECK(reg.all_of<KnockedDown>(foe));
}

TEST_CASE("Souls: a man knocked down but still in the fight takes a stab on the ground, whatever the press") {
    auto sim = soulsTestSim(23);
    const Entity hero = soulsTestHero(*sim, 30.5f, 30.5f, "Melee_Gladius");
    soulsTestFace(*sim, hero, 1.0f, 0.0f);
    const Entity foe = soulsTestPawn(*sim, 31.5f, 30.5f, "Melee_Gladius");
    entt::registry& reg = sim->things().registry();
    SOULS_REQUIRE(sim->combat().knockDown(foe, HitFrom{true, 30.5f, 30.5f}, 1.0f, 120));
    for (int s = 0; s < 3; ++s) {
        sim->step();
    }
    SOULS_REQUIRE(reg.all_of<KnockedDown>(foe));
    reg.get<Position>(hero).x = reg.get<Position>(foe).x - 1.0f;
    reg.get<Position>(hero).y = reg.get<Position>(foe).y;
    CHECK(sim->melee().criticalFor(hero, foe) == SoulsAttack::GroundStab);
    SoulsPress heavy;
    heavy.heavy = true;
    std::string why;
    SOULS_REQUIRE(sim->melee().soulsAttack(hero, foe, true, heavy, &why));
    CHECK(sim->melee().attackKind(hero) == SoulsAttack::GroundStab);
    bool hit = false;
    for (int s = 0; s < 90 && !hit; ++s) {
        sim->step();
        for (const Event& e : sim->events()) {
            hit = hit || (e.type == EventType::MeleeHit && e.thing == hero && e.message == "hit");
        }
    }
    CHECK(hit);
    // Downed for good (not just knocked down): the old finishers, not a ground stab.
    CHECK(sim->melee().criticalFor(hero, hero) == SoulsAttack::None);
}

TEST_CASE("Souls: two sword-and-shield fighters duel it out with combos, heavies and blocks, the same way every run") {
    const auto duel = [](uint64_t seed, int& swings, int& heavies, int& blocks, int& staggers, bool& over) {
        auto sim = soulsTestSim(seed);
        const Entity a = soulsTestPawn(*sim, 30.5f, 30.5f, "Melee_Gladius");
        const Entity b = soulsTestPawn(*sim, 32.5f, 30.5f, "Melee_Gladius");
        soulsTestShield(*sim, a);
        soulsTestShield(*sim, b);
        sim->submit(DraftCommand{a, true});
        sim->submit(SetFactionCommand{b, 2});
        swings = 0;
        heavies = 0;
        blocks = 0;
        staggers = 0;
        const Health& ha = sim->things().registry().get<Health>(a);
        const Health& hb = sim->things().registry().get<Health>(b);
        for (int s = 0; s < 1800 && !(ha.downed || ha.dead || hb.downed || hb.dead); ++s) {
            sim->step();
            for (const Event& e : sim->events()) {
                swings += e.type == EventType::MeleeSwing ? 1 : 0;
                heavies += e.type == EventType::MeleeSwing && e.count == 1 ? 1 : 0;
                blocks += e.type == EventType::MeleeHit && e.message == "blocked" ? 1 : 0;
                staggers += e.type == EventType::PawnStaggered ? 1 : 0;
            }
        }
        over = ha.downed || ha.dead || hb.downed || hb.dead;
        return sim->stateHash();
    };
    int swings = 0;
    int heavies = 0;
    int blocks = 0;
    int staggers = 0;
    bool over = false;
    const uint64_t first = duel(11, swings, heavies, blocks, staggers, over);
    MESSAGE("swings " << swings << ", heavies " << heavies << ", blocks " << blocks << ", staggers " << staggers << ", over " << over);
    CHECK(swings >= 8);  // ripostes and backstabs end fights sooner (2026-11-27)
    CHECK(heavies >= 1);
    CHECK(blocks >= 1);
    int swings2 = 0;
    int heavies2 = 0;
    int blocks2 = 0;
    int staggers2 = 0;
    bool over2 = false;
    CHECK(duel(11, swings2, heavies2, blocks2, staggers2, over2) == first);
    CHECK(swings2 == swings);
}

TEST_CASE("Souls: an attack, its combo press and the stamina and poise bars come back from a save as they were") {
    auto sim = soulsTestSim(9);
    const Entity hero = soulsTestHero(*sim, 30.5f, 30.5f, "Melee_Gladius");
    const Entity foe = soulsTestPawn(*sim, 31.5f, 30.5f, "Melee_Gladius");
    soulsTestFace(*sim, hero, 1.0f, 0.0f);
    soulsTestFace(*sim, foe, -1.0f, 0.0f);  // face to face (a turned back would be a backstab)
    std::string why;
    SOULS_REQUIRE(sim->melee().soulsAttack(hero, foe, true, SoulsPress{}, &why));
    sim->step();
    SOULS_REQUIRE(sim->melee().soulsAttack(hero, foe, true, SoulsPress{}, &why));  // kept for the chain
    sim->things().registry().emplace_or_replace<PoiseMeter>(foe, PoiseMeter{12.0f, sim->time().steps, -1000000, 0});
    const Melee before = sim->things().registry().get<Melee>(hero);
    CHECK(before.buffered == SoulsAttack::Light);
    SaveLoadResult result = loadSim(saveSim(*sim), soulsTestDefs());
    SOULS_REQUIRE(result.sim != nullptr);
    Sim* loaded = result.sim.get();
    const Melee& after = loaded->things().registry().get<Melee>(hero);
    CHECK(after.kind == before.kind);
    CHECK(after.buffered == before.buffered);
    CHECK(after.windupSteps == before.windupSteps);
    CHECK(loaded->melee().stamina(hero) == doctest::Approx(sim->melee().stamina(hero)));
    CHECK(loaded->melee().poiseShare(foe) == doctest::Approx(sim->melee().poiseShare(foe)));
    CHECK(loaded->stateHash() == sim->stateHash());
    for (int s = 0; s < 40; ++s) {
        sim->step();
        loaded->step();
    }
    CHECK(loaded->stateHash() == sim->stateHash());
}

TEST_CASE("Souls: switched off, a sword swings as before and nothing costs stamina") {
    auto sim = soulsTestSim(10);
    sim->melee().setSoulsEnabled(false);
    const Entity hero = soulsTestHero(*sim, 30.5f, 30.5f, "Melee_Gladius");
    soulsTestFace(*sim, hero, 1.0f, 0.0f);
    std::string why;
    SOULS_REQUIRE(sim->melee().attack(hero, kNoEntity, true, &why));
    CHECK(sim->melee().attackKind(hero) == SoulsAttack::None);
    CHECK(sim->melee().stamina(hero) == doctest::Approx(100.0f));
    Snapshot snap;
    sim->writeSnapshot(snap);
    CHECK_FALSE(snap.souls);
}

// sneed 2026-11-28: "elden ring combat has fast attacks and quick follow ups ... but there's a slight delay after you
// stop attacking that gives an opening".
TEST_CASE("Souls: a follow-up press plays early in the recovery, but a chain that ends leaves an opening") {
    auto sim = soulsTestSim(31);
    const Entity hero = soulsTestHero(*sim, 30.5f, 30.5f, "Melee_Gladius");
    soulsTestFace(*sim, hero, 1.0f, 0.0f);
    entt::registry& reg = sim->things().registry();
    std::string why;
    SOULS_REQUIRE(sim->melee().soulsAttack(hero, kNoEntity, true, SoulsPress{}, &why));
    // Pressed again at once: kept, and played as soon as the chain opens, early in the recovery.
    CHECK(sim->melee().soulsAttack(hero, kNoEntity, true, SoulsPress{}, &why));
    const uint64_t swings = sim->melee().swings();
    int intoRecovery = -1;
    for (int s = 0; s < 120 && sim->melee().swings() == swings; ++s) {
        sim->step();
        const Melee& m = reg.get<Melee>(hero);
        if (m.phase == MeleePhase::Recovery) {
            intoRecovery = m.recoverySteps - m.stepsLeft;
        }
    }
    SOULS_REQUIRE(sim->melee().swings() == swings + 1);
    const int firstRecovery = static_cast<int>(std::lround(0.2f * 30.0f));  // well before 0.2 s into it
    CHECK(intoRecovery <= firstRecovery);
    // Now let the chain end: the opening holds the attacker for openingSteps once the recovery runs out.
    for (int s = 0; s < 200 && !sim->melee().ready(hero); ++s) {
        sim->step();
    }
    const int opening = sim->melee().souls().openingSteps;
    CHECK(opening >= 6);
    CHECK(sim->melee().inOpening(hero));
    CHECK_FALSE(sim->combat().dodgeRoll(hero, 0.0f, 1.0f, &why));
    CHECK(why == "recovering from the attack");
    // A press in the opening is kept, and plays when it is over (not before).
    const uint64_t before = sim->melee().swings();
    CHECK(sim->melee().soulsAttack(hero, kNoEntity, true, SoulsPress{}, &why));
    int waited = 0;
    while (sim->melee().swings() == before && waited < 60) {
        sim->step();
        ++waited;
    }
    CHECK(sim->melee().swings() == before + 1);
    CHECK(waited >= opening - 2);
    CHECK(waited <= opening + 2);
}

TEST_CASE("Souls: the heavy is a grounded overhead and the running heavy a straight leap (no mid-air start, no drift)") {
    auto sim = soulsTestSim(32);
    const SoulsMoveset* ms = nullptr;
    for (const SoulsMoveset& m : sim->melee().souls().movesets) {
        if (m.key == "OneHandSword") {
            ms = &m;
        }
    }
    SOULS_REQUIRE(ms != nullptr);
    const BodyMotionSystem& motion = sim->combat().motion();
    const auto clipOf = [&](SoulsAttack kind, size_t i = 0) { return motion.clip(ms->clips[static_cast<size_t>(kind)].at(i)); };
    const MotionClip* heavy = clipOf(SoulsAttack::Heavy);
    const MotionClip* leap = clipOf(SoulsAttack::RunningHeavy);
    SOULS_REQUIRE(heavy != nullptr);
    SOULS_REQUIRE(leap != nullptr);
    CHECK(heavy->name == "Motion_SS_HeavyDown");
    CHECK(leap->name == "Motion_SS_Leap");
    // Each starts from standing: the first tenth of a second moves the body under 0.2 m.
    for (const MotionClip* c : {heavy, leap}) {
        float f = 0.0f, r = 0.0f, yaw = 0.0f;
        c->sample(0.1f, f, r, yaw);
        CHECK(std::sqrt(f * f + r * r) < 0.2f);
    }
    // The heavy stays on its feet's spot; the leap goes far and straight.
    float f = 0.0f, r = 0.0f, yaw = 0.0f;
    heavy->sample(heavy->hitSeconds, f, r, yaw);
    CHECK(std::fabs(f) < 0.3f);
    leap->sample(leap->hitSeconds, f, r, yaw);
    CHECK(f > 2.0f);
    CHECK(std::fabs(r) < 0.2f);
    // The carry-in stays small, so a blow is a lean, not a glide.
    for (const SoulsAttack kind : {SoulsAttack::Light, SoulsAttack::Heavy, SoulsAttack::Charged}) {
        CHECK(sim->melee().souls().kinds[static_cast<size_t>(kind)].maxWarp <= 0.5f);
    }
}
