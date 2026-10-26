#include "ColonySim/Apparel.h"
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
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace colony;

// These tests check docs/design/weapons.md (task P1).

#define WEAP_REQUIRE(...)      \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> weapDefs() {
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

std::unique_ptr<Sim> weapSim(uint64_t seed = 21) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = 96;
    settings.flatMap = true;
    settings.checkpointEvery = 50;
    return std::make_unique<Sim>(settings, weapDefs());
}

Entity weapPawn(Sim& sim, float x, float y) {
    const Entity pawn = sim.things().spawn("Human", CellPos{static_cast<int>(x), static_cast<int>(y), 0});
    Position& pos = sim.things().registry().get<Position>(pawn);
    pos.x = x;
    pos.y = y;
    return pawn;
}

Entity weapItem(Sim& sim, const char* def) {
    return sim.things().make(sim.defs().idOf("Things", def));
}

// A pawn carrying the named weapons: the first in the hands, the rest on the body.
Entity weapArmed(Sim& sim, float x, float y, std::initializer_list<const char*> weapons) {
    const Entity pawn = weapPawn(sim, x, y);
    for (const char* def : weapons) {
        CHECK(sim.combat().carryWeapon(pawn, weapItem(sim, def)));
    }
    return pawn;
}

DefId weapHandsDef(Sim& sim, Entity pawn) {
    const Gear& gear = sim.things().registry().get<Gear>(pawn);
    return gear.weapon == kNoEntity ? DefId{} : sim.things().registry().get<Thing>(gear.weapon).def;
}

bool weapFailed(const Sim& sim, uint64_t id) {
    return std::any_of(sim.events().begin(), sim.events().end(),
                       [&](const Event& e) { return e.type == EventType::CommandFailed && e.command == id; });
}

int weapCount(const Sim& sim, EventType type) {
    return static_cast<int>(
        std::count_if(sim.events().begin(), sim.events().end(), [&](const Event& e) { return e.type == type; }));
}

void weapHeal(Sim& sim, Entity pawn) {
    sim.things().registry().replace<Health>(pawn, Health{});
    sim.health().refresh(pawn);
}

Snapshot weapSnapshot(const Sim& sim) {
    Snapshot snap;
    sim.writeSnapshot(snap);
    return snap;
}

int weapSwitchSteps(Sim& sim, const char* slot) {
    return static_cast<int>(sim.defs().get(sim.defs().idOf("Rules", "CombatRules")).integer((std::string("weaponSwitchSteps.") + slot).c_str(), 0));
}

}  // namespace

