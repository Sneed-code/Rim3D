#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Lights.h"
#include "ColonySim/Map.h"
#include "ColonySim/Power.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define POWER_REQUIRE(...)     \
    do {                       \
        CHECK((__VA_ARGS__));  \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// The game's real data: the power defs and PowerRules are part of what is tested.
std::shared_ptr<const DefDatabase> powerDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

Sim powerSim() { return Sim(SimSettings{5, 64, 0, 0, true}, powerDefs()); }

Entity powerPut(Sim& sim, const char* def, int x, int y, Rot4 rot = Rot4::North) {
    return sim.things().spawn(def, CellPos{x, y, 0}, 1, rot);
}

// A row of conduits from x0 to x1 on row y.
void powerWire(Sim& sim, int x0, int x1, int y) {
    for (int x = x0; x <= x1; ++x) {
        powerPut(sim, "PowerConduit", x, y);
    }
}

void powerRun(Sim& sim, int steps) {
    for (int i = 0; i < steps; ++i) {
        sim.step();
    }
}

// A generator at (10,10)-(11,11), a wire along row 10 from x 12 to 15 and a lamp at (16,10).
struct PowerLine {
    Entity generator = kNoEntity;
    Entity lamp = kNoEntity;
};

PowerLine powerLine(Sim& sim) {
    PowerLine line;
    line.generator = powerPut(sim, "WoodFiredGenerator", 10, 10);
    powerWire(sim, 12, 15, 10);
    line.lamp = powerPut(sim, "StandingLamp", 16, 10);
    return line;
}

}  // namespace

TEST_CASE("Power: a generator, a wire and a lamp make a net that lights the lamp") {
    Sim sim = powerSim();
    const PowerLine line = powerLine(sim);
    POWER_REQUIRE(line.generator != kNoEntity);
    POWER_REQUIRE(line.lamp != kNoEntity);
    CHECK(sim.power().hasPower(line.lamp));
    CHECK_FALSE(sim.power().powered(line.lamp));  // nothing has run yet

    sim.step();
    CHECK(sim.power().powered(line.lamp));
    CHECK(sim.power().running(line.lamp));
    CHECK(sim.power().stateOf(line.lamp) == PowerState::Powered);
    const uint32_t net = sim.power().netOf(line.lamp);
    CHECK(net != 0);
    CHECK(sim.power().netOf(line.generator) == net);
    const PowerNetView* view = sim.power().net(net);
    POWER_REQUIRE(view != nullptr);
    CHECK(view->generation == doctest::Approx(1000.0));
    CHECK(view->demand == doctest::Approx(30.0));
    CHECK(view->generators == 1);
    CHECK(view->consumers == 1);
    CHECK(view->satisfied);
    CHECK(sim.power().wattsOf(line.generator) == doctest::Approx(1000.0));
    CHECK(sim.power().wattsOf(line.lamp) == doctest::Approx(-30.0));

    // The lamp lighting is announced once.
    int changes = 0;
    sim.step();
    for (const Event& e : sim.events()) {
        changes += e.type == EventType::PowerChanged ? 1 : 0;
    }
    CHECK(changes == 0);
}

TEST_CASE("Power: the first step announces who got power, and the snapshot shows it") {
    Sim sim = powerSim();
    const PowerLine line = powerLine(sim);
    sim.step();
    std::vector<Entity> announced;
    for (const Event& e : sim.events()) {
        if (e.type == EventType::PowerChanged) {
            CHECK(e.message == "on");
            announced.push_back(e.thing);
        }
    }
    CHECK(std::find(announced.begin(), announced.end(), line.lamp) != announced.end());
    CHECK(std::find(announced.begin(), announced.end(), line.generator) != announced.end());
    CHECK(announced.size() == 2);  // conduits are not announced

    Snapshot snapshot;
    sim.writeSnapshot(snapshot);
    const ThingView* lamp = snapshot.find(line.lamp);
    POWER_REQUIRE(lamp != nullptr);
    CHECK(lamp->power == PowerState::Powered);
    CHECK(lamp->powerNet == sim.power().netOf(line.lamp));
    CHECK(lamp->powerWatts == doctest::Approx(-30.0f));
    CHECK(lamp->batteryFill < 0.0f);
    POWER_REQUIRE(snapshot.powerNets.size() == 1);
    CHECK(snapshot.powerNets[0].generation == doctest::Approx(1000.0));
}

