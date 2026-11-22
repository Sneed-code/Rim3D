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
    // At the default Melee skill (4) a swing takes 1 / 0.88 of the weapon's time.
    CHECK(club >= 32);
    CHECK(club <= 36);
    CHECK(gladius >= 43);
    CHECK(gladius <= 48);
    CHECK(longsword >= 26);
    CHECK(longsword <= 30);
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
    // The player walks on under the swing; the blow lands.
    bool hit = false;
    for (int s = 0; s < 40 && !hit; ++s) {
        Position& p = sim->things().registry().get<Position>(hero);
        sim->submit(DirectMoveCommand{hero, p.x + 0.05f, p.y});
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
    CHECK(dodges >= 3);
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

TEST_CASE("Finishers: a knife at a downed enemy kneels into a slow slam that lands at its hit time, holding the body still") {
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
    CHECK(f->kneel);
    const int hitStep = static_cast<int>(std::lround(f->hitSeconds * 30.0f));
    const int total = static_cast<int>(std::lround(f->seconds * 30.0f));
    int struckAt = -1;
    int readyAt = -1;
    float sawT = 0.0f;
    float apartAtHit = -1.0f;
    for (int s = 1; s <= total + 10; ++s) {
        // The player keeps pushing forward: through a finisher the body stays where it knelt.
        const Position before = sim->things().registry().get<Position>(hero);
        if (s > hitStep / 2) {
            sim->submit(DirectMoveCommand{hero, before.x, before.y + 0.08f});
        }
        sim->step();
        if (s > hitStep / 2 && sim->melee().finisherOf(hero) != nullptr) {
            CHECK(std::fabs(sim->things().registry().get<Position>(hero).y - before.y) < 1e-3f);
        }
        for (const Event& e : sim->events()) {
            if (e.type == EventType::MeleeHit && e.thing == hero && struckAt < 0) {
                struckAt = s;
                const Position& hp = sim->things().registry().get<Position>(hero);
                const Position& ep = sim->things().registry().get<Position>(enemy);
                apartAtHit = std::hypot(hp.x - ep.x, hp.y - ep.y);
            }
        }
        Snapshot snap;
        sim->writeSnapshot(snap);
        if (const ThingView* v = snap.find(hero); v != nullptr && v->meleeFinisher != 0) {
            CHECK(v->meleeFinisherTarget == enemy);
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
    // It stepped in to kneel at its place beside the body before the blow.
    CHECK(std::fabs(apartAtHit - f->standOff) < 0.12f);
}

TEST_CASE("Finishers: a finisher whose body is taken off the map ends at once, and the next swing is the weapon's own") {
    auto sim = feelTestSim(404);
    const Entity hero = feelTestHero(*sim, 30.5f, 30.5f, "Melee_Spear");
    const Entity enemy = feelTestDowned(*sim, hero, 0.9f);
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
