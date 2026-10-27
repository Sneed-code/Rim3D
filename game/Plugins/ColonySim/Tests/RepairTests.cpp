#include "ColonySim/Apparel.h"
#include "ColonySim/Bills.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Repair.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"
#include "FuelTestHelper.h"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define REPAIR_REQUIRE(...)                  \
    do {                                     \
        const bool repairOk = (__VA_ARGS__); \
        CHECK(repairOk);                     \
        if (!repairOk) {                     \
            return;                          \
        }                                    \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> repairDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

Sim repairSim() { return Sim(SimSettings{5, 64, 0, 0, true}, repairDefs()); }

DefId repairDef(const Sim& sim, const std::string& name) { return sim.defs().idOf("Things", name); }

Entity repairSpawn(Sim& sim, const std::string& def, CellPos cell, int count = 1) {
    sim.submit(SpawnThingCommand{def, cell, count});
    sim.step();
    for (const Event& e : sim.events()) {
        if (e.type == EventType::ThingSpawned && e.cell == cell && sim.defs().get(e.def).defName() == def) {
            return testFueled(sim, e.thing);
        }
    }
    return kNoEntity;
}

Entity repairCrafter(Sim& sim, CellPos cell, int level = 8) {
    const Entity pawn = repairSpawn(sim, "Human", cell);
    Colonist colonist;
    colonist.skills.push_back(SkillRecord{"Crafting", level, Passion::None, 0.0f, false});
    sim.things().registry().emplace<Colonist>(pawn, std::move(colonist));
    return pawn;
}

// A wood-fired generator and a wire that power a machining table at (20,20), 3 cells wide.
void repairPower(Sim& sim) {
    testFueled(sim, sim.things().spawn("WoodFiredGenerator", CellPos{24, 20, 0}));
    sim.things().spawn("PowerConduit", CellPos{23, 20, 0});
}

void repairDamage(Sim& sim, Entity item, int hitPoints) { sim.things().registry().get<HitPoints>(item).current = hitPoints; }

int repairStock(const Sim& sim, const std::string& def) { return sim.jobs().bills().stockOf(repairDef(sim, def)); }

bool repairFailed(const Sim& sim) {
    return std::any_of(sim.events().begin(), sim.events().end(), [](const Event& e) { return e.type == EventType::CommandFailed; });
}

bool repairSaw(const Sim& sim, EventType type) {
    return std::any_of(sim.events().begin(), sim.events().end(), [type](const Event& e) { return e.type == type; });
}

// Steps until the check holds; the number of steps, or -1.
template <typename F>
int repairRunUntil(Sim& sim, int limit, F done) {
    for (int i = 0; i < limit; ++i) {
        if (done()) {
            return i;
        }
        sim.step();
    }
    return done() ? limit : -1;
}

}  // namespace

TEST_CASE("Repair: every upgrade part has a spec, a recipe at the machining table and a slot; the benches name what they repair") {
    Sim sim = repairSim();
    const RepairSystem& repair = sim.repair();
    CHECK(repair.upgrades().size() >= 5);
    std::vector<std::string> slots;
    for (const UpgradeSpec& spec : repair.upgrades()) {
        const std::string name = sim.defs().get(spec.part).defName();
        CHECK_MESSAGE(spec.work > 0.0f, name << " has no fitting work");
        const bool fitsAGun = spec.primary || spec.sidearm;
        CHECK_MESSAGE(fitsAGun, name << " fits no gun");
        CHECK_MESSAGE(std::find(slots.begin(), slots.end(), spec.slot) == slots.end(), name << " shares its slot");
        slots.push_back(spec.slot);
        bool made = false;
        for (const Recipe& recipe : sim.jobs().bills().recipes()) {
            for (const RecipeProduct& product : recipe.products) {
                made = made || product.item == spec.part;
            }
        }
        CHECK_MESSAGE(made, name << " has no recipe");
    }
    CHECK(repair.benchRepairs(repairDef(sim, "FueledSmithy"), "Weapons"));
    CHECK(repair.benchRepairs(repairDef(sim, "ElectricSmithy"), "Weapons"));
    CHECK(repair.benchRepairs(repairDef(sim, "TailorBenchHand"), "Apparel"));
    CHECK(!repair.benchRepairs(repairDef(sim, "TailorBenchHand"), "Weapons"));
    CHECK(repair.benchFits(repairDef(sim, "MachiningTable")));
    CHECK(!repair.benchFits(repairDef(sim, "FueledSmithy")));
    // Work types list the two givers.
    std::vector<std::string> tasks;
    for (const DefId id : sim.defs().idsOfKind("WorkTypes")) {
        for (size_t i = 0; i < sim.defs().get(id).count("tasks"); ++i) {
            tasks.push_back(sim.defs().get(id).textAt("tasks", i));
        }
    }
    CHECK(std::find(tasks.begin(), tasks.end(), "RepairWeapons") != tasks.end());
    CHECK(std::find(tasks.begin(), tasks.end(), "RepairApparel") != tasks.end());
}