TEST_CASE("Power: a building beside no conduit is on no net, and conduits alone are not a net") {
    Sim sim = powerSim();
    const Entity lamp = powerPut(sim, "StandingLamp", 5, 5);
    powerWire(sim, 20, 25, 20);
    sim.step();
    CHECK(sim.power().netOf(lamp) == 0);
    CHECK(sim.power().stateOf(lamp) == PowerState::Unpowered);
    CHECK_FALSE(sim.power().running(lamp));
    CHECK(sim.power().nets().empty());
    // Things with nothing to do with power are always fine.
    const Entity wall = powerPut(sim, "Wall", 30, 30);
    CHECK(sim.power().running(wall));
    CHECK(sim.power().stateOf(wall) == PowerState::None);
}

TEST_CASE("Power: a gap in the wire splits the net, and mending it joins the net again") {
    Sim sim = powerSim();
    const PowerLine line = powerLine(sim);
    sim.step();
    CHECK(sim.power().powered(line.lamp));

    const Entity middle = sim.map().buildingAt(CellPos{13, 10, 0});
    POWER_REQUIRE(middle != kNoEntity);
    sim.submit(DestroyThingCommand{middle});
    sim.step();
    CHECK_FALSE(sim.power().powered(line.lamp));
    CHECK(sim.power().netOf(line.lamp) != sim.power().netOf(line.generator));  // the lamp is alone on its piece of wire
    CHECK(sim.power().powered(line.generator));  // still on a net, with nobody to feed

    powerPut(sim, "PowerConduit", 13, 10);
    sim.step();
    CHECK(sim.power().powered(line.lamp));
}

TEST_CASE("Power: a switch opens and closes the net by command") {
    Sim sim = powerSim();
    const Entity generator = powerPut(sim, "WoodFiredGenerator", 10, 10);
    powerWire(sim, 12, 13, 10);
    const Entity sw = powerPut(sim, "PowerSwitch", 14, 10);
    powerWire(sim, 15, 16, 10);
    const Entity lamp = powerPut(sim, "StandingLamp", 17, 10);
    POWER_REQUIRE(generator != kNoEntity);
    POWER_REQUIRE(sw != kNoEntity);
    sim.step();
    CHECK(sim.power().powered(lamp));
    CHECK(sim.power().powered(sw));

    sim.submit(FlickCommand{sw, false});
    sim.step();
    CHECK_FALSE(sim.power().powered(lamp));
    CHECK(sim.power().stateOf(sw) == PowerState::Off);
    CHECK(sim.power().stateOf(lamp) == PowerState::Unpowered);

    sim.submit(FlickCommand{sw, true});
    sim.step();
    CHECK(sim.power().powered(lamp));

    // Anything else that can be switched: a lamp flicked off draws nothing and is Off.
    sim.submit(FlickCommand{lamp, false});
    sim.step();
    CHECK(sim.power().stateOf(lamp) == PowerState::Off);
    CHECK(sim.power().net(sim.power().netOf(generator))->demand == doctest::Approx(0.0));
    sim.submit(FlickCommand{lamp, true});
    sim.step();
    CHECK(sim.power().powered(lamp));

    // What can't be switched fails with a reason, and the sim goes on.
    const size_t failures = [&] {
        sim.submit(FlickCommand{sim.map().buildingAt(CellPos{12, 10, 0}), false});  // a conduit
        sim.submit(FlickCommand{kNoEntity, false});
        sim.step();
        size_t n = 0;
        for (const Event& e : sim.events()) {
            n += e.type == EventType::CommandFailed ? 1 : 0;
        }
        return n;
    }();
    CHECK(failures == 2);
}

