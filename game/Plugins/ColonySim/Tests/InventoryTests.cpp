// Inventory and carry weight (docs/design/inventory.md), with the game's own data.
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure. The expression is
// evaluated once (it may submit a command).
#define INV_REQUIRE(...)                                    \
    do {                                                    \
        const bool invOk = static_cast<bool>(__VA_ARGS__);  \
        CHECK_MESSAGE(invOk, #__VA_ARGS__);                 \
        if (!invOk) {                                       \
            return;                                         \
        }                                                   \
    } while (false)

namespace {

std::shared_ptr<const DefDatabase> invDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return db;
    }();
    return defs;
}

std::unique_ptr<Sim> invSim() { return std::make_unique<Sim>(SimSettings{7, 32, 0, 0, true}, invDefs()); }

struct InvSkills {
    int mining = 0;
    int plants = 0;
    int melee = 0;
};

Entity invColonist(Sim& sim, CellPos cell, InvSkills skills = {}, std::vector<std::string> traits = {}, bool protagonist = false) {
    const Entity pawn = sim.things().spawn("Human", cell);
    sim.jobs().addWorker(pawn);
    Colonist colonist;
    colonist.firstName = "Test";
    colonist.protagonist = protagonist;
    colonist.skills.push_back(SkillRecord{"Mining", skills.mining, Passion::None, 0.0f, false});
    colonist.skills.push_back(SkillRecord{"Plants", skills.plants, Passion::None, 0.0f, false});
    colonist.skills.push_back(SkillRecord{"Melee", skills.melee, Passion::None, 0.0f, false});
    for (const std::string& t : traits) {
        colonist.traits.push_back(TraitRecord{t, 0});
    }
    sim.things().registry().emplace<Colonist>(pawn, colonist);
    // Drafted, so it stands still where it was put.
    sim.submit(DraftCommand{pawn, true});
    return pawn;
}

DefId invDef(const Sim& sim, const char* name) { return sim.defs().idOf("Things", name); }

// Submits and steps once; true if the command went through.
bool invRun(Sim& sim, Command command) {
    const uint64_t id = sim.submit(std::move(command));
    sim.step();
    return std::none_of(sim.events().begin(), sim.events().end(),
                        [&](const Event& e) { return e.type == EventType::CommandFailed && e.command == id; });
}

const std::vector<Entity>& invBag(const Sim& sim, Entity pawn) {
    static const std::vector<Entity> none;
    const Gear* gear = sim.things().registry().try_get<Gear>(pawn);
    return gear != nullptr ? gear->inventory : none;
}

int invCount(const Sim& sim, Entity item) { return sim.things().registry().get<StackCount>(item).count; }

}  // namespace

TEST_CASE("Inventory: colonists carry 50 kg, the protagonist 100, and tough and brawler add to it") {
    auto sim = invSim();
    const Entity plain = invColonist(*sim, CellPos{5, 5, 0});
    const Entity hero = invColonist(*sim, CellPos{7, 5, 0}, {}, {}, true);
    const Entity tough = invColonist(*sim, CellPos{9, 5, 0}, {}, {"Tough"});
    const Entity brawler = invColonist(*sim, CellPos{11, 5, 0}, {}, {"Brawler", "Tough"});
    CHECK(sim->jobs().carryCapacity(plain) == doctest::Approx(50.0).epsilon(0.001));
    CHECK(sim->jobs().carryCapacity(hero) == doctest::Approx(100.0).epsilon(0.001));
    CHECK(sim->jobs().carryCapacity(tough) == doctest::Approx(65.0).epsilon(0.001));
    CHECK(sim->jobs().carryCapacity(brawler) == doctest::Approx(75.0).epsilon(0.001));
    // Steel is 0.5 kg: 100 fit in an empty-handed colonist's 50 kg.
    CHECK(sim->jobs().carryLimit(plain, invDef(*sim, "Steel")) == 100);
}