TEST_CASE("Repair: a damaged flak vest is mended at a tailor bench for half the cloth of a full repair") {
    Sim sim = repairSim();
    const Entity bench = repairSpawn(sim, "TailorBenchHand", CellPos{20, 20, 0});
    REPAIR_REQUIRE(bench != kNoEntity);
    repairSpawn(sim, "TorchLamp", CellPos{17, 17, 0});
    repairCrafter(sim, CellPos{18, 18, 0});
    repairSpawn(sim, "Cloth", CellPos{22, 22, 0}, 30);
    const Entity vest = repairSpawn(sim, "Apparel_FlakVest", CellPos{19, 19, 0});
    REPAIR_REQUIRE(vest != kNoEntity);
    const int max = sim.things().registry().get<HitPoints>(vest).max;
    repairDamage(sim, vest, max / 2);

    // Full repair of a 4 kg vest is 4 cloth (apparel: 1 per kg); half of it missing is 2.
    const RepairCost cost = sim.repair().costOf(vest);
    CHECK(cost.material == repairDef(sim, "Cloth"));
    CHECK(cost.count == 2);
    CHECK(cost.work == doctest::Approx(200.0f));  // 4 kg x 100 work per kg x half

    sim.submit(RepairGearCommand{vest, true});
    sim.step();
    CHECK(!repairFailed(sim));
    CHECK(sim.things().registry().all_of<RepairMark>(vest));
    Snapshot snapshot;
    sim.writeSnapshot(snapshot);
    bool marked = false;
    for (const ThingView& view : snapshot.things) {
        marked = marked || (view.id == vest && view.repairMarked);
    }
    CHECK(marked);

    bool repaired = false;
    REPAIR_REQUIRE(repairRunUntil(sim, 3000, [&] {
                       repaired = repaired || repairSaw(sim, EventType::GearRepaired);
                       return repaired;
                   }) >= 0);
    const entt::registry& reg = sim.things().registry();
    CHECK(reg.get<HitPoints>(vest).current == max);
    CHECK(!reg.all_of<RepairMark>(vest));
    CHECK(repairStock(sim, "Cloth") == 28);
    sim.stepMany(60);
    CHECK(sim.things().spawned(vest));  // put down again beside the bench, not left in the hands
}