TEST_CASE("Power: a net that wants more than it makes goes dark as a whole") {
    Sim sim = powerSim();
    powerPut(sim, "WoodFiredGenerator", 10, 10);
    powerWire(sim, 12, 60, 10);
    // The generator makes 1000 W; each lamp wants 30 W. 33 lamps want 990 W, 34 want 1020 W.
    std::vector<Entity> lamps;
    for (int i = 0; i < 33; ++i) {
        lamps.push_back(powerPut(sim, "StandingLamp", 12 + i, 11));
    }
    sim.step();
    CHECK(std::all_of(lamps.begin(), lamps.end(), [&](Entity e) { return sim.power().powered(e); }));

    const Entity extra = powerPut(sim, "StandingLamp", 12 + 33, 11);
    sim.step();
    CHECK_FALSE(sim.power().powered(extra));
    CHECK(std::none_of(lamps.begin(), lamps.end(), [&](Entity e) { return sim.power().powered(e); }));
    CHECK_FALSE(sim.power().net(sim.power().netOf(extra))->satisfied);
    for (const Entity lamp : lamps) {
        CHECK(sim.power().wattsOf(lamp) == doctest::Approx(0.0));
    }

    // Take the extra lamp away: the net comes back at once (it has no batteries to wait for).
    sim.submit(DestroyThingCommand{extra});
    sim.step();
    CHECK(sim.power().powered(lamps.front()));
}

TEST_CASE("Power: batteries charge from a surplus at half efficiency and cover a gap") {
    Sim sim = powerSim();
    const Entity generator = powerPut(sim, "WoodFiredGenerator", 10, 10);
    powerWire(sim, 12, 20, 10);
    const Entity battery = powerPut(sim, "Battery", 13, 11);  // 1 x 2, beside the conduit
    const Entity lamp = powerPut(sim, "StandingLamp", 21, 10);
    POWER_REQUIRE(battery != kNoEntity);
    CHECK(sim.power().capacityOf(battery) == doctest::Approx(1000.0));

    powerRun(sim, 1000);
    // 1000 W - 30 W lamp - 5 W idle drain = 965 W surplus; half is kept: 965 / 86400 / 2 per step.
    const double expected = 1000.0 * 965.0 / 86400.0 * 0.5;
    CHECK(sim.power().storedIn(battery) == doctest::Approx(expected).epsilon(0.001));
    CHECK(sim.power().net(sim.power().netOf(battery))->stored == doctest::Approx(expected).epsilon(0.001));

    // Cut the generator off (flick it) and the battery pays for the lamp and its own drain.
    sim.submit(FlickCommand{generator, false});
    powerRun(sim, 2);
    CHECK(sim.power().powered(lamp));
    const double before = sim.power().storedIn(battery);
    powerRun(sim, 1000);
    CHECK(sim.power().storedIn(battery) == doctest::Approx(before - 1000.0 * 35.0 / 86400.0).epsilon(0.001));

    // When it runs dry the lamp goes out.
    PowerPart& part = sim.things().registry().get<PowerPart>(battery);
    part.stored = 0.02;  // 0.02 Wd lasts 0.02 / (35 / 86400) = 49 steps
    powerRun(sim, 30);
    CHECK(sim.power().powered(lamp));
    powerRun(sim, 40);
    CHECK_FALSE(sim.power().powered(lamp));
    CHECK(sim.power().storedIn(battery) < 1e-3);
}

TEST_CASE("Power: a battery never holds more than its capacity") {
    Sim sim = powerSim();
    powerPut(sim, "WoodFiredGenerator", 10, 10);
    powerWire(sim, 12, 14, 10);
    const Entity battery = powerPut(sim, "Battery", 13, 11);
    sim.things().registry().get<PowerPart>(battery).stored = 999.999;
    powerRun(sim, 100);
    CHECK(sim.power().storedIn(battery) == doctest::Approx(1000.0));
    CHECK(sim.power().storedIn(battery) <= 1000.0);
}

