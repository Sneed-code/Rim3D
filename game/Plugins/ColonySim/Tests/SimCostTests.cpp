#include "ColonySim/Movement.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include "TestDefs.h"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <string>

// Sim cost (task S9, docs/design/sim-cost.md): the timing hook, and the cases the faster steering has to keep.

using namespace colony;

namespace {

Sim costTestSim(int size) { return Sim(SimSettings{5, size, 0, 0, true}, testDefs()); }

}  // namespace

TEST_CASE("Sim cost: timing the systems changes nothing and names every system that ran") {
    Sim plain = costTestSim(64);
    Sim timed = costTestSim(64);
    for (Sim* sim : {&plain, &timed}) {
        for (int i = 0; i < 12; ++i) {
            const Entity pawn = sim->things().spawn("Human", CellPos{10 + i, 10, 0});
            const Position& pos = sim->things().registry().get<Position>(pawn);
            sim->things().registry().get<Mover>(pawn).walkTo(pos.x, pos.y, {CellPos{40 - i, 30, 0}});
        }
    }
    StepProfile profile;
    timed.setProfile(&profile);
    plain.stepMany(300);
    timed.stepMany(300);
    timed.setProfile(nullptr);

    CHECK(plain.stateHash() == timed.stateHash());
    CHECK(profile.steps == 300);
    const auto cost = [&](const char* name) {
        for (size_t i = 0; i < profile.names.size(); ++i) {
            if (std::string(profile.names[i]) == name) {
                return profile.ms[i];
            }
        }
        return -1.0;
    };
    CHECK(cost("movement") > 0.0);
    CHECK(cost("total") >= cost("movement"));
    // Switched off again, nothing more is added.
    const double before = cost("total");
    timed.stepMany(10);
    CHECK(cost("total") == before);
}

TEST_CASE("Movement: a pawn standing alone stays exactly where it is") {
    Sim sim = costTestSim(64);
    const Entity alone = sim.things().spawn("Human", CellPos{30, 30, 0});
    const Position start = sim.things().registry().get<Position>(alone);
    sim.stepMany(200);
    const Position& now = sim.things().registry().get<Position>(alone);
    CHECK(now.x == start.x);
    CHECK(now.y == start.y);
    const Mover& mover = sim.things().registry().get<Mover>(alone);
    CHECK(mover.vx == 0.0f);
    CHECK(mover.vy == 0.0f);
}

TEST_CASE("Movement: a standing pawn is still pushed by one that stands in its space") {
    Sim sim = costTestSim(64);
    const Entity a = sim.things().spawn("Human", CellPos{30, 30, 0});
    const Entity b = sim.things().spawn("Human", CellPos{30, 30, 0});
    const Position& pa = sim.things().registry().get<Position>(a);
    const Position& pb = sim.things().registry().get<Position>(b);
    CHECK(pa.x == pb.x);
    sim.stepMany(120);
    // Both were idle, neither had a goal, and they ended apart (a crowd that didn't wake the steering would stay stacked).
    const float dx = pa.x - pb.x;
    const float dy = pa.y - pb.y;
    CHECK(std::sqrt(dx * dx + dy * dy) >= 2.0f * kPawnRadius);
}