TEST_CASE("Repair: worn-out clothes cost about a tenth of their cloth, the bigger of hit points and durability, and come back to full") {
    Sim sim = repairSim();
    const Entity bench = repairSpawn(sim, "TailorBenchHand", CellPos{20, 20, 0});
    REPAIR_REQUIRE(bench != kNoEntity);
    repairSpawn(sim, "TorchLamp", CellPos{17, 17, 0});
    repairCrafter(sim, CellPos{18, 18, 0});
    repairSpawn(sim, "Cloth", CellPos{22, 22, 0}, 30);
    const Entity shirt = repairSpawn(sim, "Apparel_TShirt", CellPos{19, 19, 0});
    const Entity hat = repairSpawn(sim, "Apparel_CowboyHat", CellPos{19, 21, 0});
    REPAIR_REQUIRE(shirt != kNoEntity && hat != kNoEntity);
    entt::registry& reg = sim.things().registry();

    // Full hit points and full durability: nothing to mend.
    sim.submit(RepairGearCommand{shirt, true});
    sim.step();
    CHECK(repairFailed(sim));

    // Hit points at 80%, durability at 50%: the bigger loss counts, half of a full repair. A full repair of a
    // 40-cloth T-shirt is 4 cloth (a tenth), so 2; a 25-cloth hat worn out is 2 (2.5 rounded down).
    reg.get<HitPoints>(shirt).current = 80;
    reg.emplace_or_replace<Durability>(shirt, Durability{0.5f});
    RepairCost cost = sim.repair().costOf(shirt);
    CHECK(cost.count == 2);
    CHECK(cost.work == doctest::Approx(std::ceil(2304.0f * 0.1f * 0.5f)));
    reg.emplace_or_replace<Durability>(shirt, Durability{1.0f});
    CHECK(sim.repair().costOf(shirt).count == 1);  // only the hit points: 20% of 4, at least 1
    reg.emplace_or_replace<Durability>(shirt, Durability{0.0f});
    reg.get<HitPoints>(shirt).current = 100;
    cost = sim.repair().costOf(shirt);
    CHECK(cost.material == repairDef(sim, "Cloth"));
    CHECK(cost.count == 4);
    reg.emplace_or_replace<Durability>(hat, Durability{0.0f});
    CHECK(sim.repair().costOf(hat).count == 2);

    // Worn out but whole: the mark takes it, a tailor mends it, both come back to full.
    sim.submit(RepairGearCommand{shirt, true});
    sim.step();
    CHECK(!repairFailed(sim));
    REPAIR_REQUIRE(repairRunUntil(sim, 4000, [&] { return !reg.all_of<RepairMark>(shirt); }) >= 0);
    CHECK(reg.get<HitPoints>(shirt).current == reg.get<HitPoints>(shirt).max);
    CHECK(ApparelStats::condition(reg, shirt) == doctest::Approx(1.0f));
    CHECK(repairStock(sim, "Cloth") == 26);
}

TEST_CASE("Repair: a colonist mends its own tattered clothes at a tailor bench without taking them off") {
    Sim sim = repairSim();
    const Entity bench = repairSpawn(sim, "TailorBenchHand", CellPos{20, 20, 0});
    REPAIR_REQUIRE(bench != kNoEntity);
    repairSpawn(sim, "TorchLamp", CellPos{17, 17, 0});
    const Entity pawn = repairCrafter(sim, CellPos{30, 30, 0});
    entt::registry& reg = sim.things().registry();
    std::string why;
    const Entity shirt = sim.apparel().make(repairDef(sim, "Apparel_TShirt"), repairDef(sim, "Cloth"), -1, CellPos{30, 30, 0}, &why);
    const Entity pants = sim.apparel().make(repairDef(sim, "Apparel_Pants"), repairDef(sim, "Cloth"), -1, CellPos{30, 30, 0}, &why);
    REPAIR_REQUIRE(shirt != kNoEntity && pants != kNoEntity);
    REPAIR_REQUIRE(sim.apparel().putOn(pawn, shirt, why));
    REPAIR_REQUIRE(sim.apparel().putOn(pawn, pants, why));
    reg.emplace_or_replace<Durability>(shirt, Durability{0.0f});  // tattered: mended without a mark
    reg.emplace_or_replace<Durability>(pants, Durability{0.6f});  // worn, not tattered: left alone

    // No cloth near the bench: nothing happens.
    sim.stepMany(600);
    CHECK(ApparelStats::condition(reg, shirt) <= 0.0f);
    CHECK(!repairSaw(sim, EventType::GearRepaired));

    repairSpawn(sim, "Cloth", CellPos{22, 22, 0}, 30);
    bool repaired = false;
    REPAIR_REQUIRE(repairRunUntil(sim, 6000, [&] {
                       repaired = repaired || repairSaw(sim, EventType::GearRepaired);
                       return repaired;
                   }) >= 0);
    CHECK(ApparelStats::condition(reg, shirt) == doctest::Approx(1.0f));
    CHECK(ApparelStats::condition(reg, pants) < 0.61f);
    const std::vector<Entity>& worn = sim.apparel().worn(pawn);
    CHECK(std::find(worn.begin(), worn.end(), shirt) != worn.end());  // still on
    CHECK(!sim.things().spawned(shirt));
    CHECK(!reg.all_of<RepairMark>(shirt));
    CHECK(repairStock(sim, "Cloth") == 26);  // a tenth of 40

    // A worn piece the player marks is mended at any durability.
    sim.submit(RepairGearCommand{pants, true});
    sim.step();
    CHECK(!repairFailed(sim));
    REPAIR_REQUIRE(repairRunUntil(sim, 6000, [&] { return !reg.all_of<RepairMark>(pants); }) >= 0);
    CHECK(ApparelStats::condition(reg, pants) == doctest::Approx(1.0f));
    CHECK(std::find(worn.begin(), worn.end(), pants) != worn.end());
    CHECK(repairStock(sim, "Cloth") == 25);  // 40% of 4, rounded down, at least 1
}

