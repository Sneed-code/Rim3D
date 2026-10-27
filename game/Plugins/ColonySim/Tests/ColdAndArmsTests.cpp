// Tests for colonists picking up loose guns (docs/design/weapons.md "Picking guns up").
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Temperature.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

#define COLDARMS_REQUIRE(...)  \
    do {                       \
        CHECK((__VA_ARGS__));  \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> coldArmsDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return db;
    }();
    return defs;
}

std::unique_ptr<Sim> coldArmsSim(float outdoor) {
    SimSettings settings;
    settings.seed = 31;
    settings.mapSize = 64;
    settings.flatMap = true;
    auto sim = std::make_unique<Sim>(settings, coldArmsDefs());
    sim->setOutdoorOverride(outdoor);
    sim->temperature().setSourceRunning([](Entity) { return true; });
    return sim;
}

Entity coldArmsColonist(Sim& sim, int x, int y) {
    const Entity pawn = sim.things().spawn("Human", CellPos{x, y, 0});
    sim.jobs().addWorker(pawn);
    sim.things().registry().emplace<Colonist>(pawn, Colonist{});
    return pawn;
}


}  // namespace

TEST_CASE("Weapons: a colonist picks up a loose weapon for a slot it has nothing in, once, and two do not fight over one") {
    auto sim = coldArmsSim(15.0f);
    const Entity first = coldArmsColonist(*sim, 20, 20);
    const Entity second = coldArmsColonist(*sim, 22, 20);
    const Entity revolver = sim->things().spawn("Gun_Revolver", CellPos{21, 24, 0});
    COLDARMS_REQUIRE(revolver != kNoEntity);
    sim->stepMany(900);
    const Entity a = sim->combat().weaponIn(first, WeaponSlot::Sidearm);
    const Entity b = sim->combat().weaponIn(second, WeaponSlot::Sidearm);
    CHECK(((a == revolver) != (b == revolver)));  // exactly one of them has it
    // The other colonist takes the second one; a third is left lying, both having a sidearm.
    const Entity second2 = sim->things().spawn("Gun_Revolver", CellPos{21, 26, 0});
    sim->stepMany(900);
    CHECK_FALSE(sim->things().spawned(second2));
    CHECK(sim->combat().weaponIn(first, WeaponSlot::Sidearm) != kNoEntity);
    CHECK(sim->combat().weaponIn(second, WeaponSlot::Sidearm) != kNoEntity);
    const Entity spare = sim->things().spawn("Gun_Revolver", CellPos{21, 27, 0});
    sim->stepMany(900);
    CHECK(sim->things().spawned(spare));
    // Another gun slot is taken (a shotgun for the primary slot) but melee weapons are left lying.
    const Entity shotgun = sim->things().spawn("Gun_PumpShotgun", CellPos{21, 28, 0});
    const Entity knife = sim->things().spawn("Melee_Knife", CellPos{21, 29, 0});
    sim->stepMany(2500);  // a colonist looks for clothes and weapons every 1,200 steps when it finds none
    CHECK_FALSE(sim->things().spawned(shotgun));
    CHECK(sim->things().spawned(knife));
}
