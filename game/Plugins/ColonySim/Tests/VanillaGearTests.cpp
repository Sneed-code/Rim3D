#include "ColonySim/Apparel.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Health.h"
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

// These tests check docs/design/vanilla-gear.md (the weapons, armor and belts of RimWorld's base game).

#define VGEAR_REQUIRE(...)     \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> vgearDefs() {
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

std::unique_ptr<Sim> vgearSim() {
    SimSettings settings;
    settings.seed = 11;
    settings.mapSize = 96;
    settings.flatMap = true;
    return std::make_unique<Sim>(settings, vgearDefs());
}

Entity vgearPawn(Sim& sim, int x, int y, const char* gun = nullptr) {
    const Entity pawn = sim.things().spawn("Human", CellPos{x, y, 0});
    if (gun != nullptr) {
        sim.combat().equip(pawn, sim.things().make(sim.defs().idOf("Things", gun)));
    }
    return pawn;
}

float vgearHurt(Sim& sim, Entity pawn) {
    float total = 0.0f;
    for (const Injury& i : sim.things().registry().get<Health>(pawn).injuries) {
        total += i.severity;
    }
    return total;
}

// Fires one shot at a point and steps until it is gone; returns the Explosion events seen.
bool vgearHarmed = false;  // a pawn was injured, lost a part or died in the last vgearFire

std::vector<Event> vgearFire(Sim& sim, Entity shooter, float x, float y, float z) {
    Rng rng(3);
    std::vector<Event> blasts;
    vgearHarmed = false;
    if (sim.combat().fire(shooter, x, y, z, 0.0f, rng) == kNoEntity) {
        return blasts;
    }
    for (int i = 0; i < 400; ++i) {
        sim.step();
        for (const Event& e : sim.events()) {
            if (e.type == EventType::Explosion) {
                blasts.push_back(e);
            }
            vgearHarmed = vgearHarmed || e.type == EventType::PawnInjured || e.type == EventType::PawnDied || e.type == EventType::PawnPartLost;
        }
        if (sim.things().registry().view<Projectile>().empty()) {
            break;
        }
    }
    return blasts;
}

}  // namespace