TEST_CASE("Repair: a rifle is mended at a smithy with steel, nothing is used when the steel is out of reach") {
    Sim sim = repairSim();
    const Entity smithy = repairSpawn(sim, "FueledSmithy", CellPos{20, 20, 0});
    REPAIR_REQUIRE(smithy != kNoEntity);
    repairSpawn(sim, "TorchLamp", CellPos{17, 17, 0});
    repairCrafter(sim, CellPos{18, 18, 0});
    const Entity rifle = repairSpawn(sim, "Gun_BoltActionRifle", CellPos{19, 19, 0});
    repairSpawn(sim, "Steel", CellPos{50, 50, 0}, 30);  // far outside the bench's 12 m
    repairDamage(sim, rifle, 40);
    sim.submit(RepairGearCommand{rifle, true});
    sim.stepMany(900);
    CHECK(sim.things().registry().get<HitPoints>(rifle).current == 40);
    CHECK(repairStock(sim, "Steel") == 30);

    repairSpawn(sim, "Steel", CellPos{22, 22, 0}, 30);
    // 3.5 kg x 3 = 10.5 steel for a full repair; 60% missing is 6.3, rounded up to 7.
    CHECK(sim.repair().costOf(rifle).count == 7);
    REPAIR_REQUIRE(repairRunUntil(sim, 4000, [&] { return sim.things().registry().get<HitPoints>(rifle).current == 100; }) >= 0);
    CHECK(repairStock(sim, "Steel") == 53);
    CHECK(!sim.things().registry().all_of<RepairMark>(rifle));
}

TEST_CASE("Repair: the mark and the order refuse what cannot be done") {
    Sim sim = repairSim();
    const Entity rifle = repairSpawn(sim, "Gun_BoltActionRifle", CellPos{19, 19, 0});
    const Entity revolver = repairSpawn(sim, "Gun_Revolver", CellPos{20, 19, 0});
    const Entity steel = repairSpawn(sim, "Steel", CellPos{21, 19, 0}, 5);
    const Entity melee = repairSpawn(sim, "Melee_Knife", CellPos{22, 19, 0});
    repairSpawn(sim, "WeaponPart_Scope", CellPos{23, 19, 0});
    sim.submit(RepairGearCommand{rifle, true});  // undamaged
    sim.step();
    CHECK(repairFailed(sim));
    repairDamage(sim, rifle, 90);
    sim.submit(RepairGearCommand{steel, true});  // not gear
    sim.step();
    CHECK(repairFailed(sim));
    sim.submit(RepairGearCommand{rifle, true});
    sim.step();
    CHECK(!repairFailed(sim));
    sim.submit(RepairGearCommand{rifle, false});
    sim.step();
    CHECK(!repairFailed(sim));
    CHECK(!sim.things().registry().all_of<RepairMark>(rifle));

    // Held in the hands (off the map): fails.
    const Entity pawn = repairCrafter(sim, CellPos{25, 25, 0});
    sim.submit(CarryWeaponCommand{pawn, revolver});
    sim.step();
    repairDamage(sim, revolver, 10);
    sim.submit(RepairGearCommand{revolver, true});
    sim.step();
    CHECK(repairFailed(sim));

    // Upgrade orders.
    sim.submit(FitUpgradeCommand{rifle, "WeaponPart_Scope", true});
    sim.step();
    CHECK(!repairFailed(sim));
    CHECK(sim.things().registry().get<FitOrder>(rifle).part == repairDef(sim, "WeaponPart_Scope"));
    sim.submit(FitUpgradeCommand{rifle, "Steel", true});
    sim.step();
    CHECK(repairFailed(sim));
    sim.submit(FitUpgradeCommand{melee, "WeaponPart_Scope", true});
    sim.step();
    CHECK(repairFailed(sim));
    const Entity pistol = repairSpawn(sim, "Gun_Autopistol", CellPos{26, 19, 0});
    sim.submit(FitUpgradeCommand{pistol, "WeaponPart_Scope", true});  // a scope fits rifles only
    sim.step();
    CHECK(repairFailed(sim));
    sim.submit(FitUpgradeCommand{rifle, "", false});
    sim.step();
    CHECK(!repairFailed(sim));
    CHECK(!sim.things().registry().all_of<FitOrder>(rifle));
}

