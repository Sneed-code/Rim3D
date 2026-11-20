#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Melee.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Save.h"
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

// docs/design/structure-damage.md: melee weapons hurt anything that can take damage (buildings, walls, trees); guns do
// half to buildings and trees for now (sneed 2026-11-20); tree trunks stop bullets, blows and the player's body.

#define STRUCT_REQUIRE(...)    \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> structTestDefs() {
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

std::unique_ptr<Sim> structTestSim(uint64_t seed = 21) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = 64;
    settings.flatMap = true;
    auto sim = std::make_unique<Sim>(settings, structTestDefs());
    // A bare field: the test puts its own trees where it wants them.
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            sim->plants().remove(CellPos{x, y, 0});
        }
    }
    return sim;
}

Entity structTestPawn(Sim& sim, float x, float y, const char* weapon = nullptr) {
    const Entity pawn = sim.things().spawn("Human", CellPos{static_cast<int>(x), static_cast<int>(y), 0});
    Position& pos = sim.things().registry().get<Position>(pawn);
    pos.x = x;
    pos.y = y;
    if (weapon != nullptr) {
        sim.combat().equip(pawn, sim.things().make(sim.defs().idOf("Things", weapon)));
    }
    return pawn;
}

Entity structTestHero(Sim& sim, float x, float y, const char* weapon) {
    const Entity hero = structTestPawn(sim, x, y, weapon);
    sim.things().registry().emplace<Colonist>(hero);
    sim.jobs().addWorker(hero);
    sim.submit(ControlPawnCommand{hero});
    sim.step();
    return hero;
}

void structTestFace(Sim& sim, Entity pawn, float x, float y) {
    const Position& p = sim.things().registry().get<Position>(pawn);
    const float dx = x - p.x;
    const float dy = y - p.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    Posture& posture = sim.things().registry().get<Posture>(pawn);
    posture.facingX = dx / len;
    posture.facingY = dy / len;
}

// The player swings at nobody and the blow runs its course; returns the MeleeHit event.
Event structTestSwing(Sim& sim, Entity hero, float atX, float atY) {
    structTestFace(sim, hero, atX, atY);
    std::string why;
    if (!sim.melee().attack(hero, kNoEntity, true, &why)) {
        MESSAGE(why);
        return Event{};
    }
    Event hit;
    for (int i = 0; i < 300 && !sim.melee().ready(hero); ++i) {
        structTestFace(sim, hero, atX, atY);
        sim.step();
        for (const Event& e : sim.events()) {
            if (e.type == EventType::MeleeHit && e.thing == hero) {
                hit = e;
            }
        }
    }
    return hit;
}

std::vector<Event> structTestFly(Sim& sim) {
    std::vector<Event> out;
    for (int i = 0; i < 200; ++i) {
        sim.step();
        out.insert(out.end(), sim.events().begin(), sim.events().end());
        if (sim.things().registry().view<Projectile>().empty()) {
            break;
        }
    }
    return out;
}

CellPos structTestTree(Sim& sim, int x, int y) {
    const CellPos c{x, y, 0};
    sim.plants().remove(c);
    CHECK(sim.plants().place(c, sim.defs().idOf("Things", "Plant_TreeOak"), 1.0f));
    return c;
}

}  // namespace

TEST_CASE("Structure damage: a melee blow takes hit points off a wall, and enough blows knock it down") {
    auto sim = structTestSim();
    const Entity wall = sim->things().spawn("Wall", CellPos{31, 30, 0});
    STRUCT_REQUIRE(wall != kNoEntity);
    const Entity hero = structTestHero(*sim, 30.0f, 30.5f, "Melee_Mace");
    const int before = sim->things().registry().get<HitPoints>(wall).current;
    const Event hit = structTestSwing(*sim, hero, 31.5f, 30.5f);
    CHECK(hit.hit == ProjectileHitKind::Building);
    CHECK(hit.target == kNoEntity);
    CHECK(hit.message == "hit");
    CHECK(hit.amount > 0.0f);
    STRUCT_REQUIRE(sim->things().exists(wall));
    CHECK(sim->things().registry().get<HitPoints>(wall).current == before - static_cast<int>(hit.amount));
    CHECK(sim->melee().structureHits() == 1);

    // Swing until it falls.
    for (int i = 0; i < 400 && sim->things().exists(wall); ++i) {
        structTestSwing(*sim, hero, 31.5f, 30.5f);
    }
    CHECK_FALSE(sim->things().exists(wall));
    CHECK(sim->map().buildingAt(CellPos{31, 30, 0}) == kNoEntity);
}

