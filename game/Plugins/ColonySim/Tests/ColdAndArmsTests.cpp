// Tests for the year run's survival fixes: colonists go to a warm room when the cold gets to them, and pick up loose weapons
// (docs/design/temperature.md "Seeking warmth", docs/design/weapons.md "Picking weapons up").
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

// A walled room 10..16 square with a door in the south wall, a campfire inside; warmed for a day.
void coldArmsWarmRoom(Sim& sim, bool fire) {
    const CellPos door{13, 16, 0};
    for (int y = 10; y <= 16; ++y) {
        for (int x = 10; x <= 16; ++x) {
            if ((x == 10 || x == 16 || y == 10 || y == 16) && !(x == door.x && y == door.y)) {
                sim.things().spawn("Wall", CellPos{x, y, 0});
            }
        }
    }
    sim.things().spawn("Door", door);
    sim.rooms().update();
    if (fire) {
        sim.things().spawn("Campfire", CellPos{12, 12, 0});
    }
    sim.stepMany(24 * 3600);
}

JobType coldArmsJob(Sim& sim, Entity pawn) { return sim.things().registry().get<Worker>(pawn).job.type; }

}  // namespace

TEST_CASE("Cold: a colonist with hypothermia setting in walks into the warm room and stays until it has recovered") {
    auto sim = coldArmsSim(-20.0f);
    coldArmsWarmRoom(*sim, true);
    COLDARMS_REQUIRE(sim->temperature().temperatureAt(CellPos{13, 13, 0}) > 20.0f);
    const Entity pawn = coldArmsColonist(*sim, 24, 24);
    const DefId hypothermia = sim->defs().idOf("Hediffs", "Hypothermia");
    COLDARMS_REQUIRE(sim->health().setEnvironmentSeverity(pawn, hypothermia, 0.3f));
    bool sought = false;
    for (int i = 0; i < 1200 && !sought; ++i) {
        sim->step();
        // Out in the frost it first seeks shelter (SeekShelter, outside its safe range); either job takes it to the warm room.
        sought = coldArmsJob(*sim, pawn) == JobType::SeekWarmth || coldArmsJob(*sim, pawn) == JobType::SeekShelter;
    }
    CHECK(sought);
    sim->stepMany(900);
    const Room* room = sim->rooms().roomOf(cellOf(sim->things().registry().get<Position>(pawn)));
    COLDARMS_REQUIRE(room != nullptr);
    CHECK(room->enclosed());
    CHECK(sim->temperature().ambientOf(pawn) > 16.0f);
    // It recovers (0.9 a day) and then goes about its business; in the end it is alive and its hypothermia gone.
    sim->stepMany(86400 / 2);
    CHECK(sim->health().environmentSeverity(pawn, hypothermia) < sim->temperature().numbers().warmEnoughSeverity + 0.001f);
    CHECK_FALSE(sim->health().incapacitated(pawn));
}

TEST_CASE("Cold: with no warm room, or a colonist that is not cold enough, nothing is sought") {
    {
        auto sim = coldArmsSim(-20.0f);
        coldArmsWarmRoom(*sim, false);  // the room is as cold as outside
        const Entity pawn = coldArmsColonist(*sim, 24, 24);
        const DefId hypothermia = sim->defs().idOf("Hediffs", "Hypothermia");
        COLDARMS_REQUIRE(sim->health().setEnvironmentSeverity(pawn, hypothermia, 0.3f));
        for (int i = 0; i < 600; ++i) {
            sim->step();
            CHECK(coldArmsJob(*sim, pawn) != JobType::SeekWarmth);
        }
    }
    {
        auto sim = coldArmsSim(-20.0f);
        coldArmsWarmRoom(*sim, true);
        const Entity pawn = coldArmsColonist(*sim, 24, 24);
        for (int i = 0; i < 100; ++i) {  // the illness has not set in yet: only a little over a game minute outside
            sim->step();
            CHECK(coldArmsJob(*sim, pawn) != JobType::SeekWarmth);
        }
    }
}

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
