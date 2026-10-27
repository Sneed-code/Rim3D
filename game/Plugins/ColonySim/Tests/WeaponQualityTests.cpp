#include "ColonySim/Apparel.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Health.h"
#include "ColonySim/Melee.h"
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

// These tests check docs/design/weapon-quality.md: a weapon's quality and stuff scale what it does.

#define WQ_REQUIRE(...)       \
    do {                      \
        CHECK(__VA_ARGS__);   \
        if (!(__VA_ARGS__)) { \
            return;           \
        }                     \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> wqDefs() {
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

std::unique_ptr<Sim> wqSim(uint64_t seed = 11) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = 64;
    settings.flatMap = true;
    return std::make_unique<Sim>(settings, wqDefs());
}

Entity wqPawn(Sim& sim, float x, float y) {
    const Entity pawn = sim.things().spawn("Human", CellPos{static_cast<int>(x), static_cast<int>(y), 0});
    Position& pos = sim.things().registry().get<Position>(pawn);
    pos.x = x;
    pos.y = y;
    return pawn;
}

// A weapon of the def, made of `stuff` (empty: none) at quality level `quality` (-1: normal).
Entity wqWeapon(Sim& sim, const char* def, const char* stuff, int quality) {
    const Entity item = sim.things().make(sim.defs().idOf("Things", def));
    auto& reg = sim.things().registry();
    if (stuff != nullptr) {
        reg.emplace<MadeOf>(item, MadeOf{sim.defs().idOf("Things", stuff)});
    }
    if (quality >= 0) {
        reg.emplace<Quality>(item, Quality{static_cast<uint8_t>(quality)});
    }
    return item;
}

constexpr int kAwful = 0;
constexpr int kExcellent = 4;
constexpr int kMasterwork = 5;
constexpr int kLegendary = 6;

// The damage of the first blow a skilled fighter lands on a bare man, armed with this weapon.
float wqFirstBlow(const char* def, const char* stuff, int quality) {
    auto sim = wqSim();
    const Entity attacker = wqPawn(*sim, 30.5f, 30.5f);
    const Entity target = wqPawn(*sim, 31.3f, 30.5f);
    sim->combat().equip(attacker, wqWeapon(*sim, def, stuff, quality));
    Colonist& c = sim->things().registry().get_or_emplace<Colonist>(attacker);
    c.skills.clear();
    c.skills.push_back(SkillRecord{"Melee", 20});
    Posture& p = sim->things().registry().get<Posture>(target);
    p.facingX = -1.0f;
    p.facingY = 0.0f;
    sim->submit(MeleeCommand{attacker, target, MeleeMode::Attack});
    for (int s = 0; s < 120; ++s) {
        sim->step();
        for (const Event& e : sim->events()) {
            if (e.type == EventType::MeleeHit && e.message == "hit") {
                return e.amount;
            }
        }
    }
    return -1.0f;
}

}  // namespace

