// The autodoor, the flood light and the vitals monitor (docs/design/autodoor-lights-monitor.md).
#include "ColonySim/Defs.h"
#include "ColonySim/Doors.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Lights.h"
#include "ColonySim/Map.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Pathfinder.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Power.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <memory>
#include <string>
#include <vector>

using namespace colony;

namespace {

std::shared_ptr<const DefDatabase> moreDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

// A flat 64 m map in daylight, with a solar generator (4 x 4, 1700 W) at `solarAt` when `withPower`.
std::unique_ptr<Sim> moreSim(bool withPower, CellPos solarAt) {
    SimSettings s;
    s.seed = 5;
    s.mapSize = 64;
    s.wildAnimals = 0;
    s.flatMap = true;
    s.checkpointEvery = 0;
    auto owned = std::make_unique<Sim>(s, moreDefs());
    Sim& sim = *owned;
    for (int y = 0; y < sim.map().height(); ++y) {
        for (int x = 0; x < sim.map().width(); ++x) {
            sim.plants().remove(CellPos{x, y, 0});
        }
    }
    sim.setOutdoorOverride(20.0f);
    sim.power().setSun([](const GameTime&) { return 1.0; });
    if (withPower) {
        sim.things().spawn("SolarGenerator", solarAt);
    }
    return owned;
}

// Steps a pawn takes to walk from (20,20) to (20,28) through a wall along y = 24 with `doorDef` in it at x = 20 and a
// powered neighbour (the solar generator at 21..24 x 24..27 when `withPower`, else walls).
int moreDoorWalk(const char* doorDef, bool withPower) {
    auto owned = moreSim(withPower, CellPos{21, 24, 0});
    Sim& sim = *owned;
    for (int x = 10; x <= 30; ++x) {
        if (x != 20 && (!withPower || x < 21 || x > 24)) {
            sim.things().spawn("Wall", CellPos{x, 24, 0});
        }
    }
    sim.things().spawn(doorDef, CellPos{20, 24, 0});
    if (withPower) {
        sim.things().spawn("PowerConduit", CellPos{20, 25, 0});  // joins the generator's side to the door
    }
    sim.step();
    const Entity pawn = sim.things().spawn("Human", CellPos{20, 20, 0});
    std::vector<CellPos> path;
    if (!sim.pathfinder().findPath(CellPos{20, 20, 0}, CellPos{20, 28, 0}, path)) {
        return -1;
    }
    const Position start = sim.things().registry().get<Position>(pawn);
    sim.things().registry().get<Mover>(pawn).walkTo(start.x, start.y, path);
    for (int i = 0; i < 600; ++i) {
        if (sim.things().registry().get<Mover>(pawn).arrived()) {
            return i;
        }
        sim.step();
    }
    return -1;
}

}  // namespace

TEST_CASE("Autodoor: powered it opens four times faster than a door, unpowered it is a plain door") {
    const int plain = moreDoorWalk("Door", false);
    const int powered = moreDoorWalk("Autodoor", true);
    const int dark = moreDoorWalk("Autodoor", false);
    CHECK(plain > 0);
    CHECK(powered > 0);
    CHECK(dark > 0);
    if (plain <= 0 || powered <= 0 || dark <= 0) {
        return;
    }
    const int openSteps = DoorNumbers::read(*moreDefs()).openSteps;
    CHECK(plain - powered >= openSteps * 3 / 4 - 4);
    CHECK(dark >= plain - 2);
    CHECK(dark <= plain + 2);
}

TEST_CASE("Flood light: a powered one lights 14 m away, a dark one lights nothing") {
    for (const bool powered : {true, false}) {
        auto owned = moreSim(powered, CellPos{30, 30, 0});
        Sim& sim = *owned;
        const Entity light = sim.things().spawn("FloodLight", CellPos{29, 31, 0});
        sim.things().spawn("PowerConduit", CellPos{29, 30, 0});  // beside the generator and the lamp
        sim.stepMany(3);
        CHECK(sim.power().running(light) == powered);
        // Beyond a standing lamp's 10 m, within the flood light's 17.45.
        CHECK((sim.lights().lampLightAt(CellPos{29, 17, 0}) > 0.0f) == powered);
    }
    const Def& def = moreDefs()->get(moreDefs()->idOf("Things", "FloodLight"));
    CHECK(def.number("glower.radius") == doctest::Approx(17.45));
}

TEST_CASE("Vitals monitor: it counts only powered and beside the bed") {
    struct Case {
        CellPos monitor;
        bool power;
        bool expect;
    };
    // The bed covers (20,20) and (20,21); the solar generator (22..25 x 17..20) is joined to the monitor by a conduit.
    // A monitor at (21,19) is a diagonal neighbour of the bed; one at (25,25) is far from it.
    const Case cases[] = {{CellPos{21, 19, 0}, true, true}, {CellPos{21, 19, 0}, false, false}, {CellPos{25, 25, 0}, true, false}};
    for (const Case& c : cases) {
        auto owned = moreSim(c.power, CellPos{22, 17, 0});
        Sim& sim = *owned;
        const Entity bed = sim.things().spawn("HospitalBed", CellPos{20, 20, 0});
        sim.things().spawn("VitalsMonitor", c.monitor);
        sim.things().spawn("PowerConduit", CellPos{21, 18, 0});  // beside the generator and the monitor at (21,19)
        sim.stepMany(3);
        CHECK(sim.jobs().bedMonitored(bed) == c.expect);
    }
}
