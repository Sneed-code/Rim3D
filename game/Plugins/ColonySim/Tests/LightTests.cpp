#include "ColonySim/Clock.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Lights.h"
#include "ColonySim/Map.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"
#include "FuelTestHelper.h"

#include <doctest.h>

#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define LIGHT_REQUIRE(...)     \
    do {                       \
        CHECK((__VA_ARGS__));  \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// The game's real data: the light rules and the lamp defs are part of what is tested.
std::shared_ptr<const DefDatabase> lightDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

Sim lightSim(int size = 64) { return Sim(SimSettings{5, size, 0, 0, true}, lightDefs()); }

// Runs the sim to an hour of the day (from a sim that has not run yet).
void lightGoTo(Sim& sim, double hour) {
    sim.stepMany(static_cast<int64_t>(hour * static_cast<double>(kSecondsPerHour)) - sim.time().seconds);
}

void lightRing(Sim& sim, int x0, int y0, int x1, int y1) {
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            if (x == x0 || x == x1 || y == y0 || y == y1) {
                sim.things().spawn("Wall", CellPos{x, y, 0});
            }
        }
    }
}

}  // namespace

TEST_CASE("Light: the sun follows the time of day") {
    Sim sim = lightSim();
    const CellPos open{30, 30, 0};
    lightGoTo(sim, 0.0);
    CHECK(sim.lights().lightAt(open) == doctest::Approx(0.0f));
    lightGoTo(sim, 6.0);
    CHECK(sim.lights().lightAt(open) == doctest::Approx(0.5f).epsilon(0.02));
    lightGoTo(sim, 12.0);
    CHECK(sim.lights().lightAt(open) == doctest::Approx(1.0f));
    lightGoTo(sim, 18.0);
    CHECK(sim.lights().lightAt(open) == doctest::Approx(0.5f).epsilon(0.02));
    lightGoTo(sim, 22.0);
    CHECK(sim.lights().lightAt(open) == doctest::Approx(0.0f));
    CHECK(sim.lights().lightAt(CellPos{-1, 0, 0}) == 0.0f);  // outside the map
}

TEST_CASE("Light: a roof keeps the sun out, and a lamp lights the room") {
    Sim sim = lightSim();
    lightRing(sim, 20, 20, 28, 26);
    lightGoTo(sim, 12.0);
    const CellPos inside{24, 23, 0};
    LIGHT_REQUIRE(sim.rooms().indoors(inside));
    CHECK(sim.lights().underOpenSky(CellPos{40, 40, 0}));
    CHECK_FALSE(sim.lights().underOpenSky(inside));
    CHECK(sim.lights().lightAt(inside) == 0.0f);        // noon, but roofed and no lamp
    CHECK(sim.lights().lightAt(CellPos{40, 40, 0}) == doctest::Approx(1.0f));

    testFueled(sim, sim.things().spawn("TorchLamp", CellPos{24, 23, 0}));
    sim.step();
    CHECK(sim.lights().sources().size() == 1);
    CHECK(sim.lights().lightAt(CellPos{24, 23, 0}) == doctest::Approx(1.0f));
    CHECK(sim.lights().lightAt(inside) > sim.lights().lightAt(CellPos{22, 21, 0}));  // dimmer towards a corner
    CHECK(sim.lights().lightAt(CellPos{22, 21, 0}) > 0.0f);
}

TEST_CASE("Light: a lamp's light falls to nothing at its radius and walls cast shadows") {
    Sim sim = lightSim();
    lightGoTo(sim, 23.0);
    testFueled(sim, sim.things().spawn("TorchLamp", CellPos{30, 30, 0}));  // radius 10
    for (int y = 28; y <= 32; ++y) {
        sim.things().spawn("Wall", CellPos{33, y, 0});
    }
    sim.step();
    const Lights& lights = sim.lights();
    CHECK(lights.lightAt(CellPos{30, 30, 0}) == doctest::Approx(1.0f));
    CHECK(lights.lightAt(CellPos{27, 30, 0}) == doctest::Approx(0.7f).epsilon(0.01));
    CHECK(lights.lightAt(CellPos{20, 30, 0}) == 0.0f);   // at the radius
    CHECK(lights.lightAt(CellPos{40, 30, 0}) == 0.0f);   // past it
    CHECK(lights.lightAt(CellPos{32, 30, 0}) > 0.0f);    // before the wall
    CHECK(lights.lightAt(CellPos{35, 30, 0}) == 0.0f);   // in the wall's shadow
    CHECK(lights.lightAt(CellPos{30, 24, 0}) > 0.0f);    // round the wall's end it is lit again
}

TEST_CASE("Light: the lamp layer is rebuilt only when something changed") {
    Sim sim = lightSim();
    testFueled(sim, sim.things().spawn("TorchLamp", CellPos{30, 30, 0}));
    sim.step();
    const uint64_t rebuilds = sim.lights().rebuilds();
    sim.stepMany(100);
    CHECK(sim.lights().rebuilds() == rebuilds);
    const Entity lamp = sim.map().buildingAt(CellPos{30, 30, 0});
    LIGHT_REQUIRE(lamp != kNoEntity);
    sim.things().despawn(lamp);
    sim.step();
    CHECK(sim.lights().rebuilds() == rebuilds + 1);
    CHECK(sim.lights().sources().empty());
    CHECK(sim.lights().lampLightAt(CellPos{30, 30, 0}) == 0.0f);
}

TEST_CASE("Light: darkness slows walking and work") {
    Sim sim = lightSim();
    lightGoTo(sim, 12.0);
    const CellPos spot{30, 30, 0};
    const Entity pawn = sim.things().spawn("Human", spot);
    CHECK(sim.lights().workSpeedAt(spot) == doctest::Approx(1.0f));
    CHECK(sim.lights().moveSpeedAt(spot) == doctest::Approx(1.0f));
    CHECK(sim.jobs().lightWorkFactor(pawn) == doctest::Approx(1.0f));
    lightGoTo(sim, 23.0);
    CHECK(sim.lights().workSpeedAt(spot) < 0.8f);
    CHECK(sim.lights().moveSpeedAt(spot) < 0.9f);
    CHECK(sim.jobs().lightWorkFactor(pawn) == doctest::Approx(sim.lights().workSpeedAt(spot)));
    // A lamp beside it brings the speed back.
    testFueled(sim, sim.things().spawn("TorchLamp", CellPos{31, 30, 0}));
    sim.step();
    CHECK(sim.jobs().lightWorkFactor(pawn) > 0.99f);
}

TEST_CASE("Light: a pawn walks slower at night than at noon") {
    auto walk = [](double hour) {
        Sim sim = lightSim(96);
        lightGoTo(sim, hour);
        const Entity pawn = sim.things().spawn("Human", CellPos{10, 40, 0});
        std::vector<CellPos> path;
        for (int x = 11; x <= 70; ++x) {
            path.push_back(CellPos{x, 40, 0});
        }
        const Position& pos = sim.things().registry().get<Position>(pawn);
        sim.things().registry().get<Mover>(pawn).walkTo(pos.x, pos.y, std::move(path));
        int steps = 0;
        while (!sim.things().registry().get<Mover>(pawn).arrived() && steps < 3000) {
            sim.step();
            ++steps;
        }
        return steps;
    };
    const int noon = walk(10.0);   // the walk stays inside the daylight hours
    const int night = walk(22.0);
    CHECK(noon > 0);
    CHECK(night > noon);
    CHECK(static_cast<double>(night) / static_cast<double>(noon) < 1.4);
}
