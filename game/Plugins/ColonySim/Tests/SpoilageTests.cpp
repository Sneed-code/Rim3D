#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Filth.h"
#include "FuelTestHelper.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Spoilage.h"
#include "ColonySim/Temperature.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define SPOIL_REQUIRE(...)                  \
    do {                                    \
        const bool spoilOk = (__VA_ARGS__); \
        CHECK(spoilOk);                     \
        if (!spoilOk) {                     \
            return;                         \
        }                                   \
    } while (0)

namespace {

// The game's real data: the food, the rules and the recipes are part of what is tested.
std::shared_ptr<const DefDatabase> spoilDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

std::unique_ptr<Sim> spoilSim(float outdoor) {
    auto sim = std::make_unique<Sim>(SimSettings{5, 48, 0, 0, true}, spoilDefs());
    sim->setOutdoorOverride(outdoor);
    return sim;
}

// Runs the spoilage system alone, on its own clock.
int64_t spoilClock = 0;
void spoilDays(Sim& sim, double days) {
    const int64_t interval = sim.spoilage().numbers().intervalSeconds;
    const int64_t end = spoilClock + static_cast<int64_t>(days * 86400.0);
    for (; spoilClock < end; spoilClock += interval) {
        sim.spoilage().step(spoilClock);
    }
}

// A ring of walls round x0..x1 by y0..y1; the room inside is roofed.
void spoilRoom(Sim& sim, int x0, int y0, int x1, int y1) {
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            if (x == x0 || x == x1 || y == y0 || y == y1) {
                sim.things().spawn("Wall", CellPos{x, y, 0});
            }
        }
    }
    sim.rooms().update();
}

Entity spoilCook(Sim& sim, CellPos cell, int level) {
    const Entity pawn = sim.things().spawn("Human", cell);
    Colonist colonist;
    colonist.skills.push_back(SkillRecord{"Cooking", level, Passion::None, 0.0f, false});
    sim.things().registry().emplace<Colonist>(pawn, std::move(colonist));
    return pawn;
}

}  // namespace

TEST_CASE("Spoilage: food rots at full speed when warm, slowly when cool and not at all when frozen") {
    auto sim = spoilSim(20.0f);
    const SpoilageSystem& spoilage = sim->spoilage();
    CHECK(spoilage.rotSpeedAt(30.0f) == 1.0f);
    CHECK(spoilage.rotSpeedAt(10.0f) == 1.0f);
    CHECK(spoilage.rotSpeedAt(5.0f) == doctest::Approx(0.5f));
    CHECK(spoilage.rotSpeedAt(0.0f) == 0.0f);
    CHECK(spoilage.rotSpeedAt(-20.0f) == 0.0f);
}

TEST_CASE("Spoilage: milk (3 days) is gone in 3 warm days, in 6 days at 5 degrees, and does not rot frozen") {
    for (const float outdoor : {20.0f, 5.0f, -5.0f}) {
        auto sim = spoilSim(outdoor);
        spoilClock = 0;
        const Entity milk = sim->things().spawn("MilkRaw", CellPos{10, 10, 0}, 3);
        SPOIL_REQUIRE(milk != kNoEntity);
        SPOIL_REQUIRE(sim->things().registry().all_of<Spoilage>(milk));
        const double days = outdoor > 10.0f ? 3.0 : (outdoor > 0.0f ? 6.0 : 4.0);
        spoilDays(*sim, days * 0.95);
        CHECK(sim->things().exists(milk));
        if (outdoor > 0.0f) {
            CHECK(sim->things().registry().get<Spoilage>(milk).rot > 0.9f);
            spoilDays(*sim, days * 0.1);
            CHECK(!sim->things().exists(milk));
        } else {
            // Frozen, it does not rot (it would still wear out in the open: 10 days).
            CHECK(sim->things().registry().get<Spoilage>(milk).rot == 0.0f);
        }
    }
}

