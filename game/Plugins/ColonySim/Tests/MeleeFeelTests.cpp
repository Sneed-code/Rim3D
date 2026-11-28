#include "ColonySim/BodyMotion.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Melee.h"
#include "ColonySim/Souls.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Save.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace colony;

// sneed's gun range feedback, 2026-11-19 (docs/design/lock-on.md "Tab, flicks and landing blows"): Tab walks out through
// every enemy, flicks switch sides, faster swings, blows that land unless dodged, enemies that dodge.

// Evaluates its condition once (it used to run it twice, so a call with side effects such as attack() ran twice).
#define FEEL_REQUIRE(...)                         \
    do {                                          \
        const bool feelOk_ = static_cast<bool>(__VA_ARGS__); \
        CHECK_MESSAGE(feelOk_, #__VA_ARGS__);     \
        if (!feelOk_) {                           \
            return;                               \
        }                                         \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> feelTestDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        namespace fs = std::filesystem;
        const fs::path root = fs::path(COLONY_DATA_DIR);
        std::vector<DefSource> sources;
        std::vector<fs::path> files;
        for (const auto& entry : fs::recursive_directory_iterator(root / "defs")) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());
        for (const fs::path& file : files) {
            std::ifstream in(file, std::ios::binary);
            std::stringstream text;
            text << in.rdbuf();
            sources.push_back(DefSource{file.parent_path().filename().string(), file.filename().string(), text.str()});
        }
        std::ifstream tuningFile(root / "tuning.json", std::ios::binary);
        std::stringstream tuning;
        tuning << tuningFile.rdbuf();
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::build(sources, tuning.str(), *db, errors);
        return db;
    }();
    return defs;
}

std::unique_ptr<Sim> feelTestSim(uint64_t seed) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = 64;
    settings.flatMap = true;
    return std::make_unique<Sim>(settings, feelTestDefs());
}

Entity feelTestPawn(Sim& sim, float x, float y, const char* weapon = nullptr) {
    const Entity pawn = sim.things().spawn("Human", CellPos{static_cast<int>(x), static_cast<int>(y), 0});
    Position& pos = sim.things().registry().get<Position>(pawn);
    pos.x = x;
    pos.y = y;
    if (weapon != nullptr) {
        sim.combat().equip(pawn, sim.things().make(sim.defs().idOf("Things", weapon)));
    }
    return pawn;
}

Entity feelTestHero(Sim& sim, float x, float y, const char* weapon) {
    const Entity hero = feelTestPawn(sim, x, y, weapon);
    sim.things().registry().emplace<Colonist>(hero);
    sim.jobs().addWorker(hero);
    sim.submit(ControlPawnCommand{hero});
    sim.step();
    return hero;
}

void feelTestFace(Sim& sim, Entity pawn, float x, float y) {
    Posture& p = sim.things().registry().get<Posture>(pawn);
    p.facingX = x;
    p.facingY = y;
}

void feelTestHeal(Sim& sim, Entity pawn) {
    sim.things().registry().replace<Health>(pawn, Health{});
    sim.health().refresh(pawn);
    sim.things().registry().remove<Stamina, PoiseMeter>(pawn);  // rested too (souls-combat.md)
}

// Steps from the start of a swing until the pawn can swing again.
int feelTestSwingSteps(const char* weapon) {
    auto sim = feelTestSim(301);
    const Entity hero = feelTestHero(*sim, 30.5f, 30.5f, weapon);
    feelTestFace(*sim, hero, 1.0f, 0.0f);
    std::string why;
    if (!sim->melee().attack(hero, kNoEntity, true, &why)) {
        MESSAGE(why);
        return -1;
    }
    int steps = 0;
    while (!sim->melee().ready(hero) && steps < 400) {
        sim->step();
        ++steps;
    }
    return steps;
}

}  // namespace

TEST_CASE("Tab: each press gives the nearest enemy not given yet, so it goes round all of them") {
    auto sim = feelTestSim(311);
    const Entity hero = feelTestHero(*sim, 30.5f, 30.5f, "Melee_Longsword");
    const Entity a = feelTestPawn(*sim, 33.5f, 30.5f);  // 3 m
    const Entity b = feelTestPawn(*sim, 34.5f, 32.5f);  // about 4.5 m
    const Entity c = feelTestPawn(*sim, 36.5f, 29.5f);  // about 6 m
    for (const Entity e : {a, b, c}) {
        sim->submit(SetFactionCommand{e, 2});
        sim->submit(SetFightResponseCommand{e, FightResponse::Flee});
    }
    sim->step();
    feelTestFace(*sim, hero, 1.0f, 0.0f);
    sim->submit(FocusCommand{hero, kNoEntity, FocusMode::Next, 1.0f, 0.0f});
    sim->step();
    CHECK(sim->melee().focusOf(hero) == a);
    sim->submit(FocusCommand{hero, kNoEntity, FocusMode::Next, 1.0f, 0.0f});
    sim->step();
    CHECK(sim->melee().focusOf(hero) == b);
    // Walk up to b: it is the nearest now. The old Tab went back to a and swapped the two for ever.
    sim->things().registry().get<Position>(hero).x = 33.5f;
    sim->things().registry().get<Position>(hero).y = 32.0f;
    sim->submit(FocusCommand{hero, kNoEntity, FocusMode::Next, 1.0f, 0.0f});
    sim->step();
    CHECK(sim->melee().focusOf(hero) == c);
    sim->submit(FocusCommand{hero, kNoEntity, FocusMode::Next, 1.0f, 0.0f});
    sim->step();
    const Entity again = sim->melee().focusOf(hero);
    CHECK((again == a || again == b));  // round again from the nearest, not the one held
    CHECK(again != c);
}