TEST_CASE("Structure damage: a bullet does half its damage to a wall") {
    auto sim = structTestSim();
    const Entity shooter = structTestPawn(*sim, 20.5f, 30.5f, "Gun_BoltActionRifle");
    const Entity wall = sim->things().spawn("Wall", CellPos{30, 30, 0});
    STRUCT_REQUIRE(wall != kNoEntity);
    const int before = sim->things().registry().get<HitPoints>(wall).current;
    Rng rng(4);
    sim->combat().fire(shooter, 30.5f, 30.5f, sim->map().heightAt(30.5f, 30.5f) + 1.2f, 0.0f, rng);
    const std::vector<Event> events = structTestFly(*sim);
    float said = -1.0f;
    for (const Event& e : events) {
        if (e.type == EventType::ProjectileHit && e.hit == ProjectileHitKind::Building && e.target == wall) {
            said = e.amount;
        }
    }
    STRUCT_REQUIRE(said > 0.0f);
    const int lost = before - sim->things().registry().get<HitPoints>(wall).current;
    CHECK(lost == static_cast<int>(said));
    // The bolt-action's bullet (its def's damage, tuned) halved.
    const Def& gun = sim->defs().get(sim->defs().idOf("Things", "Gun_BoltActionRifle"));
    const Def& bullet = sim->defs().get(sim->defs().idOf("Projectiles", gun.text("verbs.0.defaultProjectile")));
    const double full = bullet.number("projectile.damageAmountBase", 0.0);
    if (full > 0.0) {
        CHECK(lost <= static_cast<int>(std::lround(full * 0.5)) + 1);
    }
}

TEST_CASE("Structure damage: plants have hit points; damage piles up, kills them at 0 and is saved") {
    auto sim = structTestSim();
    const CellPos tree = structTestTree(*sim, 30, 30);
    Plants& plants = sim->plants();
    CHECK(plants.maxHitPoints(tree) == 200);  // RimWorld's tree
    CHECK(plants.hitPoints(tree) == 200);
    const uint64_t unhurt = plants.hash();
    CHECK_FALSE(plants.damage(tree, 50));
    CHECK(plants.hitPoints(tree) == 150);
    CHECK(plants.hash() != unhurt);
    plants.setGrowth(tree, 1.0f);  // growing on keeps the wound
    CHECK(plants.hitPoints(tree) == 150);

    SaveLoadResult loaded = loadSim(saveSim(*sim), structTestDefs());
    STRUCT_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->plants().hitPoints(tree) == 150);
    CHECK(loaded.sim->plants().hash() == plants.hash());

    CHECK(plants.damage(tree, 150));
    CHECK_FALSE(plants.has(tree));
    CHECK(plants.hitPoints(tree) == 0);
    // A new plant in the cell starts unhurt.
    structTestTree(*sim, 30, 30);
    CHECK(plants.hitPoints(tree) == 200);
}

TEST_CASE("Structure damage: the trunk stands where Unreal draws the tree, and traces meet it") {
    auto sim = structTestSim();
    const CellPos tree = structTestTree(*sim, 30, 30);
    float x = 0.0f, y = 0.0f, radius = 0.0f, height = 0.0f;
    STRUCT_REQUIRE(sim->plants().trunkOf(tree, x, y, radius, height));
    CHECK(x >= 30.15f);
    CHECK(x <= 30.85f);
    CHECK(y >= 30.15f);
    CHECK(y <= 30.85f);
    CHECK(radius == doctest::Approx(0.28f));  // the oak's visual.stem.radius, fully grown
    const float z = sim->map().heightAt(x, y) + 1.2f;
    const TraceHit hit = sim->combat().trace(x - 5.0f, y, z, x + 5.0f, y, z, 0, kNoEntity, CellPos{-1, -1, 0});
    CHECK(hit.kind == ProjectileHitKind::Plant);
    CHECK(hit.cell == tree);
    CHECK(hit.x == doctest::Approx(x - radius).epsilon(0.01));
    // Over the top of it, nothing.
    const float over = sim->map().heightAt(x, y) + height + 1.0f;
    CHECK(sim->combat().trace(x - 5.0f, y, over, x + 5.0f, y, over, 0, kNoEntity, CellPos{-1, -1, 0}).kind != ProjectileHitKind::Plant);
    // Grass doesn't stop anything.
    sim->plants().remove(tree);
    CHECK(sim->plants().place(tree, sim->defs().idOf("Things", "Plant_Grass"), 1.0f));
    CHECK(sim->combat().trace(x - 5.0f, y, z, x + 5.0f, y, z, 0, kNoEntity, CellPos{-1, -1, 0}).kind == ProjectileHitKind::None);
}