TEST_CASE("Weapons: a pawn carries a rifle, a sidearm and a melee weapon, one to a slot") {
    auto sim = weapSim();
    const Entity pawn = weapArmed(*sim, 30.5f, 30.5f, {"Gun_BoltActionRifle", "Gun_Revolver", "Melee_Knife"});
    const entt::registry& reg = sim->things().registry();
    const Gear& gear = reg.get<Gear>(pawn);
    CHECK(weapHandsDef(*sim, pawn) == sim->defs().idOf("Things", "Gun_BoltActionRifle"));  // the first goes in the hands
    CHECK(gear.stowed.size() == 2);
    CHECK(reg.get<Thing>(sim->combat().weaponIn(pawn, WeaponSlot::Primary)).def == sim->defs().idOf("Things", "Gun_BoltActionRifle"));
    CHECK(reg.get<Thing>(sim->combat().weaponIn(pawn, WeaponSlot::Sidearm)).def == sim->defs().idOf("Things", "Gun_Revolver"));
    CHECK(reg.get<Thing>(sim->combat().weaponIn(pawn, WeaponSlot::Melee)).def == sim->defs().idOf("Things", "Melee_Knife"));
    WeaponSlot hands = WeaponSlot::Melee;
    CHECK(sim->combat().handsSlot(pawn, hands));
    CHECK(hands == WeaponSlot::Primary);

    // A second rifle takes the rifle's place: the old one goes to the ground where the pawn stands.
    const Entity assault = weapItem(*sim, "Gun_AssaultRifle");
    CHECK(sim->combat().carryWeapon(pawn, assault));
    CHECK(sim->combat().weaponIn(pawn, WeaponSlot::Primary) == assault);
    CHECK(reg.get<Gear>(pawn).stowed.size() == 2);
    const Snapshot snap = weapSnapshot(*sim);
    const ThingView* view = snap.find(pawn);
    WEAP_REQUIRE(view != nullptr);
    CHECK(view->carried[0] == sim->defs().idOf("Things", "Gun_AssaultRifle"));
    CHECK(view->carried[1] == sim->defs().idOf("Things", "Gun_Revolver"));
    CHECK(view->carried[2] == sim->defs().idOf("Things", "Melee_Knife"));
    CHECK(view->weapon == sim->defs().idOf("Things", "Gun_AssaultRifle"));

    // Equip puts a weapon in the hands as before, and still drops what was in the hands; a stowed weapon
    // of the same slot goes too.
    const Entity shotgun = weapItem(*sim, "Gun_PumpShotgun");
    sim->submit(EquipCommand{pawn, shotgun});
    sim->step();
    CHECK(reg.get<Gear>(pawn).weapon == shotgun);
    CHECK(reg.get<Gear>(pawn).stowed.size() == 2);

    // Not a weapon, or someone else's: refused.
    std::string why;
    CHECK_FALSE(sim->combat().carryWeapon(pawn, weapItem(*sim, "Steel"), &why));
    CHECK_FALSE(why.empty());
    const Entity other = weapPawn(*sim, 34.5f, 30.5f);
    CHECK_FALSE(sim->combat().carryWeapon(other, shotgun, &why));
}

TEST_CASE("Weapons: a switch takes the slot's time, and the pawn can't shoot or swing meanwhile") {
    auto sim = weapSim();
    const Entity pawn = weapArmed(*sim, 30.5f, 30.5f, {"Gun_BoltActionRifle", "Gun_Revolver", "Melee_Knife"});
    const Entity enemy = weapPawn(*sim, 40.5f, 30.5f);
    sim->submit(SetFactionCommand{enemy, 2});
    sim->step();
    const Entity rifle = sim->things().registry().get<Gear>(pawn).weapon;
    const Entity revolver = sim->combat().weaponIn(pawn, WeaponSlot::Sidearm);

    const int steps = weapSwitchSteps(*sim, "Sidearm");
    WEAP_REQUIRE(steps > 1);
    sim->submit(SwitchWeaponCommand{pawn, WeaponSlot::Sidearm});
    sim->step();
    CHECK(sim->combat().switching(pawn));
    CHECK(weapHandsDef(*sim, pawn) == sim->defs().idOf("Things", "Gun_BoltActionRifle"));  // still the rifle in the hands
    {
        const Snapshot snap = weapSnapshot(*sim);
        CHECK(snap.find(pawn)->switchSlot == static_cast<uint8_t>(WeaponSlot::Sidearm));
        CHECK(snap.find(pawn)->switchStepsLeft == steps - 1);
    }
    const uint64_t shot = sim->submit(ShootCommand{pawn, enemy, 0.0f, 0.0f, 0.0f, false});
    const uint64_t again = sim->submit(SwitchWeaponCommand{pawn, WeaponSlot::Melee});
    const uint64_t swing = sim->submit(MeleeCommand{pawn, enemy, MeleeMode::Attack});
    sim->step();
    CHECK(weapFailed(*sim, shot));
    CHECK(weapFailed(*sim, again));  // switching already
    CHECK(weapFailed(*sim, swing));

    int events = 0;
    for (int s = 0; s < steps + 2; ++s) {
        events += weapCount(*sim, EventType::WeaponSwitched);
        sim->step();
    }
    events += weapCount(*sim, EventType::WeaponSwitched);
    CHECK(events == 1);
    CHECK_FALSE(sim->combat().switching(pawn));
    const Gear& gear = sim->things().registry().get<Gear>(pawn);
    CHECK(gear.weapon == revolver);  // the sidearm is in the hands...
    CHECK(std::find(gear.stowed.begin(), gear.stowed.end(), rifle) != gear.stowed.end());  // ...the rifle on the body
    CHECK(weapSnapshot(*sim).find(pawn)->weapon == sim->defs().idOf("Things", "Gun_Revolver"));
    CHECK(sim->combat().shoot(pawn, enemy, 0.0f, 0.0f, 0.0f, false));
}

