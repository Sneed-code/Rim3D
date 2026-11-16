// The protagonist eats from its bag, better food fills more, and starving slows a pawn (sneed, 2026-11-16;
// docs/design/needs.md "The protagonist eats from the bag", "Better food fills more", "Starving"), with the game's data.
#include "ColonySim/Apparel.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

#define BAG_REQUIRE(...)                                    \
    do {                                                    \
        const bool bagOk = static_cast<bool>(__VA_ARGS__);  \
        CHECK_MESSAGE(bagOk, #__VA_ARGS__);                 \
        if (!bagOk) {                                       \
            return;                                         \
        }                                                   \
    } while (false)

namespace {

std::shared_ptr<const DefDatabase> bagDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return db;
    }();
    return defs;
}

std::unique_ptr<Sim> bagSim() { return std::make_unique<Sim>(SimSettings{11, 32, 0, 0, true}, bagDefs()); }

Entity bagColonist(Sim& sim, CellPos cell, bool protagonist) {
    const Entity pawn = sim.things().spawn("Human", cell);
    sim.jobs().addWorker(pawn);
    Colonist colonist;
    colonist.firstName = protagonist ? "Hero" : "Plain";
    colonist.protagonist = protagonist;
    sim.things().registry().emplace<Colonist>(pawn, colonist);
    sim.submit(DraftCommand{pawn, true});  // stands still where it was put
    return pawn;
}

bool bagRun(Sim& sim, Command command) {
    const uint64_t id = sim.submit(std::move(command));
    sim.step();
    return std::none_of(sim.events().begin(), sim.events().end(),
                        [&](const Event& e) { return e.type == EventType::CommandFailed && e.command == id; });
}

// Spawns `count` of `def` beside the pawn and puts it all in the bag.
bool bagPut(Sim& sim, Entity pawn, const char* def, int count) {
    const CellPos at = cellOf(sim.things().registry().get<Position>(pawn));
    const Entity item = sim.things().spawn(def, CellPos{at.x + 1, at.y, at.level}, count);
    return item != kNoEntity && bagRun(sim, InventoryCommand{pawn, "Take", item, 0});
}

int bagHas(const Sim& sim, Entity pawn, const char* def) {
    const entt::registry& reg = sim.things().registry();
    const DefId id = sim.defs().idOf("Things", def);
    int total = 0;
    if (const Gear* gear = reg.try_get<Gear>(pawn)) {
        for (const Entity item : gear->inventory) {
            if (reg.get<Thing>(item).def == id) {
                const StackCount* stack = reg.try_get<StackCount>(item);
                total += stack != nullptr ? stack->count : 1;
            }
        }
    }
    return total;
}

Needs& bagNeeds(Sim& sim, Entity pawn) { return sim.things().registry().get<Needs>(pawn); }

}  // namespace

TEST_CASE("Food bag: the protagonist eats the best food in its bag before it gets hungry") {
    auto sim = bagSim();
    const Entity hero = bagColonist(*sim, CellPos{10, 10, 0}, true);
    BAG_REQUIRE(bagPut(*sim, hero, "RawBerries", 30));
    BAG_REQUIRE(bagPut(*sim, hero, "MealSimple", 2));
    BAG_REQUIRE(bagPut(*sim, hero, "MealFine", 1));
    BAG_REQUIRE(bagPut(*sim, hero, "Pemmican", 20));

    // Above the seek level (0.3) it leaves its food alone.
    bagNeeds(*sim, hero).food = 0.31f;
    sim->step();
    CHECK(bagHas(*sim, hero, "MealFine") == 1);

    // Below it, before hungry (0.25): the fine meal first, whatever else it carries, eaten at once on the spot.
    bagNeeds(*sim, hero).food = 0.29f;
    sim->step();
    CHECK(bagHas(*sim, hero, "MealFine") == 0);
    CHECK(bagHas(*sim, hero, "MealSimple") == 2);
    CHECK(bagHas(*sim, hero, "RawBerries") == 30);
    CHECK(bagNeeds(*sim, hero).food == doctest::Approx(1.0f).epsilon(0.001));
    const auto ate = std::find_if(sim->events().begin(), sim->events().end(),
                                  [&](const Event& e) { return e.type == EventType::PawnAte && e.thing == hero; });
    BAG_REQUIRE(ate != sim->events().end());
    CHECK(ate->message == "bag");
    CHECK(ate->def == sim->defs().idOf("Things", "MealFine"));

    // Next time: a simple meal, one of them (it fills 0.99).
    bagNeeds(*sim, hero).food = 0.2f;
    sim->step();
    CHECK(bagHas(*sim, hero, "MealSimple") == 1);
    CHECK(bagNeeds(*sim, hero).food == doctest::Approx(1.0f).epsilon(0.002));
}

