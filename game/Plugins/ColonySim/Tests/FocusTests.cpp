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
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
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

// These tests check docs/design/focus.md: lock-on focus, footwork, blows at the downed and fight responses.

#define FOCUS_REQUIRE(...)     \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> focusTestDefs() {
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
        for (const std::string& e : errors) {
            MESSAGE(e);
        }
        return db;
    }();
    return defs;
}

std::unique_ptr<Sim> focusTestSim(uint64_t seed = 11) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = 64;
    settings.flatMap = true;
    return std::make_unique<Sim>(settings, focusTestDefs());
}

Entity focusTestPawn(Sim& sim, float x, float y, const char* weapon = nullptr) {
    const Entity pawn = sim.things().spawn("Human", CellPos{static_cast<int>(x), static_cast<int>(y), 0});
    Position& pos = sim.things().registry().get<Position>(pawn);
    pos.x = x;
    pos.y = y;
    if (weapon != nullptr) {
        sim.combat().equip(pawn, sim.things().make(sim.defs().idOf("Things", weapon)));
    }
    return pawn;
}

Entity focusTestHero(Sim& sim, float x, float y, const char* weapon) {
    const Entity hero = focusTestPawn(sim, x, y, weapon);
    sim.things().registry().emplace<Colonist>(hero);
    sim.jobs().addWorker(hero);
    sim.submit(ControlPawnCommand{hero});
    sim.step();
    return hero;
}

void focusTestFace(Sim& sim, Entity pawn, float fx, float fy) {
    Posture& p = sim.things().registry().get<Posture>(pawn);
    p.facingX = fx;
    p.facingY = fy;
}

void focusTestHeal(Sim& sim, Entity pawn) {
    sim.things().registry().replace<Health>(pawn, Health{});
    sim.health().refresh(pawn);
}

float focusTestDistance(Sim& sim, Entity a, Entity b) {
    const Position& p = sim.things().registry().get<Position>(a);
    const Position& q = sim.things().registry().get<Position>(b);
    return std::sqrt((p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y));
}

bool focusTestFailed(const Sim& sim) {
    return std::any_of(sim.events().begin(), sim.events().end(), [](const Event& e) { return e.type == EventType::CommandFailed; });
}

int focusTestSwings(const Sim& sim, Entity pawn) {
    return static_cast<int>(std::count_if(sim.events().begin(), sim.events().end(),
                                          [&](const Event& e) { return e.type == EventType::MeleeSwing && e.thing == pawn; }));
}