TEST_CASE("Repair: a machining table fits a scope and an extended magazine, which change the gun, one part to a slot") {
    Sim sim = repairSim();
    const Entity table = repairSpawn(sim, "MachiningTable", CellPos{20, 20, 0});
    repairPower(sim);
    REPAIR_REQUIRE(table != kNoEntity);
    repairSpawn(sim, "TorchLamp", CellPos{17, 17, 0});
    const Entity pawn = repairCrafter(sim, CellPos{18, 18, 0}, 8);
    const Entity rifle = repairSpawn(sim, "Gun_BoltActionRifle", CellPos{19, 19, 0});
    repairSpawn(sim, "WeaponPart_Scope", CellPos{22, 22, 0}, 2);
    repairSpawn(sim, "WeaponPart_ExtendedMagazine", CellPos{22, 23, 0});

    sim.submit(FitUpgradeCommand{rifle, "WeaponPart_Scope", true});
    sim.step();
    CHECK(!repairFailed(sim));
    REPAIR_REQUIRE(repairRunUntil(sim, 4000, [&] { return !sim.repair().fitted(rifle).empty(); }) >= 0);
    CHECK(sim.repair().fitted(rifle) == std::vector<DefId>{repairDef(sim, "WeaponPart_Scope")});
    CHECK(repairStock(sim, "WeaponPart_Scope") == 1);  // one used
    const entt::registry& reg = sim.things().registry();
    REPAIR_REQUIRE(reg.all_of<WeaponMods>(rifle));
    CHECK(reg.get<WeaponMods>(rifle).range == doctest::Approx(1.25f));
    CHECK(!reg.all_of<FitOrder>(rifle));

    // A second scope: the Optic slot is taken.
    sim.submit(FitUpgradeCommand{rifle, "WeaponPart_Scope", true});
    sim.step();
    CHECK(repairFailed(sim));

    // The extended magazine: 5 rounds become 8 (7.5 rounded), reloading takes longer.
    sim.submit(CarryWeaponCommand{pawn, rifle});
    sim.step();
    CHECK(sim.combat().magazineSize(pawn) == 5);
    const Entity bare = repairSpawn(sim, "Gun_BoltActionRifle", CellPos{21, 19, 0});
    sim.submit(FitUpgradeCommand{bare, "WeaponPart_ExtendedMagazine", true});
    sim.step();
    CHECK(!repairFailed(sim));
    REPAIR_REQUIRE(repairRunUntil(sim, 4000, [&] { return !sim.repair().fitted(bare).empty(); }) >= 0);
    sim.submit(CarryWeaponCommand{pawn, bare});  // replaces the first in the Primary slot
    sim.step();
    CHECK(sim.combat().magazineSize(pawn) == 8);
    CHECK(sim.combat().roundsLeft(pawn) == 8);  // a gun never fired is full to its new size

    // The snapshot lists the parts.
    Snapshot snapshot;
    sim.writeSnapshot(snapshot);
    bool seen = false;
    for (const ThingView& view : snapshot.things) {
        if (view.id == rifle) {
            seen = true;
            REPAIR_REQUIRE(view.fittedCount == 1);
            CHECK(snapshot.fittedParts[view.firstFitted] == repairDef(sim, "WeaponPart_Scope"));
        }
    }
    CHECK(seen);
}

