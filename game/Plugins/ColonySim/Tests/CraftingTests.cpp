#include "ColonySim/Apparel.h"
#include "ColonySim/Bills.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Crafting.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Fuel.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"
#include "ColonySim/Zones.h"
#include "FuelTestHelper.h"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

namespace {
constexpr int kCraftUpkeep = 70;  // a little over the minute the bill upkeep runs every
}

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define CRAFT_REQUIRE(...)                  \
    do {                                    \
        const bool craftOk = (__VA_ARGS__); \
        CHECK(craftOk);                     \
        if (!craftOk) {                     \
            return;                         \
        }                                   \
    } while (0)

namespace {

// The game's real data: the benches, recipes, stuffs and quality table are part of what is tested.
std::shared_ptr<const DefDatabase> craftDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

Sim craftSim() { return Sim(SimSettings{5, 64, 0, 0, true}, craftDefs()); }

Entity craftSpawn(Sim& sim, const std::string& def, CellPos cell, int count = 1) {
    sim.submit(SpawnThingCommand{def, cell, count});
    sim.step();
    for (const Event& e : sim.events()) {
        if (e.type == EventType::ThingSpawned && e.cell == cell && sim.defs().get(e.def).defName() == def) {
            return testFueled(sim, e.thing);
        }
    }
    return kNoEntity;
}

// A colonist with the given Crafting level.
Entity craftCrafter(Sim& sim, CellPos cell, int level = 8) {
    const Entity pawn = craftSpawn(sim, "Human", cell);
    Colonist colonist;
    colonist.skills.push_back(SkillRecord{"Crafting", level, Passion::None, 0.0f, false});
    sim.things().registry().emplace<Colonist>(pawn, std::move(colonist));
    return pawn;
}

uint32_t craftAdd(Sim& sim, Entity bench, const std::string& recipe, BillMode mode, int count) {
    sim.submit(AddBillCommand{bench, recipe, mode, count});
    sim.step();
    for (const Event& e : sim.events()) {
        if (e.type == EventType::CommandFailed) {
            return 0;
        }
    }
    const BillList* list = sim.jobs().bills().billsOf(bench);
    return list != nullptr && !list->bills.empty() ? list->bills.back().id : 0;
}

int craftOnMap(const Sim& sim, const std::string& def) {
    return sim.jobs().bills().stockOf(sim.defs().idOf("Things", def));
}

Entity craftFind(const Sim& sim, const std::string& def) {
    const DefId id = sim.defs().idOf("Things", def);
    Entity found = kNoEntity;
    sim.things().registry().view<const Thing, const Position>().each([&](Entity e, const Thing& thing, const Position&) {
        if (thing.def == id && found == kNoEntity) {
            found = e;
        }
    });
    return found;
}

bool craftFailed(const Sim& sim) {
    return std::any_of(sim.events().begin(), sim.events().end(), [](const Event& e) { return e.type == EventType::CommandFailed; });
}

template <typename F>
int craftRunUntil(Sim& sim, int limit, F done) {
    for (int i = 0; i < limit; ++i) {
        if (done()) {
            return i;
        }
        sim.step();
    }
    return done() ? limit : -1;
}

}  // namespace

TEST_CASE("Crafting: every bench in the data has a work giver the work types list, and every recipe a bench") {
    Sim sim = craftSim();
    const DefDatabase& defs = sim.defs();
    std::vector<std::string> tasks;
    for (const DefId id : defs.idsOfKind("WorkTypes")) {
        for (size_t i = 0; i < defs.get(id).count("tasks"); ++i) {
            tasks.push_back(defs.get(id).textAt("tasks", i));
        }
    }
    for (const Recipe& recipe : sim.jobs().bills().recipes()) {
        if (recipe.users.empty()) {
            continue;  // surgery is done at a bed, not at a bench
        }
        for (const DefId user : recipe.users) {
            const std::string giver = defs.get(user).text("workGiver");
            CHECK_MESSAGE(!giver.empty(), recipe.defName << " is made at " << defs.get(user).defName() << ", which has no workGiver");
            CHECK_MESSAGE(std::find(tasks.begin(), tasks.end(), giver) != tasks.end(),
                          defs.get(user).defName() << " names " << giver << ", which no work type lists");
        }
    }
    const CraftingSystem& crafting = sim.crafting();
    CHECK(crafting.benchSpeed(defs.idOf("Things", "TailorBenchHand")) == doctest::Approx(0.5f));
    CHECK(crafting.benchSpeed(defs.idOf("Things", "TailorBenchElectric")) == doctest::Approx(1.0f));
    CHECK(crafting.benchSpeed(defs.idOf("Things", "FueledStove")) == doctest::Approx(1.0f));
}

