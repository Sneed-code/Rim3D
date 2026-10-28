// Manned artillery (docs/design/artillery.md): the mortar and the rocketswarm launcher, manning, aiming, scatter,
// shells over walls, roofs, ammunition and saves.
#include "ColonySim/Artillery.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Fuel.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Power.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"
#include "FuelTestHelper.h"

#include <doctest.h>

#include <cmath>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace colony;

#define ARTY_REQUIRE(cond) \
    CHECK((cond));         \
    if (!(cond)) return

namespace {

std::shared_ptr<const DefDatabase> artyDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

std::unique_ptr<Sim> artySim(uint64_t seed = 41) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = 160;
    settings.flatMap = true;
    return std::make_unique<Sim>(settings, artyDefs());
}

// A colonist with this Shooting level who works (no needs, so it stays at its post).
Entity artyColonist(Sim& sim, CellPos cell, int shooting = 8) {
    const Entity pawn = sim.things().spawn("Human", cell);
    Colonist c;
    c.skills.push_back(SkillRecord{"Shooting", shooting});
    sim.things().registry().emplace<Colonist>(pawn, c);
    sim.jobs().addWorker(pawn, {}, false);
    return pawn;
}

Entity artyHostile(Sim& sim, float x, float y) {
    const Entity pawn = sim.things().spawn("Human", CellPos{static_cast<int>(x), static_cast<int>(y), 0});
    sim.submit(SetFactionCommand{pawn, 2});
    return pawn;
}

// Holds a pawn still where it is put (the hostile would otherwise walk at the colony).
void artyHold(Sim& sim, Entity pawn, float x, float y) {
    if (sim.things().registry().valid(pawn) && sim.things().registry().all_of<Position>(pawn)) {
        Position& pos = sim.things().registry().get<Position>(pawn);
        pos.x = x;
        pos.y = y;
    }
}

int artyCount(const Sim& sim, EventType type, Entity thing = kNoEntity) {
    int n = 0;
    for (const Event& e : sim.events()) {
        n += e.type == type && (thing == kNoEntity || e.thing == thing) ? 1 : 0;
    }
    return n;
}

uint64_t artyCmd(Sim& sim, const char* action, Entity building, Entity pawn = kNoEntity, Entity target = kNoEntity, float x = 0.0f,
                 float y = 0.0f) {
    return sim.submit(ArtilleryCommand{action, building, pawn, target, x, y});
}

// Steps once (commands are applied at the start of a step) and says whether the command failed.
bool artyFailed(Sim& sim, uint64_t id) {
    sim.step();
    for (const Event& e : sim.events()) {
        if (e.type == EventType::CommandFailed && e.command == id) {
            return true;
        }
    }
    return false;
}

float artyAmmo(Sim& sim, Entity building) {
    return sim.jobs().fuel().fraction(building) * sim.jobs().fuel().specOf(building).capacity;
}

// A powered rocketswarm launcher at (40, 40), its 2 x 2 centre (41, 41).
Entity artyLauncher(Sim& sim) {
    testFueled(sim, sim.things().spawn("WoodFiredGenerator", CellPos{32, 35, 0}));
    for (int x = 34; x <= 38; ++x) {
        sim.things().spawn("PowerConduit", CellPos{x, 36, 0});
    }
    for (int y = 36; y <= 40; ++y) {
        sim.things().spawn("PowerConduit", CellPos{39, y, 0});
    }
    return sim.things().spawn("Turret_RocketswarmLauncher", CellPos{40, 40, 0});
}

}  // namespace

