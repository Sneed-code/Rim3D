#include "ColonySim/Bills.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Fuel.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Lights.h"
#include "ColonySim/Power.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define FUEL_REQUIRE(...)                  \
    do {                                   \
        const bool fuelOk = (__VA_ARGS__); \
        CHECK(fuelOk);                     \
        if (!fuelOk) {                     \
            return;                        \
        }                                  \
    } while (0)

namespace {

// The game's real data: the stove, campfire and torch lamp fuel numbers are part of what is tested.
std::shared_ptr<const DefDatabase> fuelDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

Sim fuelSim() { return Sim(SimSettings{5, 48, 0, 0, true}, fuelDefs()); }

constexpr CellPos kFuelStoveCell{20, 20, 0};

Entity fuelSpawn(Sim& sim, const std::string& def, CellPos cell, int count = 1) {
    sim.submit(SpawnThingCommand{def, cell, count});
    sim.step();
    for (const Event& e : sim.events()) {
        if (e.type == EventType::ThingSpawned && e.cell == cell && sim.defs().get(e.def).defName() == def) {
            return e.thing;
        }
    }
    return kNoEntity;
}

// A colonist with Cooking 8.
Entity fuelWorker(Sim& sim, CellPos cell) {
    const Entity pawn = fuelSpawn(sim, "Human", cell);
    Colonist colonist;
    colonist.skills.push_back(SkillRecord{"Cooking", 8, Passion::None, 0.0f, false});
    sim.things().registry().emplace<Colonist>(pawn, std::move(colonist));
    return pawn;
}

float fuelOf(const Sim& sim, Entity building) {
    const Fuel* fuel = sim.things().registry().try_get<Fuel>(building);
    return fuel != nullptr ? fuel->amount : -1.0f;
}

int fuelItems(const Sim& sim, const std::string& def) {
    const DefId id = sim.defs().idOf("Things", def);
    int total = 0;
    sim.things().registry().view<const Thing, const StackCount>().each(
        [&](const Thing& thing, const StackCount& stack) { total += thing.def == id ? stack.count : 0; });
    return total;
}

int fuelEventCount(const Sim& sim, EventType type) {
    int n = 0;
    for (const Event& e : sim.events()) {
        n += e.type == type ? 1 : 0;
    }
    return n;
}

// Steps until `done` says so, at most `limit` steps; returns the steps it took, or -1.
template <typename F>
int fuelRunUntil(Sim& sim, int limit, F done) {
    for (int i = 0; i < limit; ++i) {
        if (done()) {
            return i;
        }
        sim.step();
    }
    return done() ? limit : -1;
}

}  // namespace

TEST_CASE("Fuel: the defs give the stove, campfire and torch lamp their fuel, and new buildings start empty") {
    Sim sim = fuelSim();
    const FuelSystem& fuel = sim.jobs().fuel();
    const DefId wood = sim.defs().idOf("Things", "WoodLog");

    const FuelSpec& stove = fuel.spec(sim.defs().idOf("Things", "FueledStove"));
    CHECK(stove.present);
    CHECK(stove.item == wood);
    CHECK(stove.capacity == 50.0f);
    CHECK(stove.perUse == 1.0f);
    CHECK(stove.perSecond == 0.0f);

    const FuelSpec& campfire = fuel.spec(sim.defs().idOf("Things", "Campfire"));
    CHECK(campfire.present);
    CHECK(campfire.capacity == 500.0f);
    CHECK(campfire.perSecond * 86400.0f == doctest::Approx(12.0f));  // a wood every 2 hours

    const FuelSpec& torch = fuel.spec(sim.defs().idOf("Things", "TorchLamp"));
    CHECK(torch.perSecond * 86400.0f == doctest::Approx(7.5f));
    CHECK_FALSE(fuel.spec(sim.defs().idOf("Things", "Wall")).present);

    const Entity newStove = fuelSpawn(sim, "FueledStove", kFuelStoveCell);
    const Entity wall = fuelSpawn(sim, "Wall", CellPos{30, 30, 0});
    FUEL_REQUIRE(newStove != kNoEntity && wall != kNoEntity);
    CHECK(fuelOf(sim, newStove) == 0.0f);
    CHECK(fuelOf(sim, wall) == -1.0f);
    CHECK_FALSE(fuel.lit(newStove));
    CHECK(fuel.lit(wall));  // nothing to burn, nothing to run out of
    CHECK(fuel.fraction(wall) == -1.0f);
}