TEST_CASE("Crafting: recipes find their ingredients by stuff category and make products of the stuff") {
    Sim sim = craftSim();
    const BillSystem& bills = sim.jobs().bills();
    const DefDatabase& defs = sim.defs();
    const auto has = [](const RecipeIngredient& line, DefId item) {
        return std::find(line.items.begin(), line.items.end(), item) != line.items.end();
    };
    const Recipe* knife = bills.recipe(defs.idOf("Recipes", "MakeKnife"));
    CRAFT_REQUIRE(knife != nullptr);
    CRAFT_REQUIRE(knife->ingredients.size() == 1);
    CHECK(has(knife->ingredients[0], defs.idOf("Things", "Steel")));
    CHECK(!has(knife->ingredients[0], defs.idOf("Things", "WoodLog")));
    CHECK(knife->stuffFromIngredient);
    const Recipe* club = bills.recipe(defs.idOf("Recipes", "MakeClub"));
    CRAFT_REQUIRE(club != nullptr);
    CHECK(has(club->ingredients[0], defs.idOf("Things", "WoodLog")));

    const Recipe* shirt = bills.recipe(defs.idOf("Recipes", "Make_Apparel_TShirt"));
    CRAFT_REQUIRE(shirt != nullptr);
    CHECK(has(shirt->ingredients[0], defs.idOf("Things", "Cloth")));
    CHECK(has(shirt->ingredients[0], defs.idOf("Things", "Leather_Plain")));
    CHECK(!has(shirt->ingredients[0], defs.idOf("Things", "Steel")));
    CHECK(shirt->stuffFromIngredient);
    const Recipe* tuque = bills.recipe(defs.idOf("Recipes", "Make_Apparel_Tuque"));
    CRAFT_REQUIRE(tuque != nullptr);
    CHECK(has(tuque->ingredients[0], defs.idOf("Things", "Cloth")));
    CHECK(!has(tuque->ingredients[0], defs.idOf("Things", "Leather_Plain")));  // fabric only

    const Recipe* blocks = bills.recipe(defs.idOf("Recipes", "MakeBlocksGranite"));
    CRAFT_REQUIRE(blocks != nullptr);
    CHECK(blocks->products[0].count == 20);
    CHECK(!blocks->stuffFromIngredient);
}

TEST_CASE("Crafting: the quality table rolls the wiki's shares, never legendary, and a shift raises it") {
    Sim sim = craftSim();
    CraftingSystem& crafting = sim.crafting();
    for (int skill = 0; skill <= 20; ++skill) {
        double sum = 0.0;
        for (int q = 0; q < CraftingSystem::kQualityLevels; ++q) {
            sum += crafting.qualityChance(skill, q);
        }
        CHECK(sum == doctest::Approx(1.0).epsilon(0.001));
        CHECK(crafting.qualityChance(skill, 6) == 0.0);
    }
    CHECK(crafting.qualityChance(0, 0) == doctest::Approx(0.6462));
    CHECK(crafting.qualityChance(8, 2) == doctest::Approx(0.5083));
    CHECK(crafting.qualityChance(20, 5) == doctest::Approx(0.0952));

    for (const int skill : {0, 8, 20}) {
        int counts[CraftingSystem::kQualityLevels] = {};
        const int rolls = 20000;
        for (int i = 0; i < rolls; ++i) {
            const int q = crafting.rollQuality(skill);
            CRAFT_REQUIRE(q >= 0 && q < 6);
            ++counts[q];
        }
        for (int q = 0; q < 6; ++q) {
            // 20,000 rolls: a share is within 0.012 (3.4 standard errors at the widest) of the table.
            CHECK_MESSAGE(std::abs(counts[q] / static_cast<double>(rolls) - crafting.qualityChance(skill, q)) < 0.012,
                          "skill " << skill << " quality " << q);
        }
    }
    CHECK(crafting.rollQuality(20, 3) >= 3);
    CHECK(crafting.rollQuality(0, 10) == 6);
    // Better crafters make better things on average.
    double low = 0.0;
    double high = 0.0;
    for (int i = 0; i < 4000; ++i) {
        low += crafting.rollQuality(2);
        high += crafting.rollQuality(16);
    }
    CHECK(high > low + 4000 * 1.5);
}