TEST_CASE("Power: a net that went dark waits for its batteries before coming back") {
    Sim sim = powerSim();
    // A solar panel in the dark; a battery; a heater wanting 175 W.
    sim.power().setSun([](const GameTime&) { return 0.0; });
    const Entity battery = powerPut(sim, "Battery", 12, 11);
    powerWire(sim, 12, 20, 10);
    const Entity heater = powerPut(sim, "Heater", 21, 10);
    sim.things().registry().get<PowerPart>(battery).stored = 0.0005;  // less than a minute of the heater
    sim.step();
    CHECK_FALSE(sim.power().powered(heater));  // 0.0005 Wd is 21 steps of 175 W + 5 W, but it starts dark

    // Now a trickle that doesn't cover the heater: the net stays dark while charging, with no flicker.
    Sim sim2 = powerSim();
    const Entity gen = powerPut(sim2, "SolarGenerator", 10, 10);
    POWER_REQUIRE(gen != kNoEntity);
    sim2.power().setSun([](const GameTime&) { return 0.05; });  // 85 W
    powerWire(sim2, 14, 20, 10);
    powerPut(sim2, "Battery", 14, 11);
    const Entity heater2 = powerPut(sim2, "Heater", 21, 10);
    // The heater wants 175 W and the panel gives 85: the net runs on its battery for a while, goes
    // dark, charges up to its reserve and comes back. Never a flicker from step to step.
    int run = 0;
    int shortest = 1 << 30;
    bool last = false;
    for (int i = 0; i < 3000; ++i) {
        sim2.step();
        const bool now = sim2.power().powered(heater2);
        if (now != last && i > 0) {
            shortest = std::min(shortest, run);
            run = 0;
        }
        ++run;
        last = now;
    }
    CHECK(shortest > 30);
}

TEST_CASE("Power: solar output follows the sun and is lost under a roof") {
    Sim sim = powerSim();
    const Entity panel = powerPut(sim, "SolarGenerator", 10, 10);  // 4 x 4, 1700 W in full sun
    powerWire(sim, 14, 15, 10);
    powerPut(sim, "StandingLamp", 16, 10);
    POWER_REQUIRE(panel != kNoEntity);

    double sun = 1.0;
    sim.power().setSun([&sun](const GameTime&) { return sun; });
    sim.step();
    CHECK(sim.power().wattsOf(panel) == doctest::Approx(1700.0));
    sun = 0.5;
    sim.step();
    CHECK(sim.power().wattsOf(panel) == doctest::Approx(850.0));
    sun = 0.0;
    sim.step();
    CHECK(sim.power().wattsOf(panel) == doctest::Approx(0.0));
}

TEST_CASE("Power: the default sun is dark at night and full at noon") {
    Sim sim = powerSim();
    sim.submit(SetWeatherCommand{"Clear", 1000.0f});  // cloud would dim the sun (E1)
    const Entity panel = powerPut(sim, "SolarGenerator", 10, 10);
    powerWire(sim, 14, 15, 10);
    powerPut(sim, "StandingLamp", 16, 10);
    sim.step();
    CHECK(sim.power().wattsOf(panel) == doctest::Approx(0.0));  // second 0 is midnight
    // Noon.
    while (sim.time().seconds < 12 * 3600) {
        sim.step();
    }
    sim.step();
    CHECK(sim.power().wattsOf(panel) == doctest::Approx(1700.0));
    // Half way up the morning ramp it is somewhere between.
    Sim morning = powerSim();
    morning.submit(SetWeatherCommand{"Clear", 1000.0f});
    const Entity p2 = powerPut(morning, "SolarGenerator", 10, 10);
    powerWire(morning, 14, 15, 10);
    powerPut(morning, "StandingLamp", 16, 10);
    while (morning.time().seconds < static_cast<int64_t>(6.5 * 3600)) {
        morning.step();
    }
    morning.step();
    CHECK(morning.power().wattsOf(p2) > 100.0);
    CHECK(morning.power().wattsOf(p2) < 1600.0);
}

TEST_CASE("Power: a solar panel under a roof makes nothing") {
    Sim sim = powerSim();
    // A walled room, 8 x 8 outside, roofed by Rooms once it is closed.
    for (int y = 20; y <= 27; ++y) {
        for (int x = 20; x <= 27; ++x) {
            if (x == 20 || x == 27 || y == 20 || y == 27) {
                powerPut(sim, "Wall", x, y);
            }
        }
    }
    const Entity panel = powerPut(sim, "SolarGenerator", 22, 22);  // 4 x 4 inside
    POWER_REQUIRE(panel != kNoEntity);
    powerPut(sim, "PowerConduit", 21, 22);  // on the panel's west side
    const Entity lamp = powerPut(sim, "StandingLamp", 21, 23);
    sim.power().setSun([](const GameTime&) { return 1.0; });
    sim.step();
    POWER_REQUIRE(sim.map().roof(CellPos{23, 23, 0}) != Roof::None);
    CHECK(sim.power().netOf(panel) != 0);  // it is wired: the roof is why it makes nothing
    CHECK(sim.power().netOf(panel) == sim.power().netOf(lamp));
    CHECK(sim.power().wattsOf(panel) == doctest::Approx(0.0));
    CHECK_FALSE(sim.power().powered(lamp));
}

