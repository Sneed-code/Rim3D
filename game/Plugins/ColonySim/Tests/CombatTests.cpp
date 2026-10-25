#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Health.h"
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

#define COMBAT_REQUIRE(...)    \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// The game's real data (guns, bullets, apparel, the Human and its hitboxes), plus a wall and
// sandbags until construction (C4) adds them. These tests check docs/design/combat.md.
std::shared_ptr<const DefDatabase> combatTestDefs() {
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
        sources.push_back(DefSource{"Things", "zz_combat_test.json", R"([
            {"defName": "TestWall", "category": "Building", "passability": "Impassable", "fillPercent": 1.0, "stats": {"maxHitPoints": 300}},
            {"defName": "TestSandbags", "category": "Building", "passability": "PassThroughOnly", "fillPercent": 0.55, "stats": {"maxHitPoints": 300}}
        ])"});
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

std::unique_ptr<Sim> combatTestSim(uint64_t seed = 11) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = 96;
    settings.flatMap = true;
    return std::make_unique<Sim>(settings, combatTestDefs());
}

// A pawn at the centre of cell (x, y) facing east, holding `gun` (if any).
Entity combatTestPawn(Sim& sim, int x, int y, const char* gun = nullptr) {
    const Entity pawn = sim.things().spawn("Human", CellPos{x, y, 0});
    if (gun != nullptr) {
        const Entity item = sim.things().make(sim.defs().idOf("Things", gun));
        sim.combat().equip(pawn, item);
    }
    return pawn;
}

void combatTestHeal(Sim& sim, Entity pawn) {
    sim.things().registry().replace<Health>(pawn, Health{});
    sim.health().refresh(pawn);
}