TEST_CASE("Fuel: a campfire burns its wood over time, tells when it goes out and then stays dark") {
    Sim sim = fuelSim();
    const Entity fire = fuelSpawn(sim, "Campfire", CellPos{20, 20, 0});
    FUEL_REQUIRE(fire != kNoEntity);
    sim.things().registry().get<Fuel>(fire).amount = 3.0f;
    std::string error;
    sim.jobs().fuel().setRefuel(fire, false, false, error);  // nobody tops it up

    sim.stepMany(2 * 3600);  // two hours: one wood
    CHECK(fuelOf(sim, fire) == doctest::Approx(2.0f).epsilon(0.001));
    CHECK(sim.jobs().fuel().lit(fire));

    int outEvents = 0;
    int stepsToOut = fuelRunUntil(sim, 6 * 3600, [&] {
        outEvents += fuelEventCount(sim, EventType::OutOfFuel);
        return outEvents > 0;
    });
    FUEL_REQUIRE(stepsToOut >= 0);
    CHECK(stepsToOut == doctest::Approx(4 * 3600).epsilon(0.01));
    CHECK(fuelOf(sim, fire) == 0.0f);
    CHECK_FALSE(sim.jobs().fuel().lit(fire));
    sim.stepMany(100);
    CHECK(fuelEventCount(sim, EventType::OutOfFuel) == 0);  // said once
    CHECK(fuelOf(sim, fire) == 0.0f);
}

TEST_CASE("Fuel: a torch lamp shines only while it has fuel") {
    Sim sim = fuelSim();
    const Entity torch = fuelSpawn(sim, "TorchLamp", CellPos{20, 20, 0});
    FUEL_REQUIRE(torch != kNoEntity);
    const CellPos beside{21, 20, 0};
    sim.step();
    CHECK(sim.lights().lampLightAt(beside) == 0.0f);  // new and empty: dark

    CHECK(sim.jobs().fuel().add(torch, 3) == 3);
    sim.step();
    CHECK(sim.lights().lampLightAt(beside) > 0.5f);

    sim.things().registry().get<Fuel>(torch).amount = 0.0001f;  // the last of it
    sim.stepMany(2);
    CHECK_FALSE(sim.jobs().fuel().lit(torch));
    CHECK(sim.lights().lampLightAt(beside) == 0.0f);
}

TEST_CASE("Fuel: a wood-fired generator makes power only with fuel and burns it only while switched on") {
    Sim sim = fuelSim();
    const Entity generator = fuelSpawn(sim, "WoodFiredGenerator", CellPos{10, 10, 0});
    fuelSpawn(sim, "PowerConduit", CellPos{12, 10, 0});
    fuelSpawn(sim, "PowerConduit", CellPos{13, 10, 0});
    const Entity lamp = fuelSpawn(sim, "StandingLamp", CellPos{14, 10, 0});
    FUEL_REQUIRE(generator != kNoEntity && lamp != kNoEntity);
    sim.step();
    CHECK(sim.power().wattsOf(generator) == 0.0);  // new and empty: nothing to burn
    CHECK_FALSE(sim.power().running(lamp));

    FuelSystem& fuel = sim.jobs().fuel();
    CHECK(fuel.add(generator, 10) == 10);
    sim.stepMany(2);
    CHECK(sim.power().wattsOf(generator) == doctest::Approx(1000.0));
    CHECK(sim.power().running(lamp));
    sim.stepMany(3600 - 2);  // an hour: 63 a day is 2.625 wood
    CHECK(fuelOf(sim, generator) == doctest::Approx(10.0f - 63.0f / 24.0f).epsilon(0.001));
    sim.things().registry().get<Fuel>(generator).amount = 10.0f;

    // Switched off, it burns nothing and makes nothing.
    sim.submit(FlickCommand{generator, false});
    sim.stepMany(3600);
    CHECK(sim.power().wattsOf(generator) == 0.0);
    CHECK(fuelOf(sim, generator) == 10.0f);
    sim.submit(FlickCommand{generator, true});
    sim.stepMany(3600);
    CHECK(fuelOf(sim, generator) < 10.0f);

    // It runs dry: 0.0001 of a wood is gone in a step, the power with it.
    sim.things().registry().get<Fuel>(generator).amount = 0.0001f;
    int outEvents = 0;
    for (int i = 0; i < 3; ++i) {
        sim.step();
        outEvents += fuelEventCount(sim, EventType::OutOfFuel);
    }
    CHECK(outEvents == 1);
    CHECK(sim.power().wattsOf(generator) == 0.0);
}