TEST_CASE("Artillery: the wiki's numbers") {
    const DefDatabase& defs = *artyDefs();
    const Def* mortar = defs.find("Things", "Turret_Mortar");
    const Def* swarm = defs.find("Things", "Turret_RocketswarmLauncher");
    const Def* shell = defs.find("Things", "Shell_HighExplosive");
    ARTY_REQUIRE(mortar != nullptr && swarm != nullptr && shell != nullptr);
    CHECK(mortar->integer("costs.Steel") == 225);  // Classic mortars: 75 metal + 150 steel
    CHECK(mortar->integer("costs.ComponentIndustrial") == 6);
    CHECK(mortar->integer("stats.maxHitPoints") == 180);
    CHECK(mortar->number("artillery.missRadius") == doctest::Approx(13.0));
    CHECK(mortar->text("fuel.item") == "Shell_HighExplosive");
    CHECK(mortar->text("researchPrerequisite") == "Mortars");
    CHECK(swarm->integer("costs.Chemfuel") == 180);
    CHECK(swarm->integer("costs.Plasteel") == 20);
    CHECK(swarm->number("fuel.capacity") == doctest::Approx(180.0));
    CHECK(swarm->number("power.consumption") == doctest::Approx(150.0));
    CHECK(swarm->text("researchPrerequisite") == "RocketswarmLauncher");
    CHECK(shell->integer("stackLimit") == 25);
    const Def* recipe = defs.find("Recipes", "MakeShell_HighExplosive");
    ARTY_REQUIRE(recipe != nullptr);
    CHECK(recipe->text("researchPrerequisite") == "Mortars");
    const Def* he = defs.find("Projectiles", "Bullet_Shell_HighExplosive");
    ARTY_REQUIRE(he != nullptr);
    CHECK(he->number("explosion.damage") == doctest::Approx(50.0));
    CHECK(he->number("explosion.radius") == doctest::Approx(2.9));
    auto sim = artySim();
    GunStats stats;
    CHECK(sim->combat().gunStats(sim->defs().idOf("Things", "Gun_Mortar"), stats));
    CHECK(stats.range == doctest::Approx(500.0f));
    CHECK(stats.minRange == doctest::Approx(29.9f));
    CHECK(stats.warmup == 120);
    CHECK(stats.cooldown == 840);
    CHECK(sim->combat().gunStats(sim->defs().idOf("Things", "Gun_RocketswarmLauncher"), stats));
    CHECK(stats.range == doctest::Approx(29.9f));
    CHECK(stats.burst == 12);
}

TEST_CASE("Artillery: the mortar's miss radius follows the crew's Shooting (RimWorld's Mortar Miss Radius Multiplier)") {
    auto sim = artySim();
    const Entity mortar = sim->things().spawn("Turret_Mortar", CellPos{20, 20, 0});
    const Entity zero = artyColonist(*sim, CellPos{10, 10, 0}, 0);
    const Entity eight = artyColonist(*sim, CellPos{11, 10, 0}, 8);
    const Entity twenty = artyColonist(*sim, CellPos{12, 10, 0}, 20);
    CHECK(sim->artillery().missFactor(zero) == doctest::Approx(1.2f));
    CHECK(sim->artillery().missFactor(eight) == doctest::Approx(1.0f));
    CHECK(sim->artillery().missFactor(twenty) == doctest::Approx(0.7f));
    CHECK(sim->artillery().missRadius(mortar, twenty) == doctest::Approx(9.1f));
    CHECK(sim->artillery().missRadius(mortar, kNoEntity) == doctest::Approx(13.0f));
}

