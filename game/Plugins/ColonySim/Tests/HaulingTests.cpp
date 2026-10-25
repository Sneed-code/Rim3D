#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include "TestDefs.h"

#include <doctest.h>

using namespace colony;

TEST_CASE("Hauling: 20 pawns haul every item within one game day, none fail") {
    Sim sim(SimSettings{11, 192, 20, 150}, testDefs());
    CHECK(sim.setupErrors().empty());
    CHECK(sim.stockpileCell().has_value());
    CHECK(sim.haulers().size() == 20);
    CHECK(sim.things().count() == 170);
    CHECK(sim.itemsLeft() == 150);
    sim.stepMany(kSecondsPerDay);
    CHECK(sim.itemsLeft() == 0);
    CHECK(sim.stats().hauls == 150);
    CHECK(sim.stats().failedJobs == 0);
    // Every item ended on the map, at the stockpile.
    const entt::registry& reg = sim.things().registry();
    int onStockpile = 0;
    reg.view<const Thing, const Position>().each([&](const Thing& thing, const Position& pos) {
        if (thing.category == ThingCategory::Item && cellOf(pos) == *sim.stockpileCell()) {
            ++onStockpile;
        }
    });
    CHECK(onStockpile == 150);
}

TEST_CASE("Hauling: a carried item is off the map until it is put down") {
    Sim sim(SimSettings{11, 64, 1, 1, true}, testDefs());
    const Things& things = sim.things();
    Entity item = kNoEntity;
    things.registry().view<const Thing>().each([&](Entity e, const Thing& thing) {
        if (thing.category == ThingCategory::Item) {
            item = e;
        }
    });
    if (item == kNoEntity) {
        CHECK(false);
        return;
    }
    bool sawCarried = false;
    for (int i = 0; i < 3600 && sim.itemsLeft() > 0; ++i) {
        sim.step();
        sawCarried = sawCarried || !things.spawned(item);
    }
    CHECK(sawCarried);
    CHECK(sim.itemsLeft() == 0);
    CHECK(things.spawned(item));
}

TEST_CASE("Hauling: missing defs are reported, not crashed on") {
    Sim noDefs(SimSettings{11, 64, 2, 2, true});
    CHECK(noDefs.setupErrors().size() == 2);
    CHECK(noDefs.things().count() == 0);
    noDefs.stepMany(10);

    SimSettings wrongKind{11, 64, 1, 0, true};
    wrongKind.pawnDef = "Steel";
    Sim notAPawn(wrongKind, testDefs());
    CHECK(notAPawn.setupErrors().size() == 1);
    CHECK(notAPawn.haulers().empty());
    CHECK(notAPawn.things().count() == 0);
}

TEST_CASE("Hauling: same seed gives the same game") {
    const auto defs = testDefs();
    Sim a(SimSettings{21, 96, 5, 40}, defs);
    Sim b(SimSettings{21, 96, 5, 40}, defs);
    a.stepMany(5000);
    b.stepMany(5000);
    CHECK(a.stats().hauls == b.stats().hauls);
    if (!(a.haulers().size() == b.haulers().size())) {
        CHECK(false);
        return;
    }
    for (size_t i = 0; i < a.haulers().size(); ++i) {
        const Position& pa = a.things().registry().get<Position>(a.haulers()[i]);
        const Position& pb = b.things().registry().get<Position>(b.haulers()[i]);
        CHECK(pa.x == pb.x);
        CHECK(pa.y == pb.y);
    }
}

TEST_CASE("Hauling: paths come through the queue and never go over the step budget") {
    SimSettings settings{11, 192, 20, 150};
    settings.pathBudget = 300;  // small, so searches spread over steps and pawns wait for paths
    Sim sim(settings, testDefs());
    sim.stepMany(kSecondsPerDay);
    CHECK(sim.itemsLeft() == 0);
    CHECK(sim.stats().failedJobs == 0);
    const PathStats& paths = sim.pathfinder().stats();
    CHECK(paths.requests == 300);  // one to each item, one back to the stockpile
    CHECK(paths.failedSearches == 0);
    CHECK(paths.maxWorkInStep <= 300);
    CHECK(paths.maxWaitSteps > 1);
}