TEST_CASE("Structure damage: a bullet into a tree does half its damage, a melee blow its whole damage") {
    auto sim = structTestSim();
    const CellPos tree = structTestTree(*sim, 30, 30);
    float x = 0.0f, y = 0.0f, radius = 0.0f, height = 0.0f;
    STRUCT_REQUIRE(sim->plants().trunkOf(tree, x, y, radius, height));
    const Entity shooter = structTestPawn(*sim, 20.5f, y, "Gun_BoltActionRifle");
    Rng rng(9);
    sim->combat().fire(shooter, x, y, sim->map().heightAt(x, y) + 1.2f, 0.0f, rng);
    float said = -1.0f;
    for (const Event& e : structTestFly(*sim)) {
        if (e.type == EventType::ProjectileHit && e.hit == ProjectileHitKind::Plant) {
            said = e.amount;
            CHECK(e.cell == tree);
        }
    }
    STRUCT_REQUIRE(said > 0.0f);
    CHECK(sim->plants().hitPoints(tree) == 200 - static_cast<int>(said));

    const int beforeBlow = sim->plants().hitPoints(tree);
    const Entity hero = structTestHero(*sim, x - radius - 0.7f, y, "Melee_Mace");
    const Event blow = structTestSwing(*sim, hero, x, y);
    CHECK(blow.hit == ProjectileHitKind::Plant);
    CHECK(blow.cell == tree);
    CHECK(blow.message == "hit");
    STRUCT_REQUIRE(blow.amount > 0.0f);
    CHECK(sim->plants().hitPoints(tree) == beforeBlow - static_cast<int>(blow.amount));
    CHECK(blow.amount > said * 0.5f);  // a mace blow is no lighter than a rifle round's half
}

TEST_CASE("Structure damage: a blast hurts the plants round it") {
    auto sim = structTestSim();
    const DefId grass = sim->defs().idOf("Things", "Plant_Grass");
    for (int y = 28; y <= 32; ++y) {
        for (int x = 28; x <= 32; ++x) {
            sim->plants().place(CellPos{x, y, 0}, grass, 1.0f);
        }
    }
    const CellPos tree = structTestTree(*sim, 32, 31);
    const size_t before = sim->plants().count();
    Explosion ex;
    ex.x = 30.5f;
    ex.y = 30.5f;
    ex.z = sim->map().heightAt(30.5f, 30.5f) + 0.2f;
    ex.radius = 3.0f;
    ex.damage = 100.0f;
    ex.damageType = sim->defs().idOf("DamageTypes", "Bomb");
    const ExplosionResult result = sim->combat().explode(ex);
    CHECK(result.plantsDestroyed > 0);
    CHECK(sim->plants().count() < before);
    CHECK_FALSE(sim->plants().has(CellPos{30, 30, 0}));  // the grass at the heart of it is gone
    CHECK(sim->plants().has(tree));                      // the tree at the edge stands, hurt
    CHECK(sim->plants().hitPoints(tree) < 200);
}

TEST_CASE("Structure damage: the player can't walk through a tree trunk; it slides round it") {
    auto sim = structTestSim();
    const CellPos tree = structTestTree(*sim, 30, 30);
    float x = 0.0f, y = 0.0f, radius = 0.0f, height = 0.0f;
    STRUCT_REQUIRE(sim->plants().trunkOf(tree, x, y, radius, height));
    const Entity hero = structTestHero(*sim, x - 2.0f, y + 0.15f, "Melee_Mace");
    float closest = 100.0f;
    for (int i = 0; i < 120; ++i) {
        const Position& p = sim->things().registry().get<Position>(hero);
        sim->submit(DirectMoveCommand{hero, p.x + 0.05f, p.y});
        sim->step();
        const Position& q = sim->things().registry().get<Position>(hero);
        closest = std::min(closest, std::sqrt((q.x - x) * (q.x - x) + (q.y - y) * (q.y - y)));
    }
    CHECK(closest >= radius + kPawnRadius - 0.01f);
    // It got past by sliding round, not stuck in front.
    CHECK(sim->things().registry().get<Position>(hero).x > x + 0.5f);
}