TEST_CASE("Weapons: a switch is refused when it can't be done") {
    auto sim = weapSim();
    const Entity pawn = weapArmed(*sim, 30.5f, 30.5f, {"Gun_BoltActionRifle", "Gun_Revolver"});
    const Entity enemy = weapPawn(*sim, 40.5f, 30.5f);
    sim->step();
    std::string why;
    CHECK_FALSE(sim->combat().switchWeapon(pawn, WeaponSlot::Melee, &why));  // nothing in the slot
    CHECK_FALSE(sim->combat().switchWeapon(pawn, WeaponSlot::Primary, &why));  // already in the hands
    CHECK(sim->combat().shoot(pawn, enemy, 0.0f, 0.0f, 0.0f, true));
    CHECK_FALSE(sim->combat().switchWeapon(pawn, WeaponSlot::Sidearm, &why));  // mid-shot
    CHECK(why == "busy shooting");
    sim->stepMany(200);
    CHECK(sim->combat().switchWeapon(pawn, WeaponSlot::Sidearm, &why));
    // Downed: refused.
    const Entity victim = weapArmed(*sim, 50.5f, 30.5f, {"Gun_BoltActionRifle", "Gun_Revolver"});
    sim->submit(DamagePawnCommand{victim, "Cut", 200.0f});
    sim->stepMany(2);
    CHECK_FALSE(sim->combat().switchWeapon(victim, WeaponSlot::Sidearm, &why));
    CHECK(sim->combat().switchWeapon(pawn, WeaponSlot::Primary, &why) == false);  // switching already
}

TEST_CASE("Weapons: each gun keeps its own magazine through a switch") {
    auto sim = weapSim();
    const Entity pawn = weapArmed(*sim, 30.5f, 30.5f, {"Gun_BoltActionRifle", "Gun_Revolver"});
    const Entity enemy = weapPawn(*sim, 45.5f, 30.5f);
    sim->step();
    const int rifleSize = sim->combat().magazineSize(pawn);
    WEAP_REQUIRE(rifleSize > 1);
    CHECK(sim->combat().shoot(pawn, enemy, 0.0f, 0.0f, 0.0f, true));
    sim->stepMany(120);
    const int riflerounds = sim->combat().roundsLeft(pawn);
    CHECK(riflerounds == rifleSize - 1);
    CHECK(sim->combat().switchWeapon(pawn, WeaponSlot::Sidearm));
    sim->stepMany(weapSwitchSteps(*sim, "Sidearm") + 1);
    CHECK(sim->combat().magazineSize(pawn) != rifleSize);
    CHECK(sim->combat().roundsLeft(pawn) == sim->combat().magazineSize(pawn));  // a full revolver
    CHECK(sim->combat().switchWeapon(pawn, WeaponSlot::Primary));
    sim->stepMany(weapSwitchSteps(*sim, "Primary") + 1);
    CHECK(sim->combat().roundsLeft(pawn) == riflerounds);
}