TEST_CASE("Crafting: a smith makes a steel knife with a quality, the smithy burns wood, and hand and bench both work") {
    Sim sim = craftSim();
    sim.submit(UnlockResearchCommand{"Smithing"});
    const Entity smithy = craftSpawn(sim, "FueledSmithy", CellPos{20, 20, 0});
    CRAFT_REQUIRE(smithy != kNoEntity);
    craftSpawn(sim, "TorchLamp", CellPos{17, 17, 0});
    craftCrafter(sim, CellPos{18, 18, 0}, 12);
    craftSpawn(sim, "Steel", CellPos{19, 19, 0}, 60);
    CRAFT_REQUIRE(craftAdd(sim, smithy, "MakeKnife", BillMode::Count, 1) != 0);
    const float woodBefore = sim.things().registry().get<Fuel>(smithy).amount;
    CRAFT_REQUIRE(craftRunUntil(sim, 12000, [&] { return craftOnMap(sim, "Melee_Knife") == 1; }) >= 0);
    const Entity knife = craftFind(sim, "Melee_Knife");
    CRAFT_REQUIRE(knife != kNoEntity);
    const entt::registry& reg = sim.things().registry();
    CRAFT_REQUIRE(reg.all_of<MadeOf>(knife));
    CHECK(reg.get<MadeOf>(knife).stuff == sim.defs().idOf("Things", "Steel"));
    CHECK(craftOnMap(sim, "Steel") == 30);  // a knife takes 30
    CHECK(reg.get<Fuel>(smithy).amount == doctest::Approx(woodBefore - 1.0f));
    // The snapshot shows what it is made of and its quality.
    Snapshot snapshot;
    sim.writeSnapshot(snapshot);
    bool seen = false;
    for (const ThingView& view : snapshot.things) {
        if (view.id == knife) {
            seen = true;
            CHECK(view.stuff == sim.defs().idOf("Things", "Steel"));
            CHECK(view.quality >= 0);
            CHECK(view.quality < 6);
        }
    }
    CHECK(seen);
}

TEST_CASE("Crafting: a club is made at a crafting spot out of wood, in the wood's work time") {
    Sim sim = craftSim();
    const Entity spot = craftSpawn(sim, "CraftingSpot", CellPos{20, 20, 0});
    CRAFT_REQUIRE(spot != kNoEntity);
    craftSpawn(sim, "TorchLamp", CellPos{17, 17, 0});
    craftCrafter(sim, CellPos{18, 18, 0}, 8);
    craftSpawn(sim, "WoodLog", CellPos{19, 19, 0}, 40);
    CRAFT_REQUIRE(craftAdd(sim, spot, "MakeClub", BillMode::Count, 1) != 0);
    CRAFT_REQUIRE(craftRunUntil(sim, 12000, [&] { return craftOnMap(sim, "Melee_Club") == 1; }) >= 0);
    const Entity club = craftFind(sim, "Melee_Club");
    CRAFT_REQUIRE(club != kNoEntity);
    CHECK(sim.things().registry().get<MadeOf>(club).stuff == sim.defs().idOf("Things", "WoodLog"));
    CHECK(craftOnMap(sim, "WoodLog") == 15);
}