TEST_CASE("Inventory: miners carry mined things lighter, farmers crops and wood, fighters everything") {
    auto sim = invSim();
    const Entity novice = invColonist(*sim, CellPos{5, 5, 0});
    const Entity miner = invColonist(*sim, CellPos{7, 5, 0}, {20, 0, 0});
    const Entity farmer = invColonist(*sim, CellPos{9, 5, 0}, {0, 10, 0});
    const Entity fighter = invColonist(*sim, CellPos{11, 5, 0}, {20, 20, 20});
    const DefId granite = invDef(*sim, "ChunkGranite");
    const DefId steel = invDef(*sim, "Steel");
    const DefId wood = invDef(*sim, "WoodLog");
    const DefId medicine = invDef(*sim, "MedicineIndustrial");
    const JobSystem& jobs = sim->jobs();
    CHECK(jobs.massFor(novice, granite) == doctest::Approx(25.0));
    CHECK(jobs.massFor(miner, granite) == doctest::Approx(12.5));  // half at Mining 20
    CHECK(jobs.massFor(miner, steel) == doctest::Approx(0.25));    // steel is mined too
    CHECK(jobs.massFor(miner, wood) == doctest::Approx(0.4));      // wood is not
    CHECK(jobs.massFor(farmer, wood) == doctest::Approx(0.4 * 0.75));
    CHECK(jobs.massFor(farmer, invDef(*sim, "RawRice")) == doctest::Approx(0.03 * 0.75));
    CHECK(jobs.massFor(farmer, granite) == doctest::Approx(25.0));
    // Mining 20 and Melee 20: 0.5 x 0.8 of a granite chunk; Melee alone lightens medicine.
    CHECK(jobs.massFor(fighter, granite) == doctest::Approx(25.0 * 0.5 * 0.8));
    CHECK(jobs.massFor(fighter, medicine) == doctest::Approx(0.5 * 0.8));
    // A miner hauls two chunks where a novice hauls one... and none ever fewer than one.
    CHECK(jobs.carryLimit(novice, granite) == 2);
    CHECK(jobs.carryLimit(miner, granite) == 4);
}

TEST_CASE("Inventory: Take puts items in the bag up to the weight limit, Drop and DropAll put them down") {
    auto sim = invSim();
    const Entity pawn = invColonist(*sim, CellPos{10, 10, 0});
    const Entity steel = sim->things().spawn("Steel", CellPos{11, 10, 0}, 75);  // 37.5 kg
    const Entity granite = sim->things().spawn("ChunkGranite", CellPos{10, 11, 0}, 1);
    const Entity far = sim->things().spawn("Steel", CellPos{20, 10, 0}, 10);
    INV_REQUIRE(steel != kNoEntity);
    INV_REQUIRE(granite != kNoEntity);
    sim->step();

    CHECK_FALSE(invRun(*sim, InventoryCommand{pawn, "Take", far, 0}));  // too far
    CHECK_FALSE(invRun(*sim, InventoryCommand{pawn, "Fly", steel, 0}));

    // Part of the stack: 20 of 75.
    INV_REQUIRE(invRun(*sim, InventoryCommand{pawn, "Take", steel, 20}));
    INV_REQUIRE(invBag(*sim, pawn).size() == 1);
    const Entity bagged = invBag(*sim, pawn)[0];
    CHECK(invCount(*sim, bagged) == 20);
    CHECK(invCount(*sim, steel) == 55);
    CHECK_FALSE(sim->things().spawned(bagged));
    CHECK(sim->jobs().loadMass(pawn) == doctest::Approx(10.0));
    // The rest joins the same stack in the bag.
    INV_REQUIRE(invRun(*sim, InventoryCommand{pawn, "Take", steel, 0}));
    CHECK(invBag(*sim, pawn).size() == 1);
    CHECK(invCount(*sim, bagged) == 75);
    CHECK_FALSE(sim->things().exists(steel));
    CHECK(sim->jobs().loadMass(pawn) == doctest::Approx(37.5));
    // A 25 kg chunk no longer fits in the 12.5 kg left.
    CHECK_FALSE(invRun(*sim, InventoryCommand{pawn, "Take", granite, 0}));
    CHECK(sim->things().spawned(granite));
    // The load slows it: 37.5 of 50 kg is 0.75 of capacity, past the free 0.2.
    CHECK(sim->jobs().loadSpeedFactor(pawn) == doctest::Approx(1.0 - 0.25 * (0.75 - 0.2) / 0.8).epsilon(0.001));

    // The snapshot shows the bag and the load.
    Snapshot snap;
    sim->writeSnapshot(snap);
    const ThingView* view = snap.find(pawn);
    INV_REQUIRE(view != nullptr);
    CHECK(view->loadKg == doctest::Approx(37.5f));
    CHECK(view->capacityKg == doctest::Approx(50.0f).epsilon(0.001));
    INV_REQUIRE(view->inventoryCount == 1);
    CHECK(snap.inventory[view->firstInventory].item == bagged);
    CHECK(snap.inventory[view->firstInventory].count == 75);
    CHECK(snap.inventory[view->firstInventory].massKg == doctest::Approx(37.5f));

    // Drop 25: they go on the ground, 50 stay.
    INV_REQUIRE(invRun(*sim, InventoryCommand{pawn, "Drop", bagged, 25}));
    CHECK(invCount(*sim, bagged) == 50);
    int onGround = 0;
    for (const Entity item : sim->things().itemsAt(CellPos{10, 10, 0})) {
        onGround += sim->things().registry().get<Thing>(item).def == invDef(*sim, "Steel") ? invCount(*sim, item) : 0;
    }
    CHECK(onGround == 25);
    CHECK_FALSE(invRun(*sim, InventoryCommand{pawn, "Drop", granite, 0}));  // not in the bag
    INV_REQUIRE(invRun(*sim, InventoryCommand{pawn, "DropAll", kNoEntity, 0}));
    CHECK(invBag(*sim, pawn).empty());
    CHECK(sim->things().spawned(bagged));
    CHECK(sim->jobs().loadMass(pawn) == doctest::Approx(0.0));
    CHECK(sim->jobs().loadSpeedFactor(pawn) == doctest::Approx(1.0f));
}