TEST_CASE("Repair: fitting needs the skill the part asks for, and a bench that fits") {
    Sim sim = repairSim();
    const Entity table = repairSpawn(sim, "MachiningTable", CellPos{20, 20, 0});
    repairPower(sim);
    const Entity smithy = repairSpawn(sim, "FueledSmithy", CellPos{30, 30, 0});
    REPAIR_REQUIRE(table != kNoEntity);
    REPAIR_REQUIRE(smithy != kNoEntity);
    repairSpawn(sim, "TorchLamp", CellPos{17, 17, 0});
    repairCrafter(sim, CellPos{18, 18, 0}, 2);  // the scope asks for 6
    const Entity rifle = repairSpawn(sim, "Gun_BoltActionRifle", CellPos{19, 19, 0});
    repairSpawn(sim, "WeaponPart_Scope", CellPos{22, 22, 0});
    repairSpawn(sim, "WeaponPart_Scope", CellPos{32, 32, 0});  // by the smithy, which cannot fit parts
    sim.submit(FitUpgradeCommand{rifle, "WeaponPart_Scope", true});
    sim.stepMany(1500);
    CHECK(sim.repair().fitted(rifle).empty());
    CHECK(sim.things().registry().all_of<FitOrder>(rifle));
    // A better crafter takes it.
    repairCrafter(sim, CellPos{18, 19, 0}, 10);
    REPAIR_REQUIRE(repairRunUntil(sim, 4000, [&] { return !sim.repair().fitted(rifle).empty(); }) >= 0);
}

TEST_CASE("Repair: a smith makes an upgrade part at the machining table") {
    Sim sim = repairSim();
    sim.submit(UnlockResearchCommand{"Machining"});
    const Entity table = repairSpawn(sim, "MachiningTable", CellPos{20, 20, 0});
    repairPower(sim);
    REPAIR_REQUIRE(table != kNoEntity);
    repairSpawn(sim, "TorchLamp", CellPos{17, 17, 0});
    repairCrafter(sim, CellPos{18, 18, 0}, 8);
    repairSpawn(sim, "Steel", CellPos{19, 19, 0}, 40);
    repairSpawn(sim, "ComponentIndustrial", CellPos{19, 20, 0}, 4);
    sim.submit(AddBillCommand{table, "Make_WeaponPart_Compensator", BillMode::Count, 1});
    sim.step();
    CHECK(!repairFailed(sim));
    REPAIR_REQUIRE(repairRunUntil(sim, 6000, [&] { return repairStock(sim, "WeaponPart_Compensator") == 1; }) >= 0);
    CHECK(repairStock(sim, "Steel") == 25);
    CHECK(repairStock(sim, "ComponentIndustrial") == 3);
}

TEST_CASE("Repair: fitted parts change how the gun shoots: magazine, reload, recoil and accuracy") {
    Sim sim = repairSim();
    const Entity pawn = repairSpawn(sim, "Human", CellPos{20, 20, 0});
    const Entity rifle = repairSpawn(sim, "Gun_AssaultRifle", CellPos{21, 20, 0});
    sim.submit(CarryWeaponCommand{pawn, rifle});
    sim.step();
    CHECK(sim.combat().magazineSize(pawn) == 30);
    // Fit by hand (the job is tested above): scope and long barrel, then a magazine.
    entt::registry& reg = sim.things().registry();
    sim.repair().finishFit(rifle, repairDef(sim, "WeaponPart_Scope"));
    sim.repair().finishFit(rifle, repairDef(sim, "WeaponPart_LongBarrel"));
    sim.repair().finishFit(rifle, repairDef(sim, "WeaponPart_ExtendedMagazine"));
    const WeaponMods& mods = reg.get<WeaponMods>(rifle);
    CHECK(mods.range == doctest::Approx(1.25f * 1.15f));
    CHECK(mods.accuracy == doctest::Approx(1.05f * 1.05f));
    CHECK(mods.magazine == doctest::Approx(1.5f));
    CHECK(mods.reload == doctest::Approx(1.2f));
    CHECK(sim.combat().magazineSize(pawn) == 45);
}