TEST_CASE("Crafting: a tailor takes cloth or leather, never half of each, and the clothes are made of it") {
    Sim sim = craftSim();
    const Entity bench = craftSpawn(sim, "TailorBenchHand", CellPos{20, 20, 0});
    CRAFT_REQUIRE(bench != kNoEntity);
    craftSpawn(sim, "TorchLamp", CellPos{17, 17, 0});
    craftCrafter(sim, CellPos{18, 18, 0}, 10);
    // Tribalwear takes 60 of a stuff. 40 cloth and 40 leather are 80 together but not 60 of one.
    craftSpawn(sim, "Cloth", CellPos{19, 19, 0}, 40);
    craftSpawn(sim, "Leather_Plain", CellPos{19, 20, 0}, 40);
    CRAFT_REQUIRE(craftAdd(sim, bench, "Make_Apparel_Tribalwear", BillMode::Count, 1) != 0);
    sim.stepMany(600);
    CHECK(craftOnMap(sim, "Apparel_Tribalwear") == 0);
    CHECK(craftOnMap(sim, "Cloth") == 40);
    CHECK(craftOnMap(sim, "Leather_Plain") == 40);
    // 30 more cloth: now one of them covers it, and it is the cloth.
    craftSpawn(sim, "Cloth", CellPos{21, 21, 0}, 30);
    CRAFT_REQUIRE(craftRunUntil(sim, 20000, [&] { return craftOnMap(sim, "Apparel_Tribalwear") == 1; }) >= 0);
    const Entity wear = craftFind(sim, "Apparel_Tribalwear");
    CRAFT_REQUIRE(wear != kNoEntity);
    CHECK(sim.things().registry().get<MadeOf>(wear).stuff == sim.defs().idOf("Things", "Cloth"));
    CHECK(craftOnMap(sim, "Cloth") == 10);
    CHECK(craftOnMap(sim, "Leather_Plain") == 40);
    // The piece has hit points and a worth that follows its stuff and quality (ApparelStats).
    CHECK(sim.things().registry().get<HitPoints>(wear).max > 0);
}

TEST_CASE("Crafting: the hand tailor bench is half as fast as a bench at full speed") {
    const auto timeAt = [](const char* bench) {
        Sim sim = craftSim();
        const Entity table = craftSpawn(sim, bench, CellPos{20, 20, 0});
        craftSpawn(sim, "TorchLamp", CellPos{17, 17, 0});
        craftSpawn(sim, "Cloth", CellPos{19, 19, 0}, 40);
        craftCrafter(sim, CellPos{18, 18, 0}, 8);
        sim.submit(UnlockResearchCommand{"ComplexClothing"});
        if (table == kNoEntity || craftAdd(sim, table, "Make_Apparel_Tuque", BillMode::Count, 1) == 0) {
            return -1;
        }
        return craftRunUntil(sim, 30000, [&] { return craftOnMap(sim, "Apparel_Tuque") == 1; });
    };
    const int hand = timeAt("TailorBenchHand");
    CHECK(hand > 0);
    // The electric one has no power in the test, so it can't be compared: the speed factor is what the data says.
    // A tuque is 1,728 s of work: at crafting 8 (speed 1.0) the hand bench takes about 3,456 s, plus walking.
    CHECK(hand > 3300);
    CHECK(hand < 4200);
}

TEST_CASE("Crafting: an electric tailor bench without power works at half speed instead of stopping") {
    const auto timeAt = [](const char* bench) {
        Sim sim = craftSim();
        const Entity table = craftSpawn(sim, bench, CellPos{20, 20, 0});
        craftSpawn(sim, "TorchLamp", CellPos{17, 17, 0});
        craftSpawn(sim, "Cloth", CellPos{19, 19, 0}, 40);
        craftCrafter(sim, CellPos{18, 18, 0}, 8);
        sim.submit(UnlockResearchCommand{"ComplexClothing"});
        sim.submit(UnlockResearchCommand{"Electricity"});
        if (table == kNoEntity || craftAdd(sim, table, "Make_Apparel_Tuque", BillMode::Count, 1) == 0) {
            return -1;
        }
        sim.step();  // the power grid finds the bench has no net
        const bool electricBench = std::string(bench) == "TailorBenchElectric";
        if (electricBench) {
            CHECK_FALSE(sim.jobs().buildingRuns(table));
        }
        CHECK(sim.jobs().buildingWorks(table));
        return craftRunUntil(sim, 30000, [&] { return craftOnMap(sim, "Apparel_Tuque") == 1; });
    };
    const int handTime = timeAt("TailorBenchHand");
    const int electric = timeAt("TailorBenchElectric");
    CHECK(handTime > 0);
    CHECK(electric > 0);
    CHECK(electric == doctest::Approx(handTime).epsilon(0.1));  // both at 0.5, one for want of power

    // A bench with no unpowered speed does nothing without power: the factor is 0 for it.
    Sim sim = craftSim();
    const Entity hand = craftSpawn(sim, "TailorBenchHand", CellPos{20, 20, 0});
    CHECK(sim.jobs().unpoweredFactor(hand) == 0.0f);
    CHECK(sim.jobs().buildingWorks(hand));  // it needs no power
    const Entity tailor = craftSpawn(sim, "TailorBenchElectric", CellPos{30, 20, 0});
    CHECK(sim.jobs().unpoweredFactor(tailor) == doctest::Approx(0.5f));
}