TEST_CASE("Food bag: berries go one sitting at a time, and only the protagonist eats from its bag by itself") {
    auto sim = bagSim();
    const Entity hero = bagColonist(*sim, CellPos{10, 10, 0}, true);
    const Entity plain = bagColonist(*sim, CellPos{20, 10, 0}, false);
    BAG_REQUIRE(bagPut(*sim, hero, "RawBerries", 30));
    BAG_REQUIRE(bagPut(*sim, plain, "MealFine", 1));

    bagNeeds(*sim, hero).food = 0.2f;
    bagNeeds(*sim, plain).food = 0.2f;
    sim->step();
    // Berries are 0.05 each: as many as fill it, up to what is eaten at once, the rest stays in the bag.
    const int left = bagHas(*sim, hero, "RawBerries");
    CHECK(left < 30);
    CHECK(left >= 30 - 16);
    CHECK(bagNeeds(*sim, hero).food > 0.2f + 0.05f * static_cast<float>(29 - left));
    // Another colonist keeps its meal (it goes looking for food the usual way instead).
    CHECK(bagHas(*sim, plain, "MealFine") == 1);
}

TEST_CASE("Food bag: food for desperate times only when ravenous") {
    auto sim = bagSim();
    const Entity hero = bagColonist(*sim, CellPos{10, 10, 0}, true);
    // Chocolate is RimWorld's food for desperate times (it gives recreation; as food only when ravenous).
    const std::string name = "Chocolate";
    BAG_REQUIRE(bagPut(*sim, hero, name.c_str(), 5));
    bagNeeds(*sim, hero).food = 0.2f;  // hungry, not ravenous (0.125)
    sim->step();
    CHECK(bagHas(*sim, hero, name.c_str()) == 5);
    bagNeeds(*sim, hero).food = 0.1f;
    sim->step();
    CHECK(bagHas(*sim, hero, name.c_str()) < 5);
}

TEST_CASE("Food bag: better meals fill more") {
    auto sim = bagSim();
    const NeedRules& rules = sim->needs().rules();
    CHECK(rules.nutritionFactor("RawTasty") == doctest::Approx(1.0f));
    CHECK(rules.nutritionFactor("MealAwful") == doctest::Approx(1.0f));
    CHECK(rules.nutritionFactor("MealSimple") == doctest::Approx(1.1f));
    CHECK(rules.nutritionFactor("MealFine") == doctest::Approx(1.25f));
    CHECK(rules.nutritionFactor("MealLavish") == doctest::Approx(1.5f));

    // A simple meal (0.9) eaten on an empty stomach gives 0.99.
    const Entity hero = bagColonist(*sim, CellPos{10, 10, 0}, true);
    BAG_REQUIRE(bagPut(*sim, hero, "MealSimple", 1));
    bagNeeds(*sim, hero).food = 0.0f;
    sim->step();
    CHECK(bagHas(*sim, hero, "MealSimple") == 0);
    CHECK(bagNeeds(*sim, hero).food == doctest::Approx(0.99f).epsilon(0.002));
}

TEST_CASE("Food bag: a starving pawn walks, attacks and works at 75%") {
    auto sim = bagSim();
    const Entity pawn = bagColonist(*sim, CellPos{10, 10, 0}, false);
    const entt::registry& reg = sim->things().registry();
    const ApparelStats& stats = sim->combat().apparelStats();
    const float move = stats.moveFactor(reg, pawn);
    const float attack = stats.attackTimeFactor(reg, pawn);
    const float work = stats.workFactor(reg, pawn);
    CHECK_FALSE(isStarving(reg, pawn));

    bagNeeds(*sim, pawn).food = 0.0f;
    CHECK(isStarving(reg, pawn));
    CHECK(stats.moveFactor(reg, pawn) == doctest::Approx(move * 0.75f));
    CHECK(stats.attackTimeFactor(reg, pawn) == doctest::Approx(attack / 0.75f));
    CHECK(stats.workFactor(reg, pawn) == doctest::Approx(work * 0.75f));

    // The snapshot tells Unreal, which slows the player's own walk by it.
    Snapshot snap;
    sim->writeSnapshot(snap);
    const ThingView* view = snap.find(pawn);
    BAG_REQUIRE(view != nullptr);
    CHECK(view->starving);
    CHECK(view->moveFactor == doctest::Approx(move * 0.75f).epsilon(0.01));

    // Merely hungry is not starving.
    bagNeeds(*sim, pawn).food = 0.05f;
    CHECK(stats.moveFactor(reg, pawn) == doctest::Approx(move));
}
