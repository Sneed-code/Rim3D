#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include "TestDefs.h"

#include <doctest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace colony;

namespace {

// A flat 32 m map with test defs and no hauling test.
Sim bridgeTestSim() { return Sim(SimSettings{3, 32, 0, 0, true}, testDefs()); }

int countEvents(const Sim& sim, EventType type) {
    return static_cast<int>(std::count_if(sim.events().begin(), sim.events().end(),
                                          [type](const Event& e) { return e.type == type; }));
}

}  // namespace

TEST_CASE("Commands: applied at the start of the next step, in submit order, and logged") {
    Sim sim = bridgeTestSim();
    CHECK(sim.submit(SpawnThingCommand{"Wall", CellPos{4, 4, 0}}) == 1);
    CHECK(sim.submit(SpawnThingCommand{"Steel", CellPos{5, 4, 0}, 20}) == 2);
    CHECK(sim.things().count() == 0);  // nothing happens until the step
    CHECK(sim.commandLog().empty());

    sim.step();
    CHECK(sim.things().count() == 2);
    CHECK(sim.map().buildingAt(CellPos{4, 4, 0}) != kNoEntity);
    CHECK_FALSE(sim.map().walkable(CellPos{4, 4, 0}));
    if (!(sim.commandLog().size() == 2)) {
        CHECK(false);
        return;
    }
    CHECK(sim.commandLog()[0].step == 0);
    CHECK(std::holds_alternative<SpawnThingCommand>(sim.commandLog()[0].command));
    CHECK(std::get<SpawnThingCommand>(sim.commandLog()[0].command).def == "Wall");
    CHECK(std::get<SpawnThingCommand>(sim.commandLog()[1].command).def == "Steel");
    CHECK(commandName(sim.commandLog()[1].command) == std::string("SpawnThing"));

    sim.stepMany(5);
    CHECK(sim.submit(DestroyThingCommand{sim.map().buildingAt(CellPos{4, 4, 0})}) == 3);
    sim.step();
    CHECK(sim.commandLog().size() == 3);
    CHECK(sim.commandLog()[2].step == 6);
    CHECK(sim.map().buildingAt(CellPos{4, 4, 0}) == kNoEntity);
    CHECK(sim.map().walkable(CellPos{4, 4, 0}));
    CHECK(sim.things().count() == 1);
}

TEST_CASE("Commands: a command that can't be applied reports why and is still logged") {
    Sim sim = bridgeTestSim();
    sim.submit(SpawnThingCommand{"Wall", CellPos{4, 4, 0}});
    const uint64_t unknownDef = sim.submit(SpawnThingCommand{"NoSuchThing", CellPos{1, 1, 0}});
    const uint64_t occupied = sim.submit(SpawnThingCommand{"Wall", CellPos{4, 4, 0}});
    const uint64_t offMap = sim.submit(SpawnThingCommand{"Steel", CellPos{40, 1, 0}});
    sim.step();
    CHECK(sim.things().count() == 1);
    CHECK(sim.commandLog().size() == 4);
    std::vector<uint64_t> failed;
    for (const Event& e : sim.events()) {
        if (e.type == EventType::CommandFailed) {
            failed.push_back(e.command);
            CHECK_FALSE(e.message.empty());
        }
    }
    CHECK(failed == std::vector<uint64_t>{unknownDef, occupied, offMap});

    const Entity wall = sim.map().buildingAt(CellPos{4, 4, 0});
    sim.submit(DestroyThingCommand{wall});
    const uint64_t twice = sim.submit(DestroyThingCommand{wall});
    const uint64_t nothing = sim.submit(DestroyThingCommand{kNoEntity});
    sim.step();
    failed.clear();
    for (const Event& e : sim.events()) {
        if (e.type == EventType::CommandFailed) {
            failed.push_back(e.command);
        }
    }
    CHECK(failed == std::vector<uint64_t>{twice, nothing});
}