TEST_CASE("Spoilage: a rotted item reports ItemSpoiled and is destroyed") {
    auto sim = spoilSim(20.0f);
    spoilClock = 0;
    sim->stepMany(1);  // clears the events
    const Entity milk = sim->things().spawn("MilkRaw", CellPos{10, 10, 0});
    spoilDays(*sim, 3.2);
    bool spoiled = false;
    for (const Event& e : sim->events()) {
        spoiled = spoiled || (e.type == EventType::ItemSpoiled && e.thing == milk && e.message == "rotted");
    }
    CHECK(spoiled);
    CHECK(!sim->things().exists(milk));
}

TEST_CASE("Spoilage: a cold room keeps food that a warm one does not") {
    auto sim = spoilSim(-10.0f);
    spoilRoom(*sim, 10, 10, 16, 16);
    sim->temperature().step(0);
    const Entity inside = sim->things().spawn("MilkRaw", CellPos{13, 13, 0});
    // The room was made at -10. The day turns warm, but a shut room takes hours to follow.
    sim->setOutdoorOverride(20.0f);
    const Entity outside = sim->things().spawn("MilkRaw", CellPos{30, 30, 0});
    spoilClock = 0;
    spoilDays(*sim, 0.5);
    CHECK(sim->things().registry().get<Spoilage>(outside).rot > 0.15f);
    CHECK(sim->things().registry().get<Spoilage>(inside).rot < sim->things().registry().get<Spoilage>(outside).rot);
}

TEST_CASE("Spoilage: items wear out under the open sky, not under a roof") {
    auto sim = spoilSim(15.0f);
    spoilRoom(*sim, 10, 10, 16, 16);
    spoilClock = 0;
    const Entity open = sim->things().spawn("Cloth", CellPos{30, 30, 0}, 10);
    const Entity roofed = sim->things().spawn("Cloth", CellPos{13, 13, 0}, 10);
    SPOIL_REQUIRE(open != kNoEntity && roofed != kNoEntity);
    spoilDays(*sim, 10.0);
    entt::registry& reg = sim->things().registry();
    CHECK(reg.get<HitPoints>(open).current == 100 - 20);  // 2 hit points a day
    CHECK(reg.get<HitPoints>(roofed).current == 100);
    spoilDays(*sim, 41.0);  // 50 days in all: worn to nothing
    CHECK(!sim->things().exists(open));
    CHECK(sim->things().exists(roofed));
}

TEST_CASE("Spoilage: a stack that is put onto another averages its rot, joins its ingredients and keeps poison") {
    auto sim = spoilSim(20.0f);
    Things& things = sim->things();
    entt::registry& reg = things.registry();
    const Entity a = things.spawn("MealSimple", CellPos{10, 10, 0}, 2);
    const Entity b = things.make(sim->defs().idOf("Things", "MealSimple"), 6);
    SPOIL_REQUIRE(a != kNoEntity && b != kNoEntity);
    const DefId berries = sim->defs().idOf("Things", "RawBerries");
    const DefId rice = sim->defs().idOf("Things", "RawRice");
    reg.get<Spoilage>(a).rot = 0.8f;
    reg.get<Spoilage>(a).ingredients = {berries};
    reg.get<Spoilage>(b).rot = 0.0f;
    reg.get<Spoilage>(b).ingredients = {rice};
    reg.get<Spoilage>(b).poisoned = true;
    things.addToStack(a, b, 6);
    CHECK(reg.get<StackCount>(a).count == 8);
    CHECK(reg.get<Spoilage>(a).rot == doctest::Approx(0.2f));
    CHECK(reg.get<Spoilage>(a).poisoned);
    CHECK(reg.get<Spoilage>(a).ingredients.size() == 2);
    // Splitting a stack gives the part the same rot and the same ingredients.
    const Entity part = things.splitOff(a, 3);
    SPOIL_REQUIRE(part != kNoEntity);
    CHECK(reg.get<Spoilage>(part).rot == doctest::Approx(0.2f));
    CHECK(reg.get<Spoilage>(part).ingredients == reg.get<Spoilage>(a).ingredients);
}

