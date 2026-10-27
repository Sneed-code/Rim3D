// The electric smelter's recipes (docs/design/crafting.md): smelting gives back a quarter of the metal in a thing's own recipe.
#include "ColonySim/Apparel.h"
#include "ColonySim/Bills.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Fuel.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"
#include "FuelTestHelper.h"

#include <doctest.h>

#include <memory>
#include <string>
#include <vector>

using namespace colony;

#define SMELT_REQUIRE(...)                  \
    do {                                    \
        const bool smeltOk = (__VA_ARGS__); \
        CHECK(smeltOk);                     \
        if (!smeltOk) {                     \
            return;                         \
        }                                   \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> smeltDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

struct SmeltFixture {
    Sim sim{SimSettings{5, 64, 0, 0, true}, smeltDefs()};
    Entity smelter = kNoEntity;

    SmeltFixture() {
        sim.submit(UnlockResearchCommand{"Electricity"});
        testFueled(sim, sim.things().spawn("WoodFiredGenerator", CellPos{10, 10, 0}));
        for (int x = 12; x <= 17; ++x) {
            sim.things().spawn("PowerConduit", CellPos{x, 10, 0});
        }
        sim.submit(SpawnThingCommand{"ElectricSmelter", CellPos{19, 10, 0}, 1});
        sim.step();
        for (const Event& e : sim.events()) {
            if (e.type == EventType::ThingSpawned && sim.defs().get(e.def).defName() == "ElectricSmelter") {
                smelter = e.thing;
            }
        }
        const Entity pawn = sim.things().spawn("Human", CellPos{12, 14, 0});
        Colonist colonist;
        colonist.skills.push_back(SkillRecord{"Crafting", 8, Passion::None, 0.0f, false});
        sim.things().registry().emplace<Colonist>(pawn, std::move(colonist));
        sim.jobs().addWorker(pawn, {}, false);
    }

    int onMap(const char* def) { return sim.jobs().bills().stockOf(sim.defs().idOf("Things", def)); }
    // An item lying on the map, made of `stuff` if given.
    Entity lay(const char* def, CellPos cell, const char* stuff = nullptr) {
        const Entity item = sim.things().spawn(def, cell);
        if (stuff != nullptr) {
            sim.things().registry().emplace<MadeOf>(item, MadeOf{sim.defs().idOf("Things", stuff)});
        }
        return item;
    }
    bool bill(const char* recipe) {
        sim.submit(AddBillCommand{smelter, recipe, BillMode::Count, 1});
        sim.step();
        for (const Event& e : sim.events()) {
            if (e.type == EventType::CommandFailed) {
                return false;
            }
        }
        return true;
    }
    bool runUntilGone(Entity item) {
        for (int i = 0; i < 8000; ++i) {
            if (!sim.things().registry().valid(item)) {
                return true;
            }
            sim.step();
        }
        return false;
    }
};

}  // namespace

TEST_CASE("Smelting: a steel knife gives back a quarter of its 30 steel, rounded down") {
    SmeltFixture f;
    SMELT_REQUIRE(f.smelter != kNoEntity);
    const Entity knife = f.lay("Melee_Knife", CellPos{14, 14, 0}, "Steel");
    SMELT_REQUIRE(f.bill("SmeltWeapon"));
    SMELT_REQUIRE(f.runUntilGone(knife));
    for (int i = 0; i < 12000 && f.onMap("Steel") == 0; ++i) {
        f.sim.step();
    }
    CHECK(f.onMap("Steel") == 7);
    CHECK(f.onMap("Melee_Knife") == 0);
}

TEST_CASE("Smelting: wooden things and cloth give no metal, and a destroy bill gives nothing") {
    SmeltFixture f;
    SMELT_REQUIRE(f.smelter != kNoEntity);
    const BillSystem& bills = f.sim.jobs().bills();
    const DefDatabase& defs = f.sim.defs();
    // Clothes made of cloth, a club of wood: no metal at all. A knife of steel: a quarter of 30.
    CHECK(bills.smeltYield(defs.idOf("Things", "Apparel_TShirt"), defs.idOf("Things", "Cloth"), 0.25, 1).empty());
    CHECK(bills.smeltYield(defs.idOf("Things", "Melee_Club"), defs.idOf("Things", "WoodLog"), 0.25, 1).empty());
    const auto knives = bills.smeltYield(defs.idOf("Things", "Melee_Knife"), defs.idOf("Things", "Steel"), 0.25, 2);
    SMELT_REQUIRE(knives.size() == 1);
    CHECK(knives.front().count == 14);
    // A destroy bill eats the weapon and makes nothing.
    const Entity club = f.lay("Melee_Club", CellPos{14, 14, 0}, "WoodLog");
    SMELT_REQUIRE(f.bill("DestroyWeapon"));
    SMELT_REQUIRE(f.runUntilGone(club));
    f.sim.stepMany(1500);
    CHECK(f.onMap("Steel") == 0);
    CHECK(f.onMap("WoodLog") == 0);
}

TEST_CASE("Smelting: the benches and recipes of this task are wired to work givers") {
    SmeltFixture f;
    for (const char* name : {"SmeltWeapon", "SmeltApparel", "DestroyWeapon", "DestroyApparel", "BurnWeapon", "BurnApparel", "CremateCorpse"}) {
        const Recipe* recipe = f.sim.jobs().bills().recipe(f.sim.defs().idOf("Recipes", name));
        SMELT_REQUIRE(recipe != nullptr);
        CHECK_MESSAGE(!recipe->users.empty(), name);
    }
    const Recipe* smelt = f.sim.jobs().bills().recipe(f.sim.defs().idOf("Recipes", "SmeltWeapon"));
    SMELT_REQUIRE(smelt != nullptr);
    CHECK(smelt->ingredients.front().items.size() > 3);  // every weapon def
}