TEST_CASE("Commands: things the hauling test is using can't be destroyed") {
    Sim sim(SimSettings{11, 64, 1, 1, true}, testDefs());
    if (!(sim.haulers().size() == 1)) {
        CHECK(false);
        return;
    }
    sim.submit(DestroyThingCommand{sim.haulers()[0]});
    sim.step();
    CHECK(countEvents(sim, EventType::CommandFailed) == 1);
    CHECK(sim.things().exists(sim.haulers()[0]));
    sim.stepMany(3600);
    CHECK(sim.itemsLeft() == 0);
}

TEST_CASE("Events: spawn, despawn and destroy are reported, and cleared each step") {
    Sim sim(SimSettings{11, 64, 1, 1, true}, testDefs());
    // Setting up the map reports its spawns until the first step.
    CHECK(countEvents(sim, EventType::ThingSpawned) == 2);
    for (const Event& e : sim.events()) {
        CHECK(sim.things().exists(e.thing));
        CHECK(e.def.valid());
    }

    // Hauling: the item is picked up (despawned) and put down (spawned) at the stockpile.
    // The hauler's hands are reported too (PawnAction), in the same step as the despawn and spawn.
    int pickedUp = 0;
    int putDown = 0;
    int pickUpActions = 0;
    int putDownActions = 0;
    for (int i = 0; i < 3600 && sim.itemsLeft() > 0; ++i) {
        sim.step();
        pickedUp += countEvents(sim, EventType::ThingDespawned);
        Entity moved = kNoEntity;
        for (const Event& e : sim.events()) {
            if (e.type == EventType::ThingSpawned || e.type == EventType::ThingDespawned) {
                moved = e.thing;
            }
            if (e.type == EventType::ThingSpawned) {
                ++putDown;
                CHECK(e.cell == *sim.stockpileCell());
            }
            if (e.type == EventType::PawnAction) {
                CHECK(e.thing == sim.haulers()[0]);
                CHECK(e.def.valid());
                CHECK(e.target == moved);
                ++(e.action == PawnAction::PickUp ? pickUpActions : putDownActions);
            }
        }
    }
    CHECK(pickedUp == 1);
    CHECK(putDown == 1);
    CHECK(pickUpActions == 1);
    CHECK(putDownActions == 1);
    CHECK(std::string(pawnActionName(PawnAction::PutDown)) == "PutDown");
    CHECK(std::string(eventTypeName(EventType::PawnAction)) == "PawnAction");
    sim.step();
    CHECK(sim.events().empty());

    sim.submit(SpawnThingCommand{"Wall", CellPos{2, 2, 0}});
    sim.step();
    if (!(sim.events().size() == 1)) {
        CHECK(false);
        return;
    }
    const Event spawned = sim.events()[0];
    CHECK(spawned.type == EventType::ThingSpawned);
    CHECK(spawned.cell == CellPos{2, 2, 0});
    CHECK(sim.defs().get(spawned.def).defName() == "Wall");

    sim.submit(DestroyThingCommand{spawned.thing});
    sim.step();
    if (!(sim.events().size() == 2)) {
        CHECK(false);
        return;
    }
    CHECK(sim.events()[0].type == EventType::ThingDespawned);
    CHECK(sim.events()[1].type == EventType::ThingDestroyed);
    CHECK(sim.events()[1].thing == spawned.thing);
    CHECK(eventTypeName(sim.events()[1].type) == std::string("ThingDestroyed"));
}