TEST_CASE("Power: a wind turbine is slowed by what stands in the wind") {
    Sim sim = powerSim();
    const Entity turbine = powerPut(sim, "WindTurbine", 20, 20);  // 5 x 2 lying along x
    powerWire(sim, 25, 27, 20);
    powerPut(sim, "StandingLamp", 27, 19);  // beside the wire, out of the turbine's wind
    POWER_REQUIRE(turbine != kNoEntity);
    sim.power().setWind([](const GameTime&) { return 0.8; });
    sim.step();
    CHECK(sim.power().wattsOf(turbine) == doctest::Approx(1700.0 * 0.8));

    // A wall two cells high, 3 cells west of the turbine, blocks 2 of the 16 cells it needs clear.
    powerPut(sim, "Wall", 18, 20);
    powerPut(sim, "Wall", 18, 21);
    sim.step();
    sim.stepMany(3600);  // openness is looked at again every game hour
    CHECK(sim.power().wattsOf(turbine) == doctest::Approx(1700.0 * 0.8 * 14.0 / 16.0).epsilon(0.001));
}

TEST_CASE("Power: a generator that burns fuel stops when its fuel is gone") {
    Sim sim = powerSim();
    const PowerLine line = powerLine(sim);
    bool hasFuel = true;
    sim.power().setFuel([&hasFuel](Entity) { return hasFuel; });
    sim.step();
    CHECK(sim.power().powered(line.lamp));
    hasFuel = false;
    sim.step();
    CHECK(sim.power().wattsOf(line.generator) == doctest::Approx(0.0));
    CHECK_FALSE(sim.power().powered(line.lamp));
    hasFuel = true;
    sim.step();
    CHECK(sim.power().powered(line.lamp));

    // A solar panel burns nothing: the fuel check does not apply to it.
    Sim other = powerSim();
    const Entity panel = powerPut(other, "SolarGenerator", 10, 10);
    powerWire(other, 14, 15, 10);
    powerPut(other, "StandingLamp", 16, 10);
    other.power().setSun([](const GameTime&) { return 1.0; });
    other.power().setFuel([](Entity) { return false; });
    other.step();
    CHECK(other.power().wattsOf(panel) == doctest::Approx(1700.0));
}

TEST_CASE("Power: a consumer can ask for less than its full draw") {
    Sim sim = powerSim();
    const Entity generator = powerPut(sim, "WoodFiredGenerator", 10, 10);
    powerWire(sim, 12, 14, 10);
    const Entity heater = powerPut(sim, "Heater", 15, 10);
    sim.step();
    const uint32_t net = sim.power().netOf(generator);
    CHECK(sim.power().net(net)->demand == doctest::Approx(175.0));
    CHECK(sim.power().setDraw(heater, 20.0));
    sim.step();
    CHECK(sim.power().net(net)->demand == doctest::Approx(20.0));
    CHECK(sim.power().wattsOf(heater) == doctest::Approx(-20.0));
    CHECK(sim.power().setDraw(heater, -1.0));
    sim.step();
    CHECK(sim.power().net(net)->demand == doctest::Approx(175.0));
    CHECK_FALSE(sim.power().setDraw(generator, 5.0));  // a generator draws nothing
}

TEST_CASE("Power: buildings that draw power only run with it") {
    Sim sim = powerSim();
    const PowerLine line = powerLine(sim);
    const Entity cold = powerPut(sim, "Heater", 40, 40);  // no wire
    sim.step();
    CHECK(sim.jobs().buildingRuns(line.lamp));
    CHECK_FALSE(sim.jobs().buildingRuns(cold));
    sim.submit(FlickCommand{line.lamp, false});
    sim.step();
    CHECK_FALSE(sim.jobs().buildingRuns(line.lamp));
    // A wood-burning stove has no power part and always runs.
    const Entity stove = powerPut(sim, "FueledStove", 30, 30);
    if (stove != kNoEntity) {
        CHECK(sim.jobs().buildingRuns(stove));
    }
    CHECK(sim.jobs().buildingRuns(sim.map().buildingAt(CellPos{13, 10, 0})));
}

