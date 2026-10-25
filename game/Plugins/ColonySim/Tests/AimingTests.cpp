#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace colony;

#define AIM_REQUIRE(...)       \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// The game's real data plus a slow test projectile and the gun that fires it (no arrows or
// grenades are written yet). These tests check docs/design/aiming.md.
std::shared_ptr<const DefDatabase> aimTestDefs() {
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
        // 0.2 m a step before tuning's x3: 18 m/s, about a thrown grenade.
        sources.push_back(DefSource{"Projectiles", "zz_aim_test.json", R"([
            {"defName": "AimTestSlowShot", "projectile": {"damageDef": "Bullet", "damageAmountBase": 10, "armorPenetrationBase": 0.1, "speed": 0.2}}
        ])"});
        sources.push_back(DefSource{"Things", "zz_aim_test.json", R"([
            {"defName": "AimTestSlowGun", "category": "Item", "stackLimit": 1,
             "stats": {"rangedWeaponCooldown": 30, "accuracyTouch": 0.8, "accuracyShort": 0.8, "accuracyMedium": 0.8, "accuracyLong": 0.8},
             "verbs": [{"range": 30, "warmupTime": 9, "burstShotCount": 1, "defaultProjectile": "AimTestSlowShot"}]}
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

std::unique_ptr<Sim> aimTestSim(uint64_t seed = 7) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = 96;
    settings.flatMap = true;
    return std::make_unique<Sim>(settings, aimTestDefs());
}

Entity aimTestPawn(Sim& sim, int x, int y, const char* gun = nullptr) {
    const Entity pawn = sim.things().spawn("Human", CellPos{x, y, 0});
    if (gun != nullptr) {
        sim.combat().equip(pawn, sim.things().make(sim.defs().idOf("Things", gun)));
    }
    return pawn;
}

void aimTestSkill(Sim& sim, Entity pawn, int level) {
    Colonist& c = sim.things().registry().get_or_emplace<Colonist>(pawn);
    c.skills.clear();
    c.skills.push_back(SkillRecord{"Shooting", level});
}

void aimTestHeal(Sim& sim, Entity pawn) {
    sim.things().registry().replace<Health>(pawn, Health{});
    sim.health().refresh(pawn);
}

// The player takes the pawn over so it moves only when the test moves it.
void aimTestHold(Sim& sim, Entity pawn) {
    if (!sim.things().registry().all_of<Colonist>(pawn)) {  // only colonists can be taken over
        sim.things().registry().emplace<Colonist>(pawn);
    }
    if (!sim.things().registry().all_of<Worker>(pawn)) {
        sim.jobs().addWorker(pawn);
    }
    sim.submit(ControlPawnCommand{pawn});
    sim.step();
}

bool aimTestEvent(const Sim& sim, EventType type, Entity thing) {
    return std::any_of(sim.events().begin(), sim.events().end(),
                       [&](const Event& e) { return e.type == type && e.thing == thing; });
}

bool aimTestHitOn(const Sim& sim, Entity pawn) {
    return std::any_of(sim.events().begin(), sim.events().end(), [&](const Event& e) {
        return e.type == EventType::ProjectileHit && e.hit == ProjectileHitKind::Pawn && e.target == pawn;
    });
}

// The share of `shots` aimed shots that hit a man 22 m away walking north and south past the
// shooter, or standing still side on.
double aimTestHitRate(int skill, bool walking, int shots) {
    auto sim = aimTestSim(23);
    const Entity shooter = aimTestPawn(*sim, 20, 40, "Gun_Autopistol");
    const Entity target = aimTestPawn(*sim, 42, 40);
    aimTestSkill(*sim, shooter, skill);
    aimTestHold(*sim, target);
    // Side on to the shooter either way, as a man walking past is.
    sim->things().registry().get<Posture>(target) = Posture{Stance::Standing, 0.0f, 1.0f};
    float y = 40.5f;
    float dy = 0.13f;  // a walk, metres a step
    int fired = 0;
    int hits = 0;
    for (int i = 0; i < 200000 && fired < shots; ++i) {
        if (walking) {
            if (y > 46.0f || y < 34.0f) {
                dy = -dy;
            }
            y += dy;
            sim->submit(DirectMoveCommand{target, 42.5f, y});
        }
        if (sim->combat().shotReadyIn(shooter) == 0 && !sim->combat().reloading(shooter)) {
            sim->submit(ShootCommand{shooter, target, 0.0f, 0.0f, 0.0f, false});
        }
        sim->step();
        fired += aimTestEvent(*sim, EventType::ShotFired, shooter) ? 1 : 0;
        if (aimTestHitOn(*sim, target)) {
            ++hits;
            aimTestHeal(*sim, target);
        }
    }
    return static_cast<double>(hits) / static_cast<double>(std::max(1, fired));
}

}  // namespace

TEST_CASE("Aiming: guns and rules have magazines, reload times and skill curves") {
    auto sim = aimTestSim();
    const Entity pawn = aimTestPawn(*sim, 10, 10, "Gun_Revolver");
    CHECK(sim->combat().magazineSize(pawn) == 6);
    CHECK(sim->combat().roundsLeft(pawn) == 6);
    aimTestSkill(*sim, pawn, 0);
    CHECK(sim->combat().reactionSteps(pawn) == 15);
    aimTestSkill(*sim, pawn, 20);
    CHECK(sim->combat().reactionSteps(pawn) == 5);
    aimTestSkill(*sim, pawn, 10);
    CHECK(sim->combat().reactionSteps(pawn) == 10);
    const Entity rifleman = aimTestPawn(*sim, 12, 10, "Gun_AssaultRifle");
    CHECK(sim->combat().magazineSize(rifleman) == 30);
    CHECK(sim->combat().magazineSize(aimTestPawn(*sim, 14, 10)) == 0);  // no gun
}

TEST_CASE("Aiming: a gun runs dry, reloads by itself, faster with skill, and keeps its rounds") {
    for (const int skill : {0, 20}) {
        auto sim = aimTestSim();
        const Entity pawn = aimTestPawn(*sim, 20, 40, "Gun_Revolver");
        aimTestSkill(*sim, pawn, skill);
        int fired = 0;
        int64_t emptyAt = -1;
        bool reloadEvent = false;
        bool refused = false;
        for (int i = 0; i < 600 && emptyAt < 0; ++i) {
            if (sim->combat().shotReadyIn(pawn) == 0) {
                sim->submit(ShootCommand{pawn, kNoEntity, 60.5f, 40.5f, 1.0f, true});
            }
            sim->step();
            fired += aimTestEvent(*sim, EventType::ShotFired, pawn) ? 1 : 0;
            for (const Event& e : sim->events()) {
                if (e.type == EventType::PawnAction && e.action == PawnAction::Reload && e.thing == pawn) {
                    reloadEvent = true;
                    emptyAt = sim->time().seconds;
                }
            }
        }
        CHECK(fired == 6);
        CHECK(reloadEvent);
        CHECK(sim->combat().reloading(pawn));
        CHECK(sim->combat().roundsLeft(pawn) == 0);
        // Cooldown 48 steps, then 120 x 1.3 at skill 0 or x 0.7 at skill 20.
        const int expected = 48 + (skill == 0 ? 156 : 84);
        CHECK(sim->combat().shotReadyIn(pawn) == expected);
        sim->submit(ShootCommand{pawn, kNoEntity, 60.5f, 40.5f, 1.0f, true});
        sim->step();
        for (const Event& e : sim->events()) {
            refused = refused || (e.type == EventType::CommandFailed && e.message.find("reloading") != std::string::npos);
        }
        CHECK(refused);
        Snapshot snap;
        sim->writeSnapshot(snap);
        CHECK(snap.find(pawn)->reloading);
        CHECK(snap.find(pawn)->rounds == 0);
        CHECK(snap.find(pawn)->magazineSize == 6);
        sim->stepMany(expected - 2);
        CHECK(sim->combat().reloading(pawn));
        sim->step();
        CHECK(!sim->combat().reloading(pawn));
        CHECK(sim->combat().roundsLeft(pawn) == 6);
        CHECK(sim->time().seconds - emptyAt == expected);
    }

    // A reload on order: refused while full, done after one shot; the gun keeps its count.
    auto sim = aimTestSim();
    const Entity a = aimTestPawn(*sim, 20, 40, "Gun_BoltActionRifle");
    sim->submit(ReloadCommand{a});
    sim->step();
    bool failed = false;
    for (const Event& e : sim->events()) {
        failed = failed || (e.type == EventType::CommandFailed && e.message.find("full") != std::string::npos);
    }
    CHECK(failed);
    sim->submit(ShootCommand{a, kNoEntity, 60.5f, 40.5f, 1.0f, true});
    sim->stepMany(2);
    CHECK(sim->combat().roundsLeft(a) == 4);
    const Entity gun = sim->things().registry().get<Gear>(a).weapon;
    const Entity b = aimTestPawn(*sim, 20, 41);
    sim->combat().dropGear(a);
    AIM_REQUIRE(sim->combat().equip(b, gun));
    CHECK(sim->combat().roundsLeft(b) == 4);
    sim->stepMany(60);  // b's trigger has nothing to do with a's cooldown
    sim->submit(ReloadCommand{b});
    sim->step();
    CHECK(sim->combat().reloading(b));
}

TEST_CASE("Aiming: a perfectly led shot meets a walking target, an unled one misses") {
    for (const bool lead : {true, false}) {
        auto sim = aimTestSim();
        const Entity shooter = aimTestPawn(*sim, 10, 40, "Gun_BoltActionRifle");
        const Entity target = aimTestPawn(*sim, 46, 37);
        aimTestHold(*sim, target);
        float y = 37.5f;
        const auto walk = [&] {
            y += 0.15f;
            sim->submit(DirectMoveCommand{target, 46.5f, y});
            sim->step();
        };
        for (int i = 0; i < 5; ++i) {
            walk();  // under way, facing north
        }
        float x = 0.0f;
        float yy = 0.0f;
        float z = 0.0f;
        if (lead) {
            AIM_REQUIRE(sim->combat().leadPoint(shooter, target, 1.0f, x, yy, z));
        } else {
            AIM_REQUIRE(sim->combat().aimPoint(target, x, yy, z));
        }
        Rng rng(3);
        sim->combat().fire(shooter, x, yy, z, 0.0f, rng);
        // In the game a shot is fired after the target's move in that step and flies its first
        // stretch at once; here it was fired between steps, so the target waits one step.
        sim->step();
        bool hit = aimTestHitOn(*sim, target);
        for (int i = 0; i < 30 && !sim->things().registry().view<Projectile>().empty(); ++i) {
            walk();
            hit = hit || aimTestHitOn(*sim, target);
        }
        CHECK(hit == lead);
    }
}

TEST_CASE("Aiming: a good shot leads a walking man about as well as it hits a standing one") {
    const double standing = aimTestHitRate(20, false, 300);
    const double walking = aimTestHitRate(20, true, 300);
    const double walkingNovice = aimTestHitRate(0, true, 300);
    MESSAGE("skill 20: standing " << standing << ", walking " << walking << "; skill 0 walking " << walkingNovice);
    CHECK(standing > 0.15);
    CHECK(walking > 0.8 * standing);
    CHECK(walkingNovice < walking);
}

TEST_CASE("Aiming: a fighter waits its reaction time before shooting a new target") {
    int64_t firstShot[2] = {-1, -1};
    for (const int skill : {0, 20}) {
        auto sim = aimTestSim();
        const Entity colonist = aimTestPawn(*sim, 30, 40, "Gun_Revolver");
        const Entity raider = aimTestPawn(*sim, 45, 40);
        aimTestSkill(*sim, colonist, skill);
        aimTestHold(*sim, raider);  // stands in the open, unarmed
        sim->submit(SetFactionCommand{raider, 2});
        sim->submit(DraftCommand{colonist, true});
        int64_t reactUntil = -1;
        int64_t pickedAt = -1;
        for (int i = 0; i < 200 && firstShot[skill / 20] < 0; ++i) {
            sim->step();
            const Fighter& f = sim->things().registry().get<Fighter>(colonist);
            if (pickedAt < 0 && f.target == raider) {
                pickedAt = sim->time().seconds - 1;  // the plan ran in the step just taken
                reactUntil = f.reactUntil;
            }
            if (aimTestEvent(*sim, EventType::ShotFired, colonist)) {
                firstShot[skill / 20] = sim->time().seconds - 1;
            }
        }
        AIM_REQUIRE(pickedAt >= 0);
        CHECK(reactUntil - pickedAt == (skill == 0 ? 15 : 5));
        // Then the revolver's warm-up of 9 steps.
        CHECK(firstShot[skill / 20] >= reactUntil + 8);
    }
    CHECK(firstShot[1] < firstShot[0]);
}

TEST_CASE("Aiming: a drafted fighter tops up its magazine when there is nothing to shoot") {
    auto sim = aimTestSim();
    const Entity colonist = aimTestPawn(*sim, 30, 40, "Gun_AssaultRifle");
    const Entity gun = sim->things().registry().get<Gear>(colonist).weapon;
    sim->things().registry().emplace<Magazine>(gun, Magazine{20});
    sim->submit(DraftCommand{colonist, true});
    sim->stepMany(40);
    CHECK(sim->combat().reloading(colonist));
    sim->stepMany(200);
    CHECK(!sim->combat().reloading(colonist));
    CHECK(sim->combat().roundsLeft(colonist) == 30);
}

TEST_CASE("Aiming: a fighter sidesteps a slow projectile but not a bullet") {
    for (const bool slow : {true, false}) {
        auto sim = aimTestSim();
        const Entity colonist = aimTestPawn(*sim, 40, 40, "Gun_Revolver");
        const Entity thrower = aimTestPawn(*sim, 22, 40, slow ? "AimTestSlowGun" : "Gun_BoltActionRifle");
        aimTestHold(*sim, thrower);
        sim->submit(DraftCommand{colonist, true});
        sim->stepMany(20);  // drafted and standing
        const CellPos before = cellOf(sim->things().registry().get<Position>(colonist));
        float x = 0.0f, y = 0.0f, z = 0.0f;
        AIM_REQUIRE(sim->combat().aimPoint(colonist, x, y, z));
        Rng rng(5);
        AIM_REQUIRE(sim->combat().fire(thrower, x, y, z, 0.0f, rng) != kNoEntity);
        bool dodged = false;
        bool hit = false;
        for (int i = 0; i < 150 && !sim->things().registry().view<Projectile>().empty(); ++i) {
            sim->step();
            for (const Event& e : sim->events()) {
                dodged = dodged || (e.type == EventType::PawnAction && e.action == PawnAction::Dodge && e.thing == colonist);
            }
            hit = hit || aimTestHitOn(*sim, colonist);
        }
        CHECK(dodged == slow);
        CHECK(hit == !slow);
        if (slow) {
            CHECK(cellOf(sim->things().registry().get<Position>(colonist)) != before);
            sim->stepMany(60);  // and back to its place
            CHECK(cellOf(sim->things().registry().get<Position>(colonist)) == before);
        }
    }
}

TEST_CASE("Aiming: magazines, reloads and reaction times survive a save and load") {
    auto sim = aimTestSim();
    const Entity colonist = aimTestPawn(*sim, 30, 40, "Gun_Revolver");
    const Entity raider = aimTestPawn(*sim, 45, 40, "Gun_AssaultRifle");
    sim->submit(SetFactionCommand{raider, 2});
    sim->submit(DraftCommand{colonist, true});
    bool sawReload = false;
    for (int i = 0; i < 2000 && !sawReload; ++i) {
        aimTestHeal(*sim, colonist);
        aimTestHeal(*sim, raider);
        sim->step();
        sawReload = sim->combat().reloading(colonist) || sim->combat().reloading(raider);
    }
    AIM_REQUIRE(sawReload);
    const std::string json = saveToJson(*sim);
    CHECK(json.find("\"magazine\"") != std::string::npos);
    CHECK(json.find("\"reactUntil\"") != std::string::npos);
    SaveLoadResult loaded = loadSim(saveSim(*sim), aimTestDefs());
    AIM_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->stateHash() == sim->stateHash());
    CHECK(loaded.sim->combat().reloading(colonist) == sim->combat().reloading(colonist));
    CHECK(loaded.sim->combat().roundsLeft(raider) == sim->combat().roundsLeft(raider));
    sim->stepMany(300);
    loaded.sim->stepMany(300);
    CHECK(loaded.sim->stateHash() == sim->stateHash());
}