TEST_CASE("Repair: the commands go through replay files, the same commands give the same game, and a save keeps parts and marks") {
    const auto run = [] {
        auto sim = std::make_unique<Sim>(SimSettings{5, 64, 0, 0, true}, repairDefs());
        repairSpawn(*sim, "MachiningTable", CellPos{20, 20, 0});
        repairPower(*sim);
        repairSpawn(*sim, "FueledSmithy", CellPos{30, 20, 0});
        repairSpawn(*sim, "TorchLamp", CellPos{17, 17, 0});
        repairCrafter(*sim, CellPos{18, 18, 0}, 12);
        const Entity rifle = repairSpawn(*sim, "Gun_BoltActionRifle", CellPos{19, 19, 0});
        const Entity revolver = repairSpawn(*sim, "Gun_Revolver", CellPos{19, 21, 0});
        repairSpawn(*sim, "WeaponPart_Scope", CellPos{22, 22, 0});
        repairSpawn(*sim, "Steel", CellPos{23, 22, 0}, 40);
        repairDamage(*sim, revolver, 30);
        sim->submit(RepairGearCommand{revolver, true});
        sim->submit(FitUpgradeCommand{rifle, "WeaponPart_Scope", true});
        sim->stepMany(200);
        return sim;
    };
    const std::unique_ptr<Sim> first = run();
    const std::unique_ptr<Sim> second = run();
    CHECK(first->stateHash() == second->stateHash());

    Replay replay;
    LoggedCommand repairCommand;
    repairCommand.step = 3;
    repairCommand.command = RepairGearCommand{kNoEntity, false};
    LoggedCommand fitCommand;
    fitCommand.step = 4;
    fitCommand.command = FitUpgradeCommand{kNoEntity, "WeaponPart_Scope", true};
    replay.commands = {repairCommand, fitCommand};
    Replay loaded;
    std::string error;
    CHECK(readReplay(writeReplay(replay), loaded, error));
    CHECK(error.empty());
    CHECK(writeReplay(loaded) == writeReplay(replay));
    REPAIR_REQUIRE(loaded.commands.size() == 2);
    const auto* repairGot = std::get_if<RepairGearCommand>(&loaded.commands[0].command);
    REPAIR_REQUIRE(repairGot != nullptr);
    CHECK(!repairGot->repair);
    const auto* fitGot = std::get_if<FitUpgradeCommand>(&loaded.commands[1].command);
    REPAIR_REQUIRE(fitGot != nullptr);
    CHECK(fitGot->part == "WeaponPart_Scope");
    CHECK(fitGot->fit);
}

TEST_CASE("Repair: a save with a fitted gun, an order and a mark loads back the same and carries on the same") {
    Sim original = repairSim();
    repairSpawn(original, "MachiningTable", CellPos{20, 20, 0});
    repairPower(original);
    repairSpawn(original, "TorchLamp", CellPos{17, 17, 0});
    repairCrafter(original, CellPos{18, 18, 0}, 12);
    const Entity rifle = repairSpawn(original, "Gun_BoltActionRifle", CellPos{19, 19, 0});
    const Entity fitted = repairSpawn(original, "Gun_AssaultRifle", CellPos{19, 21, 0});
    const Entity revolver = repairSpawn(original, "Gun_Revolver", CellPos{19, 23, 0});
    repairSpawn(original, "WeaponPart_LongBarrel", CellPos{22, 22, 0});
    original.repair().finishFit(fitted, repairDef(original, "WeaponPart_Compensator"));
    repairDamage(original, revolver, 20);
    original.submit(RepairGearCommand{revolver, true});
    original.submit(FitUpgradeCommand{rifle, "WeaponPart_LongBarrel", true});
    original.stepMany(20);  // the order and the mark stand; nothing is finished yet
    CHECK(original.things().registry().all_of<FitOrder>(rifle));
    CHECK(original.things().registry().all_of<RepairMark>(revolver));

    const std::vector<uint8_t> bytes = saveSim(original);
    SaveLoadResult loaded = loadSim(bytes, repairDefs());
    REPAIR_REQUIRE(loaded.error.empty());
    REPAIR_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->stateHash() == original.stateHash());
    CHECK(saveSim(*loaded.sim) == bytes);
    const entt::registry& reg = loaded.sim->things().registry();
    CHECK(reg.all_of<RepairMark>(revolver));
    CHECK(reg.get<FitOrder>(rifle).part == repairDef(*loaded.sim, "WeaponPart_LongBarrel"));
    REPAIR_REQUIRE(reg.all_of<FittedParts>(fitted));
    REPAIR_REQUIRE(reg.all_of<WeaponMods>(fitted));  // made again from the parts
    CHECK(reg.get<WeaponMods>(fitted).recoil == doctest::Approx(0.7f));
    original.stepMany(3000);
    loaded.sim->stepMany(3000);
    CHECK(loaded.sim->stateHash() == original.stateHash());
    CHECK(!loaded.sim->repair().fitted(rifle).empty());  // the order finished on both sides
}