TEST_CASE("Spoilage: a dirty kitchen and a poor cook make poisoned meals more likely") {
    auto sim = spoilSim(15.0f);
    const CellPos cell{20, 20, 0};
    const Entity expert = spoilCook(*sim, CellPos{10, 10, 0}, 12);
    const Entity novice = spoilCook(*sim, CellPos{10, 12, 0}, 0);
    SpoilageSystem& spoilage = sim->spoilage();
    const float clean = spoilage.poisonChance(expert, cell);
    CHECK(spoilage.poisonChance(novice, cell) == doctest::Approx(clean + spoilage.numbers().unskilledChance));
    CHECK(spoilage.poisonChance(novice, cell) > clean);
    sim->filth().add(cell, "Blood", 3);
    const float dirty = spoilage.poisonChance(expert, cell);
    CHECK(dirty > clean);
    CHECK(dirty <= spoilage.numbers().dirtyChance + 0.0001f);
    // A clean, floored room is the safest place.
    sim->filth().clear(cell);
    CHECK(spoilage.poisonChance(expert, cell) == doctest::Approx(clean));
}

TEST_CASE("Spoilage: meals made by a bad cook in filth are sometimes poisoned, and eating them sometimes makes the eater ill") {
    auto sim = spoilSim(15.0f);
    const CellPos cell{20, 20, 0};
    const Entity novice = spoilCook(*sim, CellPos{20, 21, 0}, 0);
    for (int dx = -1; dx <= 1; ++dx) {
        sim->filth().add(CellPos{cell.x + dx, cell.y, 0}, "Blood", 3);
    }
    SpoilageSystem& spoilage = sim->spoilage();
    const DefId berries = sim->defs().idOf("Things", "RawBerries");
    const std::vector<BillUsed> used{BillUsed{berries, 10}, BillUsed{berries, 5}};
    int poisoned = 0;
    for (int i = 0; i < 400; ++i) {
        const Entity meal = sim->things().make(sim->defs().idOf("Things", "MealSimple"));
        spoilage.madeBy(meal, used, novice, cell);
        const Spoilage& s = sim->things().registry().get<Spoilage>(meal);
        SPOIL_REQUIRE(s.ingredients.size() == 1 && s.ingredients[0] == berries);
        poisoned += s.poisoned ? 1 : 0;
        sim->things().destroy(meal);
    }
    const float chance = spoilage.poisonChance(novice, cell);
    CHECK(chance > 0.1f);
    CHECK(poisoned > static_cast<int>(400 * chance * 0.5f));
    CHECK(poisoned < static_cast<int>(400 * chance * 1.6f));

    int illnesses = 0;
    spoilage.setPoisonHandler([&](Entity) { ++illnesses; });
    const Entity bad = sim->things().spawn("MealSimple", CellPos{25, 25, 0});
    sim->things().registry().get<Spoilage>(bad).poisoned = true;
    const Entity clean = sim->things().spawn("MealSimple", CellPos{26, 25, 0});
    int got = 0;
    for (int i = 0; i < 400; ++i) {
        got += spoilage.eaten(novice, bad) ? 1 : 0;
        CHECK(!spoilage.eaten(novice, clean));
    }
    CHECK(got == illnesses);
    CHECK(got > 100);  // 35% of 400, give or take
    CHECK(got < 190);
}