TEST_CASE("Tab: a round starts again after a while, and standing enemies come before downed ones") {
    auto sim = feelTestSim(312);
    const Entity hero = feelTestHero(*sim, 30.5f, 30.5f, "Melee_Longsword");
    const Entity down = feelTestPawn(*sim, 32.5f, 30.5f);    // 2 m, on the ground
    const Entity standing = feelTestPawn(*sim, 35.5f, 30.5f);  // 5 m
    for (const Entity e : {down, standing}) {
        sim->submit(SetFactionCommand{e, 2});
        sim->submit(SetFightResponseCommand{e, FightResponse::Flee});
    }
    sim->step();
    sim->things().registry().get<Health>(down).downed = true;
    FEEL_REQUIRE(sim->health().incapacitated(down));
    feelTestFace(*sim, hero, 1.0f, 0.0f);
    sim->submit(FocusCommand{hero, kNoEntity, FocusMode::Next, 1.0f, 0.0f});
    sim->step();
    CHECK(sim->melee().focusOf(hero) == standing);
    sim->submit(FocusCommand{hero, kNoEntity, FocusMode::Next, 1.0f, 0.0f});
    sim->step();
    CHECK(sim->melee().focusOf(hero) == down);  // still reachable by Tab
    for (int s = 0; s < sim->melee().rules().focusRoundSteps + 5; ++s) {
        sim->step();
        sim->things().registry().get<Health>(down).downed = true;
    }
    sim->submit(FocusCommand{hero, kNoEntity, FocusMode::Next, 1.0f, 0.0f});
    sim->step();
    CHECK(sim->melee().focusOf(hero) == standing);  // a new round: the nearest standing one
}

TEST_CASE("Tab: the view decides what is in front, and anySide lets the practice range lock on to its own targets") {
    auto sim = feelTestSim(313);
    const Entity hero = feelTestHero(*sim, 30.5f, 30.5f, "Melee_Longsword");
    const Entity east = feelTestPawn(*sim, 34.5f, 30.5f);   // 4 m east
    const Entity north = feelTestPawn(*sim, 30.5f, 35.5f);  // 5 m north
    sim->step();
    feelTestFace(*sim, hero, 1.0f, 0.0f);
    sim->submit(FocusCommand{hero, kNoEntity, FocusMode::Next});
    sim->step();
    CHECK(sim->melee().focusOf(hero) == kNoEntity);  // its own side: nobody
    sim->submit(FocusCommand{hero, kNoEntity, FocusMode::Next, 0.0f, 1.0f, true});  // looking north, the body east
    sim->step();
    CHECK(sim->melee().focusOf(hero) == north);
    (void)east;
}

TEST_CASE("Flicks: right and left switch to the nearest enemy on that side of the one held") {
    auto sim = feelTestSim(314);
    const Entity hero = feelTestHero(*sim, 30.5f, 30.5f, "Melee_Longsword");
    const Entity ahead = feelTestPawn(*sim, 34.5f, 30.5f);
    const Entity plusY = feelTestPawn(*sim, 34.0f, 33.5f);   // Unreal draws +Y to the player's right
    const Entity minusY = feelTestPawn(*sim, 34.0f, 27.5f);
    const Entity farPlusY = feelTestPawn(*sim, 30.5f, 37.5f);
    for (const Entity e : {ahead, plusY, minusY, farPlusY}) {
        sim->submit(SetFactionCommand{e, 2});
        sim->submit(SetFightResponseCommand{e, FightResponse::Flee});
    }
    sim->submit(FocusCommand{hero, ahead, FocusMode::Set});
    sim->step();
    sim->submit(FocusCommand{hero, kNoEntity, FocusMode::Right, 1.0f, 0.0f});
    sim->step();
    CHECK(sim->melee().focusOf(hero) == plusY);  // the smallest turn that way
    sim->submit(FocusCommand{hero, kNoEntity, FocusMode::Left, 1.0f, 0.0f});
    sim->step();
    CHECK(sim->melee().focusOf(hero) == ahead);
    sim->submit(FocusCommand{hero, kNoEntity, FocusMode::Left, 1.0f, 0.0f});
    sim->step();
    CHECK(sim->melee().focusOf(hero) == minusY);
}

TEST_CASE("Flicks: with one other enemy left, on the other side, a flick still switches to it, and Tab does too") {
    // PC gun range 2026-11-20: "flick from 1048614 to 1048614 (DID NOT SWITCH), 1 living targets besides the one held".
    auto sim = feelTestSim(318);
    const Entity hero = feelTestHero(*sim, 30.5f, 30.5f, "Melee_Longsword");
    const Entity ahead = feelTestPawn(*sim, 34.5f, 30.5f);
    const Entity minusY = feelTestPawn(*sim, 34.0f, 27.5f);  // to the left
    for (const Entity e : {ahead, minusY}) {
        sim->submit(SetFactionCommand{e, 2});
        sim->submit(SetFightResponseCommand{e, FightResponse::Flee});
    }
    sim->submit(FocusCommand{hero, ahead, FocusMode::Set});
    sim->step();
    sim->submit(FocusCommand{hero, kNoEntity, FocusMode::Right, 1.0f, 0.0f});
    sim->step();
    CHECK(sim->melee().focusOf(hero) == minusY);
    sim->submit(FocusCommand{hero, kNoEntity, FocusMode::Right, 1.0f, 0.0f});
    sim->step();
    CHECK(sim->melee().focusOf(hero) == ahead);
    sim->submit(FocusCommand{hero, kNoEntity, FocusMode::Next, 1.0f, 0.0f});
    sim->step();
    CHECK(sim->melee().focusOf(hero) == minusY);
    sim->submit(FocusCommand{hero, kNoEntity, FocusMode::Next, 1.0f, 0.0f});
    sim->step();
    CHECK(sim->melee().focusOf(hero) == ahead);
}

TEST_CASE("Swing speed: the club swings twice as fast, other melee weapons half again as fast (sneed 2026-11-19)") {
    const int club = feelTestSwingSteps("Melee_Club");        // RimWorld 2 s, now 1 s
    const int gladius = feelTestSwingSteps("Melee_Gladius");  // RimWorld 2 s, now 1.33 s
    const int longsword = feelTestSwingSteps("Melee_Longsword");  // 2.45 s, halved (#363), now a third
    MESSAGE("club " << club << ", gladius " << gladius << ", longsword " << longsword << " steps");
    // At the default Melee skill (4) a swing takes 1 / 0.88 of the weapon's time, and every swing 0.75 of that
    // (attackTimeFactor: 0.75 "33% faster", then sneed 2026-11-23 again "slow it down 33%": 1.125).
    const float f = feelTestSim(1)->melee().rules().attackTimeFactor;
    CHECK(club >= static_cast<int>(32.0f * f));
    CHECK(club <= static_cast<int>(std::ceil(36.0f * f)));
    if (feelTestSim(1)->melee().soulsOn()) {
        // Elden Ring style (souls-combat.md): the gladius swings its moveset's light attack, about a second all told.
        CHECK(gladius >= 20);
        CHECK(gladius <= 40);
    } else {
        CHECK(gladius >= static_cast<int>(43.0f * f));
        CHECK(gladius <= static_cast<int>(std::ceil(48.0f * f)));
    }
    CHECK(longsword >= static_cast<int>(26.0f * f));
    CHECK(longsword <= static_cast<int>(std::ceil(30.0f * f)));
}