TEST_CASE("Fuel: a stove with no wood is stopped, and a colonist brings wood and the cooking starts") {
    Sim sim = fuelSim();
    const Entity stove = fuelSpawn(sim, "FueledStove", kFuelStoveCell);
    const Entity cook = fuelWorker(sim, CellPos{10, 10, 0});
    fuelSpawn(sim, "RawBerries", CellPos{12, 12, 0}, 25);
    FUEL_REQUIRE(stove != kNoEntity && cook != kNoEntity);
    sim.submit(AddBillCommand{stove, "CookMealSimple", BillMode::Count, 2});
    sim.stepMany(600);
    // Nothing to burn: no meal, and the berries stay where they lie.
    CHECK(fuelItems(sim, "MealSimple") == 0);
    CHECK(fuelItems(sim, "RawBerries") == 25);

    fuelSpawn(sim, "WoodLog", CellPos{14, 14, 0}, 20);
    bool sawRefuelAction = false;
    int refueled = 0;
    FUEL_REQUIRE(fuelRunUntil(sim, 4000, [&] {
                     for (const Event& e : sim.events()) {
                         sawRefuelAction = sawRefuelAction || (e.type == EventType::PawnAction && e.action == PawnAction::Refuel &&
                                                               e.thing == cook && e.target == stove);
                         refueled += e.type == EventType::Refueled ? static_cast<int>(e.amount) : 0;
                     }
                     return fuelItems(sim, "MealSimple") == 2;
                 }) >= 0);
    CHECK(sawRefuelAction);
    CHECK(refueled == 20);
    CHECK(fuelOf(sim, stove) == doctest::Approx(18.0f));  // 20 wood in, one burnt per meal
    CHECK(fuelItems(sim, "WoodLog") == 0);                // it was all carried in
}

TEST_CASE("Fuel: auto refuel waits until the fuel is low, can be turned off, and a fill order tops it up") {
    Sim sim = fuelSim();
    const Entity stove = fuelSpawn(sim, "FueledStove", kFuelStoveCell);
    const Entity wall = fuelSpawn(sim, "Wall", CellPos{30, 30, 0});
    fuelWorker(sim, CellPos{10, 10, 0});
    fuelSpawn(sim, "WoodLog", CellPos{14, 14, 0}, 75);
    fuelSpawn(sim, "WoodLog", CellPos{14, 16, 0}, 75);
    FUEL_REQUIRE(stove != kNoEntity);
    FuelSystem& fuel = sim.jobs().fuel();

    // Refused on a thing that burns nothing.
    sim.submit(SetRefuelCommand{wall, true, true});
    sim.step();
    CHECK(fuelEventCount(sim, EventType::CommandFailed) == 1);

    // Off: it stays empty.
    sim.submit(SetRefuelCommand{stove, false, false});
    sim.stepMany(1500);
    CHECK(fuelOf(sim, stove) == 0.0f);
    CHECK_FALSE(fuel.wantsFuel(stove));

    // A fill order works even with auto off, and takes it to the top: 50 of the first stack of 75.
    sim.submit(SetRefuelCommand{stove, false, true});
    sim.step();
    CHECK(fuel.wantsFuel(stove));
    FUEL_REQUIRE(fuelRunUntil(sim, 3000, [&] { return fuelOf(sim, stove) >= 50.0f; }) >= 0);
    CHECK(fuelOf(sim, stove) == 50.0f);
    CHECK_FALSE(sim.things().registry().get<Fuel>(stove).fillOrdered);  // done
    CHECK_FALSE(fuel.wantsFuel(stove));
    CHECK(fuelItems(sim, "WoodLog") == 100);
    sim.stepMany(1500);
    CHECK(fuelItems(sim, "WoodLog") == 100);  // full: nobody takes more

    // On again: used down to 15 it is refuelled, at 16 it is not.
    sim.submit(SetRefuelCommand{stove, true, false});
    sim.things().registry().get<Fuel>(stove).amount = 16.0f;
    sim.stepMany(1000);
    CHECK(fuelOf(sim, stove) == 16.0f);
    sim.things().registry().get<Fuel>(stove).amount = 15.0f;
    FUEL_REQUIRE(fuelRunUntil(sim, 3000, [&] { return fuelOf(sim, stove) > 15.0f; }) >= 0);
    CHECK(fuelOf(sim, stove) == 50.0f);  // all that fits: 35 more
}