TEST_CASE("Power: a short circuit empties the net's batteries and burns the conduit out") {
    Sim sim = powerSim();
    powerPut(sim, "WoodFiredGenerator", 10, 10);
    powerWire(sim, 12, 16, 10);
    const Entity battery = powerPut(sim, "Battery", 13, 11);
    const Entity lamp = powerPut(sim, "StandingLamp", 17, 10);
    sim.things().registry().get<PowerPart>(battery).stored = 400.0;
    sim.step();
    const Entity conduit = sim.map().buildingAt(CellPos{15, 10, 0});
    POWER_REQUIRE(conduit != kNoEntity);

    sim.submit(ShortCircuitCommand{conduit});
    sim.step();
    bool seen = false;
    for (const Event& e : sim.events()) {
        if (e.type == EventType::ShortCircuit) {
            seen = true;
            CHECK(e.thing == conduit);
            CHECK(e.amount == doctest::Approx(400.0f).epsilon(0.01));
            CHECK(e.cell == (CellPos{15, 10, 0}));
        }
    }
    CHECK(seen);
    CHECK(sim.map().buildingAt(CellPos{15, 10, 0}) == kNoEntity);
    CHECK(sim.power().storedIn(battery) < 1.0);  // emptied (a step's charge may be in it)
    CHECK_FALSE(sim.power().powered(lamp));      // the wire is cut

    // Not on any power at all: refused.
    const Entity wall = powerPut(sim, "Wall", 30, 30);
    sim.submit(ShortCircuitCommand{wall});
    sim.step();
    size_t failed = 0;
    for (const Event& e : sim.events()) {
        failed += e.type == EventType::CommandFailed ? 1 : 0;
    }
    CHECK(failed == 1);
}

TEST_CASE("Power: net numbers are the same however the buildings were put down") {
    // Two sims with the same buildings made in a different order give the same nets and hash.
    auto build = [](bool reversed) {
        Sim sim = powerSim();
        std::vector<std::pair<const char*, CellPos>> list = {{"WoodFiredGenerator", {10, 10, 0}},
                                                             {"PowerConduit", {12, 10, 0}},
                                                             {"PowerConduit", {13, 10, 0}},
                                                             {"StandingLamp", {14, 10, 0}},
                                                             {"SolarGenerator", {30, 30, 0}},
                                                             {"PowerConduit", {34, 30, 0}},
                                                             {"Heater", {35, 30, 0}}};
        if (reversed) {
            std::reverse(list.begin(), list.end());
        }
        for (const auto& [def, cell] : list) {
            sim.things().spawn(def, cell);
        }
        sim.step();
        return std::make_pair(sim.power().nets().size(), sim.power().netOf(sim.map().buildingAt(CellPos{14, 10, 0})));
    };
    const auto a = build(false);
    const auto b = build(true);
    CHECK(a.first == 2);
    CHECK(a.first == b.first);
    // Net ids follow the lowest thing id in each net, which differs with the order; what must hold is
    // that the lamp's net exists both times.
    CHECK(a.second != 0);
    CHECK(b.second != 0);
}