TEST_CASE("Landing blows: the player's swing at its lock just past its reach still goes at it, and lands once in reach") {
    auto sim = feelTestSim(315);
    const Entity hero = feelTestHero(*sim, 30.5f, 30.5f, "Melee_Gladius");
    const float engage = sim->melee().engageDistance(hero, kNoEntity) + sim->melee().rules().reachSlack;
    const Entity enemy = feelTestPawn(*sim, 30.5f + engage + 0.4f, 30.5f);  // a little out of reach
    sim->submit(SetFactionCommand{enemy, 2});
    sim->submit(SetFightResponseCommand{enemy, FightResponse::Flee});
    sim->submit(FocusCommand{hero, enemy, FocusMode::Set});
    sim->step();
    FEEL_REQUIRE(!sim->melee().inReach(hero, enemy));
    CHECK(sim->melee().playerTarget(hero) == enemy);
    sim->submit(MeleeCommand{hero});
    sim->step();
    CHECK(sim->things().registry().get<Melee>(hero).target == enemy);
    // The player runs on under the swing (since sneed 2026-11-28 the sword's swing carries nobody in: the feet are the
    // player's); the blow lands.
    bool hit = false;
    for (int s = 0; s < 40 && !hit; ++s) {
        Position& p = sim->things().registry().get<Position>(hero);
        sim->submit(DirectMoveCommand{hero, p.x + 0.08f, p.y});
        sim->step();
        for (const Event& e : sim->events()) {
            hit = hit || (e.type == EventType::MeleeHit && e.thing == hero && (e.message == "hit" || e.message == "deflected"));
        }
    }
    CHECK(hit);
}

TEST_CASE("Landing blows: with no lock, the swing takes the enemy ahead in reach") {
    auto sim = feelTestSim(316);
    const Entity hero = feelTestHero(*sim, 30.5f, 30.5f, "Melee_Gladius");
    const Entity enemy = feelTestPawn(*sim, 31.6f, 30.9f);  // ahead and a little to the side
    sim->submit(SetFactionCommand{enemy, 2});
    sim->submit(SetFightResponseCommand{enemy, FightResponse::Flee});
    sim->step();
    feelTestFace(*sim, hero, 1.0f, 0.0f);
    CHECK(sim->melee().playerTarget(hero) == enemy);
    feelTestFace(*sim, hero, -1.0f, 0.0f);
    CHECK(sim->melee().playerTarget(hero) == kNoEntity);  // behind it: not taken
}

TEST_CASE("Landing blows: a target that shuffles a little after the line is fixed is still hit") {
    auto sim = feelTestSim(317);
    const Entity attacker = feelTestPawn(*sim, 30.5f, 30.5f, "Melee_Mace");
    const Entity target = feelTestPawn(*sim, 31.5f, 30.5f);
    sim->step();
    std::string why;
    FEEL_REQUIRE(sim->melee().attack(attacker, target, false, &why));
    bool hit = false;
    bool moved = false;
    for (int s = 0; s < 60 && !hit; ++s) {
        const Melee& m = sim->things().registry().get<Melee>(attacker);
        if (m.committed && !moved) {
            // After the line is fixed it steps a little aside (not a dodge): about 15 degrees off the line.
            sim->things().registry().get<Position>(target).y += 0.25f;
            moved = true;
        }
        sim->step();
        for (const Event& e : sim->events()) {
            hit = hit || (e.type == EventType::MeleeHit && e.thing == attacker && e.target == target && e.message != "missed");
        }
    }
    CHECK(moved);
    CHECK(hit);
}

TEST_CASE("Swing speed: a person's fists punch at half the first batch's pace with no pause between, and weakly (sneed 2026-11-24)") {
    const int fists = feelTestSwingSteps(nullptr);  // RimWorld 2 s at Melee 10; Melee 4: 68 steps
    const MeleeRules& rules = feelTestSim(1)->melee().rules();
    MESSAGE("fists " << fists << " steps");
    // Their own pace (not times attackTimeFactor), plus the pause.
    const float expected = 68.0f * rules.unarmedTimeFactor + static_cast<float>(rules.unarmedGapSteps);
    CHECK(fists >= static_cast<int>(expected) - 1);
    CHECK(fists <= static_cast<int>(std::ceil(expected)) + 1);
    // "fist attacks are too fast still, half it" (2026-11-24): twice the 2026-11-23 pace of about 20 steps.
    CHECK(fists >= 32);
    CHECK(fists <= 37);
    CHECK(rules.unarmedGapSteps == 0);       // "no delay between"
    CHECK(rules.unarmedPowerFactor < 1.0f);  // "and weak"
}

TEST_CASE("Swing speed: the player's next punch starts in the last steps of the last one's recovery (sneed 2026-11-25)") {
    auto sim = feelTestSim(302);
    const Entity hero = feelTestHero(*sim, 30.5f, 30.5f, nullptr);
    const Entity other = feelTestPawn(*sim, 34.5f, 30.5f, "Melee_Mace");
    feelTestFace(*sim, hero, 1.0f, 0.0f);
    const int chain = sim->melee().rules().playerFistChainSteps;
    FEEL_REQUIRE(chain > 0);
    std::string why;
    FEEL_REQUIRE(sim->melee().attack(hero, kNoEntity, true, &why));
    // Early in the recovery: not yet.
    int steps = 0;
    while (sim->things().registry().get<Melee>(hero).phase != MeleePhase::Recovery && steps < 200) {
        sim->step();
        ++steps;
    }
    FEEL_REQUIRE(sim->things().registry().get<Melee>(hero).stepsLeft > chain);
    CHECK_FALSE(sim->melee().attack(hero, kNoEntity, true, &why));
    // Within the last playerFistChainSteps: the next punch winds up at once.
    while (sim->things().registry().get<Melee>(hero).stepsLeft > chain && steps < 400) {
        sim->step();
        ++steps;
    }
    CHECK(sim->things().registry().get<Melee>(hero).phase == MeleePhase::Recovery);
    CHECK(sim->melee().attack(hero, kNoEntity, true, &why));
    CHECK(sim->things().registry().get<Melee>(hero).phase == MeleePhase::Windup);
    // Only the player's bare fists: a pawn with a weapon still waits for its recovery to end.
    sim->step();
    FEEL_REQUIRE(sim->melee().attack(other, kNoEntity, true, &why));
    steps = 0;
    while (!(sim->things().registry().get<Melee>(other).phase == MeleePhase::Recovery &&
             sim->things().registry().get<Melee>(other).stepsLeft <= chain) && steps < 400) {
        sim->step();
        ++steps;
    }
    CHECK_FALSE(sim->melee().attack(other, kNoEntity, true, &why));
}