TEST_CASE("Weapons: everything carried is dropped, stripped or lost with the pawn") {
    auto sim = weapSim();
    const Entity pawn = weapArmed(*sim, 30.5f, 30.5f, {"Gun_BoltActionRifle", "Gun_Revolver", "Melee_Knife"});
    const Entity knife = sim->combat().weaponIn(pawn, WeaponSlot::Melee);
    CHECK(sim->apparel().hasGear(pawn));
    CHECK(sim->combat().unequip(pawn, knife));  // a weapon on the body can be put down
    CHECK(sim->things().spawned(knife));
    CHECK(sim->things().registry().get<Gear>(pawn).stowed.size() == 1);
    sim->combat().dropGear(pawn);
    CHECK(sim->things().registry().try_get<Gear>(pawn) == nullptr);
    int guns = 0;
    for (const Entity item : sim->things().itemsAt(CellPos{30, 30, 0})) {
        guns += sim->combat().isGun(sim->things().registry().get<Thing>(item).def) ? 1 : 0;
    }
    CHECK(guns == 2);
}

TEST_CASE("Weapons: weapons on the body add to the load, slow the walk and cut what the hands can haul") {
    auto sim = weapSim();
    const Entity light = weapArmed(*sim, 30.5f, 30.5f, {"Gun_BoltActionRifle"});
    const Entity heavy = weapArmed(*sim, 34.5f, 30.5f, {"Gun_BoltActionRifle", "Gun_Revolver", "Melee_BreachAxe"});
    const DefId steel = sim->defs().idOf("Things", "Steel");
    CHECK(sim->jobs().bodyWeaponMass(light) == doctest::Approx(0.0));  // the weapon in the hands is the usual outfit
    const double mass = sim->jobs().bodyWeaponMass(heavy);
    CHECK(mass == doctest::Approx(1.4 + 2.5));
    CHECK(sim->jobs().loadSpeedFactor(light) == doctest::Approx(1.0f));
    const float factor = sim->jobs().loadSpeedFactor(heavy);
    CHECK(factor < 1.0f);
    CHECK(factor > 0.7f);
    CHECK(sim->jobs().carryLimit(heavy, steel) <= sim->jobs().carryLimit(light, steel));
    CHECK(sim->jobs().carryLimit(heavy, steel) >= 1);
}

TEST_CASE("Weapons: a fighter picks the weapon for the distance") {
    // Near: a knife; middle distance: the revolver; far: the rifle. The enemy is an unarmed raider that holds still.
    const struct {
        float gap;
        const char* expected;
    } cases[] = {{1.6f, "Melee_Knife"}, {6.0f, "Gun_Revolver"}, {20.0f, "Gun_BoltActionRifle"}};
    for (const auto& c : cases) {
        auto sim = weapSim(31);
        // Starts with the rifle in the hands.
        const Entity fighter = weapArmed(*sim, 30.5f, 30.5f, {"Gun_BoltActionRifle", "Gun_Revolver", "Melee_Knife"});
        const Entity enemy = weapPawn(*sim, 30.5f + c.gap, 30.5f);
        sim->submit(SetFactionCommand{enemy, 2});
        sim->submit(DraftCommand{fighter, true});
        sim->step();
        for (int s = 0; s < 200; ++s) {
            sim->step();
            weapHeal(*sim, fighter);
            weapHeal(*sim, enemy);
        }
        MESSAGE("gap " << c.gap);
        CHECK(weapHandsDef(*sim, fighter) == sim->defs().idOf("Things", c.expected));
    }
}

TEST_CASE("Weapons: a fighter with a rifle and a knife swings at an enemy that comes close, and goes back to the rifle") {
    auto sim = weapSim(32);
    const Entity fighter = weapArmed(*sim, 30.5f, 30.5f, {"Gun_BoltActionRifle", "Melee_Knife"});
    const Entity enemy = weapPawn(*sim, 31.7f, 30.5f);
    sim->submit(SetFactionCommand{enemy, 2});
    sim->submit(DraftCommand{fighter, true});
    sim->step();
    int swings = 0;
    for (int s = 0; s < 300; ++s) {
        sim->step();
        weapHeal(*sim, fighter);
        weapHeal(*sim, enemy);
        for (const Event& e : sim->events()) {
            swings += e.type == EventType::MeleeSwing && e.thing == fighter ? 1 : 0;
        }
    }
    CHECK(weapHandsDef(*sim, fighter) == sim->defs().idOf("Things", "Melee_Knife"));
    CHECK(swings >= 2);

    // The enemy backs off to 25 m: out of knife range, the fighter takes the rifle up again.
    Position& pos = sim->things().registry().get<Position>(enemy);
    pos.x = 55.5f;
    for (int s = 0; s < 300; ++s) {
        sim->step();
        weapHeal(*sim, fighter);
        weapHeal(*sim, enemy);
    }
    CHECK(weapHandsDef(*sim, fighter) == sim->defs().idOf("Things", "Gun_BoltActionRifle"));
}

