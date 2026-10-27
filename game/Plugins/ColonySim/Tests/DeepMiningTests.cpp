#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/DeepResources.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Mineables.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define DEEP_REQUIRE(...)             \
    do {                              \
        const bool deepOk = (__VA_ARGS__); \
        CHECK(deepOk);                \
        if (!deepOk) {                \
            return;                   \
        }                             \
    } while (0)

namespace {

// The game's real data: the deep drill, the scanner, the deposits and the rules are what is tested.
std::shared_ptr<const DefDatabase> deepDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

DefId deepDef(const Sim& sim, const char* name) { return sim.defs().idOf("Things", name); }

// A mild 64 m map with a clear sky and a solar generator (1700 W in full sun, 4 x 4) at (2,2) that feeds a conduit row along
// y = 5 from x = 6 to x = 9 and a column down x = 9. The drill sits at (10,5) and the scanner (3 x 3) at (10,10).
std::unique_ptr<Sim> deepSim() {
    auto owned = std::make_unique<Sim>(SimSettings{5, 64, 0, 0, true}, deepDefs());
    Sim& sim = *owned;
    sim.setOutdoorOverride(20.0f);
    sim.submit(SetWeatherCommand{"Clear", 100000.0f});
    sim.power().setSun([](const GameTime&) { return 1.0; });
    sim.things().spawn("SolarGenerator", CellPos{2, 2, 0});
    for (int x = 6; x <= 9; ++x) {
        sim.things().spawn("PowerConduit", CellPos{x, 5, 0});
    }
    for (int y = 6; y <= 11; ++y) {
        sim.things().spawn("PowerConduit", CellPos{9, y, 0});
    }
    return owned;
}

Entity deepWorker(Sim& sim, CellPos cell, int mining = 8, int intellectual = 8) {
    const Entity pawn = sim.things().spawn("Human", cell);
    sim.jobs().addWorker(pawn);
    Colonist colonist;
    colonist.skills.push_back(SkillRecord{"Mining", mining, Passion::None, 0.0f, false});
    colonist.skills.push_back(SkillRecord{"Intellectual", intellectual, Passion::None, 0.0f, false});
    sim.things().registry().emplace<Colonist>(pawn, colonist);
    for (int h = 0; h < 24; ++h) {
        sim.submit(SetScheduleCommand{pawn, h, ScheduleKind::Work});
    }
    sim.step();
    return pawn;
}

int deepItemCount(Sim& sim, const char* def) {
    int total = 0;
    const DefId id = deepDef(sim, def);
    for (const auto [e, thing, stack] : sim.things().registry().view<const Thing, const StackCount>().each()) {
        (void)e;
        total += thing.def == id ? stack.count : 0;
    }
    return total;
}

// Puts a machine's work near the end of a cycle (or of the guaranteed scan time), so a test need not run days.
void deepPreload(Sim& sim, CellPos building, double work) {
    DeepResourcesSaved s = sim.deepResources().saved();
    s.work.push_back(DeepWork{building, work});
    CHECK(sim.deepResources().restore(s));
}

}  // namespace

TEST_CASE("Deep: items with a deep object are the deposit kinds, with the wiki's sizes and portions") {
    auto owner = deepSim();
    Sim& sim = *owner;
    const DeepResources& deep = sim.deepResources();
    CHECK(deep.kinds().size() == 6);
    CHECK(deep.portionOf(deepDef(sim, "Steel")) == 45);
    CHECK(deep.portionOf(deepDef(sim, "Silver")) == 40);
    CHECK(deep.portionOf(deepDef(sim, "Plasteel")) == 10);
    CHECK(deep.portionOf(deepDef(sim, "Uranium")) == 10);
    CHECK(deep.portionOf(deepDef(sim, "Jade")) == 10);
    CHECK(deep.portionOf(deepDef(sim, "Gold")) == 10);
    CHECK(deep.portionOf(deepDef(sim, "WoodLog")) == 0);
    CHECK(deep.unitsPerCell() == 300.0);
    CHECK(deep.cellCount() == 0);
}