TEST_CASE("Crafting: the stonecutter turns a chunk into blocks") {
    Sim sim = craftSim();
    sim.submit(UnlockResearchCommand{"Stonecutting"});
    const Entity table = craftSpawn(sim, "StonecutterTable", CellPos{20, 20, 0});
    CRAFT_REQUIRE(table != kNoEntity);
    craftSpawn(sim, "TorchLamp", CellPos{17, 17, 0});
    craftCrafter(sim, CellPos{18, 18, 0}, 8);
    craftSpawn(sim, "ChunkGranite", CellPos{19, 19, 0}, 1);
    CRAFT_REQUIRE(craftAdd(sim, table, "MakeBlocksGranite", BillMode::Count, 1) != 0);
    CRAFT_REQUIRE(craftRunUntil(sim, 12000, [&] { return craftOnMap(sim, "BlocksGranite") == 20; }) >= 0);
    CHECK(craftOnMap(sim, "ChunkGranite") == 0);
    // Blocks have no quality and no stuff of their own.
    const Entity blocks = craftFind(sim, "BlocksGranite");
    CRAFT_REQUIRE(blocks != kNoEntity);
    CHECK(!sim.things().registry().all_of<Quality>(blocks));
    CHECK(!sim.things().registry().all_of<MadeOf>(blocks));
}

TEST_CASE("Crafting: the electric stove cooks too") {
    Sim sim = craftSim();
    const Entity stove = craftSpawn(sim, "ElectricStove", CellPos{20, 20, 0});
    CRAFT_REQUIRE(stove != kNoEntity);
    const Recipe* simple = sim.jobs().bills().recipe(sim.defs().idOf("Recipes", "CookMealSimple"));
    CRAFT_REQUIRE(simple != nullptr);
    CHECK(sim.jobs().bills().canMake(sim.defs().idOf("Things", "ElectricStove"), *simple));
    CHECK(craftAdd(sim, stove, "CookMealSimple", BillMode::Count, 2) != 0);
}

TEST_CASE("Crafting: a bill's skill range, ingredient radius and ingredient filter decide who works and with what") {
    Sim sim = craftSim();
    const Entity spot = craftSpawn(sim, "CraftingSpot", CellPos{20, 20, 0});
    CRAFT_REQUIRE(spot != kNoEntity);
    craftSpawn(sim, "TorchLamp", CellPos{17, 17, 0});
    craftCrafter(sim, CellPos{18, 18, 0}, 3);
    craftSpawn(sim, "WoodLog", CellPos{50, 50, 0}, 60);  // far away
    const uint32_t bill = craftAdd(sim, spot, "MakeClub", BillMode::Count, 1);
    CRAFT_REQUIRE(bill != 0);

    const auto options = [&](int skillMin, int skillMax, float radius, std::vector<std::string> refused = {}) {
        SetBillOptionsCommand c;
        c.building = spot;
        c.bill = bill;
        c.skillMin = skillMin;
        c.skillMax = skillMax;
        c.radius = radius;
        c.disallowed = std::move(refused);
        sim.submit(c);
        sim.step();
        return !craftFailed(sim);
    };

    // The crafter is skilled 3: a bill for skill 5 and up is not theirs.
    CHECK(options(5, 20, 0.0f));
    sim.stepMany(400);
    CHECK(craftOnMap(sim, "WoodLog") == 60);
    CHECK(sim.things().registry().get<Worker>(craftFind(sim, "Human")).job.type != JobType::DoBill);
    // A radius of 10 m leaves the wood 40 m away out of reach...
    CHECK(options(0, 20, 10.0f));
    sim.stepMany(400);
    CHECK(craftOnMap(sim, "WoodLog") == 60);
    // ...and a filter that refuses wood leaves nothing to use.
    CHECK(options(0, 20, 0.0f, {"WoodLog"}));
    sim.stepMany(400);
    CHECK(craftOnMap(sim, "WoodLog") == 60);
    // Without them the club gets made.
    CHECK(options(0, 20, 0.0f));
    CRAFT_REQUIRE(craftRunUntil(sim, 12000, [&] { return craftOnMap(sim, "Melee_Club") == 1; }) >= 0);

    // Bad settings are refused.
    CHECK(!options(8, 4, 0.0f));
    CHECK(!options(0, 25, 0.0f));
    CHECK(!options(0, 20, -1.0f));
    CHECK(!options(0, 20, 0.0f, {"NoSuchItem"}));
    CHECK(!options(0, 20, 0.0f, {"Wall"}));  // a building is not an ingredient
    SetBillOptionsCommand noBill;
    noBill.building = spot;
    noBill.bill = 99;
    sim.submit(noBill);
    sim.step();
    CHECK(craftFailed(sim));
}