TEST_CASE("Vanilla gear: every gun, melee weapon, armor piece and belt is defined with its recipe") {
    const DefDatabase& defs = *vgearDefs();
    const char* things[] = {
        "Gun_Revolver", "Gun_Autopistol", "Gun_MachinePistol", "Gun_PumpShotgun", "Gun_ChainShotgun", "Gun_BoltActionRifle",
        "Gun_AssaultRifle", "Gun_HeavySMG", "Gun_LMG", "Gun_Minigun", "Gun_SniperRifle", "Gun_ChargeRifle", "Gun_ChargeLance",
        "Gun_ShortBow", "Gun_RecurveBow", "Gun_Greatbow", "Gun_Pila", "Gun_DoomsdayLauncher", "Gun_TripleRocketLauncher",
        "Gun_EMPLauncher", "Gun_IncendiaryLauncher", "Gun_SmokeLauncher", "Grenade_Frag", "Grenade_EMP", "Grenade_Molotov",
        "Melee_Knife", "Melee_Club", "Melee_Gladius", "Melee_Spear", "Melee_Mace", "Melee_Longsword", "Melee_BreachAxe",
        "Melee_Monosword", "Melee_Plasmasword", "Melee_Zeushammer",
        "Apparel_FlakVest", "Apparel_FlakPants", "Apparel_FlakJacket", "Apparel_SimpleHelmet", "Apparel_FlakHelmet",
        "Apparel_PlateArmor", "Apparel_MarineArmor", "Apparel_MarineHelmet", "Apparel_ReconArmor", "Apparel_ReconHelmet",
        "Apparel_CataphractArmor", "Apparel_CataphractHelmet", "Apparel_ShieldBelt", "Apparel_SmokepopPack",
        "Apparel_FirefoamPopPack", "Plasteel", "Uranium", "Chemfuel", "ComponentSpacer", "Gold", "FabricationBench"};
    for (const char* name : things) {
        CHECK_MESSAGE(defs.find("Things", name) != nullptr, name);
    }
    // Everything that can be made has a recipe that makes it; what is traded for only (the ultratech
    // swords, hammer and the two big launchers) does not.
    std::vector<std::string> made;
    for (const DefId id : defs.idsOfKind("Recipes")) {
        const Def& recipe = defs.get(id);
        for (const char* name : things) {
            if (recipe.has((std::string("products.") + name).c_str())) {
                made.push_back(name);
            }
        }
    }
    const auto isMade = [&](const char* name) { return std::find(made.begin(), made.end(), name) != made.end(); };
    for (const char* name : {"Gun_Revolver", "Gun_Autopistol", "Gun_MachinePistol", "Gun_PumpShotgun", "Gun_ChainShotgun",
                             "Gun_BoltActionRifle", "Gun_AssaultRifle", "Gun_HeavySMG", "Gun_LMG", "Gun_Minigun", "Gun_SniperRifle",
                             "Gun_ChargeRifle", "Gun_ChargeLance", "Gun_ShortBow", "Gun_RecurveBow", "Gun_Greatbow", "Gun_Pila",
                             "Gun_EMPLauncher", "Gun_IncendiaryLauncher", "Gun_SmokeLauncher", "Grenade_Frag", "Grenade_EMP",
                             "Grenade_Molotov", "Apparel_FlakVest", "Apparel_FlakPants", "Apparel_FlakJacket", "Apparel_SimpleHelmet",
                             "Apparel_FlakHelmet", "Apparel_PlateArmor", "Apparel_MarineArmor", "Apparel_MarineHelmet",
                             "Apparel_ReconArmor", "Apparel_ReconHelmet", "Apparel_CataphractArmor", "Apparel_CataphractHelmet",
                             "Apparel_ShieldBelt", "Apparel_SmokepopPack", "Apparel_FirefoamPopPack", "ComponentSpacer"}) {
        CHECK_MESSAGE(isMade(name), name);
    }
    for (const char* name : {"Melee_Monosword", "Melee_Plasmasword", "Melee_Zeushammer", "Gun_DoomsdayLauncher", "Gun_TripleRocketLauncher"}) {
        CHECK_MESSAGE(!isMade(name), name);
    }
}

TEST_CASE("Vanilla gear: numbers from the RimWorld wiki") {
    const DefDatabase& defs = *vgearDefs();
    const Def* sniper = defs.find("Things", "Gun_SniperRifle");
    const Def* lmg = defs.find("Things", "Gun_LMG");
    const Def* minigun = defs.find("Things", "Gun_Minigun");
    const Def* bullet = defs.find("Projectiles", "Bullet_SniperRifle");
    const Def* mono = defs.find("Things", "Melee_Monosword");
    const Def* plasteel = defs.find("Things", "Plasteel");
    const Def* marine = defs.find("Things", "Apparel_MarineArmor");
    VGEAR_REQUIRE(sniper != nullptr);
    VGEAR_REQUIRE(lmg != nullptr);
    VGEAR_REQUIRE(minigun != nullptr);
    VGEAR_REQUIRE(bullet != nullptr);
    VGEAR_REQUIRE(mono != nullptr);
    VGEAR_REQUIRE(plasteel != nullptr);
    VGEAR_REQUIRE(marine != nullptr);
    CHECK(sniper->number("verbs.0.range") == doctest::Approx(44.9));
    CHECK(sniper->number("verbs.0.warmupTime") == doctest::Approx(105.0));   // 210 ticks
    CHECK(sniper->number("stats.rangedWeaponCooldown") == doctest::Approx(45.0));  // 90 ticks
    CHECK(sniper->number("stats.accuracyLong") == doctest::Approx(0.90));
    CHECK(bullet->number("projectile.damageAmountBase") == doctest::Approx(25.0));
    CHECK(bullet->number("projectile.armorPenetrationBase") == doctest::Approx(0.38));
    CHECK(lmg->integer("verbs.0.burstShotCount") == 6);
    CHECK(minigun->integer("verbs.0.burstShotCount") == 25);
    CHECK(mono->count("tools") == 3);
    CHECK(mono->number("tools.1.power") == doctest::Approx(25.0));
    CHECK(mono->number("tools.1.armorPenetration") == doctest::Approx(0.90));
    CHECK(plasteel->number("stuffProps.statFactors.maxHitPoints") == doctest::Approx(2.8));
    CHECK(plasteel->number("stuffProps.statFactors.armorRatingSharp") == doctest::Approx(1.14));
    CHECK(marine->number("stats.armorRatingSharp") == doctest::Approx(1.06));
    CHECK(marine->number("stats.armorRatingBlunt") == doctest::Approx(0.45));
    CHECK(marine->number("stats.armorRatingHeat") == doctest::Approx(0.54));
    CHECK(marine->number("stats.mass") == doctest::Approx(15.0));
    CHECK(defs.find("Recipes", "MakeMarineArmor")->number("workAmount") == doctest::Approx(60000 * 1.44));
}

