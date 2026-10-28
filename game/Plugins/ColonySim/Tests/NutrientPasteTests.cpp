// The nutrient paste dispenser and its hoppers (docs/design/nutrient-paste.md).
#include "ColonySim/Colonist.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Mood.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Power.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Research.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

namespace {

std::shared_ptr<const DefDatabase> pasteDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

// A flat 64 m map in daylight. The dispenser (3 x 4) stands at 20..22 x 20..23 with a hopper at (19,21); a solar
// generator at (4,4) is wired to it by conduits ending at (19,20) when `withPower`.
struct PasteFixture {
    Sim sim;
    Entity dispenser = kNoEntity;
    Entity hopper = kNoEntity;

    explicit PasteFixture(bool withPower) : sim(settings(), pasteDefs()) {
        for (int y = 0; y < sim.map().height(); ++y) {
            for (int x = 0; x < sim.map().width(); ++x) {
                sim.plants().remove(CellPos{x, y, 0});
            }
        }
        sim.setOutdoorOverride(20.0f);
        sim.power().setSun([](const GameTime&) { return 1.0; });
        if (withPower) {
            sim.things().spawn("SolarGenerator", CellPos{4, 4, 0});
            sim.things().spawn("SolarGenerator", CellPos{4, 8, 0});
            for (int y = 4; y <= 12; ++y) {
                sim.things().spawn("PowerConduit", CellPos{8, y, 0});
            }
            for (int x = 9; x <= 19; ++x) {
                sim.things().spawn("PowerConduit", CellPos{x, 12, 0});
            }
            for (int y = 13; y <= 20; ++y) {
                sim.things().spawn("PowerConduit", CellPos{19, y, 0});
            }
        }
        dispenser = sim.things().spawn("NutrientPasteDispenser", CellPos{20, 20, 0});
        hopper = sim.things().spawn("Hopper", CellPos{19, 21, 0});
        sim.stepMany(9 * kSecondsPerHour);  // daytime
    }
    static SimSettings settings() {
        SimSettings s;
        s.seed = 5;
        s.mapSize = 64;
        s.wildAnimals = 0;
        s.flatMap = true;
        s.checkpointEvery = 0;
        return s;
    }
    entt::registry& reg() { return sim.things().registry(); }
    Entity colonist(const char* trait = nullptr) {
        const Entity pawn = sim.things().spawn("Human", CellPos{15, 24, 0});
        Colonist c;
        if (trait != nullptr) {
            c.traits.push_back(TraitRecord{trait, 0});
        }
        reg().emplace<Colonist>(pawn, c);
        sim.jobs().addWorker(pawn);
        return pawn;
    }
    int items(const char* def) {
        int n = 0;
        for (const Entity item : sim.things().itemsAt(CellPos{19, 21, 0})) {
            n += reg().get<Thing>(item).def == sim.defs().idOf("Things", def) ? reg().get<StackCount>(item).count : 0;
        }
        return n;
    }
    // Hungry: runs until the pawn has eaten or `limit` seconds pass. True when it ate.
    bool eat(Entity pawn, int limit = 3000) {
        reg().get<Needs>(pawn).food = 0.1f;
        for (int i = 0; i < limit; ++i) {
            sim.step();
            for (const Event& e : sim.events()) {
                pasteMade += e.type == EventType::ThingSpawned && sim.defs().get(e.def).defName() == "MealNutrientPaste" ? 1 : 0;
                if (e.type == EventType::PawnAte && e.thing == pawn) {
                    sim.step();
                    return true;
                }
            }
        }
        return false;
    }
    // Paste meals on the map, eaten or not: eaten ones are gone, so this counts events.
    int pasteMade = 0;
    bool remembers(Entity pawn, const char* thought) {
        const Mood* mood = reg().try_get<Mood>(pawn);
        return mood != nullptr && std::any_of(mood->memories.begin(), mood->memories.end(), [&](const Memory& m) { return m.def == thought; });
    }
};

}  // namespace