TEST_CASE("Crafting: bills can be moved up and down the list") {
    Sim sim = craftSim();
    const Entity spot = craftSpawn(sim, "CraftingSpot", CellPos{20, 20, 0});
    CRAFT_REQUIRE(spot != kNoEntity);
    const uint32_t a = craftAdd(sim, spot, "MakeClub", BillMode::Count, 1);
    const uint32_t b = craftAdd(sim, spot, "MakeKnife", BillMode::Count, 1);
    const uint32_t c = craftAdd(sim, spot, "MakeClub", BillMode::Forever, 0);
    CRAFT_REQUIRE(a != 0 && b != 0 && c != 0);
    const auto order = [&] {
        std::vector<uint32_t> ids;
        for (const Bill& bill : sim.jobs().bills().billsOf(spot)->bills) {
            ids.push_back(bill.id);
        }
        return ids;
    };
    CHECK(order() == std::vector<uint32_t>{a, b, c});
    sim.submit(MoveBillCommand{spot, c, 0});
    sim.step();
    CHECK(order() == std::vector<uint32_t>{c, a, b});
    sim.submit(MoveBillCommand{spot, c, 99});  // past the end is the last place
    sim.step();
    CHECK(order() == std::vector<uint32_t>{a, b, c});
    sim.submit(MoveBillCommand{spot, a, 1});
    sim.step();
    CHECK(order() == std::vector<uint32_t>{b, a, c});
    sim.submit(MoveBillCommand{spot, 77, 0});
    sim.step();
    CHECK(craftFailed(sim));
}

TEST_CASE("Crafting: an 'until you have' bill with a restart stock waits until the stock has fallen") {
    Sim sim = craftSim();
    const Entity spot = craftSpawn(sim, "CraftingSpot", CellPos{20, 20, 0});
    CRAFT_REQUIRE(spot != kNoEntity);
    craftSpawn(sim, "WoodLog", CellPos{19, 19, 0}, 75);
    craftSpawn(sim, "WoodLog", CellPos{19, 20, 0}, 75);
    const uint32_t bill = craftAdd(sim, spot, "MakeClub", BillMode::UntilHave, 2);
    CRAFT_REQUIRE(bill != 0);
    SetBillOptionsCommand c;
    c.building = spot;
    c.bill = bill;
    c.unpauseAt = 1;  // once 2 are made, start again only when under 1
    sim.submit(c);
    sim.step();
    CRAFT_REQUIRE(!craftFailed(sim));
    BillSystem& bills = sim.jobs().bills();
    const Bill* b = bills.find(spot, bill);
    CRAFT_REQUIRE(b != nullptr);
    CHECK(bills.shouldDo(*b));

    // Two clubs lie on the map: the bill is satisfied...
    craftSpawn(sim, "Melee_Club", CellPos{30, 30, 0});
    craftSpawn(sim, "Melee_Club", CellPos{31, 30, 0});
    sim.stepMany(kCraftUpkeep);
    CHECK(b->satisfied);
    CHECK(!bills.shouldDo(*b));
    // ...one is lost and it stays paused, because one is not under 1...
    sim.submit(DestroyThingCommand{craftFind(sim, "Melee_Club")});
    sim.stepMany(kCraftUpkeep);
    CHECK(craftOnMap(sim, "Melee_Club") == 1);
    CHECK(b->satisfied);
    CHECK(!bills.shouldDo(*b));
    // ...and with none left it starts again.
    sim.submit(DestroyThingCommand{craftFind(sim, "Melee_Club")});
    sim.stepMany(kCraftUpkeep);
    CHECK(craftOnMap(sim, "Melee_Club") == 0);
    CHECK(!b->satisfied);
    CHECK(bills.shouldDo(*b));
}