TEST_CASE("Fuel: a run needs one wood a meal, so a four-meal run waits for four") {
    Sim sim = fuelSim();
    const Entity stove = fuelSpawn(sim, "FueledStove", kFuelStoveCell);
    const Entity cook = fuelWorker(sim, CellPos{10, 10, 0});
    fuelSpawn(sim, "RawBerries", CellPos{12, 12, 0}, 80);
    FUEL_REQUIRE(stove != kNoEntity && cook != kNoEntity);
    sim.submit(SetRefuelCommand{stove, false, false});
    sim.things().registry().get<Fuel>(stove).amount = 3.0f;
    sim.submit(AddBillCommand{stove, "CookMealSimpleBulk", BillMode::Count, 1});
    sim.stepMany(1500);
    CHECK(fuelItems(sim, "MealSimple") == 0);  // 3 wood, 4 meals
    CHECK(fuelOf(sim, stove) == 3.0f);

    sim.things().registry().get<Fuel>(stove).amount = 4.0f;
    FUEL_REQUIRE(fuelRunUntil(sim, 3000, [&] { return fuelItems(sim, "MealSimple") == 4; }) >= 0);
    CHECK(fuelOf(sim, stove) == 0.0f);
    CHECK(fuelItems(sim, "RawBerries") <= 80 - 40);
}

TEST_CASE("Fuel: a stove that runs dry mid-run puts the berries back") {
    Sim sim = fuelSim();
    const Entity stove = fuelSpawn(sim, "FueledStove", kFuelStoveCell);
    const Entity cook = fuelWorker(sim, CellPos{10, 10, 0});
    fuelSpawn(sim, "RawBerries", CellPos{12, 12, 0}, 25);
    FUEL_REQUIRE(stove != kNoEntity && cook != kNoEntity);
    sim.submit(SetRefuelCommand{stove, false, false});
    sim.things().registry().get<Fuel>(stove).amount = 1.0f;
    sim.submit(AddBillCommand{stove, "CookMealSimple", BillMode::Count, 1});
    // The cook gets as far as working at the stove ...
    FUEL_REQUIRE(fuelRunUntil(sim, 2000, [&] {
                     const Worker* worker = sim.things().registry().try_get<Worker>(cook);
                     return worker != nullptr && worker->job.type == JobType::DoBill && worker->job.step >= 4;
                 }) >= 0);
    // ... and the fuel is taken away (a fire, a thief, a later system).
    sim.things().registry().get<Fuel>(stove).amount = 0.0f;
    sim.stepMany(200);
    CHECK(fuelItems(sim, "MealSimple") == 0);
    CHECK(fuelItems(sim, "RawBerries") == 25);  // the ten in the job came back
}