TEST_CASE("Defending: an enemy fighter dodges some of the player's knife swings by skill, and the rest land") {
    auto sim = feelTestSim(318);
    const Entity hero = feelTestHero(*sim, 30.5f, 30.5f, "Melee_Knife");
    const Entity enemy = feelTestPawn(*sim, 31.4f, 30.5f);
    sim->submit(SetFactionCommand{enemy, 2});
    sim->submit(SetFightResponseCommand{enemy, FightResponse::Flee});
    sim->submit(FocusCommand{hero, enemy, FocusMode::Set});
    sim->step();
    int swings = 0;
    int hits = 0;
    int dodges = 0;
    for (int s = 0; s < 3000 && swings < 40; ++s) {
        // Keep it near and on its feet: a practice partner.
        Position& e = sim->things().registry().get<Position>(enemy);
        Position& p = sim->things().registry().get<Position>(hero);
        if (std::fabs(e.x - p.x) + std::fabs(e.y - p.y) > 1.6f) {
            e.x = p.x + 0.9f;
            e.y = p.y;
        }
        feelTestHeal(*sim, enemy);
        if (sim->melee().ready(hero)) {
            sim->submit(MeleeCommand{hero});
            ++swings;
        }
        sim->step();
        for (const Event& ev : sim->events()) {
            hits += ev.type == EventType::MeleeHit && ev.thing == hero && ev.message == "hit" ? 1 : 0;
            dodges += ev.type == EventType::PawnAction && ev.action == PawnAction::Dodge && ev.thing == enemy ? 1 : 0;
        }
    }
    MESSAGE(swings << " swings, " << hits << " hits, " << dodges << " dodges");
    CHECK(dodges >= 1);  // (3 or more before the swings got a quarter faster, sneed 2026-11-23: fewer are read in time)
    CHECK(hits >= 10);
}

TEST_CASE("Blows at the downed: a spear and a knife have a downward stab") {
    auto sim = feelTestSim(319);
    for (const char* weapon : {"Melee_Spear", "Melee_Knife"}) {
        const std::vector<DefId> low = sim->melee().lowAttackClipsOf(sim->defs().idOf("Things", weapon));
        CHECK_MESSAGE(low.size() == 1, weapon);
        if (!low.empty()) {
            const std::string name = sim->defs().get(low.front()).defName();
            CHECK_MESSAGE(name.find("Stab_Low") != std::string::npos, name);
        }
    }
}

TEST_CASE("Flicks: on the practice range (anySide) a flick from the one enemy goes to a target beside it") {
    // PC smoke test 2026-11-19: the range's Tab had locked on to a woman who had turned on the player (an enemy); the
    // others were the player's own side, which a flick dropped because an enemy was about, so it never switched.
    auto sim = feelTestSim(320);
    const Entity hero = feelTestHero(*sim, 30.5f, 30.5f, "Gun_Revolver");
    const Entity turned = feelTestPawn(*sim, 40.5f, 30.5f);
    const Entity beside = feelTestPawn(*sim, 40.5f, 33.5f);
    sim->submit(SetFactionCommand{turned, 2});
    sim->submit(SetFightResponseCommand{turned, FightResponse::Flee});
    sim->submit(FocusCommand{hero, turned, FocusMode::Set});
    sim->step();
    sim->submit(FocusCommand{hero, kNoEntity, FocusMode::Right, 1.0f, 0.0f, true});
    sim->step();
    CHECK(sim->melee().focusOf(hero) == beside);
    sim->submit(FocusCommand{hero, kNoEntity, FocusMode::Next, 1.0f, 0.0f, true});
    sim->step();
    CHECK(sim->melee().focusOf(hero) == turned);  // Tab still gives the enemy first
}

// ---- Finishers, aim assist and facing (sneed 2026-11-20, docs/design/lock-on.md "Finishers") ---------------------------

namespace {

// A hero with `weapon` and a downed enemy `metres` ahead of it; returns the enemy.
Entity feelTestDowned(Sim& sim, Entity hero, float metres) {
    const Position& p = sim.things().registry().get<Position>(hero);
    const Entity enemy = feelTestPawn(sim, p.x + metres, p.y);
    sim.submit(SetFactionCommand{enemy, 2});
    sim.step();
    // A real downed body: knocked out by blows to the head (the health refresh undoes a downed flag set by hand,
    // and a body set down by hand would still stand in the way of a step-in).
    int head = -1;
    const std::vector<std::string> labels =
        sim.health().partLabels(sim.things().registry().get<Thing>(enemy).def);
    for (size_t i = 0; i < labels.size(); ++i) {
        if (labels[i] == "head") {
            head = static_cast<int>(i);
        }
    }
    for (int blow = 0; blow < 40 && !sim.things().registry().get<Health>(enemy).downed; ++blow) {
        sim.submit(DamagePawnCommand{enemy, "Blunt", 5.0f, head});
        sim.step();
    }
    Health& health = sim.things().registry().get<Health>(enemy);
    health.movingFactor = 0.0f;
    return enemy;
}

}  // namespace

TEST_CASE("Finishers: every melee weapon of sneed's four kinds has its own finisher at the downed") {
    auto sim = feelTestSim(401);
    const MeleeSystem& melee = sim->melee();
    const auto moveOf = [&](const char* weapon) -> std::string {
        const int f = melee.finisherFor(sim->defs().idOf("Things", weapon));
        return f > 0 ? melee.rules().finishers[static_cast<size_t>(f - 1)].move : std::string("none");
    };
    CHECK(moveOf("Melee_Spear") == "Spear");
    CHECK(moveOf("Melee_Club") == "Club");
    CHECK(moveOf("Melee_Knife") == "Knife");
    CHECK(moveOf("Melee_Gladius") == "Sword");
    CHECK(moveOf("Melee_Longsword") == "Sword");
}