TEST_CASE("Crafting: a bill can send its products to a stockpile") {
    Sim sim = craftSim();
    const Entity spot = craftSpawn(sim, "CraftingSpot", CellPos{20, 20, 0});
    CRAFT_REQUIRE(spot != kNoEntity);
    craftSpawn(sim, "TorchLamp", CellPos{17, 17, 0});
    craftCrafter(sim, CellPos{18, 18, 0}, 8);
    craftSpawn(sim, "WoodLog", CellPos{19, 19, 0}, 40);
    // Two stockpiles; the products are to go to the farther one.
    sim.submit(CreateStockpileCommand{CellRect{24, 24, 2, 2, 0}, StoragePriority::Normal});
    sim.submit(CreateStockpileCommand{CellRect{40, 40, 2, 2, 0}, StoragePriority::Normal});
    sim.step();
    CRAFT_REQUIRE(sim.zones().stockpiles().size() == 2);
    const uint32_t far = sim.zones().stockpiles()[1].id;
    const uint32_t bill = craftAdd(sim, spot, "MakeClub", BillMode::Count, 1);
    CRAFT_REQUIRE(bill != 0);
    SetBillOptionsCommand c;
    c.building = spot;
    c.bill = bill;
    c.storeZone = far;
    sim.submit(c);
    sim.step();
    CRAFT_REQUIRE(!craftFailed(sim));
    sim.submit(SetBillOptionsCommand{spot, bill, 0, 20, 0.0f, 9999, 0, {}});  // no such stockpile
    sim.step();
    CHECK(craftFailed(sim));
    CRAFT_REQUIRE(craftRunUntil(sim, 12000, [&] { return craftOnMap(sim, "Melee_Club") == 1; }) >= 0);
    const Entity club = craftFind(sim, "Melee_Club");
    CRAFT_REQUIRE(club != kNoEntity);
    CHECK(sim.things().registry().all_of<StoreIn>(club));
    // A hauler (the crafter, with nothing else to do) carries it to the far stockpile.
    const auto stored = [&] {
        const Position* pos = sim.things().registry().try_get<Position>(club);
        return pos != nullptr && sim.zones().zoneAt(cellOf(*pos)) == far;
    };
    CHECK(craftRunUntil(sim, 6000, stored) >= 0);
    sim.stepMany(kCraftUpkeep);
    CHECK(!sim.things().registry().all_of<StoreIn>(club));  // the mark goes once it is there
}

