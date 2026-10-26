#include "ColonySim/Bills.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Fuel.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Spoilage.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define KEEP_REQUIRE(...)                  \
    do {                                   \
        const bool keepOk = (__VA_ARGS__); \
        CHECK(keepOk);                     \
        if (!keepOk) {                     \
            return;                        \
        }                                  \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> keepDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

}  // namespace

TEST_CASE("Preserved food: pemmican keeps 150 days, a survival meal 60, a simple meal 4, in a warm roofed room") {
    Sim sim(SimSettings{5, 48, 0, 0, true}, keepDefs());
    sim.setOutdoorOverride(20.0f);
    for (int x = 10; x <= 14; ++x) {
        for (int y = 10; y <= 14; ++y) {
            if (x == 10 || x == 14 || y == 10 || y == 14) {
                sim.things().spawn("Wall", CellPos{x, y, 0});
            }
        }
    }
    sim.rooms().update();
    const Entity pemmican = sim.things().spawn("Pemmican", CellPos{12, 12, 0}, 20);
    const Entity survival = sim.things().spawn("MealSurvivalPack", CellPos{11, 12, 0}, 2);
    const Entity simple = sim.things().spawn("MealSimple", CellPos{13, 12, 0}, 2);
    KEEP_REQUIRE(pemmican != kNoEntity && survival != kNoEntity && simple != kNoEntity);
    int64_t clock = 0;
    const auto days = [&](double d) {
        const int64_t interval = sim.spoilage().numbers().intervalSeconds;
        const int64_t end = clock + static_cast<int64_t>(d * 86400.0);
        for (; clock < end; clock += interval) {
            sim.spoilage().step(clock);
        }
    };
    days(5);
    CHECK_FALSE(sim.things().exists(simple));
    CHECK(sim.things().exists(survival));
    days(50);  // day 55
    CHECK(sim.things().exists(survival));
    days(10);  // day 65
    CHECK_FALSE(sim.things().exists(survival));
    CHECK(sim.things().exists(pemmican));
    days(80);  // day 145
    CHECK(sim.things().exists(pemmican));
    days(10);  // day 155
    CHECK_FALSE(sim.things().exists(pemmican));
}

TEST_CASE("Preserved food: a batch of 50 kibble, 16 pemmican or a survival meal burns one wood, not one per product") {
    Sim sim(SimSettings{5, 48, 0, 0, true}, keepDefs());
    const BillSystem& bills = sim.jobs().bills();
    const Recipe* kibble = bills.recipe(sim.defs().idOf("Recipes", "MakeKibble"));
    const Recipe* pemmican = bills.recipe(sim.defs().idOf("Recipes", "MakePemmican"));
    const Recipe* survival = bills.recipe(sim.defs().idOf("Recipes", "CookMealSurvival"));
    const Recipe* bulk = bills.recipe(sim.defs().idOf("Recipes", "CookMealSimpleBulk"));
    KEEP_REQUIRE(kibble != nullptr && pemmican != nullptr && survival != nullptr && bulk != nullptr);
    CHECK(kibble->productCount() == 50);
    CHECK(kibble->fuelUses() == 1);
    CHECK(pemmican->productCount() == 16);
    CHECK(pemmican->fuelUses() == 1);
    CHECK(survival->fuelUses() == 1);
    CHECK(bulk->fuelUses() == 4);  // meals still burn one wood each
}