TEST_CASE("Finishers: a knife at a downed enemy plays its whole-body kneeling clip, the blow at its hit time, the body held") {
    // sneed 2026-11-22: "you only made the arms move but not the whole body as an animation". The knife's finisher is
    // a measured whole-body clip (down on the knees, both hands driven down), not a pose made by hand.
    auto sim = feelTestSim(402);
    const Entity hero = feelTestHero(*sim, 30.5f, 30.5f, "Melee_Knife");
    const Entity enemy = feelTestDowned(*sim, hero, 0.9f);
    feelTestFace(*sim, hero, 1.0f, 0.0f);
    std::string why;
    const bool attacked = sim->melee().attack(hero, enemy, true, &why);
    FEEL_REQUIRE(attacked);
    const MeleeFinisher* f = sim->melee().finisherOf(hero);
    FEEL_REQUIRE(f != nullptr);
    CHECK(f->move == "Knife");
    const int finisher = sim->things().registry().get<Melee>(hero).finisher;
    CHECK(sim->melee().finisherPlaysClip(finisher));
    const BodyMotionSystem& motion = sim->combat().motion();
    const MotionClip* clip = motion.clip(f->clip);
    FEEL_REQUIRE(clip != nullptr);
    FEEL_REQUIRE(motion.moving(hero));
    CHECK(sim->things().registry().get<BodyMotion>(hero).clip == clip->def);
    const int hitStep = static_cast<int>(std::lround(f->hitSeconds * 30.0f * sim->melee().rules().attackTimeFactor * sim->melee().rules().lowTimeFactor));
    // The finisher lasts as long as the clip, played at the pace that lands its blow on the hit step.
    const int total = sim->things().registry().get<Melee>(hero).finisherSteps;
    const int clipTotal = static_cast<int>(std::lround(clip->seconds * static_cast<float>(hitStep) / sim->melee().finisherClipHit(finisher)));
    CHECK(std::abs(total - clipTotal) <= 1);
    const Position start = sim->things().registry().get<Position>(hero);
    Position end = start;
    int struckAt = -1;
    int readyAt = -1;
    float sawT = 0.0f;
    for (int s = 1; s <= total + 10; ++s) {
        // The player keeps pushing forward: through a finisher the body stays where the clip has it.
        const bool inFinisher = sim->melee().finisherOf(hero) != nullptr;
        if (s > hitStep / 2 && inFinisher) {
            const Position& now = sim->things().registry().get<Position>(hero);
            sim->submit(DirectMoveCommand{hero, now.x, now.y + 0.08f});
        }
        sim->step();
        if (inFinisher) {
            end = sim->things().registry().get<Position>(hero);
        }
        for (const Event& e : sim->events()) {
            if (e.type == EventType::MeleeHit && e.thing == hero && struckAt < 0) {
                struckAt = s;
            }
        }
        Snapshot snap;
        sim->writeSnapshot(snap);
        if (const ThingView* v = snap.find(hero); v != nullptr && v->meleeFinisher != 0) {
            CHECK(v->meleeFinisherTarget == enemy);
            if (s < total) {  // (on the last step the clip, rounded to whole steps, may just have ended)
                CHECK(v->motionClip == clip->def);  // Unreal plays the clip on the whole body
            }
            CHECK_FALSE(v->motionUpper);
            sawT = std::max(sawT, v->meleeFinisherT);
        }
        if (readyAt < 0 && sim->melee().ready(hero)) {
            readyAt = s;
        }
    }
    CHECK(struckAt == hitStep);
    CHECK(readyAt >= total - 1);
    CHECK(readyAt <= total + 1);
    CHECK(sawT > 0.9f);
    // No glide in towards the body: it moved only its few cm to its place by the body (shiftClip) plus the clip's own,
    // and the player's walking never moved it sideways.
    CHECK(std::hypot(end.x - start.x, end.y - start.y) < 0.35f);  // its step back to its place, the clip's lunge and back
    CHECK(std::fabs(end.y - start.y) < 0.06f);
}

TEST_CASE("Finishers: too far from the body for its clip, a weapon's blow at the downed is its plain low blow") {
    auto sim = feelTestSim(406);
    const Entity hero = feelTestHero(*sim, 30.5f, 30.5f, "Melee_Gladius");
    const Entity enemy = feelTestDowned(*sim, hero, 2.2f);  // the gladius finisher stands 0.85 m off, its slack 0.6
    feelTestFace(*sim, hero, 1.0f, 0.0f);
    std::string why;
    const bool attacked = sim->melee().attack(hero, enemy, true, &why);
    FEEL_REQUIRE(attacked);
    CHECK(sim->melee().finisherOf(hero) == nullptr);
    CHECK(sim->melee().winding(hero));
}

TEST_CASE("Finishers: right on top of the body the finisher still plays, stepping back only its slack") {
    // PC pictures 2026-11-22: the gladius's finisher never played because the body lay too close.
    auto sim = feelTestSim(411);
    const Entity hero = feelTestHero(*sim, 30.5f, 30.5f, "Melee_Gladius");
    const Entity enemy = feelTestDowned(*sim, hero, 0.25f);
    feelTestFace(*sim, hero, 1.0f, 0.0f);
    std::string why;
    FEEL_REQUIRE(sim->melee().attack(hero, enemy, true, &why));
    const MeleeFinisher* f = sim->melee().finisherOf(hero);
    FEEL_REQUIRE(f != nullptr);
    const Position start = sim->things().registry().get<Position>(hero);
    for (int s = 0; s < static_cast<int>(std::lround(f->hitSeconds * 30.0f)); ++s) {
        sim->step();
    }
    const Position& now = sim->things().registry().get<Position>(hero);
    CHECK(std::hypot(now.x - start.x, now.y - start.y) < f->clipSlack + 0.1f);
}