TEST_CASE("Deep: a deposit takes its lump size and a drill takes it out a portion at a time, nearest cell first") {
    auto owner = deepSim();
    Sim& sim = *owner;
    DeepResources& deep = sim.deepResources();
    const DefId steel = deepDef(sim, "Steel");
    DeepFind find;
    DEEP_REQUIRE(deep.placeDeposit(steel, CellPos{30, 30, 0}, 5, &find));
    CHECK(find.cells == 5);
    CHECK(deep.cellCount() == 5);
    CHECK(deep.totalUnits() == 5 * 300);
    CHECK(deep.has(CellPos{30, 30, 0}));
    CHECK(deep.defAt(CellPos{30, 30, 0}) == steel);
    CHECK(deep.unitsAt(CellPos{30, 30, 0}) == 300);
    CHECK(deep.hasResourceNear(CellPos{31, 31, 0}, 2.6));
    CHECK_FALSE(deep.hasResourceNear(CellPos{40, 40, 0}, 2.6));

    // 300 units at 45 a portion: six whole portions and a last one of 30.
    for (int i = 0; i < 6; ++i) {
        const DeepPortion p = deep.takePortion(CellPos{30, 30, 0}, 2.6);
        CHECK(p.def == steel);
        CHECK(p.count == 45);
        CHECK_FALSE(p.chunk);
        CHECK(p.from == CellPos{30, 30, 0});  // the cell right under the drill empties first
    }
    CHECK(deep.unitsAt(CellPos{30, 30, 0}) == 30);
    const DeepPortion last = deep.takePortion(CellPos{30, 30, 0}, 2.6);
    CHECK(last.count == 30);
    CHECK_FALSE(deep.has(CellPos{30, 30, 0}));
    CHECK(deep.cellCount() == 4);
    CHECK(deep.totalUnits() == 4 * 300);

    // Nothing in reach: a rock chunk.
    const DeepPortion nothing = deep.takePortion(CellPos{50, 50, 0}, 2.6);
    CHECK(nothing.chunk);
    CHECK(nothing.count == 1);
    CHECK_FALSE(nothing.def.valid());
}

TEST_CASE("Deep: a deposit goes round water and cells that already hold one") {
    auto owner = deepSim();
    Sim& sim = *owner;
    DeepResources& deep = sim.deepResources();
    const DefId gold = deepDef(sim, "Gold");
    for (int y = 28; y <= 32; ++y) {
        const_cast<Map&>(sim.map()).setTerrain(CellPos{31, y, 0}, Terrain::Water);
    }
    DEEP_REQUIRE(deep.placeDeposit(gold, CellPos{30, 30, 0}, 40));
    for (int y = 28; y <= 32; ++y) {
        CHECK_FALSE(deep.has(CellPos{31, y, 0}));
    }
    CHECK(deep.cellCount() == 40);
    DEEP_REQUIRE(deep.placeDeposit(deepDef(sim, "Jade"), CellPos{30, 30, 0}, 10));
    CHECK(deep.cellCount() == 50);
    CHECK(deep.defAt(CellPos{30, 30, 0}) == gold);  // the first one stays
    CHECK_FALSE(deep.placeDeposit(gold, CellPos{31, 30, 0}, 3));  // water
}

TEST_CASE("Deep: found deposits follow the commonality table and the lump sizes") {
    Sim big(SimSettings{9, 256, 0, 0, true}, deepDefs());
    DeepResources& deep = big.deepResources();
    std::map<uint32_t, int> byDef;
    for (int i = 0; i < 120; ++i) {
        DeepFind find;
        DEEP_REQUIRE(deep.findDeposit(DefId{}, find));
        ++byDef[find.def.value];
        CHECK(find.cells >= 1);
        CHECK(find.cells <= 40);
    }
    const DefId steel = deepDef(big, "Steel");
    // Steel is 4 of 7.5: it should be the commonest by a clear margin.
    for (const auto& [def, n] : byDef) {
        if (def != steel.value) {
            CHECK(byDef[steel.value] > n);
        }
    }
    CHECK(byDef.size() >= 4);
    CHECK(deep.found() == 120);
    DeepFind forced;
    DEEP_REQUIRE(deep.findDeposit(deepDef(big, "Uranium"), forced));
    CHECK(forced.def == deepDef(big, "Uranium"));
    CHECK(forced.cells >= 4);
    CHECK(forced.cells <= 10);
    CHECK_FALSE(deep.findDeposit(deepDef(big, "WoodLog"), forced));
}

