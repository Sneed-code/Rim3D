#include "ColonySim/BodyMotion.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Looks.h"
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

// These tests check docs/design/lock-on.md: Elden Ring style lock-on (sneed 2026-11-18).

#define LOCK_REQUIRE(...)      \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> lockTestDefs() {
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

std::unique_ptr<Sim> lockTestSim(uint64_t seed) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = 64;
    settings.flatMap = true;
    return std::make_unique<Sim>(settings, lockTestDefs());
}

Entity lockTestPawn(Sim& sim, float x, float y, const char* weapon = nullptr) {
    const Entity pawn = sim.things().spawn("Human", CellPos{static_cast<int>(x), static_cast<int>(y), 0});
    Position& pos = sim.things().registry().get<Position>(pawn);
    pos.x = x;
    pos.y = y;
    if (weapon != nullptr) {
        sim.combat().equip(pawn, sim.things().make(sim.defs().idOf("Things", weapon)));
    }
    return pawn;
}

Entity lockTestHero(Sim& sim, float x, float y, const char* weapon) {
    const Entity hero = lockTestPawn(sim, x, y, weapon);
    sim.things().registry().emplace<Colonist>(hero);
    sim.jobs().addWorker(hero);
    sim.submit(ControlPawnCommand{hero});
    sim.step();
    return hero;
}

float lockTestDistance(Sim& sim, Entity a, Entity b) {
    const Position& p = sim.things().registry().get<Position>(a);
    const Position& q = sim.things().registry().get<Position>(b);
    return std::sqrt((p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y));
}

float lockTestFacingCos(Sim& sim, Entity pawn, Entity target) {
    const Posture& p = sim.things().registry().get<Posture>(pawn);
    const Position& a = sim.things().registry().get<Position>(pawn);
    const Position& b = sim.things().registry().get<Position>(target);
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float d = std::sqrt(dx * dx + dy * dy);
    return d > 0.01f ? (p.facingX * dx + p.facingY * dy) / d : 1.0f;
}

}  // namespace

TEST_CASE("Lock-on: a weapon's swing goes at the chest, fists at the head") {
    auto sim = lockTestSim(91);
    const Entity hero = lockTestHero(*sim, 30.5f, 30.5f, "Melee_Longsword");
    const Entity enemy = lockTestPawn(*sim, 31.5f, 30.5f);
    sim->step();
    float cx = 0.0f, cy = 0.0f, cz = 0.0f;
    LOCK_REQUIRE(sim->combat().aimPoint(enemy, cx, cy, cz));
    float x = 0.0f, y = 0.0f, z = 0.0f;
    LOCK_REQUIRE(sim->melee().focusPoint(hero, enemy, false, x, y, z));
    CHECK(x == doctest::Approx(cx));
    CHECK(z == doctest::Approx(cz));  // the chest
    LOCK_REQUIRE(sim->melee().focusPoint(hero, enemy, true, x, y, z));
    MESSAGE("chest " << cz << " m, head " << z << " m");
    CHECK(z > cz + 0.2f);  // a punch goes at the head
    CHECK(z < cz + 0.8f);
}

TEST_CASE("Lock-on: with a gun the lock stays but the body doesn't turn to it") {
    auto sim = lockTestSim(92);
    const Entity hero = lockTestHero(*sim, 30.5f, 30.5f, "Gun_Revolver");
    const Entity enemy = lockTestPawn(*sim, 30.5f, 36.5f);  // to its north
    sim->submit(SetFactionCommand{enemy, 2});
    sim->submit(SetFightResponseCommand{enemy, FightResponse::Flee});
    sim->submit(FocusCommand{hero, enemy, FocusMode::Set});
    sim->step();
    Posture& p = sim->things().registry().get<Posture>(hero);
    p.facingX = 1.0f;
    p.facingY = 0.0f;
    for (int s = 0; s < 10; ++s) {
        sim->step();
    }
    CHECK(sim->melee().focusOf(hero) == enemy);
    CHECK(lockTestFacingCos(*sim, hero, enemy) < 0.2f);  // still facing east: free aim
}