const ThingView* focusTestView(const Snapshot& snap, Entity e) {
    for (const ThingView& v : snap.things) {
        if (v.id == e) {
            return &v;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("Focus: Tab picks the nearest enemy in front first, cycles, and F1 lets go") {
    auto sim = focusTestSim(81);
    const Entity hero = focusTestHero(*sim, 30.5f, 30.5f, "Melee_Longsword");
    const Entity front = focusTestPawn(*sim, 36.5f, 30.5f);  // 6 m ahead
    const Entity behind = focusTestPawn(*sim, 26.5f, 30.5f);  // 4 m behind: counts as 8
    const Entity far = focusTestPawn(*sim, 50.5f, 50.5f);    // beyond 20 m
    const Entity friendly = focusTestPawn(*sim, 31.5f, 31.5f);
    for (const Entity e : {front, behind, far}) {
        sim->submit(SetFactionCommand{e, 2});
    }
    sim->step();
    (void)friendly;
    focusTestFace(*sim, hero, 1.0f, 0.0f);
    sim->submit(FocusCommand{hero, kNoEntity, FocusMode::Next});
    sim->step();
    CHECK_FALSE(focusTestFailed(*sim));
    CHECK(sim->melee().focusOf(hero) == front);
    sim->submit(FocusCommand{hero, kNoEntity, FocusMode::Next});
    sim->step();
    CHECK(sim->melee().focusOf(hero) == behind);  // never the friend, never the far one
    sim->submit(FocusCommand{hero, kNoEntity, FocusMode::Next});
    sim->step();
    CHECK(sim->melee().focusOf(hero) == front);  // round again

    // Locked on, the hero faces its focus while it walks across it, and the snapshot says so.
    for (int s = 0; s < 20; ++s) {
        const Position& p = sim->things().registry().get<Position>(hero);
        sim->submit(DirectMoveCommand{hero, p.x, p.y + 0.1f});
        sim->step();
    }
    const Posture& posture = sim->things().registry().get<Posture>(hero);
    const Position& hp = sim->things().registry().get<Position>(hero);
    const Position& fp = sim->things().registry().get<Position>(front);
    const float dx = fp.x - hp.x;
    const float dy = fp.y - hp.y;
    const float d = std::sqrt(dx * dx + dy * dy);
    CHECK((posture.facingX * dx + posture.facingY * dy) / d > 0.99f);
    Snapshot snap;
    sim->writeSnapshot(snap);
    const ThingView* view = focusTestView(snap, hero);
    FOCUS_REQUIRE(view != nullptr);
    CHECK(view->focusTarget == front);
    CHECK(view->focusPlayer);
    const ThingView* targetView = focusTestView(snap, front);
    FOCUS_REQUIRE(targetView != nullptr);
    CHECK(targetView->lowestPart == doctest::Approx(1.0f));  // the HP bar of a man unhurt
    sim->things().registry().get<Health>(front).zones.assign(6, 1.0f);
    sim->things().registry().get<Health>(front).zones[1] = 0.4f;
    Snapshot hurt;
    sim->writeSnapshot(hurt);
    const ThingView* hurtView = focusTestView(hurt, front);
    FOCUS_REQUIRE(hurtView != nullptr);
    CHECK(hurtView->lowestPart == doctest::Approx(0.4f));  // its worst body part

    // F1 lets go; a second F1 has nothing to let go of.
    sim->submit(FocusCommand{hero, kNoEntity, FocusMode::Clear});
    sim->step();
    CHECK(sim->melee().focusOf(hero) == kNoEntity);
    sim->submit(FocusCommand{hero, kNoEntity, FocusMode::Clear});
    sim->step();
    CHECK(focusTestFailed(*sim));
    // Set locks on to anyone near, even a friend; a dead one lets go by itself.
    sim->submit(FocusCommand{hero, behind, FocusMode::Set});
    sim->step();
    CHECK(sim->melee().focusOf(hero) == behind);
    sim->health().kill(behind, DeathCause::BloodLoss);
    sim->step();
    CHECK(sim->melee().focusOf(hero) == kNoEntity);
}

TEST_CASE("Focus: the player's swing goes at the focus, and down at a body on the ground") {
    auto sim = focusTestSim(82);
    const Entity hero = focusTestHero(*sim, 30.5f, 30.5f, "Melee_Longsword");
    const Entity enemy = focusTestPawn(*sim, 31.6f, 31.0f);  // a standing man (no fighter: it doesn't sidestep)
    sim->submit(FocusCommand{hero, enemy, FocusMode::Set});
    sim->step();
    focusTestFace(*sim, hero, -1.0f, 0.0f);  // turned away: the focus turns it back
    sim->submit(MeleeCommand{hero, kNoEntity, MeleeMode::Attack});
    int hits = 0;
    for (int s = 0; s < 40; ++s) {
        sim->step();
        for (const Event& e : sim->events()) {
            if (e.type == EventType::MeleeHit && e.message != "missed" && e.target == enemy) {
                ++hits;
            }
        }
    }
    CHECK(hits == 1);

    // No focus, a man down at its feet: the swing picks him and goes down (meleeLow), and lands.
    sim->submit(FocusCommand{hero, kNoEntity, FocusMode::Clear});
    while (!sim->melee().ready(hero)) {
        sim->step();
    }
    Health& h = sim->things().registry().get<Health>(enemy);
    h.downed = true;
    sim->things().registry().get<Posture>(enemy).stance = Stance::Prone;
    Position& ep = sim->things().registry().get<Position>(enemy);
    ep.x = 31.4f;
    ep.y = 30.5f;
    focusTestFace(*sim, hero, 1.0f, 0.0f);
    sim->submit(MeleeCommand{hero, kNoEntity, MeleeMode::Attack});
    sim->step();
    Snapshot snap;
    sim->writeSnapshot(snap);
    const ThingView* view = focusTestView(snap, hero);
    FOCUS_REQUIRE(view != nullptr);
    CHECK(view->meleeLow);
    int low = 0;
    for (int s = 0; s < 40; ++s) {
        sim->step();
        for (const Event& e : sim->events()) {
            low += e.type == EventType::MeleeHit && e.target == enemy && e.message != "missed" ? 1 : 0;
        }
    }
    CHECK(low == 1);
}

TEST_CASE("Focus: melee fighters lock on, step round each other between swings, and still fight") {
    auto sim = focusTestSim(83);
    const Entity raider = focusTestPawn(*sim, 34.5f, 30.5f, "Melee_Knife");
    const Entity colonist = focusTestPawn(*sim, 30.5f, 30.5f, "Melee_Club");
    Colonist& c = sim->things().registry().emplace<Colonist>(colonist);
    c.skills.push_back(SkillRecord{"Melee", 16});
    c.traits.push_back(TraitRecord{"Nimble", 0});
    sim->submit(SetFactionCommand{raider, 2});
    sim->submit(DraftCommand{colonist, true});
    sim->submit(AttackOrderCommand{colonist, raider});
    sim->step();
    int swings = 0;
    int focused = 0;
    int facing = 0;
    int close = 0;
    for (int s = 0; s < 900; ++s) {
        sim->step();
        focusTestHeal(*sim, raider);
        focusTestHeal(*sim, colonist);
        swings += focusTestSwings(*sim, colonist) + focusTestSwings(*sim, raider);
        if (focusTestDistance(*sim, raider, colonist) < 2.0f) {
            ++close;
            focused += sim->melee().focusOf(colonist) == raider ? 1 : 0;
            const Posture& p = sim->things().registry().get<Posture>(colonist);
            const Position& a = sim->things().registry().get<Position>(colonist);
            const Position& b = sim->things().registry().get<Position>(raider);
            const float d = focusTestDistance(*sim, raider, colonist);
            facing += d > 0.01f && (p.facingX * (b.x - a.x) + p.facingY * (b.y - a.y)) / d > 0.9f ? 1 : 0;
        }
    }
    MESSAGE("swings " << swings << ", footwork " << sim->melee().footworks() << ", close " << close << ", focused " << focused
                      << ", facing " << facing);
    CHECK(close > 100);
    CHECK(swings >= 8);                        // footwork doesn't stop the fight
    CHECK(sim->melee().footworks() >= 4);      // they move between swings
    CHECK(focused >= close * 8 / 10);          // locked on while at it
    CHECK(facing >= close * 7 / 10);           // and facing the enemy (not their walk) most of the time
}

TEST_CASE("Focus: fight responses: flee on sight, flee when hurt, fists") {
    // FleeOnSight: an enemy pawn that sees the knife coming runs and never swings.
    auto sim = focusTestSim(84);
    const Entity runner = focusTestPawn(*sim, 36.5f, 30.5f, "Melee_Knife");
    const Entity hunter = focusTestPawn(*sim, 30.5f, 30.5f, "Melee_Knife");
    sim->submit(SetFactionCommand{runner, 2});
    sim->submit(SetFightResponseCommand{runner, FightResponse::FleeOnSight});
    sim->submit(DraftCommand{hunter, true});
    sim->submit(AttackOrderCommand{hunter, runner});
    sim->step();
    CHECK_FALSE(focusTestFailed(*sim));
    CHECK(sim->melee().responseOf(runner) == FightResponse::FleeOnSight);
    int runnerSwings = 0;
    int runnerSprints = 0;
    int hunterSprints = 0;
    for (int s = 0; s < 300; ++s) {
        sim->step();
        runnerSwings += focusTestSwings(*sim, runner);
        runnerSprints += sim->fighters().sprinting(runner) ? 1 : 0;  // runs from the enemy at a sprint
        hunterSprints += sim->fighters().sprinting(hunter) ? 1 : 0;  // and the knife sprints after it
    }
    MESSAGE("runner sprinted " << runnerSprints << " steps, hunter " << hunterSprints);
    CHECK(runnerSwings == 0);
    CHECK(runnerSprints > 30);
    CHECK(hunterSprints > 30);
    CHECK(sim->things().registry().get<Fighter>(runner).fallingBack);

    // Flee: it fights until hurt, then runs and stops swinging.
    auto sim2 = focusTestSim(85);
    const Entity wimp = focusTestPawn(*sim2, 32.5f, 30.5f, "Melee_Knife");
    const Entity foe = focusTestPawn(*sim2, 30.5f, 30.5f, "Melee_Club");
    sim2->submit(SetFactionCommand{wimp, 2});
    sim2->submit(SetFightResponseCommand{wimp, FightResponse::Flee});
    sim2->submit(DraftCommand{foe, true});
    sim2->submit(AttackOrderCommand{foe, wimp});
    int hurtAt = -1;
    int swingsAfter = 0;
    for (int s = 0; s < 600; ++s) {
        sim2->step();
        if (sim2->things().registry().get<Health>(wimp).downed) {
            break;
        }
        if (hurtAt >= 0 && s > hurtAt + 30) {
            swingsAfter += focusTestSwings(*sim2, wimp);
        }
        if (hurtAt < 0 && sim2->things().registry().get<Fighter>(wimp).cowerUntil > sim2->time().steps) {
            hurtAt = s;
        }
    }
    CHECK(hurtAt >= 0);
    CHECK(swingsAfter == 0);

    // Fists: an unarmed pawn charges like a brawler.
    auto sim3 = focusTestSim(86);
    const Entity boxer = focusTestPawn(*sim3, 30.5f, 30.5f);
    CHECK_FALSE(sim3->melee().charges(boxer));
    sim3->submit(SetFightResponseCommand{boxer, FightResponse::Fists});
    sim3->step();
    CHECK(sim3->melee().charges(boxer));
}

TEST_CASE("Focus: focus, footwork and responses survive a save and a replay") {
    auto sim = focusTestSim(87);
    const Entity raider = focusTestPawn(*sim, 33.5f, 30.5f, "Melee_Knife");
    const Entity colonist = focusTestPawn(*sim, 30.5f, 30.5f, "Melee_Club");
    sim->submit(SetFactionCommand{raider, 2});
    sim->submit(SetFightResponseCommand{raider, FightResponse::Fists});
    sim->submit(DraftCommand{colonist, true});
    sim->submit(AttackOrderCommand{colonist, raider});
    sim->submit(FocusCommand{colonist, raider, FocusMode::Set});
    sim->stepMany(120);
    SaveLoadResult loaded = loadSim(saveSim(*sim), focusTestDefs());
    FOCUS_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->stateHash() == sim->stateHash());
    CHECK(loaded.sim->melee().responseOf(raider) == FightResponse::Fists);
    for (int s = 0; s < 400; ++s) {
        sim->step();
        loaded.sim->step();
        if (sim->stateHash() != loaded.sim->stateHash()) {
            FAIL("different after " << s << " steps");
            break;
        }
    }
    const Replay replay = recordReplay(*sim);
    std::string error;
    Replay back;
    FOCUS_REQUIRE(readReplay(writeReplay(replay), back, error));
    int focus = 0;
    int response = 0;
    for (const auto& c : back.commands) {
        if (const auto* f = std::get_if<FocusCommand>(&c.command)) {
            focus += f->target == raider && f->mode == FocusMode::Set ? 1 : 0;
        }
        if (const auto* r = std::get_if<SetFightResponseCommand>(&c.command)) {
            response += r->response == FightResponse::Fists ? 1 : 0;
        }
    }
    CHECK(focus == 1);
    CHECK(response == 1);
}