// Steps until no projectile is left (at most `limit` steps) and returns every ProjectileHit.
std::vector<Event> combatTestFly(Sim& sim, int limit = 200) {
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

// Fires one shot from `shooter` at the chest of `target` with the spread an aimed shot of that
// skill would have, and returns what stopped it.
Event combatTestShot(Sim& sim, Entity shooter, Entity target, float spreadSlope, Rng& rng) {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    sim.combat().aimPoint(target, x, y, z);
    sim.combat().fire(shooter, x, y, z, spreadSlope, rng);
    const std::vector<Event> hits = combatTestFly(sim);
    return hits.empty() ? Event{} : hits.front();
}

bool combatTestIsUnder(const Sim& sim, Entity pawn, int part, const std::string& label) {
    const std::vector<BodyPartInfo> parts = sim.health().bodyParts(sim.things().registry().get<Thing>(pawn).def);
    for (int p = part; p >= 0; p = parts[static_cast<size_t>(p)].parent) {
        if (parts[static_cast<size_t>(p)].label == label) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("Combat: guns, bullets, apparel and hitboxes load from the real data") {
    auto sim = combatTestSim();
    const DefDatabase& defs = sim->defs();
    COMBAT_REQUIRE(defs.find("Things", "Gun_BoltActionRifle") != nullptr);
    COMBAT_REQUIRE(defs.find("Projectiles", "Bullet_BoltActionRifle") != nullptr);
    // RimWorld's 70 m/s, as metres per step, times the tuning multiplier (3).
    CHECK(defs.find("Projectiles", "Bullet_BoltActionRifle")->number("projectile.speed") == doctest::Approx(7.0).epsilon(0.001));

    const Entity pawn = combatTestPawn(*sim, 20, 20, "Gun_Revolver");
    std::vector<HitboxView> boxes;
    CHECK(sim->combat().hitboxesOf(pawn, boxes));
    CHECK(boxes.size() == 17);
    const ThingView* view = nullptr;
    Snapshot snap;
    sim->writeSnapshot(snap);
    view = snap.find(pawn);
    COMBAT_REQUIRE(view != nullptr);
    CHECK(view->weapon == defs.idOf("Things", "Gun_Revolver"));
    CHECK(view->stance == Stance::Standing);
}

TEST_CASE("Combat: an aimed shot hits a standing man as often as RimWorld's hit chance says") {
    auto sim = combatTestSim();
    const Entity shooter = combatTestPawn(*sim, 20, 40, "Gun_BoltActionRifle");
    const int skill = sim->combat().shootingSkill(shooter);
    CHECK(skill == 4);  // no Colonist: CombatRules.defaultShootingSkill
    const DefId rifle = sim->defs().idOf("Things", "Gun_BoltActionRifle");
    Rng rng(5);
    for (const int distance : {12, 25}) {
        const Entity target = combatTestPawn(*sim, 20 + distance, 40);
        sim->things().registry().get<Posture>(target).facingX = -1.0f;  // facing the shooter
        sim->things().registry().get<Posture>(target).facingY = 0.0f;
        const float spreadSlope = sim->combat().spread(rifle, skill, static_cast<float>(distance), false);
        int hits = 0;
        const int shots = 500;
        for (int i = 0; i < shots; ++i) {
            const Event hit = combatTestShot(*sim, shooter, target, spreadSlope, rng);
            if (hit.hit == ProjectileHitKind::Pawn || hit.hit == ProjectileHitKind::Armor) {
                ++hits;
                combatTestHeal(*sim, target);
            }
        }
        // RimWorld: shooter accuracy 94% per metre at skill 4, the rifle's accuracy at that range.
        const double gun = distance == 12 ? 0.80 : 0.90;
        const double expected = std::pow(0.94, distance) * gun;
        const double got = static_cast<double>(hits) / shots;
        MESSAGE("distance " << distance << ": hit " << got << ", RimWorld " << expected);
        CHECK(std::fabs(got - expected) < 0.07);
        sim->things().destroy(target);
    }
}

TEST_CASE("Combat: bullets fly with speed and drop and land where they were aimed") {
    auto sim = combatTestSim();
    const Entity shooter = combatTestPawn(*sim, 10, 10, "Gun_BoltActionRifle");
    Rng rng(1);
    const Entity shot = sim->combat().fire(shooter, 60.5f, 10.5f, 0.0f, 0.0f, rng);
    COMBAT_REQUIRE(shot != kNoEntity);
    const Projectile start = sim->things().registry().get<Projectile>(shot);
    CHECK(start.z == doctest::Approx(1.45f));
    CHECK(start.vz / start.vx > -1.45f / 50.0f);  // aimed above the point a little, for the drop
    sim->step();
    const Projectile after = sim->things().registry().get<Projectile>(shot);
    CHECK(after.x - start.x == doctest::Approx(start.vx));
    CHECK(after.vz == doctest::Approx(start.vz - 0.0109f));
    const std::vector<Event> hits = combatTestFly(*sim);
    COMBAT_REQUIRE(hits.size() == 1);
    CHECK(hits[0].hit == ProjectileHitKind::Ground);
    CHECK(hits[0].x == doctest::Approx(60.5f).epsilon(0.01));
    CHECK(std::fabs(hits[0].y - 10.5f) < 0.01f);
    CHECK(hits[0].z == doctest::Approx(0.0f).epsilon(0.02));
}

TEST_CASE("Combat: walls stop bullets and break, sandbags hide a crouching pawn") {
    auto sim = combatTestSim();
    const Entity shooter = combatTestPawn(*sim, 10, 30, "Gun_BoltActionRifle");
    const Entity target = combatTestPawn(*sim, 30, 30);
    Rng rng(2);

    SUBCASE("a wall") {
        const Entity wall = sim->things().spawn("TestWall", CellPos{20, 30, 0});
        COMBAT_REQUIRE(wall != kNoEntity);
        const Event hit = combatTestShot(*sim, shooter, target, 0.0f, rng);
        CHECK(hit.hit == ProjectileHitKind::Building);
        CHECK(hit.target == wall);
        CHECK(hit.x == doctest::Approx(20.0f).epsilon(0.001));
        CHECK(sim->things().registry().get<HitPoints>(wall).current == 300 - 18);
        int shots = 1;
        while (sim->things().exists(wall) && shots < 40) {
            combatTestShot(*sim, shooter, target, 0.0f, rng);
            ++shots;
        }
        CHECK(shots == 17);  // 300 hit points, 18 a shot
        CHECK(!sim->things().exists(wall));
        CHECK(combatTestShot(*sim, shooter, target, 0.0f, rng).hit == ProjectileHitKind::Pawn);
    }
    SUBCASE("sandbags") {
        COMBAT_REQUIRE(sim->things().spawn("TestSandbags", CellPos{29, 30, 0}) != kNoEntity);
        sim->combat().setStance(target, Stance::Crouching);
        CHECK(combatTestShot(*sim, shooter, target, 0.0f, rng).hit == ProjectileHitKind::Building);
        sim->combat().setStance(target, Stance::Standing);
        CHECK(combatTestShot(*sim, shooter, target, 0.0f, rng).hit == ProjectileHitKind::Pawn);
    }
}

TEST_CASE("Combat: hitboxes follow the stance and the hit lands in the part that was hit") {
    auto sim = combatTestSim();
    const Entity shooter = combatTestPawn(*sim, 10, 30, "Gun_BoltActionRifle");
    const Entity target = combatTestPawn(*sim, 25, 30);
    Rng rng(3);
    const auto shootAtHeight = [&](float z) {
        sim->combat().fire(shooter, 25.5f, 30.5f, z, 0.0f, rng);
        const std::vector<Event> hits = combatTestFly(*sim);
        combatTestHeal(*sim, target);
        return hits.empty() ? Event{} : hits.front();
    };
    Event head = shootAtHeight(1.65f);
    CHECK(head.hit == ProjectileHitKind::Pawn);
    CHECK(combatTestIsUnder(*sim, target, head.part, "head"));
    sim->combat().fire(shooter, 25.5f, 30.62f, 0.6f, 0.0f, rng);  // not between the legs
    std::vector<Event> legHits = combatTestFly(*sim);
    combatTestHeal(*sim, target);
    Event leg = legHits.empty() ? Event{} : legHits.front();
    CHECK(leg.hit == ProjectileHitKind::Pawn);
    CHECK((combatTestIsUnder(*sim, target, leg.part, "left leg") || combatTestIsUnder(*sim, target, leg.part, "right leg")));

    sim->combat().setStance(target, Stance::Crouching);
    CHECK(shootAtHeight(1.65f).hit != ProjectileHitKind::Pawn);  // over a crouching head
    Event crouched = shootAtHeight(0.95f);
    CHECK(crouched.hit == ProjectileHitKind::Pawn);
    CHECK(combatTestIsUnder(*sim, target, crouched.part, "head"));

    sim->combat().setStance(target, Stance::Prone);
    CHECK(shootAtHeight(1.0f).hit != ProjectileHitKind::Pawn);  // over someone lying down
    CHECK(shootAtHeight(0.15f).hit == ProjectileHitKind::Pawn);
}

TEST_CASE("Combat: armor stops or softens hits on the parts it covers") {
    auto sim = combatTestSim();
    const Entity shooter = combatTestPawn(*sim, 10, 30, "Gun_Revolver");
    const Entity target = combatTestPawn(*sim, 18, 30);
    const Entity vest = sim->things().make(sim->defs().idOf("Things", "Apparel_FlakVest"));
    COMBAT_REQUIRE(sim->combat().equip(target, vest));
    CHECK(sim->things().registry().get<EquippedBy>(vest).pawn == target);
    Rng rng(4);
    int torso = 0;
    int stopped = 0;
    int bruised = 0;
    for (int i = 0; i < 300; ++i) {
        sim->combat().fire(shooter, 18.5f, 30.5f, 1.15f, 0.0f, rng);  // the middle of the chest
        int injuries = 0;
        for (int s = 0; s < 20; ++s) {
            sim->step();
            for (const Event& e : sim->events()) {
                if (e.type == EventType::ProjectileHit && (e.hit == ProjectileHitKind::Armor || e.hit == ProjectileHitKind::Pawn) &&
                    combatTestIsUnder(*sim, target, e.part, "torso") && !combatTestIsUnder(*sim, target, e.part, "neck") &&
                    !combatTestIsUnder(*sim, target, e.part, "left shoulder") && !combatTestIsUnder(*sim, target, e.part, "right shoulder")) {
                    ++torso;
                    stopped += e.hit == ProjectileHitKind::Armor ? 1 : 0;
                    bruised += (e.hit == ProjectileHitKind::Pawn && e.amount == doctest::Approx(6.0f)) ? 1 : 0;
                }
                injuries += e.type == EventType::PawnInjured ? 1 : 0;
            }
            if (sim->things().registry().view<Projectile>().empty()) {
                break;
            }
        }
        combatTestHeal(*sim, target);
    }
    // Flak vest 100% sharp less the revolver's 18% penetration: 41% bounce off, 41% half damage.
    COMBAT_REQUIRE(torso > 200);
    CHECK(static_cast<double>(stopped) / torso == doctest::Approx(0.41).epsilon(0.2));
    CHECK(static_cast<double>(bruised) / torso == doctest::Approx(0.41).epsilon(0.2));
}

TEST_CASE("Combat: shoot orders wait for the warm-up, fire bursts and cool down") {
    auto sim = combatTestSim();
    const Entity rifleman = combatTestPawn(*sim, 10, 30);
    const Entity gunner = combatTestPawn(*sim, 10, 50);
    const Entity target = combatTestPawn(*sim, 40, 40);
    const Entity rifle = sim->things().spawn("Gun_BoltActionRifle", CellPos{11, 30, 0});
    const Entity assault = sim->things().make(sim->defs().idOf("Things", "Gun_AssaultRifle"));
    sim->submit(EquipCommand{rifleman, rifle});
    sim->submit(EquipCommand{gunner, assault});
    sim->submit(ShootCommand{rifleman, target, 0.0f, 0.0f, 0.0f, false});
    sim->submit(ShootCommand{gunner, kNoEntity, 40.5f, 40.5f, 1.0f, false});
    sim->submit(ShootCommand{target, kNoEntity, 0.0f, 0.0f, 0.0f, false});  // no gun: fails
    std::vector<int64_t> rifleShots;
    std::vector<int64_t> burstShots;
    int failed = 0;
    for (int i = 0; i < 120; ++i) {
        const int64_t step = sim->time().seconds;
        sim->step();
        for (const Event& e : sim->events()) {
            if (e.type == EventType::ShotFired) {
                (e.thing == rifleman ? rifleShots : burstShots).push_back(step);
            }
            failed += e.type == EventType::CommandFailed ? 1 : 0;
        }
        if (i == 60) {
            sim->submit(ShootCommand{rifleman, target, 0.0f, 0.0f, 0.0f, false});  // cooling down: fails
        }
    }
    CHECK(!sim->things().spawned(rifle));  // picked up
    CHECK(failed == 2);
    COMBAT_REQUIRE(rifleShots.size() == 1);
    CHECK(rifleShots[0] == 50);  // 51 steps of warm-up, counting the step the order came in
    COMBAT_REQUIRE(burstShots.size() == 3);
    CHECK(burstShots[0] == 29);
    CHECK(burstShots[1] - burstShots[0] == 5);
    CHECK(burstShots[2] - burstShots[1] == 5);
    Snapshot snap;
    sim->writeSnapshot(snap);
    CHECK(snap.find(rifleman)->facingX > 0.6f);  // turned toward the target
    CHECK(snap.find(rifleman)->shotReadyIn == 0);

    // The player's trigger fires at once.
    sim->submit(ShootCommand{rifleman, kNoEntity, 40.5f, 30.5f, 1.2f, true});
    sim->step();
    bool fired = false;
    for (const Event& e : sim->events()) {
        fired = fired || (e.type == EventType::ShotFired && e.thing == rifleman);
    }
    CHECK(fired);
    sim->writeSnapshot(snap);
    CHECK(snap.projectiles.size() == 1);
    CHECK(snap.find(rifleman)->shotReadyIn > 0);
}

TEST_CASE("Combat: a firefight downs someone, and saving mid-flight changes nothing") {
    const auto setUp = [](Sim& sim) {
        const Entity a = combatTestPawn(sim, 20, 30, "Gun_Revolver");
        const Entity b = combatTestPawn(sim, 40, 34, "Gun_AssaultRifle");
        sim.things().registry().get<Posture>(b).stance = Stance::Crouching;
        return std::make_pair(a, b);
    };
    const auto keepShooting = [](Sim& sim, Entity a, Entity b) {
        if (sim.combat().shotReadyIn(a) == 0 && !sim.health().incapacitated(a) && !sim.health().incapacitated(b)) {
            sim.submit(ShootCommand{a, b, 0.0f, 0.0f, 0.0f, false});
        }
        if (sim.combat().shotReadyIn(b) == 0 && !sim.health().incapacitated(a) && !sim.health().incapacitated(b)) {
            sim.submit(ShootCommand{b, a, 0.0f, 0.0f, 0.0f, false});
        }
    };

    auto straight = combatTestSim(21);
    const auto [a, b] = setUp(*straight);
    auto saved = combatTestSim(21);
    setUp(*saved);
    bool savedMidFlight = false;
    int downedAt = -1;
    for (int i = 0; i < 3000; ++i) {
        keepShooting(*straight, a, b);
        straight->step();
        if (!savedMidFlight) {
            keepShooting(*saved, a, b);
            saved->step();
            if (!saved->things().registry().view<Projectile>().empty() && i > 100) {
                SaveLoadResult loaded = loadSim(saveSim(*saved), combatTestDefs());
                COMBAT_REQUIRE(loaded.sim != nullptr);
                CHECK(loaded.sim->stateHash() == saved->stateHash());
                CHECK(saveToJson(*saved).find("\"projectile\"") != std::string::npos);
                saved = std::move(loaded.sim);
                savedMidFlight = true;
            }
        } else {
            keepShooting(*saved, a, b);
            saved->step();
        }
        if (downedAt < 0 && (straight->health().incapacitated(a) || straight->health().incapacitated(b))) {
            downedAt = i;
        }
    }
    CHECK(savedMidFlight);
    CHECK(downedAt > 0);
    MESSAGE("someone went down after " << downedAt << " steps");
    CHECK(saved->stateHash() == straight->stateHash());
}