TEST_CASE("Artillery: a colonist ordered to a loaded mortar walks there and fires at the picked point; the shell lands in the miss radius") {
    auto sim = artySim();
    const Entity mortar = sim->things().spawn("Turret_Mortar", CellPos{20, 20, 0});  // centre (21, 21)
    // A wall between the mortar and the point: shells fly over it.
    for (int y = 10; y <= 40; ++y) {
        sim->things().spawn("Wall", CellPos{40, y, 0});
    }
    CHECK(sim->jobs().fuel().add(mortar, 1) == 1);
    const Entity gunner = artyColonist(*sim, CellPos{12, 12, 0});
    const float ax = 101.0f;
    const float ay = 21.0f;
    sim->step();
    // Nobody mans it: the aim alone does nothing.
    CHECK_FALSE(artyFailed(*sim, artyCmd(*sim, "Aim", mortar, kNoEntity, kNoEntity, ax, ay)));
    sim->stepMany(300);
    CHECK(sim->artillery().shotsFired() == 0);
    CHECK_FALSE(artyFailed(*sim, artyCmd(*sim, "Man", mortar, gunner)));
    CHECK(sim->things().registry().get<Worker>(gunner).job.type == JobType::ManArtillery);
    int firedAt = -1;
    int landedAt = -1;
    float lx = 0.0f;
    float ly = 0.0f;
    for (int i = 0; i < 600 && landedAt < 0; ++i) {
        sim->step();
        for (const Event& e : sim->events()) {
            if (e.type == EventType::ShotFired && e.thing == mortar) {
                firedAt = i;
            }
            if (e.type == EventType::Explosion && firedAt >= 0) {
                landedAt = i;
                lx = e.x;
                ly = e.y;
            }
        }
    }
    ARTY_REQUIRE(firedAt >= 0);
    CHECK(sim->artillery().manned(mortar));
    CHECK(landedAt > firedAt + 10);  // 80 m at 41 m/s (times the tuning's projectile speed, 3): 4.1 m a step
    CHECK(std::hypot(lx - ax, ly - ay) <= 13.0f + 0.01f);
    CHECK(artyAmmo(*sim, mortar) == doctest::Approx(0.0f));
    // Empty: it holds its fire.
    sim->stepMany(1200);
    CHECK(sim->artillery().shotsFired() == 1);
    // Ordered off: the job ends and nobody mans it.
    artyCmd(*sim, "StopManning", mortar);
    sim->step();
    CHECK(sim->things().registry().get<Worker>(gunner).job.type != JobType::ManArtillery);
    CHECK_FALSE(sim->artillery().manned(mortar));
}

TEST_CASE("Artillery: shells scatter evenly within the radius and none land outside it") {
    auto sim = artySim(7);
    const Entity mortar = sim->things().spawn("Turret_Mortar", CellPos{20, 20, 0});
    const Entity gunner = artyColonist(*sim, CellPos{12, 12, 0}, 20);  // 9.1 m at full Manipulation
    const float radius = sim->artillery().missRadius(mortar, gunner);
    CHECK(radius < 10.0f);
    artyCmd(*sim, "Man", mortar, gunner);
    artyCmd(*sim, "Aim", mortar, kNoEntity, kNoEntity, 101.0f, 81.0f);
    std::vector<float> misses;  // as a share of the radius when it was fired (the crew's Manipulation may change)
    float firedRadius = radius;
    for (int i = 0; i < 40000 && misses.size() < 30; ++i) {
        if (artyAmmo(*sim, mortar) < 1.0f) {
            sim->jobs().fuel().add(mortar, 1);
        }
        const float now = sim->artillery().missRadius(mortar, gunner);
        sim->step();
        for (const Event& e : sim->events()) {
            if (e.type == EventType::ShotFired && e.thing == mortar) {
                firedRadius = now;
            }
            if (e.type == EventType::Explosion) {
                misses.push_back(std::hypot(e.x - 101.0f, e.y - 81.0f) / firedRadius);
            }
        }
    }
    ARTY_REQUIRE(misses.size() == 30);
    int inner = 0;
    for (const float m : misses) {
        CHECK(m <= 1.001f);
        inner += m < 0.7071f ? 1 : 0;  // half the disc's area
    }
    CHECK(inner >= 6);
    CHECK(inner <= 24);
}