TEST_CASE("Weapon quality: the factors are the wiki's, by quality and stuff") {
    auto sim = wqSim();
    const DefId cut = sim->defs().idOf("DamageTypes", "Cut");
    const DefId blunt = sim->defs().idOf("DamageTypes", "Blunt");
    CombatSystem& combat = sim->combat();
    CHECK(combat.meleeDamageFactor(wqWeapon(*sim, "Melee_Knife", nullptr, -1), cut) == doctest::Approx(1.0f));
    CHECK(combat.meleeDamageFactor(wqWeapon(*sim, "Melee_Knife", nullptr, kAwful), cut) == doctest::Approx(0.8f));
    CHECK(combat.meleeDamageFactor(wqWeapon(*sim, "Melee_Knife", nullptr, kExcellent), cut) == doctest::Approx(1.2f));
    CHECK(combat.meleeDamageFactor(wqWeapon(*sim, "Melee_Knife", nullptr, kMasterwork), cut) == doctest::Approx(1.45f));
    CHECK(combat.meleeDamageFactor(wqWeapon(*sim, "Melee_Knife", nullptr, kLegendary), cut) == doctest::Approx(1.65f));
    // Steel says nothing about melee damage: 1. Wood: 0.4 sharp, 0.9 blunt, times the quality's.
    CHECK(combat.meleeDamageFactor(wqWeapon(*sim, "Melee_Knife", "Steel", kExcellent), cut) == doctest::Approx(1.2f));
    CHECK(combat.meleeDamageFactor(wqWeapon(*sim, "Melee_Club", "WoodLog", -1), blunt) == doctest::Approx(0.9f));
    CHECK(combat.meleeDamageFactor(wqWeapon(*sim, "Melee_Club", "WoodLog", kLegendary), blunt) == doctest::Approx(0.9f * 1.65f));
    CHECK(combat.meleeDamageFactor(wqWeapon(*sim, "Melee_Knife", "WoodLog", -1), cut) == doctest::Approx(0.4f));
    CHECK(combat.meleeDamageFactor(kNoEntity, cut) == doctest::Approx(1.0f));
    // Guns: accuracy and damage have their own columns.
    const Entity rifle = wqWeapon(*sim, "Gun_Revolver", nullptr, kMasterwork);
    CHECK(combat.rangedAccuracyFactor(rifle) == doctest::Approx(1.35f));
    CHECK(combat.rangedDamageFactor(rifle) == doctest::Approx(1.25f));
    CHECK(combat.rangedAccuracyFactor(wqWeapon(*sim, "Gun_Revolver", nullptr, kAwful)) == doctest::Approx(0.8f));
    CHECK(combat.rangedDamageFactor(wqWeapon(*sim, "Gun_Revolver", nullptr, kAwful)) == doctest::Approx(0.9f));
    CHECK(combat.rangedDamageFactor(wqWeapon(*sim, "Gun_Revolver", nullptr, -1)) == doctest::Approx(1.0f));
}

TEST_CASE("Weapon quality: a better melee weapon hits harder, a worse one softer") {
    const float normal = wqFirstBlow("Melee_Knife", nullptr, -1);
    WQ_REQUIRE(normal > 0.0f);
    CHECK(wqFirstBlow("Melee_Knife", nullptr, kLegendary) == doctest::Approx(normal * 1.65f).epsilon(0.02));
    CHECK(wqFirstBlow("Melee_Knife", nullptr, kAwful) == doctest::Approx(normal * 0.8f).epsilon(0.02));
    CHECK(wqFirstBlow("Melee_Knife", "WoodLog", -1) == doctest::Approx(normal * 0.4f).epsilon(0.02));
    // Fists have no weapon, so nothing scales them.
    CHECK(wqFirstBlow("Gun_Revolver", nullptr, kLegendary) == doctest::Approx(wqFirstBlow("Gun_Revolver", nullptr, -1)));
}

TEST_CASE("Weapon quality: a good gun's bullets carry its damage factor") {
    for (const int quality : {-1, kAwful, kMasterwork, kLegendary}) {
        auto sim = wqSim();
        const Entity shooter = wqPawn(*sim, 30.5f, 30.5f);
        const Entity target = wqPawn(*sim, 40.5f, 30.5f);
        const Entity gun = wqWeapon(*sim, "Gun_Revolver", nullptr, quality);
        WQ_REQUIRE(sim->combat().carryWeapon(shooter, gun));
        std::string why;
        WQ_REQUIRE(sim->combat().shoot(shooter, target, 40.5f, 30.5f, 1.0f, false, &why));
        float factor = -1.0f;
        for (int s = 0; s < 200 && factor < 0.0f; ++s) {
            sim->step();
            for (const auto [shot, p] : sim->things().registry().view<const Projectile>().each()) {
                if (p.launcher == shooter) {
                    factor = p.damageFactor;
                }
            }
        }
        CHECK(factor == doctest::Approx(sim->combat().rangedDamageFactor(gun)));
    }
}