TEST_CASE("Vanilla gear: apparel loads into the apparel system with its armor") {
    auto sim = vgearSim();
    const DefDatabase& defs = *vgearDefs();
    for (const char* name : {"Apparel_FlakJacket", "Apparel_MarineArmor", "Apparel_MarineHelmet", "Apparel_ReconArmor",
                             "Apparel_CataphractArmor", "Apparel_ShieldBelt", "Apparel_SmokepopPack"}) {
        CHECK_MESSAGE(sim->apparel().stats().isApparel(defs.idOf("Things", name)), name);
    }
    const Entity marine = sim->things().make(defs.idOf("Things", "Apparel_MarineArmor"));
    const Entity recon = sim->things().make(defs.idOf("Things", "Apparel_ReconArmor"));
    const entt::registry& reg = sim->things().registry();
    CHECK(sim->apparel().stats().armor(reg, marine, ArmorType::Sharp) == doctest::Approx(1.06f));
    CHECK(sim->apparel().stats().armor(reg, recon, ArmorType::Blunt) == doctest::Approx(0.40f));
}

TEST_CASE("Vanilla gear: a rocket bursts where it lands and hurts a man nearby, a smoke or EMP shell hurts nobody") {
    auto sim = vgearSim();
    const Entity shooter = vgearPawn(*sim, 10, 10, "Gun_DoomsdayLauncher");
    const Entity near = vgearPawn(*sim, 40, 10);
    const Entity far = vgearPawn(*sim, 40, 40);
    const std::vector<Event> blasts = vgearFire(*sim, shooter, 40.5f, 10.5f, 0.0f);
    VGEAR_REQUIRE(blasts.size() == 1);
    CHECK(blasts[0].amount == doctest::Approx(7.8f));
    CHECK(vgearHarmed);
    CHECK(vgearHurt(*sim, far) == 0.0f);
    (void)near;

    auto quiet = vgearSim();
    const Entity gunner = vgearPawn(*quiet, 10, 10, "Gun_SmokeLauncher");
    const Entity target = vgearPawn(*quiet, 20, 10);
    const std::vector<Event> puff = vgearFire(*quiet, gunner, 20.5f, 10.5f, 0.0f);
    CHECK(puff.empty());  // the shell lands and bursts, but does no damage, so there is no blast to see
    CHECK(!vgearHarmed);
    CHECK(vgearHurt(*quiet, target) == 0.0f);
}

TEST_CASE("Vanilla gear: an arrow wounds the man it hits") {
    auto sim = vgearSim();
    const Entity archer = vgearPawn(*sim, 10, 10, "Gun_ShortBow");
    const Entity target = vgearPawn(*sim, 20, 10);
    Rng rng(5);
    for (int shot = 0; shot < 40 && vgearHurt(*sim, target) == 0.0f; ++shot) {
        if (sim->combat().fire(archer, 20.5f, 10.5f, 1.0f, 0.0f, rng) == kNoEntity) {
            sim->step();
            continue;
        }
        for (int i = 0; i < 120; ++i) {
            sim->step();
            if (sim->things().registry().view<Projectile>().empty()) {
                break;
            }
        }
    }
    CHECK(vgearHurt(*sim, target) > 0.0f);
}