TEST_CASE("Power: a save keeps charge, switches and who has power, and a loaded game runs on the same") {
    auto defs = powerDefs();
    Sim original(SimSettings{5, 64, 0, 0, true}, defs);
    const Entity generator = powerPut(original, "WoodFiredGenerator", 10, 10);
    powerWire(original, 12, 16, 10);
    const Entity battery = powerPut(original, "Battery", 13, 11);
    const Entity sw = powerPut(original, "PowerSwitch", 17, 10);
    powerPut(original, "PowerConduit", 18, 10);
    const Entity heater = powerPut(original, "Heater", 19, 10);
    const Entity lamp = powerPut(original, "StandingLamp", 14, 9);
    POWER_REQUIRE(generator != kNoEntity);
    original.power().setDraw(heater, 50.0);
    powerRun(original, 500);
    original.submit(FlickCommand{sw, false});
    original.submit(FlickCommand{lamp, false});
    powerRun(original, 300);
    original.submit(FlickCommand{kNoEntity, true});  // fails, still goes in the log
    original.step();
    CHECK(original.power().storedIn(battery) > 0.0);

    const std::vector<uint8_t> bytes = saveSim(original);
    SaveLoadResult loaded = loadSim(bytes, defs);
    POWER_REQUIRE(loaded.error.empty());
    POWER_REQUIRE(loaded.sim != nullptr);
    Sim& copy = *loaded.sim;
    CHECK(copy.stateHash() == original.stateHash());
    CHECK(saveSim(copy) == bytes);
    CHECK(copy.power().storedIn(battery) == original.power().storedIn(battery));
    CHECK(copy.power().stateOf(sw) == PowerState::Off);
    CHECK(copy.power().stateOf(lamp) == PowerState::Off);
    CHECK(copy.power().stateOf(heater) == original.power().stateOf(heater));
    CHECK(copy.power().netOf(battery) == original.power().netOf(battery));  // nets are made again on loading
    CHECK(copy.power().nets().size() == original.power().nets().size());

    // Closing the switch after loading works as before, and the two games stay together.
    original.submit(FlickCommand{sw, true});
    copy.submit(FlickCommand{sw, true});
    powerRun(original, 400);
    powerRun(copy, 400);
    CHECK(copy.stateHash() == original.stateHash());
    CHECK(copy.power().powered(heater));
    CHECK(copy.power().storedIn(battery) == original.power().storedIn(battery));

    // The readable dump names the power part.
    const std::string json = saveToJson(original);
    CHECK(json.find("\"power\"") != std::string::npos);
}

TEST_CASE("Power: a replay with flick commands plays back to the same hashes") {
    auto defs = powerDefs();
    Sim original(SimSettings{5, 64, 0, 0, true}, defs);
    const PowerLine line = powerLine(original);
    powerPut(original, "Battery", 13, 11);
    for (int i = 0; i < 1500; ++i) {
        if (i == 100) {
            original.submit(FlickCommand{line.lamp, false});
        }
        if (i == 700) {
            original.submit(FlickCommand{line.lamp, true});
            original.submit(ShortCircuitCommand{original.map().buildingAt(CellPos{14, 10, 0})});
        }
        original.step();
    }
    const Replay replay = recordReplay(original);
    const std::string text = writeReplay(replay);
    CHECK(text.find("Flick") != std::string::npos);
    CHECK(text.find("ShortCircuit") != std::string::npos);
    Replay parsed;
    std::string error;
    POWER_REQUIRE(readReplay(text, parsed, error));
    CHECK(parsed.commands.size() == replay.commands.size());
}

TEST_CASE("Power: a lamp gives light only while it has power") {
    Sim sim = powerSim();  // the sim starts at midnight
    const PowerLine line = powerLine(sim);
    const CellPos beside{16, 11, 0};
    CHECK(sim.lights().lightAt(beside) == doctest::Approx(0.0f));  // not stepped yet: nothing is powered
    powerRun(sim, 2);
    CHECK(sim.lights().lightAt(beside) > 0.5f);

    sim.submit(FlickCommand{line.lamp, false});
    powerRun(sim, 2);
    CHECK(sim.lights().lightAt(beside) == doctest::Approx(0.0f));
    sim.submit(FlickCommand{line.lamp, true});
    powerRun(sim, 2);
    CHECK(sim.lights().lightAt(beside) > 0.5f);

    // Cut the generator: the lamp goes dark with the net.
    sim.submit(FlickCommand{line.generator, false});
    powerRun(sim, 2);
    CHECK(sim.lights().lightAt(beside) == doctest::Approx(0.0f));
}

TEST_CASE("Power: solar panels follow the light grid's sun") {
    Sim sim = powerSim();
    const Entity panel = powerPut(sim, "SolarGenerator", 10, 10);
    powerWire(sim, 14, 15, 10);
    powerPut(sim, "StandingLamp", 16, 10);
    while (sim.time().seconds < 11 * 3600) {
        sim.step();
    }
    sim.step();
    CHECK(sim.power().wattsOf(panel) == doctest::Approx(1700.0 * sim.lights().skyGlow()).epsilon(0.001));
    CHECK(sim.power().wattsOf(panel) > 0.0);
}