TEST_CASE("Snapshot: every thing on the map, sorted by id, with what Unreal draws") {
    Sim sim(SimSettings{11, 64, 2, 3, true}, testDefs());
    sim.submit(SpawnThingCommand{"FueledStove", CellPos{10, 20, 0}, 1, Rot4::East});
    sim.submit(SpawnThingCommand{"Steel", CellPos{30, 30, 0}, 50});
    sim.step();

    Snapshot snap;
    sim.writeSnapshot(snap);
    CHECK(snap.gameSeconds == 1);
    CHECK(snap.mapWidth == 64);
    CHECK(snap.mapHeight == 64);
    CHECK(snap.things.size() == 7);
    CHECK(std::is_sorted(snap.things.begin(), snap.things.end(),
                         [](const ThingView& a, const ThingView& b) { return a.id < b.id; }));

    const Entity stove = sim.map().buildingAt(CellPos{10, 20, 0});
    const ThingView* view = snap.find(stove);
    if (view == nullptr) {
        CHECK(false);
        return;
    }
    CHECK(view->category == ThingCategory::Building);
    CHECK(sim.defs().get(view->def).defName() == "FueledStove");
    CHECK(view->rotation == Rot4::East);
    CHECK(view->sizeX == 1);  // 3x1 turned East
    CHECK(view->sizeY == 3);
    CHECK(view->x == doctest::Approx(10.5f));
    CHECK(view->y == doctest::Approx(21.5f));

    int pawns = 0;
    for (const ThingView& t : snap.things) {
        pawns += t.category == ThingCategory::Pawn ? 1 : 0;
        if (t.category == ThingCategory::Item && sim.defs().get(t.def).defName() == "Steel" && t.stackCount == 50) {
            CHECK(t.hitPoints == 100);
            CHECK(t.maxHitPoints == 100);
        }
    }
    CHECK(pawns == 2);
    CHECK(snap.find(kNoEntity) == nullptr);

    // A carried item is off the map, so not in the snapshot; writing again reuses the buffer.
    for (int i = 0; i < 3600; ++i) {
        sim.step();
        sim.writeSnapshot(snap);
        if (snap.things.size() < 7) {
            break;
        }
    }
    CHECK(snap.things.size() < 7);
}

TEST_CASE("State hash: same seed and commands, same hash; anything different, a different hash") {
    const auto defs = testDefs();
    const auto run = [&defs](bool extraWall, int wallStep) {
        Sim sim(SimSettings{21, 96, 5, 40}, defs);
        for (int i = 0; i < 4000; ++i) {
            if (i == 100) {
                sim.submit(SpawnThingCommand{"Steel", CellPos{40, 50, 0}, 10});
            }
            if (extraWall && i == wallStep) {
                sim.submit(SpawnThingCommand{"Wall", CellPos{41, 51, 0}});
            }
            sim.step();
        }
        return sim.stateHash();
    };
    const uint64_t a = run(false, 0);
    CHECK(a == run(false, 0));
    CHECK(a != run(true, 200));

    // A different seed changes it, and so does stepping on.
    Sim b(SimSettings{22, 96, 5, 40}, defs);
    Sim c(SimSettings{21, 96, 5, 40}, defs);
    CHECK(b.stateHash() != c.stateHash());
    const uint64_t before = c.stateHash();
    c.step();
    CHECK(c.stateHash() != before);
}

TEST_CASE("State hash: checkpoints every game hour by default") {
    Sim sim = bridgeTestSim();
    sim.stepMany(3 * kSecondsPerHour + 10);
    if (!(sim.checkpoints().size() == 3)) {
        CHECK(false);
        return;
    }
    CHECK(sim.checkpoints()[0].step == kSecondsPerHour);
    CHECK(sim.checkpoints()[2].step == 3 * kSecondsPerHour);

    SimSettings off{3, 32, 0, 0, true};
    off.checkpointEvery = 0;
    Sim quiet(off, testDefs());
    quiet.stepMany(2 * kSecondsPerHour);
    CHECK(quiet.checkpoints().empty());
}

TEST_CASE("Defs: the content hash changes with any def or tuning value") {
    const auto build = [](const char* things, const char* tuning) {
        DefDatabase db;
        std::vector<std::string> errors;
        DefDatabase::build({{"Things", "a.json", things}}, tuning, db, errors);
        return db.contentHash();
    };
    const char* base = R"([{"defName": "Steel", "category": "Item", "stackLimit": 75}])";
    CHECK(build(base, "") == build(base, ""));
    CHECK(build(base, "") != build(R"([{"defName": "Steel", "category": "Item", "stackLimit": 76}])", ""));
    CHECK(build(base, "") != build(base, R"({"values": {"foodRate": 1.5}})"));
    CHECK(DefDatabase().contentHash() == build("[]", ""));
    CHECK(testDefs()->contentHash() == testDefs()->contentHash());
}