TEST_CASE("Inventory: a light load costs nothing, a full one a quarter of the speed, an overload more") {
    auto sim = invSim();
    const Entity pawn = invColonist(*sim, CellPos{10, 10, 0});
    const Entity hero = invColonist(*sim, CellPos{12, 10, 0}, {}, {}, true);
    const Entity a = sim->things().spawn("Steel", CellPos{10, 11, 0}, 16);  // 8 kg: under the free 10 kg
    sim->step();
    INV_REQUIRE(invRun(*sim, InventoryCommand{pawn, "Take", a, 0}));
    CHECK(sim->jobs().loadSpeedFactor(pawn) == doctest::Approx(1.0f));
    const Entity b = sim->things().spawn("Steel", CellPos{10, 11, 0}, 75);
    const Entity c = sim->things().spawn("Steel", CellPos{11, 11, 0}, 75);
    sim->step();
    INV_REQUIRE(invRun(*sim, InventoryCommand{pawn, "Take", b, 0}));
    // 75 + 16 = 91 steel is 45.5 kg; 9 more fit and then no more.
    INV_REQUIRE(invRun(*sim, InventoryCommand{pawn, "Take", c, 0}));
    CHECK(sim->jobs().loadMass(pawn) == doctest::Approx(50.0));
    CHECK(invCount(*sim, c) == 66);
    CHECK(sim->jobs().loadSpeedFactor(pawn) == doctest::Approx(0.75f).epsilon(0.001));
    CHECK_FALSE(invRun(*sim, InventoryCommand{pawn, "Take", c, 1}));
    // The protagonist carries twice as much at the same speed.
    INV_REQUIRE(invRun(*sim, InventoryCommand{hero, "Take", c, 0}));  // 33 kg of 100
    CHECK(sim->jobs().loadSpeedFactor(hero) > 0.9f);
    // Overloaded (as if a trait were lost): slower still, never under a fifth.
    Colonist& colonist = sim->things().registry().get<Colonist>(hero);
    colonist.protagonist = false;  // 33 kg of 50 now; put more in by hand
    const Entity d = sim->things().spawn("ChunkGranite", CellPos{14, 11, 0}, 1);
    sim->step();
    Gear& gear = sim->things().registry().get<Gear>(hero);
    sim->things().despawn(d);
    gear.inventory.push_back(d);
    sim->things().registry().emplace<EquippedBy>(d, EquippedBy{hero});  // 58 kg of 50
    CHECK(sim->jobs().loadSpeedFactor(hero) == doctest::Approx(1.0 - 0.25 - (58.0 / 50.0 - 1.0)).epsilon(0.01));
    for (int i = 0; i < 6; ++i) {
        const Entity e = sim->things().spawn("ChunkGranite", CellPos{14, 11, 0}, 1);
        sim->things().despawn(e);
        gear.inventory.push_back(e);
        sim->things().registry().emplace<EquippedBy>(e, EquippedBy{hero});
    }
    CHECK(sim->jobs().loadSpeedFactor(hero) == doctest::Approx(0.2f));
}

TEST_CASE("Inventory: the bag is worn gear's company: stripped with it, and kept by a save") {
    auto sim = invSim();
    const Entity pawn = invColonist(*sim, CellPos{10, 10, 0});
    const Entity meds = sim->things().spawn("MedicineIndustrial", CellPos{10, 11, 0}, 5);
    sim->step();
    INV_REQUIRE(invRun(*sim, InventoryCommand{pawn, "Take", meds, 0}));
    INV_REQUIRE(invBag(*sim, pawn).size() == 1);

    const std::string json = saveToJson(*sim);
    CHECK(json.find("\"inventory\"") != std::string::npos);
    SaveLoadResult loaded = loadSim(saveSim(*sim), invDefs());
    INV_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->stateHash() == sim->stateHash());
    INV_REQUIRE(invBag(*loaded.sim, pawn).size() == 1);
    CHECK(invBag(*loaded.sim, pawn)[0] == meds);
    CHECK(invCount(*loaded.sim, meds) == 5);
    CHECK(loaded.sim->jobs().loadMass(pawn) == doctest::Approx(sim->jobs().loadMass(pawn)));

    // Stripped: everything falls out where it stands.
    sim->combat().dropGear(pawn);
    CHECK(sim->things().spawned(meds));
    CHECK_FALSE(sim->things().registry().all_of<EquippedBy>(meds));
}