TEST_CASE("Artillery: a manned mortar picks the nearest enemy in range by itself, not one inside its minimum range; hold fire stops it") {
    auto sim = artySim();
    const Entity mortar = sim->things().spawn("Turret_Mortar", CellPos{20, 20, 0});
    sim->jobs().fuel().add(mortar, 1);
    const Entity gunner = artyColonist(*sim, CellPos{12, 12, 0});
    artyCmd(*sim, "Man", mortar, gunner);
    const Entity close = artyHostile(*sim, 41.0f, 21.0f);  // 20 m: inside 29.9
    for (int i = 0; i < 400; ++i) {
        artyHold(*sim, close, 41.0f, 21.0f);
        sim->step();
    }
    CHECK(sim->artillery().shotsFired() == 0);
    artyCmd(*sim, "HoldFire", mortar);
    const Entity far = artyHostile(*sim, 81.0f, 61.0f);
    for (int i = 0; i < 400; ++i) {
        artyHold(*sim, close, 41.0f, 21.0f);
        artyHold(*sim, far, 81.0f, 61.0f);
        sim->step();
    }
    CHECK(sim->artillery().shotsFired() == 0);
    artyCmd(*sim, "FireAtWill", mortar);
    for (int i = 0; i < 400 && sim->artillery().shotsFired() == 0; ++i) {
        artyHold(*sim, close, 41.0f, 21.0f);
        artyHold(*sim, far, 81.0f, 61.0f);
        sim->step();
    }
    CHECK(sim->artillery().shotsFired() == 1);
    CHECK(sim->things().registry().get<Artillery>(mortar).phase == TurretPhase::Cooling);
}

TEST_CASE("Artillery: aims out of range, too close or into a mountain are refused") {
    auto sim = artySim();
    const Entity mortar = sim->things().spawn("Turret_Mortar", CellPos{20, 20, 0});
    sim->step();
    CHECK(artyFailed(*sim, artyCmd(*sim, "Aim", mortar, kNoEntity, kNoEntity, 30.0f, 21.0f)));
    sim->step();
    CHECK(artyFailed(*sim, artyCmd(*sim, "Aim", mortar, kNoEntity, kNoEntity, 500.0f, 21.0f)));  // off the 160 m map
    const_cast<Map&>(sim->map()).setThickRoof(CellPos{100, 100, 0}, true);
    CHECK(artyFailed(*sim, artyCmd(*sim, "Aim", mortar, kNoEntity, kNoEntity, 100.5f, 100.5f)));
    CHECK_FALSE(artyFailed(*sim, artyCmd(*sim, "Aim", mortar, kNoEntity, kNoEntity, 90.5f, 100.5f)));
    sim->step();
    CHECK(sim->things().registry().get<Artillery>(mortar).hasAim);
    CHECK(artyFailed(*sim, artyCmd(*sim, "Spin", mortar)));
    const Entity wall = sim->things().spawn("Wall", CellPos{5, 5, 0});
    CHECK(artyFailed(*sim, artyCmd(*sim, "Aim", wall, kNoEntity, kNoEntity, 90.5f, 100.5f)));
}

TEST_CASE("Artillery: a mountain's roof stops a shell; a mortar under a roof can't fire") {
    auto sim = artySim();
    const Entity mortar = sim->things().spawn("Turret_Mortar", CellPos{20, 20, 0});
    // A shell already on its way into a mountain (a roof held up by the wall beside it).
    sim->things().spawn("Wall", CellPos{103, 100, 0});
    Map& map = const_cast<Map&>(sim->map());
    map.setThickRoof(CellPos{100, 100, 0}, true);
    entt::registry& reg = sim->things().registry();
    const Entity shell = reg.create();
    ArtilleryShell s;
    s.def = sim->defs().idOf("Projectiles", "Bullet_Shell_HighExplosive");
    s.gun = sim->defs().idOf("Things", "Gun_Mortar");
    s.launcher = mortar;
    s.fromX = 21.0f;
    s.fromY = 21.0f;
    s.toX = 100.5f;
    s.toY = 100.5f;
    s.steps = 3;
    reg.emplace<ArtilleryShell>(shell, s);
    int rock = 0;
    int blasts = 0;
    for (int i = 0; i < 5; ++i) {
        sim->step();
        for (const Event& e : sim->events()) {
            rock += e.type == EventType::ProjectileHit && e.hit == ProjectileHitKind::Rock ? 1 : 0;
            blasts += e.type == EventType::Explosion ? 1 : 0;
        }
    }
    CHECK(rock == 1);
    CHECK(blasts == 0);
    CHECK_FALSE(reg.valid(shell));

    // The mortar under a roof (held up by a wall close by): manned and loaded, it holds.
    sim->things().spawn("Wall", CellPos{24, 20, 0});
    for (int y = 20; y <= 21; ++y) {
        for (int x = 20; x <= 21; ++x) {
            map.setThickRoof(CellPos{x, y, 0}, true);
        }
    }
    sim->jobs().fuel().add(mortar, 1);
    const Entity gunner = artyColonist(*sim, CellPos{12, 12, 0});
    artyCmd(*sim, "Man", mortar, gunner);
    artyCmd(*sim, "Aim", mortar, kNoEntity, kNoEntity, 91.0f, 21.0f);
    sim->stepMany(600);
    CHECK(sim->artillery().manned(mortar));
    CHECK(sim->artillery().shotsFired() == 0);
}

