#include "ColonySim/Sim.h"

#include <doctest.h>

using namespace colony;

TEST_CASE("Sim: each step is one game second") {
    Sim sim(SimSettings{5, 32});
    CHECK(sim.time().seconds == 0);
    sim.step();
    CHECK(sim.time().seconds == 1);
    sim.stepMany(kSecondsPerDay);
    CHECK(sim.time().day() == 1);
}

TEST_CASE("Sim: the seed decides the map") {
    const Sim a(SimSettings{77, 64});
    const Sim b(SimSettings{77, 64});
    const Sim c(SimSettings{78, 64});
    CHECK(a.map().contentHash() == b.map().contentHash());
    CHECK(a.map().contentHash() != c.map().contentHash());
}