TEST_CASE("Crafting: the new commands round trip through replay files and the same commands give the same game") {
    const auto run = [] {
        auto sim = std::make_unique<Sim>(SimSettings{5, 64, 0, 0, true}, craftDefs());
        sim->submit(UnlockResearchCommand{"Smithing"});
        const Entity spot = craftSpawn(*sim, "CraftingSpot", CellPos{20, 20, 0});
        craftSpawn(*sim, "TorchLamp", CellPos{17, 17, 0});
        craftCrafter(*sim, CellPos{18, 18, 0}, 14);
        craftSpawn(*sim, "WoodLog", CellPos{19, 19, 0}, 75);
        craftSpawn(*sim, "Steel", CellPos{19, 20, 0}, 75);
        craftAdd(*sim, spot, "MakeClub", BillMode::Count, 2);
        craftAdd(*sim, spot, "MakeKnife", BillMode::Count, 2);
        sim->submit(SetBillOptionsCommand{spot, 2, 2, 18, 30.0f, 0, 3, {"Silver"}});
        sim->submit(MoveBillCommand{spot, 2, 0});
        sim->stepMany(8000);
        return sim;
    };
    const std::unique_ptr<Sim> firstRun = run();
    const std::unique_ptr<Sim> secondRun = run();
    const Sim& first = *firstRun;
    const Sim& second = *secondRun;
    CHECK(first.stateHash() == second.stateHash());
    CHECK(craftOnMap(first, "Melee_Club") + craftOnMap(first, "Melee_Knife") > 0);
    // Same qualities.
    const auto qualities = [](const Sim& sim) {
        std::vector<int> out;
        sim.things().registry().view<const Quality>().each([&](const Quality& q) { out.push_back(q.level); });
        return out;
    };
    CHECK(qualities(first) == qualities(second));
    CHECK(!qualities(first).empty());

    // The commands go through the replay text and back.
    Replay replay;
    LoggedCommand options;
    options.step = 10;
    options.command = SetBillOptionsCommand{kNoEntity, 2, 2, 18, 30.5f, 7, 3, {"Silver", "Steel"}};
    LoggedCommand move;
    move.step = 11;
    move.command = MoveBillCommand{kNoEntity, 2, 1};
    replay.commands = {options, move};
    Replay loaded;
    std::string error;
    CHECK(readReplay(writeReplay(replay), loaded, error));
    CHECK(error.empty());
    CHECK(writeReplay(loaded) == writeReplay(replay));
    CRAFT_REQUIRE(loaded.commands.size() == 2);
    const auto* got = std::get_if<SetBillOptionsCommand>(&loaded.commands[0].command);
    CRAFT_REQUIRE(got != nullptr);
    CHECK(got->skillMin == 2);
    CHECK(got->skillMax == 18);
    CHECK(got->radius == doctest::Approx(30.5f));
    CHECK(got->storeZone == 7);
    CHECK(got->unpauseAt == 3);
    CHECK(got->disallowed == std::vector<std::string>{"Silver", "Steel"});
    CHECK(std::get_if<MoveBillCommand>(&loaded.commands[1].command) != nullptr);
}

TEST_CASE("Crafting: a save in the middle of a run, with bill settings and a marked product, loads back the same") {
    Sim original = craftSim();
    original.submit(UnlockResearchCommand{"Smithing"});
    const Entity spot = craftSpawn(original, "CraftingSpot", CellPos{20, 20, 0});
    craftSpawn(original, "TorchLamp", CellPos{17, 17, 0});
    craftCrafter(original, CellPos{18, 18, 0}, 9);
    craftSpawn(original, "WoodLog", CellPos{19, 19, 0}, 75);
    original.submit(CreateStockpileCommand{CellRect{40, 40, 2, 2, 0}, StoragePriority::Normal});
    original.step();
    CRAFT_REQUIRE(!original.zones().stockpiles().empty());
    const uint32_t bill = craftAdd(original, spot, "MakeClub", BillMode::Count, 3);
    CRAFT_REQUIRE(bill != 0);
    original.submit(SetBillOptionsCommand{spot, bill, 1, 19, 45.0f, original.zones().stockpiles()[0].id, 2, {"Steel"}});
    // Save the moment the first club lies on the floor marked for the stockpile.
    CRAFT_REQUIRE(craftRunUntil(original, 12000, [&] { return craftFind(original, "Melee_Club") != kNoEntity; }) >= 0);
    const std::vector<uint8_t> bytes = saveSim(original);
    SaveLoadResult loaded = loadSim(bytes, craftDefs());
    CRAFT_REQUIRE(loaded.error.empty());
    CRAFT_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->stateHash() == original.stateHash());
    CHECK(saveSim(*loaded.sim) == bytes);
    const Bill* kept = loaded.sim->jobs().bills().find(spot, bill);
    CRAFT_REQUIRE(kept != nullptr);
    CHECK(kept->skillMin == 1);
    CHECK(kept->skillMax == 19);
    CHECK(kept->radius == doctest::Approx(45.0f));
    CHECK(kept->unpauseAt == 2);
    CHECK(kept->disallowed.size() == 1);
    const Entity club = craftFind(*loaded.sim, "Melee_Club");
    CRAFT_REQUIRE(club != kNoEntity);
    CHECK(loaded.sim->things().registry().all_of<StoreIn>(club));
    original.stepMany(6000);
    loaded.sim->stepMany(6000);
    CHECK(loaded.sim->stateHash() == original.stateHash());  // the quality stream carried on from the same place
}