TEST_CASE("Deep: the scanner finds a deposit on average in 3 days of work and always by 6") {
    auto owner = deepSim();
    Sim& sim = *owner;
    DeepResources& deep = sim.deepResources();
    const CellPos building{10, 10, 0};
    DeepFind find;
    CHECK_FALSE(deep.scanStep(building, 1.0, find));
    CHECK(deep.workAt(building) == 1.0);
    // At the guaranteed time the next second finds.
    deepPreload(sim, CellPos{20, 20, 0}, deep.scanGuaranteedSeconds() - 0.5);
    CHECK(deep.scanStep(CellPos{20, 20, 0}, 1.0, find));
    CHECK(deep.workAt(CellPos{20, 20, 0}) == 0.0);
    CHECK(deep.cellCount() > 0);

    // Many scanners, run until found: the mean is near 3 days (259,200 s) of work.
    Sim other(SimSettings{11, 128, 0, 0, true}, deepDefs());
    double total = 0.0;
    const int runs = 40;
    for (int r = 0; r < runs; ++r) {
        const CellPos at{r, 0, 0};
        double seconds = 0.0;
        DeepFind f;
        // A big step stands for many seconds at once; the chance per second is speed / average, so use speed 100.
        while (!other.deepResources().scanStep(at, 100.0, f)) {
            seconds += 100.0;
            if (seconds > 2.0e6) {
                break;
            }
        }
        total += seconds;
    }
    const double mean = total / runs;
    CHECK(mean > 120000.0);
    CHECK(mean < 450000.0);
}

TEST_CASE("Deep: a powered scanner under open sky is worked by a colonist until it finds a deposit") {
    auto owner = deepSim();
    Sim& sim = *owner;
    const Entity scanner = sim.things().spawn("GroundPenetratingScanner", CellPos{10, 10, 0});
    DEEP_REQUIRE(scanner != kNoEntity);
    sim.step();
    deepPreload(sim, sim.things().registry().get<Footprint>(scanner).origin, sim.deepResources().scanGuaranteedSeconds() - 3.0);
    const Entity pawn = deepWorker(sim, CellPos{14, 14, 0});
    std::vector<Event> seen;
    for (int i = 0; i < 400 && sim.deepResources().cellCount() == 0; ++i) {
        sim.step();
        seen.insert(seen.end(), sim.events().begin(), sim.events().end());
    }
    CHECK(sim.deepResources().cellCount() > 0);
    CHECK(std::any_of(seen.begin(), seen.end(), [](const Event& e) { return e.type == EventType::DeepDepositFound; }));
    CHECK(std::any_of(seen.begin(), seen.end(),
                      [](const Event& e) { return e.type == EventType::PawnAction && e.action == PawnAction::Scan; }));
    (void)pawn;
    // The snapshot lists what was found.
    Snapshot snap;
    sim.writeSnapshot(snap);
    CHECK(snap.deepResources.size() == sim.deepResources().cellCount());
}

TEST_CASE("Deep: a scanner under a roof, or without power, is not worked") {
    auto owner = deepSim();
    Sim& sim = *owner;
    const Entity scanner = sim.things().spawn("GroundPenetratingScanner", CellPos{10, 10, 0});
    DEEP_REQUIRE(scanner != kNoEntity);
    sim.step();
    std::string why;
    CHECK(sim.jobs().deepWorkable(scanner, why));
    const_cast<Map&>(sim.map()).setThickRoof(CellPos{11, 11, 0}, true);
    CHECK_FALSE(sim.jobs().deepWorkable(scanner, why));
    CHECK(why == "the scanner is under a roof");
    const_cast<Map&>(sim.map()).setThickRoof(CellPos{11, 11, 0}, false);
    CHECK(sim.jobs().deepWorkable(scanner, why));

    Sim dark(SimSettings{5, 64, 0, 0, true}, deepDefs());
    const Entity unpowered = dark.things().spawn("GroundPenetratingScanner", CellPos{10, 10, 0});
    dark.step();
    CHECK_FALSE(dark.jobs().deepWorkable(unpowered, why));
    CHECK(why == "the machine has no power");
}