TEST_CASE("Finishers: a gladius a little close to the body steps back with its clip, to stand off its place at the blow") {
    // sneed 2026-11-22 PC pictures: the gladius lunged past the body. The clip's step is laid over with a shift so the
    // body is at the finisher's standOff when the blow lands, wherever in the slack the swing started.
    auto sim = feelTestSim(410);
    const Entity hero = feelTestHero(*sim, 30.5f, 30.5f, "Melee_Gladius");
    const Entity enemy = feelTestDowned(*sim, hero, 1.0f);
    feelTestFace(*sim, hero, 1.0f, 0.0f);
    std::string why;
    FEEL_REQUIRE(sim->melee().attack(hero, enemy, true, &why));
    const MeleeFinisher* f = sim->melee().finisherOf(hero);
    FEEL_REQUIRE(f != nullptr);
    FEEL_REQUIRE(sim->melee().finisherPlaysClip(sim->things().registry().get<Melee>(hero).finisher));
    const BodyMotionSystem& motion = sim->combat().motion();
    const MotionClip* clip = motion.clip(f->clip);
    FEEL_REQUIRE(clip != nullptr);
    const int hitStep = static_cast<int>(std::lround(f->hitSeconds * 30.0f));
    for (int s = 0; s < hitStep; ++s) {
        sim->step();
    }
    const Position& a = sim->things().registry().get<Position>(hero);
    const Position& b = sim->things().registry().get<Position>(enemy);
    const float d = std::hypot(a.x - b.x, a.y - b.y);
    CAPTURE(d);
    CHECK(std::fabs(d - f->standOff) < 0.1f);
    CHECK(std::fabs(a.y - b.y) < 0.12f);  // straight back along the line to the body
}

TEST_CASE("Finishers: every finisher of sneed's weapons plays a measured whole-body clip") {
    auto sim = feelTestSim(407);
    for (const char* weapon : {"Melee_Spear", "Melee_Club", "Melee_Knife", "Melee_Gladius", "Melee_Longsword", "Melee_Mace"}) {
        CAPTURE(weapon);
        const int f = sim->melee().finisherFor(sim->defs().idOf("Things", weapon));
        CHECK(f > 0);
        CHECK(sim->melee().finisherPlaysClip(f));
        // Its blow when the blade comes down (clipHitSeconds), inside the clip.
        const MotionClip* clip = sim->combat().motion().clip(sim->melee().rules().finishers[static_cast<size_t>(f - 1)].clip);
        FEEL_REQUIRE(clip != nullptr);
        CHECK(sim->melee().finisherClipHit(f) > 0.0f);
        CHECK(sim->melee().finisherClipHit(f) < clip->seconds);
    }
}

// ---- Blocking (sneed 2026-11-22: "my right click with a melee weapon or fists doesn't block") ------------------------

TEST_CASE("Block: the player's fists block, a gun never does") {
    auto sim = feelTestSim(408);
    const Entity bare = feelTestHero(*sim, 30.5f, 30.5f, nullptr);
    std::string why;
    CHECK(sim->melee().block(bare, &why));
    sim->stepMany(sim->melee().rules().blockSteps + 1);
    CHECK(sim->melee().blocking(bare));
    const Entity rifleman = feelTestPawn(*sim, 20.5f, 20.5f, "Gun_BoltActionRifle");
    CHECK_FALSE(sim->melee().block(rifleman, &why));
}

TEST_CASE("Block: held, the block stays up as long as the button is down and drops soon after it is let go") {
    auto sim = feelTestSim(409);
    const Entity hero = feelTestHero(*sim, 30.5f, 30.5f, "Melee_Longsword");
    const MeleeRules& rules = sim->melee().rules();
    std::string why;
    // The button held for three times the single block's length: sent every step, as Unreal does.
    const int held = rules.blockSteps + rules.playerBlockSteps * 3;
    int up = 0;
    for (int s = 0; s < held; ++s) {
        sim->melee().block(hero, &why);
        sim->step();
        up += sim->melee().blocking(hero) ? 1 : 0;
    }
    CHECK(up >= held - rules.blockSteps - 1);  // up the whole time after the first raise
    int stillUp = 0;
    for (int s = 0; s < rules.playerBlockSteps; ++s) {
        sim->step();
        stillUp += sim->melee().blocking(hero) ? 1 : 0;
    }
    CHECK(stillUp <= rules.playerBlockHoldSteps);
}

TEST_CASE("Block: bare fists stop a punch, and take a sword's blow on the forearm instead of the body") {
    auto sim = feelTestSim(410);
    const Entity hero = feelTestHero(*sim, 30.5f, 30.5f, nullptr);
    feelTestFace(*sim, hero, 1.0f, 0.0f);
    const Entity swordsman = feelTestPawn(*sim, 31.4f, 30.5f, "Melee_Gladius");
    sim->submit(SetFactionCommand{swordsman, 2});
    sim->step();
    feelTestFace(*sim, swordsman, -1.0f, 0.0f);
    std::string why;
    const uint64_t armBlocksBefore = sim->melee().armBlocks();
    // (The enemy may already be swinging at the hero on its own.)
    const bool swung = sim->melee().winding(swordsman) || sim->melee().attack(swordsman, hero, false, &why);
    CAPTURE(why);
    FEEL_REQUIRE(swung);
    std::vector<std::string> parts = sim->health().partLabels(sim->things().registry().get<Thing>(hero).def);
    int struckPart = -2;
    for (int s = 0; s < 90 && struckPart == -2; ++s) {
        sim->melee().block(hero, &why);
        feelTestFace(*sim, hero, 1.0f, 0.0f);
        sim->step();
        for (const Event& e : sim->events()) {
            if (e.type == EventType::MeleeHit && e.thing == swordsman) {
                struckPart = e.part;
            }
        }
    }
    CHECK(sim->melee().armBlocks() == armBlocksBefore + 1);
    FEEL_REQUIRE(struckPart >= 0);
    FEEL_REQUIRE(static_cast<size_t>(struckPart) < parts.size());
    const std::string& label = parts[static_cast<size_t>(struckPart)];
    CAPTURE(label);
    CHECK((label.find("arm") != std::string::npos || label.find("hand") != std::string::npos || label.find("finger") != std::string::npos ||
           label.find("humerus") != std::string::npos || label.find("radius") != std::string::npos || label.find("clavicle") != std::string::npos));
}