TEST_CASE("Artillery: the rocketswarm launcher fires one volley of 12 at the picked point with a full 180 chemfuel, then waits for a new aim") {
    auto sim = artySim();
    const Entity launcher = artyLauncher(*sim);
    const Entity gunner = artyColonist(*sim, CellPos{30, 44, 0});
    sim->step();
    CHECK(sim->power().running(launcher));
    artyCmd(*sim, "Man", launcher, gunner);
    // An enemy close by: it never picks targets itself.
    const Entity enemy = artyHostile(*sim, 61.0f, 41.0f);
    for (int i = 0; i < 300; ++i) {
        artyHold(*sim, enemy, 61.0f, 41.0f);
        sim->step();
    }
    CHECK(sim->artillery().manned(launcher));
    CHECK(sim->artillery().shotsFired() == 0);
    // Aimed but only half full: it waits for the rest.
    sim->jobs().fuel().add(launcher, 90);
    artyCmd(*sim, "Aim", launcher, kNoEntity, kNoEntity, 61.0f, 51.0f);
    for (int i = 0; i < 200; ++i) {
        artyHold(*sim, enemy, 61.0f, 41.0f);
        sim->step();
    }
    CHECK(sim->artillery().shotsFired() == 0);
    sim->jobs().fuel().add(launcher, 90);
    std::set<std::pair<int, int>> cells;
    int bursts = 0;
    int firstShot = -1;
    int lastShot = -1;
    for (int i = 0; i < 400; ++i) {
        artyHold(*sim, enemy, 61.0f, 41.0f);
        sim->step();
        for (const Event& e : sim->events()) {
            if (e.type == EventType::ShotFired && e.thing == launcher) {
                firstShot = firstShot < 0 ? i : firstShot;
                lastShot = i;
            }
            if (e.type == EventType::Explosion) {
                ++bursts;
                cells.insert({e.cell.x, e.cell.y});
                CHECK(std::hypot(e.x - 61.0f, e.y - 51.0f) <= 5.75f + 1.0f);  // a rocket may burst a little short
            }
        }
    }
    CHECK(sim->artillery().shotsFired() == 12);
    CHECK(lastShot - firstShot == 55);  // 11 gaps of 5 steps
    CHECK(bursts == 12);
    CHECK(cells.size() >= 10);  // each rocket goes for a cell of its own
    CHECK(artyAmmo(*sim, launcher) == doctest::Approx(0.0f));
    CHECK_FALSE(sim->things().registry().get<Artillery>(launcher).hasAim);
}