TEST_CASE("Lock-on: with melee, a sideways walk circles the target at the same distance") {
    auto sim = lockTestSim(93);
    const Entity hero = lockTestHero(*sim, 30.5f, 30.5f, "Melee_Longsword");
    const Entity enemy = lockTestPawn(*sim, 33.5f, 30.5f);
    sim->step();
    sim->submit(FocusCommand{hero, enemy, FocusMode::Set});
    sim->step();
    const float before = lockTestDistance(*sim, hero, enemy);
    // The camera faces the target, so a press of D is a step along the tangent: here straight north each step.
    for (int s = 0; s < 60; ++s) {
        const Position& p = sim->things().registry().get<Position>(hero);
        const Position& t = sim->things().registry().get<Position>(enemy);
        float tx = -(t.y - p.y);
        float ty = t.x - p.x;
        const float n = std::sqrt(tx * tx + ty * ty);
        sim->submit(DirectMoveCommand{hero, p.x + tx / n * 0.1f, p.y + ty / n * 0.1f});
        sim->step();
    }
    const float after = lockTestDistance(*sim, hero, enemy);
    MESSAGE("distance " << before << " m to " << after << " m");
    CHECK(after == doctest::Approx(before).epsilon(0.03));
    const Position& p = sim->things().registry().get<Position>(hero);
    CHECK(p.y > 32.5f);  // it went round
    CHECK(lockTestFacingCos(*sim, hero, enemy) > 0.99f);
    // Walking straight back is not circling: it backs off, still locked and facing.
    for (int s = 0; s < 20; ++s) {
        const Position& q = sim->things().registry().get<Position>(hero);
        const Position& t = sim->things().registry().get<Position>(enemy);
        const float d = lockTestDistance(*sim, hero, enemy);
        sim->submit(DirectMoveCommand{hero, q.x - (t.x - q.x) / d * 0.05f, q.y - (t.y - q.y) / d * 0.05f});
        sim->step();
    }
    CHECK(lockTestDistance(*sim, hero, enemy) > after + 0.8f);
    CHECK(sim->melee().focusOf(hero) == enemy);
}

TEST_CASE("Lock-on: the player's lock lets go 3 s after losing sight of the target") {
    auto sim = lockTestSim(94);
    const Entity hero = lockTestHero(*sim, 30.5f, 30.5f, "Melee_Longsword");
    const Entity enemy = lockTestPawn(*sim, 36.5f, 30.5f);
    sim->step();
    sim->submit(FocusCommand{hero, enemy, FocusMode::Set});
    sim->step();
    for (int s = 0; s < 120; ++s) {
        sim->step();
    }
    CHECK(sim->melee().focusOf(hero) == enemy);  // in sight: kept
    for (int y = 27; y <= 34; ++y) {
        sim->things().spawn("Wall", CellPos{33, y, 0});
    }
    int steps = 0;
    while (sim->melee().focusOf(hero) == enemy && steps < 300) {
        sim->step();
        ++steps;
    }
    MESSAGE("let go after " << steps << " steps behind the wall");
    CHECK(steps >= 80);
    CHECK(steps <= 110);
}

TEST_CASE("Lock-on: a fighter hand to hand turns at once on a new enemy swinging at it") {
    auto sim = lockTestSim(95);
    const Entity colonist = lockTestPawn(*sim, 30.5f, 30.5f, "Melee_Knife");
    sim->things().registry().emplace<Colonist>(colonist);
    const Entity far = lockTestPawn(*sim, 34.0f, 30.5f);  // the first enemy, 3.5 m off: it runs
    sim->submit(SetFactionCommand{far, 2});
    sim->submit(SetFightResponseCommand{far, FightResponse::FleeOnSight});
    sim->submit(DraftCommand{colonist, true});
    for (int s = 0; s < 40; ++s) {
        sim->step();
    }
    LOCK_REQUIRE(sim->things().registry().get<Fighter>(colonist).target == far);
    const Position at = sim->things().registry().get<Position>(colonist);
    const Entity close = lockTestPawn(*sim, at.x - 0.9f, at.y, "Melee_Club");  // a second comes up behind and swings
    sim->submit(SetFactionCommand{close, 2});
    sim->step();
    std::string why;
    if (sim->melee().ready(close)) {  // its own AI may have started the swing already
        const bool started = sim->melee().attack(close, colonist, false, &why);
        MESSAGE(why);
        LOCK_REQUIRE(started);
    }
    LOCK_REQUIRE(sim->melee().winding(close));
    int steps = 0;
    while (sim->things().registry().get<Fighter>(colonist).target != close && steps < 30) {
        sim->step();
        ++steps;
    }
    MESSAGE("turned on the new enemy after " << steps << " steps");
    CHECK(steps <= 2);
}

TEST_CASE("Lock-on: a gunman backs off from a man rushing in with a knife, and shoots") {
    auto sim = lockTestSim(96);
    const Entity gunman = lockTestPawn(*sim, 30.5f, 30.5f, "Gun_Revolver");
    sim->submit(SetFactionCommand{gunman, 2});
    const Entity rusher = lockTestPawn(*sim, 34.5f, 30.5f, "Melee_Knife");
    sim->things().registry().emplace<Colonist>(rusher);
    sim->submit(DraftCommand{rusher, true});
    sim->submit(AttackOrderCommand{rusher, gunman});
    const Position start = sim->things().registry().get<Position>(gunman);
    int shots = 0;
    float furthest = 0.0f;
    for (int s = 0; s < 150; ++s) {
        sim->step();
        sim->things().registry().replace<Health>(rusher, Health{});
        sim->health().refresh(rusher);
        for (const Event& e : sim->events()) {
            shots += e.type == EventType::ShotFired && e.thing == gunman ? 1 : 0;
        }
        const Position& p = sim->things().registry().get<Position>(gunman);
        furthest = std::max(furthest, start.x - p.x);
    }
    MESSAGE("backed off " << furthest << " m, " << shots << " shots");
    CHECK(furthest > 0.7f);
    CHECK(shots >= 1);
}