TEST_CASE("Finishers: a finisher whose body is taken off the map ends at once, and the next swing is the weapon's own") {
    auto sim = feelTestSim(404);
    const Entity hero = feelTestHero(*sim, 30.5f, 30.5f, "Melee_Spear");
    const Entity enemy = feelTestDowned(*sim, hero, 1.4f);
    feelTestFace(*sim, hero, 1.0f, 0.0f);
    std::string why;
    FEEL_REQUIRE(sim->melee().attack(hero, enemy, true, &why));
    FEEL_REQUIRE(sim->melee().finisherOf(hero) != nullptr);
    for (int s = 0; s < 10; ++s) {
        sim->step();
    }
    sim->things().destroy(enemy);
    sim->step();
    CHECK(sim->melee().finisherOf(hero) == nullptr);
    CHECK(sim->melee().ready(hero));
    Snapshot snap;
    sim->writeSnapshot(snap);
    const ThingView* v = snap.find(hero);
    FEEL_REQUIRE(v != nullptr);
    CHECK(v->meleeFinisher == 0);
}

TEST_CASE("Finishers: a swing at someone standing is the usual quick swing") {
    auto sim = feelTestSim(403);
    const Entity hero = feelTestHero(*sim, 30.5f, 30.5f, "Melee_Knife");
    const Entity enemy = feelTestPawn(*sim, 31.3f, 30.5f);
    sim->submit(SetFactionCommand{enemy, 2});
    sim->step();
    std::string why;
    const bool attacked = sim->melee().attack(hero, enemy, true, &why);
    FEEL_REQUIRE(attacked);
    CHECK(sim->melee().finisherOf(hero) == nullptr);
}

TEST_CASE("Finishers: a finisher under way survives a save and load") {
    auto sim = feelTestSim(404);
    const Entity hero = feelTestHero(*sim, 30.5f, 30.5f, "Melee_Spear");
    const Entity enemy = feelTestDowned(*sim, hero, 0.8f);
    std::string why;
    const bool attacked = sim->melee().attack(hero, enemy, true, &why);
    FEEL_REQUIRE(attacked);
    for (int s = 0; s < 10; ++s) {
        sim->step();
    }
    const Melee before = sim->things().registry().get<Melee>(hero);
    SaveLoadResult result = loadSim(saveSim(*sim), feelTestDefs());
    FEEL_REQUIRE(result.sim != nullptr);
    Sim* loaded = result.sim.get();
    const Melee& after = loaded->things().registry().get<Melee>(hero);
    CHECK(after.finisher == before.finisher);
    CHECK(after.finisherFrom == before.finisherFrom);
    CHECK(after.finisherSteps == before.finisherSteps);
    CHECK(loaded->stateHash() == sim->stateHash());
}

TEST_CASE("Aim assist: with no lock the player's swing goes at a neutral ahead too, never at its own side") {
    auto sim = feelTestSim(405);
    const Entity hero = feelTestHero(*sim, 30.5f, 30.5f, "Melee_Gladius");
    const Entity neutral = feelTestPawn(*sim, 31.6f, 30.9f);
    const Entity friendly = feelTestPawn(*sim, 31.4f, 30.1f);
    sim->submit(SetFactionCommand{neutral, 3});
    sim->submit(SetFightResponseCommand{neutral, FightResponse::Flee});
    sim->step();
    FEEL_REQUIRE(sim->fighters().factionOf(friendly) == sim->fighters().factionOf(hero));
    feelTestFace(*sim, hero, 1.0f, 0.0f);
    CHECK(sim->melee().playerTarget(hero) == neutral);
}

TEST_CASE("Facing: a swing at someone while walking keeps the body turned to them, not to the walk") {
    auto sim = feelTestSim(406);
    const Entity hero = feelTestHero(*sim, 30.5f, 30.5f, "Melee_Gladius");
    const Entity enemy = feelTestPawn(*sim, 31.4f, 30.5f);  // to the east
    sim->submit(SetFactionCommand{enemy, 2});
    sim->submit(SetFightResponseCommand{enemy, FightResponse::Flee});
    sim->step();
    sim->submit(MeleeCommand{hero, enemy});
    for (int s = 0; s < 6; ++s) {
        const Position p = sim->things().registry().get<Position>(hero);
        sim->submit(DirectMoveCommand{hero, p.x, p.y + 0.05f});  // walking north
        sim->step();
        if (sim->melee().winding(hero)) {
            const Posture& posture = sim->things().registry().get<Posture>(hero);
            CHECK(posture.facingX > 0.7f);
        }
    }
}

TEST_CASE("Corpses: the player's swing at nobody goes down at a corpse lying before it and lands (sneed 2026-11-22)") {
    auto sim = feelTestSim(318);
    const Entity hero = feelTestHero(*sim, 30.5f, 30.5f, "Melee_Gladius");
    const Entity body = feelTestPawn(*sim, 31.7f, 30.6f);
    int torso = -1;
    const std::vector<std::string> labels = sim->health().partLabels(sim->things().registry().get<Thing>(body).def);
    for (size_t i = 0; i < labels.size(); ++i) {
        if (labels[i] == "torso") {
            torso = static_cast<int>(i);
        }
    }
    FEEL_REQUIRE(torso >= 0);
    sim->submit(DamagePawnCommand{body, "Blunt", 120.0f, torso});
    for (int i = 0; i < 20; ++i) {
        sim->step();
    }
    FEEL_REQUIRE(sim->things().registry().get<Health>(body).dead);
    feelTestFace(*sim, hero, 1.0f, 0.0f);
    CHECK(sim->melee().playerTarget(hero) == kNoEntity);  // the dead draw no aim assist ...
    CHECK(sim->melee().corpseAhead(hero) == body);         // ... but a swing at nobody goes down at the body
    feelTestFace(*sim, hero, -1.0f, 0.0f);
    CHECK(sim->melee().corpseAhead(hero) == kNoEntity);    // behind it: not taken
    feelTestFace(*sim, hero, 1.0f, 0.0f);
    int hits = 0;
    for (int swing = 0; swing < 3; ++swing) {
        std::string why;
        FEEL_REQUIRE(sim->melee().attack(hero, kNoEntity, true, &why));
        for (int s = 0; s < 200 && !(s > 2 && sim->melee().ready(hero)); ++s) {
            sim->step();
            for (const Event& e : sim->events()) {
                hits += e.type == EventType::MeleeHit && e.target == body && e.message == "hit" ? 1 : 0;
            }
        }
    }
    MESSAGE(hits << " of 3 swings landed on the corpse");
    CHECK(hits >= 2);
}