TEST_CASE("Nutrient paste: the research unlocks the dispenser and the hoppers, with the wiki's numbers") {
    const DefDatabase& defs = *pasteDefs();
    const Def& dispenser = defs.get(defs.idOf("Things", "NutrientPasteDispenser"));
    const Def& hopper = defs.get(defs.idOf("Things", "Hopper"));
    CHECK(dispenser.text("researchPrerequisite") == "NutrientPaste");
    CHECK(hopper.text("researchPrerequisite") == "NutrientPaste");
    CHECK(dispenser.integer("costs.Steel") == 125);
    CHECK(dispenser.integer("costs.ComponentIndustrial") == 3);
    CHECK(dispenser.number("power.consumption") == doctest::Approx(200.0));
    CHECK(dispenser.number("building.nutritionPerMeal") == doctest::Approx(0.3));
    CHECK(hopper.integer("costs.Steel") == 15);
    const Def& meal = defs.get(defs.idOf("Things", "MealNutrientPaste"));
    CHECK(meal.number("stats.nutrition") == doctest::Approx(0.9));
    CHECK(meal.text("ingestible.preferability") == "MealAwful");
    const Def& project = defs.get(defs.idOf("Research", "NutrientPaste"));
    CHECK(project.count("unlocks.Things") == 2);
}

TEST_CASE("Nutrient paste: a hungry colonist eats a meal the dispenser makes from the hopper's food") {
    for (const char* trait : {static_cast<const char*>(nullptr), "Ascetic"}) {
        PasteFixture f(true);
        f.sim.things().spawn("RawRice", CellPos{19, 21, 0}, 30);
        const float nutrition = static_cast<float>(f.sim.defs().get(f.sim.defs().idOf("Things", "RawRice")).number("stats.nutrition"));
        CHECK(nutrition > 0.0f);
        const Entity pawn = f.colonist(trait);
        f.sim.step();
        CHECK(f.sim.power().running(f.dispenser));
        CHECK(f.eat(pawn));
        CHECK(f.pasteMade == 1);
        // 0.3 nutrition of rice is taken, in whole items (rounded up).
        const int taken = 30 - f.items("RawRice");
        CHECK(taken == static_cast<int>(std::ceil(0.3f / nutrition - 0.001f)));
        CHECK(f.reg().get<Needs>(pawn).food >= 0.9f);
        f.sim.stepMany(2);
        CHECK(f.remembers(pawn, "AteNutrientPasteMeal") == (trait == nullptr));
    }
}

TEST_CASE("Nutrient paste: no power, an empty hopper or too little food makes no meal") {
    {
        PasteFixture f(false);  // no power
        f.sim.things().spawn("RawRice", CellPos{19, 21, 0}, 30);
        const Entity pawn = f.colonist();
        f.sim.step();
        CHECK(!f.sim.power().running(f.dispenser));
        f.eat(pawn, 600);  // it may eat the raw rice itself
        CHECK(f.pasteMade == 0);
    }
    {
        PasteFixture f(true);  // an empty hopper
        const Entity pawn = f.colonist();
        f.sim.step();
        f.eat(pawn, 600);
        CHECK(f.pasteMade == 0);
    }
    {
        PasteFixture f(true);  // 0.05 nutrition of rice is not a meal's 0.3
        f.sim.things().spawn("RawRice", CellPos{19, 21, 0}, 2);
        const Entity pawn = f.colonist();
        f.sim.step();
        f.eat(pawn, 600);
        CHECK(f.pasteMade == 0);
    }
}

TEST_CASE("Nutrient paste: the dispenser takes only raw food, never hay") {
    PasteFixture f(true);
    f.sim.things().spawn("Hay", CellPos{19, 21, 0}, 90);
    const Entity pawn = f.colonist();
    f.sim.step();
    // Hay is food of a kind (a pawn will eat it when ravenous) but the dispenser leaves it alone.
    f.eat(pawn, 600);
    CHECK(f.pasteMade == 0);
}