TEST_CASE("Spoilage: a cooked meal remembers its ingredients and rots in the warm") {
    Sim sim(SimSettings{5, 48, 0, 0, true}, spoilDefs());
    sim.setOutdoorOverride(20.0f);
    sim.submit(SpawnThingCommand{"FueledStove", CellPos{20, 20, 0}});
    sim.submit(SpawnThingCommand{"RawBerries", CellPos{12, 12, 0}, 25});
    sim.submit(SpawnThingCommand{"Human", CellPos{10, 10, 0}});
    sim.step();
    Entity cook = kNoEntity;
    for (const Event& e : sim.events()) {
        cook = e.type == EventType::ThingSpawned && sim.defs().get(e.def).defName() == "Human" ? e.thing : cook;
    }
    SPOIL_REQUIRE(cook != kNoEntity);
    Colonist colonist;
    colonist.skills.push_back(SkillRecord{"Cooking", 8, Passion::None, 0.0f, false});
    sim.things().registry().emplace<Colonist>(cook, std::move(colonist));
    const Entity stove = testFueled(sim, sim.map().buildingAt(CellPos{20, 20, 0}));
    sim.submit(AddBillCommand{stove, "CookMealSimple", BillMode::Count, 1});
    Entity meal = kNoEntity;
    for (int i = 0; i < 3000 && meal == kNoEntity; ++i) {
        sim.step();
        sim.things().registry().view<const Thing, const Spoilage>().each([&](Entity e, const Thing& thing, const Spoilage&) {
            meal = sim.defs().get(thing.def).defName() == "MealSimple" ? e : meal;
        });
    }
    SPOIL_REQUIRE(meal != kNoEntity);
    const Spoilage& s = sim.things().registry().get<Spoilage>(meal);
    SPOIL_REQUIRE(s.ingredients.size() == 1);
    CHECK(sim.defs().get(s.ingredients[0]).defName() == "RawBerries");
    // The snapshot shows how far it has rotted.
    sim.stepMany(7200);
    Snapshot snap;
    sim.writeSnapshot(snap);
    float rot = -1.0f;
    for (const ThingView& view : snap.things) {
        if (view.id == meal) {
            rot = view.rot;
        }
    }
    CHECK(rot > 0.0f);
    CHECK(rot < 1.0f);
}

TEST_CASE("Spoilage: rot, wear, ingredients and poison survive a save, and the game goes on the same") {
    SimSettings settings{9, 48, 0, 0, true};
    settings.checkpointEvery = 600;
    Sim original(settings, spoilDefs());
    original.setOutdoorOverride(15.0f);
    const Entity meal = original.things().spawn("MealSimple", CellPos{10, 10, 0}, 3);
    original.things().spawn("Cloth", CellPos{30, 30, 0}, 4);
    original.things().spawn("MilkRaw", CellPos{31, 30, 0}, 4);
    entt::registry& reg = original.things().registry();
    reg.get<Spoilage>(meal).poisoned = true;
    reg.get<Spoilage>(meal).ingredients = {original.defs().idOf("Things", "RawBerries"), original.defs().idOf("Things", "RawRice")};
    std::sort(reg.get<Spoilage>(meal).ingredients.begin(), reg.get<Spoilage>(meal).ingredients.end(),
              [](DefId a, DefId b) { return a.value < b.value; });
    original.stepMany(86400 / 2);
    const std::vector<uint8_t> bytes = saveSim(original);
    SaveLoadResult loaded = loadSim(bytes, spoilDefs());
    SPOIL_REQUIRE(loaded.error.empty());
    SPOIL_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->stateHash() == original.stateHash());
    CHECK(saveSim(*loaded.sim) == bytes);
    const Spoilage& before = reg.get<Spoilage>(meal);
    const Spoilage& after = loaded.sim->things().registry().get<Spoilage>(meal);
    CHECK(after.rot == before.rot);
    CHECK(after.rot > 0.0f);
    CHECK(after.poisoned);
    CHECK(after.ingredients == before.ingredients);
    original.stepMany(86400 / 2);
    loaded.sim->setOutdoorOverride(15.0f);
    loaded.sim->stepMany(86400 / 2);
    CHECK(loaded.sim->stateHash() == original.stateHash());
}

TEST_CASE("Spoilage: wear under the open sky goes on the same after a save and load, with the real weather") {
    auto db = std::make_shared<DefDatabase>();
    std::vector<std::string> errors;
    DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
    SimSettings settings{3, 128, 0, 0};
    settings.scenario = "Crashlanded";
    Sim original(settings, db);
    // The weather's wear factor is not saved; the first minute after a load must still use it.
    original.stepMany(86400 / 2);
    const std::vector<uint8_t> bytes = saveSim(original);
    SaveLoadResult loaded = loadSim(bytes, db);
    SPOIL_REQUIRE(loaded.error.empty());
    SPOIL_REQUIRE(loaded.sim != nullptr);
    original.stepMany(600);
    loaded.sim->stepMany(600);
    CHECK(loaded.sim->stateHash() == original.stateHash());
}