TEST_CASE("Corpses and the downed: a corpse before the player gets the very finisher a downed enemy gets, and both come quicker (sneed 2026-11-24)") {
    auto sim = feelTestSim(319);
    const MeleeRules& rules = sim->melee().rules();
    CHECK(rules.lowTimeFactor < 0.7f);  // "x3 attacks in the time it takes x2"
    CHECK(rules.lowTimeFactor > 0.6f);
    // At a downed enemy, locked on.
    const Entity hero = feelTestHero(*sim, 30.5f, 30.5f, "Melee_Knife");
    const Entity enemy = feelTestDowned(*sim, hero, 0.9f);
    feelTestFace(*sim, hero, 1.0f, 0.0f);
    std::string why;
    FEEL_REQUIRE(sim->melee().attack(hero, enemy, true, &why));
    const int downedFinisher = sim->things().registry().get<Melee>(hero).finisher;
    const int downedSteps = sim->things().registry().get<Melee>(hero).finisherSteps;
    CHECK(downedFinisher != 0);
    for (int s = 0; s < 300 && !(s > 2 && sim->melee().ready(hero)); ++s) {
        sim->step();
    }
    // Kill it where it lies; the player swings at nobody with the body before it.
    int torso = -1;
    const std::vector<std::string> labels = sim->health().partLabels(sim->things().registry().get<Thing>(enemy).def);
    for (size_t i = 0; i < labels.size(); ++i) {
        torso = labels[i] == "torso" ? static_cast<int>(i) : torso;
    }
    FEEL_REQUIRE(torso >= 0);
    sim->submit(DamagePawnCommand{enemy, "Blunt", 120.0f, torso});
    for (int i = 0; i < 20 && !sim->things().registry().get<Health>(enemy).dead; ++i) {
        sim->step();
    }
    FEEL_REQUIRE(sim->things().registry().get<Health>(enemy).dead);
    sim->submit(FocusCommand{hero, kNoEntity, FocusMode::Clear});
    sim->step();
    feelTestFace(*sim, hero, 1.0f, 0.0f);
    FEEL_REQUIRE(sim->melee().corpseAhead(hero) == enemy);
    FEEL_REQUIRE(sim->melee().attack(hero, kNoEntity, true, &why));
    const Melee& m = sim->things().registry().get<Melee>(hero);
    CHECK(m.finisher == downedFinisher);   // the same move as at the downed enemy
    CHECK(m.target == enemy);
    CHECK(std::abs(m.finisherSteps - downedSteps) <= 1);
    // Quicker than the finisher's own time at the weapons' pace.
    const MeleeFinisher& f = rules.finishers[static_cast<size_t>(downedFinisher - 1)];
    const int unhurried = static_cast<int>(std::lround(f.seconds * 30.0f * rules.attackTimeFactor));
    MESSAGE("finisher " << downedSteps << " steps, " << unhurried << " at the old pace");
    CHECK(downedSteps < unhurried);
}

// sneed 2026-11-28: two blows at the downed, "one is to injure, that's a hack; the impale is when it's calculated the
// final attack kills them, and it's a bit slower"; "critical that the impale goes through the centre, the chest".
TEST_CASE("Finishers: the sword hacks a downed enemy it won't kill, and impales, through the torso, one it will") {
    auto sim = feelTestSim(431);
    const Entity hero = feelTestHero(*sim, 30.5f, 30.5f, "Melee_Gladius");
    const Entity enemy = feelTestDowned(*sim, hero, 1.0f);
    feelTestFace(*sim, hero, 1.0f, 0.0f);
    entt::registry& reg = sim->things().registry();
    FEEL_REQUIRE(sim->health().incapacitated(enemy));
    FEEL_REQUIRE(!reg.get<Health>(enemy).dead);
    const auto torsoLeft = [&] {
        const int part = sim->combat().hitboxPart(enemy, "torso");
        return sim->health().zoneHealth(enemy, sim->health().partZone(enemy, part)) * sim->health().partZoneHp(enemy, part);
    };
    // A whole torso: the impale wouldn't kill it, so the blow is the hack.
    FEEL_REQUIRE(torsoLeft() > 30.0f);
    CHECK_FALSE(sim->melee().impaleKills(hero, enemy));
    std::string why;
    FEEL_REQUIRE(sim->melee().attack(hero, enemy, true, &why));
    const MeleeFinisher* hack = sim->melee().finisherOf(hero);
    FEEL_REQUIRE(hack != nullptr);
    CHECK_FALSE(hack->impale);
    CHECK(hack->lethal == "OneHandImpale");
    const int hackWindup = reg.get<Melee>(hero).stepsLeft;
    for (int s = 0; s < 200 && !sim->melee().ready(hero); ++s) {
        sim->step();
    }
    CHECK_FALSE(reg.get<Health>(enemy).dead);
    // Wear the torso down by hand until the impale's blow would end it: now the blow is the impale.
    const int part = sim->combat().hitboxPart(enemy, "torso");
    for (int i = 0; i < 60 && !sim->melee().impaleKills(hero, enemy) && !reg.get<Health>(enemy).dead; ++i) {
        sim->submit(DamagePawnCommand{enemy, "Cut", 2.0f, part});
        sim->step();
    }
    FEEL_REQUIRE(!reg.get<Health>(enemy).dead);
    FEEL_REQUIRE(sim->melee().impaleKills(hero, enemy));
    Position& at = reg.get<Position>(hero);
    const Position& foe = reg.get<Position>(enemy);
    at.x = foe.x - 1.0f;
    at.y = foe.y;
    for (int s = 0; s < 30; ++s) {
        sim->step();
    }
    FEEL_REQUIRE(sim->melee().attack(hero, enemy, true, &why));
    const MeleeFinisher* impale = sim->melee().finisherOf(hero);
    FEEL_REQUIRE(impale != nullptr);
    CHECK(impale->impale);
    CHECK(impale->drawWeapon);
    const int impaleWindup = reg.get<Melee>(hero).stepsLeft;
    CHECK(impaleWindup > hackWindup);              // a bit slower...
    CHECK(impaleWindup <= hackWindup * 13 / 10);   // ...not much
    bool torsoHit = false;
    for (int s = 0; s < 200 && !sim->melee().ready(hero); ++s) {
        sim->step();
        for (const Event& e : sim->events()) {
            if (e.type == EventType::MeleeHit && e.thing == hero && e.target == enemy) {
                torsoHit = torsoHit || sim->health().partZone(enemy, e.part) == sim->health().partZone(enemy, part);
            }
        }
    }
    CHECK(torsoHit);
    CHECK(reg.get<Health>(enemy).dead);
    CHECK(sim->melee().impales() == 1);
}