TEST_CASE("Artillery: the player's colonist mans a gun it stands beside and stops manning when it walks off") {
    auto sim = artySim();
    const Entity mortar = sim->things().spawn("Turret_Mortar", CellPos{20, 20, 0});
    const Entity hero = artyColonist(*sim, CellPos{40, 40, 0});
    sim->submit(ControlPawnCommand{hero});
    sim->step();
    CHECK(artyFailed(*sim, artyCmd(*sim, "Man", mortar, hero)));  // too far
    sim->things().registry().get<Position>(hero).x = 22.5f;
    sim->things().registry().get<Position>(hero).y = 19.5f;
    sim->step();
    CHECK_FALSE(artyFailed(*sim, artyCmd(*sim, "Man", mortar, hero)));
    sim->step();
    CHECK(sim->artillery().manned(mortar));
    CHECK(sim->things().registry().get<Artillery>(mortar).crew == hero);
    sim->things().registry().get<Position>(hero).x = 30.5f;
    sim->step();
    CHECK_FALSE(sim->artillery().manned(mortar));
    CHECK(sim->things().registry().get<Artillery>(mortar).crew == kNoEntity);
}

TEST_CASE("Artillery: only the colony's colonists who can fight man a gun; a drafted one is let go to walk there") {
    auto sim = artySim();
    const Entity mortar = sim->things().spawn("Turret_Mortar", CellPos{20, 20, 0});
    const Entity pacifist = artyColonist(*sim, CellPos{12, 12, 0});
    sim->things().registry().get<Colonist>(pacifist).disabledWorkTags = {"Violent"};
    const Entity raider = artyHostile(*sim, 14.0f, 12.0f);
    const Entity soldier = artyColonist(*sim, CellPos{12, 16, 0});
    sim->step();
    CHECK(artyFailed(*sim, artyCmd(*sim, "Man", mortar, pacifist)));
    CHECK(artyFailed(*sim, artyCmd(*sim, "Man", mortar, raider)));
    sim->submit(DraftCommand{soldier, true});
    sim->step();
    CHECK(sim->things().registry().all_of<Fighter>(soldier));
    CHECK_FALSE(artyFailed(*sim, artyCmd(*sim, "Man", mortar, soldier)));
    sim->stepMany(200);
    CHECK(sim->artillery().manned(mortar));
}

TEST_CASE("Artillery: a save in the middle of a shell's flight loads the same and goes on the same") {
    auto sim = artySim();
    const Entity mortar = sim->things().spawn("Turret_Mortar", CellPos{20, 20, 0});
    sim->jobs().fuel().add(mortar, 1);
    const Entity gunner = artyColonist(*sim, CellPos{12, 12, 0});
    artyCmd(*sim, "Man", mortar, gunner);
    artyCmd(*sim, "Aim", mortar, kNoEntity, kNoEntity, 141.0f, 121.0f);
    bool flying = false;
    for (int i = 0; i < 800 && !flying; ++i) {
        sim->step();
        flying = !sim->things().registry().view<ArtilleryShell>().empty();
    }
    ARTY_REQUIRE(flying);
    sim->stepMany(10);
    // In the snapshot while it flies: on its arc, above the ground.
    Snapshot snap;
    sim->writeSnapshot(snap);
    bool seen = false;
    for (const ProjectileView& p : snap.projectiles) {
        seen = seen || (p.def == sim->defs().idOf("Projectiles", "Bullet_Shell_HighExplosive") && p.z > sim->map().heightAt(p.x, p.y) + 2.0f);
    }
    CHECK(seen);
    const std::string json = saveToJson(*sim);
    CHECK(json.find("\"artillery\"") != std::string::npos);
    CHECK(json.find("\"shell\"") != std::string::npos);
    SaveLoadResult loaded = loadSim(saveSim(*sim), artyDefs());
    ARTY_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->stateHash() == sim->stateHash());
    int blasts = 0;
    for (int i = 0; i < 200; ++i) {
        sim->step();
        loaded.sim->step();
        blasts += artyCount(*loaded.sim, EventType::Explosion);
        ARTY_REQUIRE(loaded.sim->stateHash() == sim->stateHash());
    }
    CHECK(blasts == 1);
}
