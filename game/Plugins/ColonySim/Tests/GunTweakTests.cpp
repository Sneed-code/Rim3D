#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Explosives.h"
#include "ColonySim/Health.h"
#include "ColonySim/Random.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace colony;

#define GT_REQUIRE(...)        \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

// sneed's gun changes of 2026-11-23: tighter minigun and LMG, the chain shotgun's buckshot, slow straight rockets, a slower
// arcing grenade launcher and bouncing grenades (docs/design/combat.md "Gun changes").
namespace {

std::shared_ptr<const DefDatabase> gtDefs() {
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

std::unique_ptr<Sim> gtSim() {
    SimSettings settings;
    settings.seed = 5;
    settings.mapSize = 96;
    settings.flatMap = true;
    return std::make_unique<Sim>(settings, gtDefs());
}

Entity gtPawn(Sim& sim, int x, int y, const char* gun) {
    const Entity pawn = sim.things().spawn("Human", CellPos{x, y, 0});
    if (gun != nullptr) {
        sim.combat().equip(pawn, sim.things().make(sim.defs().idOf("Things", gun)));
    }
    return pawn;
}

void gtHeal(Sim& sim, Entity pawn) {
    sim.things().registry().replace<Health>(pawn, Health{});
    sim.health().refresh(pawn);
}

std::vector<Entity> gtProjectiles(Sim& sim) {
    std::vector<Entity> out;
    for (const Entity e : sim.things().registry().view<Projectile>()) {
        out.push_back(e);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Steps until no projectile is left, collecting ProjectileHit events.
std::vector<Event> gtFly(Sim& sim, int limit = 400) {
    std::vector<Event> hits;
    for (int i = 0; i < limit; ++i) {
        sim.step();
        for (const Event& e : sim.events()) {
            if (e.type == EventType::ProjectileHit) {
                hits.push_back(e);
            }
        }
        if (sim.things().registry().view<Projectile>().empty()) {
            break;
        }
    }
    return hits;
}

int gtCount(const std::vector<Event>& events, EventType type) {
    return static_cast<int>(std::count_if(events.begin(), events.end(), [&](const Event& e) { return e.type == type; }));
}

}  // namespace

TEST_CASE("Gun changes: the minigun and LMG spray a fifth as wide, the chain shotgun has none") {
    const DefDatabase& defs = *gtDefs();
    CHECK(defs.find("Things", "Gun_Minigun")->number("spreadFactor") == doctest::Approx(0.2));
    CHECK(defs.find("Things", "Gun_LMG")->number("spreadFactor") == doctest::Approx(0.2));
    CHECK(defs.find("Things", "Gun_ChainShotgun")->number("spreadFactor") == doctest::Approx(0.0));
    CHECK(defs.find("Things", "Gun_ChainShotgun")->number("recoilPerShot") == doctest::Approx(0.0));
    CHECK(defs.find("Things", "Gun_ChainShotgun")->count("recoilKicks") == 3);  // the visible kick stays

    for (const char* gun : {"Gun_Minigun", "Gun_LMG"}) {
        auto sim = gtSim();
        const Entity shooter = gtPawn(*sim, 30, 40, gun);
        const DefId id = sim->defs().idOf("Things", gun);
        const float distance = 20.0f;
        const float tx = 30.5f + distance;
        const float ty = 40.5f;
        const float tz = sim->map().heightAt(tx, ty) + 1.0f;
        const int skill = sim->combat().shootingSkill(shooter);
        const float cone = sim->combat().spread(id, skill, distance, false, 1.0f);
        CHECK(cone > 0.0f);
        sim->combat().shoot(shooter, kNoEntity, tx, ty, tz, false);
        std::set<Entity> seen;
        float widest = 0.0f;
        int shots = 0;
        for (int i = 0; i < 400 && shots < 12; ++i) {
            sim->step();
            for (const Entity e : gtProjectiles(*sim)) {
                if (!seen.insert(e).second) {
                    continue;
                }
                const Projectile& p = sim->things().registry().get<Projectile>(e);
                if (p.age > 1) {
                    continue;
                }
                widest = std::max(widest, std::fabs(p.vy / p.vx));
                ++shots;
            }
        }
        GT_REQUIRE(shots >= 12);
        const float recoil = sim->things().registry().get<Shooter>(shooter).recoil;
        // The whole spray (cone and the recoil the shots built) is a fifth of what it was; the cone alone would be wider.
        CHECK(widest <= (cone + recoil) * 0.2f * 1.02f + 1e-4f);
        CHECK(widest > 0.0f);
    }
}

TEST_CASE("Gun changes: a chain shotgun shot is nine pellets with a ninth of the damage each and one ShotFired") {
    auto sim = gtSim();
    const Entity shooter = gtPawn(*sim, 30, 40, "Gun_ChainShotgun");
    Rng rng(3);
    sim->step();
    const DefDatabase& defs = *gtDefs();
    const Def* bullet = defs.find("Projectiles", "Bullet_ChainShotgun");
    GT_REQUIRE(bullet != nullptr);
    CHECK(bullet->integer("projectile.pellets") == 9);
    const Entity first = sim->combat().fire(shooter, 40.5f, 40.5f, 1.0f, 0.0f, rng);
    GT_REQUIRE(first != kNoEntity);
    CHECK(gtCount(sim->events(), EventType::ShotFired) == 1);  // nine pellets, one ShotFired
    const std::vector<Entity> pellets = gtProjectiles(*sim);
    CHECK(pellets.size() == 9);
    std::set<std::pair<int, int>> directions;
    for (const Entity e : pellets) {
        const Projectile& p = sim->things().registry().get<Projectile>(e);
        CHECK(p.damageScale == doctest::Approx(1.0f / 9.0f));
        directions.insert({static_cast<int>(p.vy * 1e4f), static_cast<int>(p.vz * 1e4f)});
    }
    CHECK(directions.size() > 5);  // they fly apart
}

TEST_CASE("Gun changes: buckshot is strong up close and weak far away") {
    int near = 0;
    int far = 0;
    for (const float distance : {3.0f, 12.0f}) {
        int pelletsOnBody = 0;
        int events = 0;
        for (int trial = 0; trial < 6; ++trial) {
            auto sim = gtSim();
            const Entity shooter = gtPawn(*sim, 30, 40, "Gun_ChainShotgun");
            const Entity target = gtPawn(*sim, 30 + static_cast<int>(distance), 40, nullptr);
            gtHeal(*sim, target);
            Rng rng(static_cast<uint32_t>(trial + 1));
            float x = 0.0f, y = 0.0f, z = 0.0f;
            sim->combat().aimPoint(target, x, y, z);
            sim->combat().fire(shooter, x, y, z, 0.0f, rng);
            const std::vector<Event> hits = gtFly(*sim);
            events += static_cast<int>(hits.size());
            for (const Event& h : hits) {
                if (h.hit == ProjectileHitKind::Pawn || h.hit == ProjectileHitKind::Armor) {
                    ++pelletsOnBody;
                }
            }
        }
        CHECK(events == 6 * 9);  // every pellet ends with its own hit event
        (distance < 5.0f ? near : far) = pelletsOnBody;
    }
    CHECK(near >= 6 * 5);  // at 3 m most of the nine are on the body
    CHECK(far <= 6 * 3);   // at 12 m only a few
    CHECK(near > far * 2);
}

TEST_CASE("Gun changes: rockets leave at 10 m/s, speed up 7 m/s every second up to 60, fly straight and reach 30 metres") {
    for (const char* gun : {"Gun_TripleRocketLauncher", "Gun_DoomsdayLauncher"}) {
        auto sim = gtSim();
        const Entity shooter = gtPawn(*sim, 20, 40, gun);
        Rng rng(1);
        const float tx = 50.5f;
        const float ty = 40.5f;
        const float tz = sim->map().heightAt(tx, ty);
        const Entity rocket = sim->combat().fire(shooter, tx, ty, tz, 0.0f, rng);
        GT_REQUIRE(rocket != kNoEntity);
        const Projectile start = sim->things().registry().get<Projectile>(rocket);
        const auto speedOf = [](const Projectile& p) { return std::sqrt(p.vx * p.vx + p.vy * p.vy + p.vz * p.vz) * 30.0f; };
        CHECK(speedOf(start) == doctest::Approx(10.0f).epsilon(0.005));  // metres per real second at 1x
        // No drop: the aim is the straight line to the point.
        CHECK(start.vz / start.vx == doctest::Approx((tz - start.z) / (tx - start.x)).epsilon(0.01));
        int steps = 0;
        float slope = start.vz / start.vx;
        bool straight = true;
        bool burst = false;
        float speedAt30 = 0.0f;
        for (int i = 0; i < 600 && !burst; ++i) {
            sim->step();
            ++steps;
            if (sim->things().registry().valid(rocket) && sim->things().registry().all_of<Projectile>(rocket)) {
                const Projectile& p = sim->things().registry().get<Projectile>(rocket);
                straight = straight && p.vz / p.vx == doctest::Approx(slope).epsilon(0.001);
                if (steps == 30) {
                    speedAt30 = speedOf(p);
                }
            }
            for (const Event& e : sim->events()) {
                if (e.type == EventType::ProjectileHit) {
                    burst = true;
                    CHECK(e.hit == ProjectileHitKind::Ground);
                    CHECK(std::fabs(e.x - tx) < 1.2f);
                    CHECK(std::fabs(e.y - ty) < 0.1f);
                }
            }
        }
        CHECK(straight);
        CHECK(burst);
        CHECK(speedAt30 == doctest::Approx(17.0f).epsilon(0.02));  // 10 + 7 after a second
        // 30 m at 10 m/s plus 7 m/s/s: about 1.9 s.
        CHECK(steps >= 50);
        CHECK(steps <= 64);
    }
}

TEST_CASE("Gun changes: a rocket's motor stops speeding it up at 60 m/s") {
    auto sim = gtSim();
    const Entity shooter = gtPawn(*sim, 5, 5, "Gun_TripleRocketLauncher");
    Rng rng(1);
    // Up into the sky, so nothing stops it.
    const Entity rocket = sim->combat().fire(shooter, 5.6f, 5.5f, 400.0f, 0.0f, rng);
    GT_REQUIRE(rocket != kNoEntity);
    for (int i = 0; i < 300; ++i) {
        sim->step();
    }
    GT_REQUIRE(sim->things().registry().valid(rocket));
    const Projectile& p = sim->things().registry().get<Projectile>(rocket);
    CHECK(std::sqrt(p.vx * p.vx + p.vy * p.vy + p.vz * p.vz) * 30.0f == doctest::Approx(60.0f).epsilon(0.01));
}

TEST_CASE("Gun changes (second batch): charge lance, machine pistol store, chain shotgun blasts, cooldowns and damage") {
    auto sim = gtSim();
    const DefId lance = sim->defs().idOf("Things", "Gun_ChargeLance");
    CHECK(sim->combat().chargeFactorFor(lance, 0.0f) == doctest::Approx(1.0f));
    CHECK(sim->combat().chargeFactorFor(lance, 1.0f) == doctest::Approx(1.5f));
    CHECK(sim->combat().chargeFactorFor(lance, 2.0f) == doctest::Approx(2.0f));  // "at max charge up it does x2 damage"
    CHECK(sim->combat().chargeFactorFor(lance, 3.5f) == doctest::Approx(2.0f));
    CHECK(sim->combat().chargeFactorFor(sim->defs().idOf("Things", "Gun_Revolver"), 2.0f) == doctest::Approx(1.0f));
    CHECK(sim->defs().find("Things", "Gun_ChargeLance")->number("charge.forceSeconds") == doctest::Approx(4.0));

    // A held trigger's charge goes on the next shot only.
    const Entity hero = gtPawn(*sim, 30, 40, "Gun_ChargeLance");
    ShootCommand c{hero, kNoEntity, 45.5f, 40.5f, 1.0f, true};
    c.charge = 2.0f;
    sim->submit(c);
    float factor = 0.0f;
    for (int i = 0; i < 5 && factor == 0.0f; ++i) {
        sim->step();
        for (const Entity e : gtProjectiles(*sim)) {
            factor = sim->things().registry().get<Projectile>(e).damageFactor;
        }
    }
    const Entity plain = gtPawn(*sim, 30, 50, "Gun_ChargeLance");
    Rng rng(2);
    const Entity unCharged = sim->combat().fire(plain, 45.5f, 50.5f, 1.0f, 0.0f, rng);
    GT_REQUIRE(unCharged != kNoEntity);
    CHECK(factor == doctest::Approx(2.0f * sim->things().registry().get<Projectile>(unCharged).damageFactor));
    CHECK(sim->things().registry().get<Shooter>(hero).chargeFactor == doctest::Approx(1.0f));

    // The machine pistol holds half: 5 of the 9 a 3-shot burst gives.
    const Entity mp = gtPawn(*sim, 30, 60, "Gun_MachinePistol");
    int stored = 0, size = 0, left = 0, from = 0;
    CHECK(sim->combat().shotStore(mp, stored, size, left, from));
    CHECK(size == 5);
    // The chain shotgun fires three blasts, not a store of nine.
    const Entity cs = gtPawn(*sim, 30, 70, "Gun_ChainShotgun");
    CHECK(sim->combat().shotStore(cs, stored, size, left, from));
    CHECK(size == 3);

    const DefDatabase& defs = *gtDefs();
    // Cooldowns: minigun x5 (45 -> 225 steps), chain shotgun x1.5 (40.5 -> 60.75).
    CHECK(defs.find("Things", "Gun_Minigun")->number("stats.rangedWeaponCooldown") == doctest::Approx(225.0));
    CHECK(defs.find("Things", "Gun_ChainShotgun")->number("stats.rangedWeaponCooldown") == doctest::Approx(60.75));
    // Revolver +25%, spray-and-pray guns -25% (after the 0.5 for all bullets).
    CHECK(defs.find("Projectiles", "Bullet_Revolver")->number("projectile.damageAmountBase") == doctest::Approx(18.0 * 0.5 * 1.25));
    CHECK(defs.find("Projectiles", "Bullet_Minigun")->number("projectile.damageAmountBase") == doctest::Approx(10.0 * 0.5 * 0.75));
    CHECK(defs.find("Projectiles", "Bullet_LMG")->number("projectile.damageAmountBase") == doctest::Approx(12.0 * 0.5 * 0.75));
    CHECK(defs.find("Projectiles", "Bullet_HeavySMG")->number("projectile.damageAmountBase") == doctest::Approx(12.0 * 0.5 * 0.75));
    CHECK(defs.find("Things", "Gun_MachinePistol")->number("spreadFactor") == doctest::Approx(0.4));
}

TEST_CASE("Gun changes: the grenade launcher lobs slower, in an arc, and still reaches its range") {
    auto sim = gtSim();
    const Entity shooter = gtPawn(*sim, 20, 40, "Gun_GrenadeLauncher");
    Rng rng(1);
    const float range = 23.9f;
    const float tx = 20.5f + range;
    const float ty = 40.5f;
    const float tz = sim->map().heightAt(tx, ty);
    const Entity shell = sim->combat().fire(shooter, tx, ty, tz, 0.0f, rng);
    GT_REQUIRE(shell != kNoEntity);
    const Projectile start = sim->things().registry().get<Projectile>(shell);
    const float speed = std::sqrt(start.vx * start.vx + start.vy * start.vy + start.vz * start.vz);
    CHECK(speed * 30.0f == doctest::Approx(36.0f).epsilon(0.01));  // sneed 2026-11-23 second batch: faster again
    CHECK(start.vz > 0.0f);  // thrown up, the lower arc
    float arc = 0.0f;  // the most the shell is above the straight line from the muzzle to the point
    float firstBounceX = -1.0f;
    for (int i = 0; i < 300 && firstBounceX < 0.0f; ++i) {
        sim->step();
        if (sim->things().registry().valid(shell) && sim->things().registry().all_of<Projectile>(shell)) {
            const Projectile& flying = sim->things().registry().get<Projectile>(shell);
            arc = std::max(arc, flying.z - (start.z + (tz - start.z) * (flying.x - start.x) / (tx - start.x)));
        }
        for (const Event& e : sim->events()) {
            if (e.type == EventType::GrenadeLanded) {
                firstBounceX = e.x;
            }
        }
    }
    GT_REQUIRE(firstBounceX > 0.0f);
    CHECK(arc > 0.8f);                               // a visible arc
    CHECK(std::fabs(firstBounceX - tx) < 1.0f);      // first touchdown at the aimed point
    // Far beyond reach it still goes as far as it can (the 45 degree shot) instead of falling short.
    const Entity far = sim->combat().fire(shooter, 20.5f + 200.0f, ty, 0.0f, 0.0f, rng);
    GT_REQUIRE(far != kNoEntity);
    const Projectile longShot = sim->things().registry().get<Projectile>(far);
    CHECK(longShot.vz / std::sqrt(longShot.vx * longShot.vx + longShot.vy * longShot.vy) == doctest::Approx(1.0f).epsilon(0.02));
}

TEST_CASE("Gun changes: a shell bounces off the ground and bursts 3 seconds after the first bounce") {
    auto sim = gtSim();
    const Entity shooter = gtPawn(*sim, 20, 40, "Gun_GrenadeLauncher");
    Rng rng(1);
    const float tx = 35.5f;
    sim->combat().fire(shooter, tx, 40.5f, sim->map().heightAt(tx, 40.5f), 0.0f, rng);
    int bounces = 0;
    int firstBounce = -1;
    int blast = -1;
    float lastBounceX = 0.0f;
    float blastX = 0.0f;
    for (int i = 1; i < 400 && blast < 0; ++i) {
        sim->step();
        for (const Event& e : sim->events()) {
            if (e.type == EventType::GrenadeLanded) {
                ++bounces;
                firstBounce = firstBounce < 0 ? i : firstBounce;
                lastBounceX = e.x;
            } else if (e.type == EventType::Explosion) {
                blast = i;
                blastX = e.x;
            }
        }
    }
    GT_REQUIRE(blast > 0);
    CHECK(bounces >= 1);
    CHECK(blast - firstBounce == 90);  // 3 s at 30 steps a second
    CHECK(blast < 150);                // and sooner than 5 s from the shot
    CHECK(blastX >= lastBounceX - 0.01f);
    CHECK(blastX > tx - 0.5f);         // it rolled on a little, not back
}

TEST_CASE("Gun changes: a shell that never touches anything bursts 5 seconds after it was fired") {
    auto sim = gtSim();
    Entity shell = sim->things().registry().create();
    Projectile p;
    p.def = sim->defs().idOf("Projectiles", "Shell_Frag");
    p.weapon = sim->defs().idOf("Things", "Gun_GrenadeLauncher");
    p.x = 30.5f;
    p.y = 40.5f;
    p.z = 1.5f;
    p.vz = 0.9f;  // straight up: 165 steps to come down, longer than the fuse
    p.level = 0;
    p.startCell = CellPos{30, 40, 0};
    sim->things().registry().emplace<Projectile>(shell, p);
    int blast = -1;
    for (int i = 1; i < 300 && blast < 0; ++i) {
        sim->step();
        for (const Event& e : sim->events()) {
            CHECK(e.type != EventType::GrenadeLanded);
            if (e.type == EventType::Explosion) {
                blast = i;
            }
        }
    }
    CHECK(blast == 150);
}

TEST_CASE("Gun changes: a shell bounces off a wall and bursts at once on a pawn") {
    {
        auto sim = gtSim();
        for (int y = 36; y <= 44; ++y) {
            sim->things().spawn("Wall", CellPos{40, y, 0});
        }
        const Entity shooter = gtPawn(*sim, 30, 40, "Gun_GrenadeLauncher");
        Rng rng(1);
        sim->combat().fire(shooter, 40.5f, 40.5f, 1.0f, 0.0f, rng);
        float bounceX = 99.0f;
        float blastX = 99.0f;
        int blast = -1;
        int firstBounce = -1;
        for (int i = 1; i < 400 && blast < 0; ++i) {
            sim->step();
            for (const Event& e : sim->events()) {
                if (e.type == EventType::GrenadeLanded && firstBounce < 0) {
                    firstBounce = i;
                    bounceX = e.x;
                } else if (e.type == EventType::Explosion) {
                    blast = i;
                    blastX = e.x;
                }
            }
        }
        GT_REQUIRE(blast > 0);
        CHECK(bounceX < 40.0f);  // off the wall's near face
        CHECK(blastX < 40.0f);   // it came back, it did not go through
        CHECK(blast - firstBounce == 90);
    }
    {
        auto sim = gtSim();
        const Entity shooter = gtPawn(*sim, 30, 40, "Gun_GrenadeLauncher");
        const Entity target = gtPawn(*sim, 38, 40, nullptr);
        gtHeal(*sim, target);
        Rng rng(1);
        float x = 0.0f, y = 0.0f, z = 0.0f;
        sim->combat().aimPoint(target, x, y, z);
        sim->combat().fire(shooter, x, y, z, 0.0f, rng);
        int blast = -1;
        int bounces = 0;
        for (int i = 1; i < 400 && blast < 0; ++i) {
            sim->step();
            for (const Event& e : sim->events()) {
                bounces += e.type == EventType::GrenadeLanded ? 1 : 0;
                if (e.type == EventType::Explosion) {
                    blast = i;
                    CHECK(std::fabs(e.x - 38.5f) < 1.0f);
                }
            }
        }
        GT_REQUIRE(blast > 0);
        CHECK(bounces == 0);
        CHECK(blast < 40);
    }
}

TEST_CASE("Gun changes: a thrown frag grenade bounces, bursts on a pawn at once, a molotov still bursts on landing") {
    const DefId item = gtDefs()->idOf("Things", "Grenade_Frag");
    {
        auto sim = gtSim();
        const Entity thrower = gtPawn(*sim, 40, 40, nullptr);
        gtHeal(*sim, thrower);
        GT_REQUIRE(sim->explosives().give(thrower, item, 1) == 1);
        sim->submit(ThrowCommand{thrower, "", kNoEntity, 48.5f, 40.5f, true});
        int firstBounce = -1;
        int blast = -1;
        int thrown = -1;
        for (int i = 1; i < 500 && blast < 0; ++i) {
            sim->step();
            for (const Event& e : sim->events()) {
                if (e.type == EventType::GrenadeThrown) {
                    thrown = i;
                } else if (e.type == EventType::GrenadeLanded && firstBounce < 0) {
                    firstBounce = i;
                } else if (e.type == EventType::Explosion) {
                    blast = i;
                }
            }
        }
        GT_REQUIRE(blast > 0);
        GT_REQUIRE(firstBounce > 0);
        CHECK(blast - firstBounce == 90);
        CHECK(blast - thrown <= 150);
    }
    {
        auto sim = gtSim();
        const Entity thrower = gtPawn(*sim, 40, 40, nullptr);
        gtHeal(*sim, thrower);
        const Entity target = gtPawn(*sim, 44, 40, nullptr);
        gtHeal(*sim, target);
        // A grenade flying flat at his chest (as a hand grenade does for a moment on its arc): it goes off on the man.
        float cx = 0.0f, cy = 0.0f, cz = 0.0f;
        sim->combat().aimPoint(target, cx, cy, cz);
        Grenade g;
        g.def = sim->defs().idOf("Projectiles", "Proj_GrenadeFrag");
        g.item = item;
        g.thrower = thrower;
        g.x = 41.0f;
        g.y = cy;
        g.z = cz + 0.75f;  // it falls 0.75 m on the way, to be at his chest when it gets there
        g.vx = 0.3f;
        g.level = 0;
        g.startCell = CellPos{40, 40, 0};
        sim->things().registry().emplace<Grenade>(sim->things().registry().create(), g);
        int blast = -1;
        const int thrown = 0;
        int bounces = 0;
        float blastX = 0.0f;
        for (int i = 1; i < 500 && blast < 0; ++i) {
            sim->step();
            for (const Event& e : sim->events()) {
                if (e.type == EventType::GrenadeLanded) {
                    ++bounces;
                } else if (e.type == EventType::Explosion) {
                    blast = i;
                    blastX = e.x;
                }
            }
        }
        GT_REQUIRE(blast > 0);
        CHECK(bounces == 0);
        CHECK(blast - thrown < 20);  // on the man, not 3 s after a bounce
        CHECK(std::fabs(blastX - 44.5f) < 1.0f);
    }
    {
        auto sim = gtSim();
        const DefId molotov = sim->defs().idOf("Things", "Grenade_Molotov");
        const Entity thrower = gtPawn(*sim, 40, 40, nullptr);
        GT_REQUIRE(sim->explosives().give(thrower, molotov, 1) == 1);
        CHECK(!sim->combat().bounceRules(sim->defs().idOf("Projectiles", "Proj_GrenadeMolotov")).bounces());
        sim->submit(ThrowCommand{thrower, "Grenade_Molotov", kNoEntity, 47.5f, 40.5f, true});
        int landed = -1;
        int blast = -1;
        for (int i = 1; i < 200 && blast < 0; ++i) {
            sim->step();
            for (const Event& e : sim->events()) {
                if (e.type == EventType::GrenadeLanded) {
                    landed = i;
                } else if (e.type == EventType::Explosion) {
                    blast = i;
                }
            }
        }
        GT_REQUIRE(blast > 0);
        CHECK(blast == landed);
    }
}