TEST_CASE("Deep: a deep drill is only worked with deposit in reach, and a miner's yield scales the portion") {
    for (const int level : {0, 8, 20}) {
        auto owner = deepSim();
        Sim& sim = *owner;
        const Entity drill = sim.things().spawn("DeepDrill", CellPos{10, 5, 0});
        DEEP_REQUIRE(drill != kNoEntity);
        sim.step();
        std::string why;
        CHECK_FALSE(sim.jobs().deepWorkable(drill, why));
        CHECK(why == "there is nothing under the drill");
        DEEP_REQUIRE(sim.deepResources().placeDeposit(deepDef(sim, "Steel"), CellPos{11, 6, 0}, 6));
        CHECK(sim.jobs().deepWorkable(drill, why));

        const CellPos origin = sim.things().registry().get<Footprint>(drill).origin;
        deepPreload(sim, origin, sim.deepResources().drillCycleSeconds() - 3.0);
        deepWorker(sim, CellPos{14, 8, 0}, level);
        const int before = sim.deepResources().totalUnits();
        for (int i = 0; i < 600 && deepItemCount(sim, "Steel") == 0; ++i) {
            sim.step();
        }
        // RimWorld's yield curve: 60% at level 0, 100% at 8, 113% at 20. The cell loses the whole portion.
        const int expected = level == 0 ? 27 : (level == 8 ? 45 : 51);
        CHECK(deepItemCount(sim, "Steel") == expected);
        CHECK(sim.deepResources().totalUnits() == before - 45);
        CHECK(sim.deepResources().portions() == 1);
    }
}

TEST_CASE("Deep: a drill whose deposit ran out under it brings up a rock chunk") {
    auto owner = deepSim();
    Sim& sim = *owner;
    DeepResources& deep = sim.deepResources();
    const Entity drill = sim.things().spawn("DeepDrill", CellPos{10, 5, 0});
    DEEP_REQUIRE(drill != kNoEntity);
    sim.mineables().place(CellPos{40, 40, 0}, deepDef(sim, "Rock_Granite"));  // so the map has a rock type
    sim.step();
    const DeepPortion p = deep.takePortion(CellPos{10, 5, 0}, 2.6);
    CHECK(p.chunk);
}

TEST_CASE("Deep: deposits, drill progress and the find stream survive a save and load") {
    auto owner = deepSim();
    Sim& sim = *owner;
    DeepResources& deep = sim.deepResources();
    DeepFind find;
    DEEP_REQUIRE(deep.findDeposit(DefId{}, find));
    DEEP_REQUIRE(deep.findDeposit(deepDef(sim, "Silver"), find));
    deep.takePortion(find.center, 2.6);
    DeepFind scratch;
    deep.scanStep(CellPos{20, 20, 0}, 5.0, scratch);
    for (int i = 0; i < 10; ++i) {
        sim.step();
    }
    const std::vector<uint8_t> bytes = saveSim(sim);
    SaveLoadResult loaded = loadSim(bytes, deepDefs());
    DEEP_REQUIRE(loaded.sim != nullptr);
    Sim& copy = *loaded.sim;
    CHECK(copy.deepResources().cellCount() == deep.cellCount());
    CHECK(copy.deepResources().totalUnits() == deep.totalUnits());
    CHECK(copy.deepResources().workAt(CellPos{20, 20, 0}) == deep.workAt(CellPos{20, 20, 0}));
    CHECK(copy.deepResources().portions() == deep.portions());
    CHECK(copy.stateHash() == sim.stateHash());
    // The stream carried on from the same place: both find the same next deposit.
    DeepFind a;
    DeepFind b;
    DEEP_REQUIRE(deep.findDeposit(DefId{}, a));
    DEEP_REQUIRE(copy.deepResources().findDeposit(DefId{}, b));
    CHECK(a.def == b.def);
    CHECK(a.center == b.center);
    CHECK(a.cells == b.cells);
}