TEST_CASE("Weapons: a fighter doesn't flip between two weapons for a target on the edge of a band") {
    auto sim = weapSim(33);
    const Entity fighter = weapArmed(*sim, 30.5f, 30.5f, {"Gun_BoltActionRifle", "Gun_Revolver"});
    const Entity enemy = weapPawn(*sim, 30.5f + 9.5f, 30.5f);  // just beyond the sidearm band
    sim->submit(SetFactionCommand{enemy, 2});
    sim->submit(DraftCommand{fighter, true});
    sim->step();
    int switches = 0;
    for (int s = 0; s < 400; ++s) {
        sim->step();
        weapHeal(*sim, fighter);
        weapHeal(*sim, enemy);
        switches += weapCount(*sim, EventType::WeaponSwitched);
    }
    CHECK(switches <= 1);
}

TEST_CASE("Weapons: carried weapons and a switch in progress survive a save and load, and replay") {
    auto sim = weapSim();
    const Entity pawn = weapArmed(*sim, 30.5f, 30.5f, {"Gun_BoltActionRifle", "Gun_Revolver", "Melee_Knife"});
    sim->step();
    sim->submit(SwitchWeaponCommand{pawn, WeaponSlot::Melee});
    sim->stepMany(5);  // mid-switch
    WEAP_REQUIRE(sim->combat().switching(pawn));

    const std::string json = saveToJson(*sim);
    CHECK(json.find("\"weapons\"") != std::string::npos);
    SaveLoadResult loaded = loadSim(saveSim(*sim), weapDefs());
    WEAP_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->stateHash() == sim->stateHash());
    CHECK(loaded.sim->combat().switching(pawn));
    CHECK(loaded.sim->combat().switchStepsLeft(pawn) == sim->combat().switchStepsLeft(pawn));
    CHECK(loaded.sim->things().registry().get<Gear>(pawn).stowed.size() == 2);
    sim->stepMany(100);
    loaded.sim->stepMany(100);
    CHECK(loaded.sim->stateHash() == sim->stateHash());
    CHECK(weapHandsDef(*loaded.sim, pawn) == sim->defs().idOf("Things", "Melee_Knife"));

    const Replay recorded = recordReplay(*sim);
    const std::string text = writeReplay(recorded);
    CHECK(text.find("\"SwitchWeapon\"") != std::string::npos);
    Replay reread;
    std::string error;
    WEAP_REQUIRE(readReplay(text, reread, error));
    CHECK(writeReplay(reread) == text);
}

TEST_CASE("Weapons: a carried weapon and a switch change the state hash") {
    auto a = weapSim();
    auto b = weapSim();
    const Entity pa = weapArmed(*a, 30.5f, 30.5f, {"Gun_BoltActionRifle", "Gun_Revolver"});
    weapArmed(*b, 30.5f, 30.5f, {"Gun_BoltActionRifle"});
    a->step();
    b->step();
    CHECK(a->stateHash() != b->stateHash());
    auto c = weapSim();
    const Entity pc = weapArmed(*c, 30.5f, 30.5f, {"Gun_BoltActionRifle", "Gun_Revolver"});
    c->step();
    CHECK(a->stateHash() == c->stateHash());
    c->submit(SwitchWeaponCommand{pc, WeaponSlot::Sidearm});
    a->step();
    c->step();
    CHECK(a->stateHash() != c->stateHash());
    (void)pa;
}