TEST_CASE("Fuel: the snapshot shows fuel, the same commands give the same game and a save in the middle loads back the same") {
    const auto session = [](Sim& sim, int from, int to) {
        for (int i = from; i < to; ++i) {
            if (i == 0) {
                sim.submit(SpawnThingCommand{"Human", CellPos{10, 10, 0}});
                sim.submit(SpawnThingCommand{"Human", CellPos{10, 12, 0}});
                sim.submit(SpawnThingCommand{"FueledStove", kFuelStoveCell});
                sim.submit(SpawnThingCommand{"Campfire", CellPos{30, 30, 0}});
                sim.submit(SpawnThingCommand{"TorchLamp", CellPos{32, 30, 0}});
                sim.submit(SpawnThingCommand{"RawBerries", CellPos{12, 12, 0}, 60});
                sim.submit(SpawnThingCommand{"WoodLog", CellPos{14, 14, 0}, 75});
                sim.submit(SpawnThingCommand{"WoodLog", CellPos{14, 16, 0}, 75});
            }
            if (i == 5) {
                const Entity stove = sim.map().buildingAt(kFuelStoveCell);
                sim.submit(AddBillCommand{stove, "CookMealSimple", BillMode::Count, 4});
                sim.submit(SetRefuelCommand{sim.map().buildingAt(CellPos{32, 30, 0}), false, true});
                sim.submit(SetRefuelCommand{sim.map().buildingAt(CellPos{30, 30, 0}), false, false});  // keep the wood
            }
            sim.step();
        }
    };
    SimSettings settings{9, 64, 0, 0, true};
    settings.checkpointEvery = 300;

    Sim straight(settings, fuelDefs());
    session(straight, 0, 5000);
    Sim again(settings, fuelDefs());
    session(again, 0, 5000);
    CHECK(straight.stateHash() == again.stateHash());

    Snapshot snap;
    straight.writeSnapshot(snap);
    const ThingView* stoveView = snap.find(straight.map().buildingAt(kFuelStoveCell));
    const ThingView* torchView = snap.find(straight.map().buildingAt(CellPos{32, 30, 0}));
    FUEL_REQUIRE(stoveView != nullptr && torchView != nullptr);
    CHECK(stoveView->fuelCapacity == 50.0f);
    CHECK(stoveView->fuel > 0.0f);
    CHECK(stoveView->autoRefuel);
    CHECK(torchView->fuelCapacity == 75.0f);
    CHECK(torchView->fuel > 20.0f);  // filled on the order (what wood was near), burning slowly
    CHECK_FALSE(torchView->autoRefuel);

    // Save while a colonist is part way through putting wood in.
    Sim original(settings, fuelDefs());
    int saveAt = 0;
    bool midRefuel = false;
    while (!midRefuel && saveAt < 4000) {
        saveAt += 5;
        session(original, saveAt - 5, saveAt);
        original.things().registry().view<const Worker>().each([&](const Worker& worker) {
            midRefuel = midRefuel || (worker.job.type == JobType::Refuel && worker.job.step == 3 && worker.job.progress > 50.0f);
        });
    }
    FUEL_REQUIRE(midRefuel);
    const std::vector<uint8_t> bytes = saveSim(original);
    SaveLoadResult loaded = loadSim(bytes, fuelDefs());
    INFO(loaded.error);
    FUEL_REQUIRE(loaded.error.empty());
    FUEL_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->stateHash() == original.stateHash());
    session(original, saveAt, 5000);
    session(*loaded.sim, saveAt, 5000);
    CHECK(loaded.sim->stateHash() == original.stateHash());
    CHECK(loaded.sim->stateHash() == straight.stateHash());
}

TEST_CASE("Fuel: a stove saved before fuel existed loads full") {
    // The colony-cook file was made before C11: its stove cooked for free. It keeps cooking.
    std::ifstream file(std::string(COLONY_TEST_SAVES_DIR) + "/colony-cook-format19.save", std::ios::binary);
    FUEL_REQUIRE(file.good());
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    SaveLoadResult old = loadSim(bytes, fuelDefs());
    FUEL_REQUIRE(old.sim != nullptr);
    int stoves = 0;
    const DefId stoveDef = old.sim->defs().idOf("Things", "FueledStove");
    for (const auto [entity, thing, fuel] : old.sim->things().registry().view<const Thing, const Fuel>().each()) {
        if (thing.def == stoveDef) {
            ++stoves;
            CHECK(fuel.amount == 50.0f);
        }
    }
    CHECK(stoves >= 1);
}
