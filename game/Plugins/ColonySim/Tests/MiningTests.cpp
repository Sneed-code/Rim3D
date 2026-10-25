#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Designations.h"
#include "ColonySim/Events.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Mineables.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Regions.h"
#include "ColonySim/Replay.h"
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

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define MINING_REQUIRE(...)    \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// Small defs so the work is quick: rock of 160 hit points is two picks, a tree needs 200 work.
std::shared_ptr<const DefDatabase> miningTestDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        const std::vector<DefSource> sources = {
            {"Things", "test.json", R"([
        {"defName": "Human", "category": "Pawn", "stats": {"moveSpeed": 0.15333}},
        {"defName": "Steel", "category": "Item", "stackLimit": 75},
        {"defName": "WoodLog", "category": "Item", "stackLimit": 75},
        {"defName": "RawBerries", "category": "Item", "stackLimit": 75},
        {"defName": "ChunkGranite", "category": "Item", "stackLimit": 1},
        {"defName": "Rock_Granite", "category": "Mineable", "mineable": {"hitPoints": 160, "dropDef": "ChunkGranite", "dropChance": 1.0}},
        {"defName": "Rock_Limestone", "category": "Mineable", "mineable": {"hitPoints": 160, "dropDef": "ChunkGranite", "dropChance": 0.0}},
        {"defName": "Ore_Steel", "category": "Mineable", "mineable": {"hitPoints": 240, "dropDef": "Steel", "dropCount": 40, "isOre": true}},
        {"defName": "Plant_TreeOak", "category": "Plant", "plant": {"growDays": 30, "isTree": true, "harvestWork": 200,
            "harvestYield": 46, "harvestedThingDef": "WoodLog", "harvestMinGrowth": 0.65, "fertilityMin": 0.7,
            "lifespanDaysPerGrowDays": 9}},
        {"defName": "Plant_Berry", "category": "Plant", "plant": {"growDays": 6, "harvestWork": 100, "harvestYield": 10,
            "harvestedThingDef": "RawBerries", "harvestAfterGrowth": 0.3, "harvestMinGrowth": 0.65, "fertilityMin": 0.5,
            "lifespanDaysPerGrowDays": 8}},
        {"defName": "Plant_Grass", "category": "Plant", "plant": {"growDays": 2.5, "harvestWork": 50, "fertilityMin": 0.05,
            "lifespanDaysPerGrowDays": 8}}
    ])"},
            {"Terrain", "test.json", R"([{"defName": "Soil", "fertility": 1.0}, {"defName": "Gravel", "fertility": 1.0},
                                         {"defName": "SoilRich", "fertility": 1.0}])"},
            {"WorkTypes", "test.json", R"([
        {"defName": "Mining", "naturalPriority": 600, "workTags": ["Mining"], "tasks": ["Mine"]},
        {"defName": "PlantCutting", "naturalPriority": 500, "tasks": ["CutOrHarvestDesignatedPlants"]},
        {"defName": "Hauling", "naturalPriority": 300, "tasks": ["HaulToStockpile"]}
    ])"},
            {"Skills", "test.json", R"([{"defName": "Mining"}, {"defName": "Plants"}])"},
            {"Rules", "test.json", R"([{"defName": "SkillRules", "maxLevel": 20,
        "xpToNextLevel": [1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000, 10000,
                          12000, 14000, 16000, 18000, 20000, 22000, 24000, 26000, 28000, 30000, 32000],
        "passionLearnFactor": {"None": 0.35, "Minor": 1.0, "Major": 1.5}}])"}};
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::build(sources, "", *db, errors);
        return db;
    }();
    return defs;
}

std::shared_ptr<const DefDatabase> miningRealDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return db;
    }();
    return defs;
}

// A flat 32 m map of soil with the test defs and nothing on it.
Sim miningSim() { return Sim(SimSettings{5, 32, 0, 0, true}, miningTestDefs()); }

DefId miningDef(const Sim& sim, const char* name) { return sim.defs().idOf("Things", name); }

Entity miningPawn(Sim& sim, CellPos cell) {
    const Entity pawn = sim.things().spawn("Human", cell);
    sim.jobs().addWorker(pawn);
    return pawn;
}

// A pawn with a Colonist record whose Mining and Plants levels are given.
Entity miningColonist(Sim& sim, CellPos cell, int mining, int plants, Passion passion = Passion::None) {
    const Entity pawn = miningPawn(sim, cell);
    Colonist colonist;
    colonist.skills.push_back(SkillRecord{"Mining", mining, passion, 0.0f, false});
    colonist.skills.push_back(SkillRecord{"Plants", plants, passion, 0.0f, false});
    sim.things().registry().emplace<Colonist>(pawn, colonist);
    return pawn;
}

void miningPlaceRock(Sim& sim, CellPos cell, const char* def = "Rock_Granite") {
    sim.mineables().place(cell, miningDef(sim, def));
}

JobType miningJob(Sim& sim, Entity pawn) { return sim.things().registry().get<Worker>(pawn).job.type; }

struct MiningLog {
    std::vector<Event> events;
    int count(EventType type) const {
        return static_cast<int>(std::count_if(events.begin(), events.end(), [&](const Event& e) { return e.type == type; }));
    }
    int actions(PawnAction action) const {
        return static_cast<int>(std::count_if(events.begin(), events.end(),
                                              [&](const Event& e) { return e.type == EventType::PawnAction && e.action == action; }));
    }
    const Event* first(EventType type) const {
        const auto it = std::find_if(events.begin(), events.end(), [&](const Event& e) { return e.type == type; });
        return it == events.end() ? nullptr : &*it;
    }
};

template <typename F>
bool miningRunUntil(Sim& sim, MiningLog& log, int limit, F done) {
    for (int i = 0; i < limit; ++i) {
        if (done()) {
            return true;
        }
        sim.step();
        log.events.insert(log.events.end(), sim.events().begin(), sim.events().end());
    }
    return done();
}

// The total of one item def lying on the map.
int miningItemCount(Sim& sim, const char* def) {
    int total = 0;
    const DefId id = miningDef(sim, def);
    for (const auto [e, thing, stack] : sim.things().registry().view<const Thing, const StackCount>().each()) {
        (void)e;
        total += thing.def == id ? stack.count : 0;
    }
    return total;
}

}  // namespace

TEST_CASE("Mining: a new map has two or three rock types in patches, and ore lumps in the rock") {
    SimSettings settings;
    settings.seed = 42;
    settings.mapSize = 256;
    settings.checkpointEvery = 0;
    Sim sim(settings, miningRealDefs());
    const Mineables& rock = sim.mineables();

    size_t rockCells = 0;
    size_t covered = 0;
    for (int y = 0; y < sim.map().height(); ++y) {
        for (int x = 0; x < sim.map().width(); ++x) {
            const CellPos c{x, y, 0};
            if (sim.map().terrain(c) == Terrain::Rock) {
                ++rockCells;
                covered += rock.has(c) ? 1 : 0;
            } else {
                CHECK_FALSE(rock.has(c));
            }
        }
    }
    CHECK(rockCells > 1000);
    CHECK(covered == rockCells);
    CHECK(rock.count() == rockCells);
    CHECK(rock.rockTypes().size() >= 2);
    CHECK(rock.rockTypes().size() <= 3);

    size_t oreCells = 0;
    for (const DefId def : rock.defs()) {
        if (rock.isOre(def)) {
            oreCells += rock.countOf(def);
        } else {
            const bool onThisMap = std::find(rock.rockTypes().begin(), rock.rockTypes().end(), def) != rock.rockTypes().end();
            CHECK((rock.countOf(def) > 0) == onThisMap);
        }
    }
    CHECK(oreCells > 0);
    CHECK(oreCells < rockCells / 5);

    // Hit points come from the defs: sandstone 400, steel ore 1,500.
    for (const MineableRecord& r : rock.records()) {
        CHECK(r.hitPoints == rock.maxHitPoints(r.def));
    }
    CHECK(rock.maxHitPoints(sim.defs().idOf("Things", "Rock_Granite")) == 900);
    CHECK(rock.maxHitPoints(sim.defs().idOf("Things", "Ore_Steel")) == 1500);

    // The same seed gives the same rock; another seed another.
    Sim again(settings, miningRealDefs());
    CHECK(again.mineables().hash() == rock.hash());
    settings.seed = 43;
    Sim other(settings, miningRealDefs());
    CHECK(other.mineables().hash() != rock.hash());
}

TEST_CASE("Mining: Mineables::hit turns rock into rough stone and opens the way") {
    Sim sim = miningSim();
    // A wall of rock across the whole map splits it in two.
    for (int y = 0; y < 32; ++y) {
        miningPlaceRock(sim, CellPos{16, y, 0});
    }
    Mineables& rock = sim.mineables();
    CHECK(rock.count() == 32);
    CHECK_FALSE(sim.map().walkable(CellPos{16, 5, 0}));
    CHECK_FALSE(sim.regions().canReach(CellPos{3, 5, 0}, CellPos{28, 5, 0}));
    CHECK(sim.map().obstacleDm(CellPos{16, 5, 0}) == 255);

    const uint64_t before = rock.hash();
    CHECK_FALSE(rock.hit(CellPos{16, 5, 0}, 100));
    CHECK(rock.hitPoints(CellPos{16, 5, 0}) == 60);
    CHECK(rock.hash() != before);
    CHECK_FALSE(sim.regions().canReach(CellPos{3, 5, 0}, CellPos{28, 5, 0}));
    CHECK(rock.hit(CellPos{16, 5, 0}, 60));
    CHECK_FALSE(rock.has(CellPos{16, 5, 0}));
    CHECK(sim.map().terrain(CellPos{16, 5, 0}) == Terrain::RoughStone);
    CHECK(sim.map().walkable(CellPos{16, 5, 0}));
    CHECK(sim.map().obstacleDm(CellPos{16, 5, 0}) == 0);
    CHECK(sim.regions().canReach(CellPos{3, 5, 0}, CellPos{28, 5, 0}));  // regions were rebuilt
    CHECK(rock.count() == 31);
    CHECK_FALSE(rock.hit(CellPos{16, 5, 0}, 10));  // nothing left to hit
}

TEST_CASE("Mining: designating marks only what suits, and cancelling takes the marks off") {
    Sim sim = miningSim();
    miningPlaceRock(sim, CellPos{10, 10, 0});
    miningPlaceRock(sim, CellPos{11, 10, 0});
    sim.plants().place(CellPos{5, 5, 0}, miningDef(sim, "Plant_TreeOak"), 1.0f);
    sim.plants().place(CellPos{6, 5, 0}, miningDef(sim, "Plant_Grass"), 1.0f);

    sim.submit(DesignateCommand{CellRect{0, 0, 32, 32, 0}, DesignationKind::Mine});
    sim.submit(DesignateCommand{CellRect{0, 0, 3, 3, 0}, DesignationKind::Mine});             // no rock: fails
    sim.submit(DesignateCommand{CellRect{0, 0, 32, 32, 0}, DesignationKind::CutPlant});
    sim.submit(DesignateCommand{CellRect{5, 5, 2, 1, 0}, DesignationKind::HarvestPlant});     // grass gives nothing
    sim.submit(DesignateCommand{CellRect{0, 0, 32, 32, 0}, DesignationKind::None});           // not a mark: fails
    sim.step();
    int failed = 0;
    for (const Event& e : sim.events()) {
        failed += e.type == EventType::CommandFailed ? 1 : 0;
    }
    CHECK(failed == 2);
    const Designations& marks = sim.designations();
    CHECK(marks.at(CellPos{10, 10, 0}) == DesignationKind::Mine);
    CHECK(marks.at(CellPos{11, 10, 0}) == DesignationKind::Mine);
    CHECK(marks.at(CellPos{6, 5, 0}) == DesignationKind::CutPlant);
    CHECK(marks.at(CellPos{5, 5, 0}) == DesignationKind::HarvestPlant);  // changed from cut: the later mark wins
    CHECK(marks.count() == 4);

    Snapshot snapshot;
    sim.writeSnapshot(snapshot);
    CHECK(snapshot.designations.size() == 4);

    sim.submit(CancelDesignationsCommand{CellRect{0, 0, 32, 32, 0}, DesignationKind::CutPlant});
    sim.step();
    CHECK(marks.count() == 3);
    sim.submit(CancelDesignationsCommand{CellRect{10, 10, 1, 1, 0}, DesignationKind::None});
    sim.submit(CancelDesignationsCommand{CellRect{10, 10, 1, 1, 0}, DesignationKind::None});  // already gone: fails
    sim.step();
    CHECK(marks.count() == 2);
    failed = 0;
    for (const Event& e : sim.events()) {
        failed += e.type == EventType::CommandFailed ? 1 : 0;
    }
    CHECK(failed == 1);
}

TEST_CASE("Mining: a colonist walks over, picks the rock down in two hits and leaves a chunk") {
    Sim sim = miningSim();
    const CellPos rock{12, 8, 0};
    miningPlaceRock(sim, rock);
    const Entity pawn = miningPawn(sim, CellPos{8, 8, 0});
    sim.submit(DesignateCommand{CellRect{12, 8, 1, 1, 0}, DesignationKind::Mine});

    MiningLog log;
    MINING_REQUIRE(miningRunUntil(sim, log, 1000, [&] { return !sim.mineables().has(rock); }));
    // Walked about 3 m (20 s), then two hits 144 s apart at the default skill 8 (100% speed).
    const int64_t t = sim.time().seconds;
    CHECK(t >= 288);
    CHECK(t < 360);
    CHECK(log.actions(PawnAction::Mine) == 2);
    CHECK(log.count(EventType::CellMined) == 1);
    CHECK(log.first(EventType::CellMined)->cell == rock);
    CHECK(sim.map().terrain(rock) == Terrain::RoughStone);
    CHECK(sim.designations().count() == 0);
    CHECK(miningItemCount(sim, "ChunkGranite") == 1);
    const std::vector<Entity>& there = sim.things().itemsAt(rock);
    CHECK(there.size() == 1);

    // The job ended "done"; the pawn stood next to the rock, never in it.
    MiningLog rest;
    miningRunUntil(sim, rest, 5, [&] { return false; });
    bool done = false;
    for (const Event& e : log.events) {
        done = done || (e.type == EventType::JobEnded && e.job == JobType::Mine && e.message == "done");
    }
    CHECK(done);
    CHECK(sim.stats().failedJobs == 0);
    const Position& at = sim.things().registry().get<Position>(pawn);
    CHECK(std::abs(at.x - 12.5f) + std::abs(at.y - 8.5f) < 2.5f);
}

TEST_CASE("Mining: ore gives its full count, a rock that holds no chunk gives nothing") {
    Sim sim = miningSim();
    miningPlaceRock(sim, CellPos{12, 8, 0}, "Ore_Steel");
    miningPlaceRock(sim, CellPos{12, 12, 0}, "Rock_Limestone");
    miningPawn(sim, CellPos{8, 8, 0});
    sim.submit(DesignateCommand{CellRect{12, 8, 1, 1, 0}, DesignationKind::Mine});
    sim.submit(DesignateCommand{CellRect{12, 12, 1, 1, 0}, DesignationKind::Mine});

    MiningLog log;
    MINING_REQUIRE(miningRunUntil(sim, log, 3000, [&] { return sim.mineables().count() == 0; }));
    CHECK(miningItemCount(sim, "Steel") == 40);  // skill 8: 100% yield
    CHECK(miningItemCount(sim, "ChunkGranite") == 0);
    CHECK(log.count(EventType::CellMined) == 2);
}

TEST_CASE("Mining: mining speed follows the Mining level, and learning raises it") {
    // Level 0 is 4% speed, level 16 is 196%: the first pick lands at 144 / speed seconds.
    auto firstHit = [](int level) {
        Sim sim = miningSim();
        miningPlaceRock(sim, CellPos{12, 8, 0});
        miningColonist(sim, CellPos{11, 8, 0}, level, 8);
        sim.submit(DesignateCommand{CellRect{12, 8, 1, 1, 0}, DesignationKind::Mine});
        MiningLog log;
        miningRunUntil(sim, log, 6000, [&] { return log.actions(PawnAction::Mine) > 0; });
        return sim.time().seconds;
    };
    const int64_t slow = firstHit(0);
    const int64_t average = firstHit(8);
    const int64_t quick = firstHit(16);
    CHECK(slow > 3000);   // 144 / 0.04 = 3,600
    CHECK(slow < 3700);
    CHECK(average > 140);
    CHECK(average < 160);
    CHECK(quick > 70);    // 144 / 1.96 = 73
    CHECK(quick < 90);

    // A pawn learns while it works, faster with passion, and rises a level when the experience fills.
    Sim sim = miningSim();
    miningPlaceRock(sim, CellPos{12, 8, 0});
    const Entity pawn = miningColonist(sim, CellPos{11, 8, 0}, 3, 8, Passion::Major);
    sim.submit(DesignateCommand{CellRect{12, 8, 1, 1, 0}, DesignationKind::Mine});
    MiningLog log;
    miningRunUntil(sim, log, 200, [&] { return false; });
    const Colonist& colonist = sim.things().registry().get<Colonist>(pawn);
    // About 200 s at 0.0486 xp a second with Major passion (1.5x): roughly 14 xp.
    CHECK(colonist.skills[0].xp > 10.0f);
    CHECK(colonist.skills[0].xp < 16.0f);
    CHECK(colonist.skills[1].xp == 0.0f);

    SkillRecord& skill = sim.things().registry().get<Colonist>(pawn).skills[0];
    skill.xp = 3999.9f;  // level 3 needs 4,000
    miningRunUntil(sim, log, 10, [&] { return false; });
    CHECK(sim.things().registry().get<Colonist>(pawn).skills[0].level == 4);
}

TEST_CASE("Mining: rock with no way to stand next to it is left alone until a side opens") {
    Sim sim = miningSim();
    // A 3x3 block: the middle has no free neighbour.
    for (int y = 10; y < 13; ++y) {
        for (int x = 10; x < 13; ++x) {
            miningPlaceRock(sim, CellPos{x, y, 0});
        }
    }
    const Entity pawn = miningPawn(sim, CellPos{5, 11, 0});
    sim.submit(DesignateCommand{CellRect{11, 11, 1, 1, 0}, DesignationKind::Mine});
    MiningLog log;
    miningRunUntil(sim, log, 400, [&] { return false; });
    CHECK(sim.mineables().has(CellPos{11, 11, 0}));
    CHECK(miningJob(sim, pawn) == JobType::Wait);  // nothing it can do
    CHECK(sim.stats().failedJobs == 0);

    // Mark the side facing it too: it digs that first (the middle stays out of reach), then the middle.
    sim.submit(DesignateCommand{CellRect{10, 11, 1, 1, 0}, DesignationKind::Mine});
    MINING_REQUIRE(miningRunUntil(sim, log, 4000, [&] { return !sim.mineables().has(CellPos{10, 11, 0}); }));
    MINING_REQUIRE(miningRunUntil(sim, log, 4000, [&] { return !sim.mineables().has(CellPos{11, 11, 0}); }));
    CHECK(sim.stats().failedJobs == 0);
}

TEST_CASE("Mining: two colonists never dig the same rock") {
    Sim sim = miningSim();
    miningPlaceRock(sim, CellPos{12, 8, 0});
    miningPlaceRock(sim, CellPos{12, 9, 0});
    const Entity a = miningPawn(sim, CellPos{10, 8, 0});
    const Entity b = miningPawn(sim, CellPos{10, 9, 0});
    sim.submit(DesignateCommand{CellRect{12, 8, 1, 2, 0}, DesignationKind::Mine});
    MiningLog log;
    miningRunUntil(sim, log, 60, [&] { return false; });
    const Job& ja = sim.things().registry().get<Worker>(a).job;
    const Job& jb = sim.things().registry().get<Worker>(b).job;
    MINING_REQUIRE(ja.type == JobType::Mine);
    MINING_REQUIRE(jb.type == JobType::Mine);
    CHECK_FALSE(ja.cell == jb.cell);
    MINING_REQUIRE(miningRunUntil(sim, log, 1500, [&] { return sim.mineables().count() == 0; }));
    CHECK(log.count(EventType::CellMined) == 2);
}

TEST_CASE("Mining: cancelling stops the miner but the damage stays on the rock") {
    Sim sim = miningSim();
    const CellPos rock{12, 8, 0};
    miningPlaceRock(sim, rock);
    const Entity pawn = miningPawn(sim, CellPos{11, 8, 0});
    sim.submit(DesignateCommand{CellRect{12, 8, 1, 1, 0}, DesignationKind::Mine});
    MiningLog log;
    MINING_REQUIRE(miningRunUntil(sim, log, 400, [&] { return log.actions(PawnAction::Mine) == 1; }));
    CHECK(sim.mineables().hitPoints(rock) == 80);
    CHECK(miningJob(sim, pawn) == JobType::Mine);
    sim.submit(CancelDesignationsCommand{CellRect{12, 8, 1, 1, 0}, DesignationKind::None});
    sim.step();
    CHECK(miningJob(sim, pawn) != JobType::Mine);
    CHECK(sim.stats().failedJobs == 0);  // cancelling isn't a failure
    CHECK(sim.mineables().has(rock));
    CHECK(sim.mineables().hitPoints(rock) == 80);

    // Marked again, a second pick finishes it.
    sim.submit(DesignateCommand{CellRect{12, 8, 1, 1, 0}, DesignationKind::Mine});
    MINING_REQUIRE(miningRunUntil(sim, log, 2000, [&] { return !sim.mineables().has(rock); }));
}

TEST_CASE("Mining: a colonist with Mining switched off leaves the rock") {
    Sim sim = miningSim();
    miningPlaceRock(sim, CellPos{12, 8, 0});
    const Entity pawn = miningPawn(sim, CellPos{11, 8, 0});
    sim.submit(SetWorkPriorityCommand{pawn, "Mining", 0});
    sim.submit(DesignateCommand{CellRect{12, 8, 1, 1, 0}, DesignationKind::Mine});
    MiningLog log;
    miningRunUntil(sim, log, 600, [&] { return false; });
    CHECK(sim.mineables().has(CellPos{12, 8, 0}));
    sim.submit(SetWorkPriorityCommand{pawn, "Mining", 1});
    MINING_REQUIRE(miningRunUntil(sim, log, 1500, [&] { return !sim.mineables().has(CellPos{12, 8, 0}); }));
}

TEST_CASE("Chopping: felling a grown tree gives wood by growth and skill, a sapling gives none") {
    Sim sim = miningSim();
    sim.plants().place(CellPos{12, 8, 0}, miningDef(sim, "Plant_TreeOak"), 1.0f);
    sim.plants().place(CellPos{12, 12, 0}, miningDef(sim, "Plant_TreeOak"), 0.3f);
    const Entity pawn = miningPawn(sim, CellPos{9, 8, 0});
    sim.submit(DesignateCommand{CellRect{12, 8, 1, 1, 0}, DesignationKind::CutPlant});

    MiningLog log;
    MINING_REQUIRE(miningRunUntil(sim, log, 800, [&] { return !sim.plants().has(CellPos{12, 8, 0}); }));
    // 200 work at 100% speed (skill 8): about 200 s plus the walk.
    CHECK(sim.time().seconds >= 200);
    CHECK(sim.time().seconds < 260);
    CHECK(miningItemCount(sim, "WoodLog") == 46);
    CHECK(log.count(EventType::PlantRemoved) == 1);
    CHECK(log.first(EventType::PlantRemoved)->message == "felled");
    CHECK(log.first(EventType::PlantRemoved)->amount == 46.0f);
    CHECK(log.actions(PawnAction::Cut) == 1);
    CHECK(sim.designations().count() == 0);
    (void)pawn;

    // The sapling: felled for nothing.
    sim.submit(DesignateCommand{CellRect{12, 12, 1, 1, 0}, DesignationKind::CutPlant});
    MINING_REQUIRE(miningRunUntil(sim, log, 800, [&] { return !sim.plants().has(CellPos{12, 12, 0}); }));
    CHECK(miningItemCount(sim, "WoodLog") == 46);

    // A tree at the threshold gives half, at full growth all of it, scaled by the Plants level.
    Sim two = miningSim();
    two.plants().place(CellPos{12, 8, 0}, miningDef(two, "Plant_TreeOak"), 0.65f);
    two.plants().place(CellPos{12, 10, 0}, miningDef(two, "Plant_TreeOak"), 1.0f);
    miningColonist(two, CellPos{9, 9, 0}, 8, 0);  // Plants 0: yield 60%
    two.submit(DesignateCommand{CellRect{12, 8, 1, 3, 0}, DesignationKind::CutPlant});
    MiningLog log2;
    MINING_REQUIRE(miningRunUntil(two, log2, 6000, [&] { return !two.plants().has(CellPos{12, 8, 0}) && !two.plants().has(CellPos{12, 10, 0}); }));
    // 46 x 0.5 x 0.6 = 13.8 -> 14, and 46 x 1.0 x 0.6 = 27.6 -> 28.
    CHECK(miningItemCount(two, "WoodLog") == 42);
}

TEST_CASE("Harvesting: a berry bush is picked and grows back, a tree harvested comes down") {
    Sim sim = miningSim();
    const CellPos bush{12, 8, 0};
    sim.plants().place(bush, miningDef(sim, "Plant_Berry"), 0.4f);  // not ripe yet
    miningPawn(sim, CellPos{9, 8, 0});
    sim.submit(DesignateCommand{CellRect{12, 8, 1, 1, 0}, DesignationKind::HarvestPlant});
    MiningLog log;
    miningRunUntil(sim, log, 300, [&] { return false; });
    CHECK(sim.plants().has(bush));
    CHECK(miningItemCount(sim, "RawBerries") == 0);
    CHECK(sim.designations().count() == 1);  // the mark waits

    sim.plants().setGrowth(bush, 1.0f);  // ripe
    MINING_REQUIRE(miningRunUntil(sim, log, 800, [&] { return miningItemCount(sim, "RawBerries") > 0; }));
    CHECK(miningItemCount(sim, "RawBerries") == 10);
    CHECK(sim.plants().has(bush));
    CHECK(sim.plants().growth(bush) == doctest::Approx(0.3f));  // harvestAfterGrowth
    CHECK(log.count(EventType::PlantHarvested) == 1);
    CHECK(sim.designations().count() == 0);

    // Cutting the bush kills it and gives nothing.
    sim.plants().setGrowth(bush, 1.0f);
    sim.submit(DesignateCommand{CellRect{12, 8, 1, 1, 0}, DesignationKind::CutPlant});
    MINING_REQUIRE(miningRunUntil(sim, log, 800, [&] { return !sim.plants().has(bush); }));
    CHECK(miningItemCount(sim, "RawBerries") == 10);

    // Harvesting a tree (no regrowth) fells it.
    const CellPos tree{14, 10, 0};
    sim.plants().place(tree, miningDef(sim, "Plant_TreeOak"), 1.0f);
    sim.submit(DesignateCommand{CellRect{14, 10, 1, 1, 0}, DesignationKind::HarvestPlant});
    MINING_REQUIRE(miningRunUntil(sim, log, 1500, [&] { return !sim.plants().has(tree); }));
    CHECK(miningItemCount(sim, "WoodLog") == 46);
}

TEST_CASE("Harvesting: a mark whose plant dies of old age is pruned") {
    Sim sim = miningSim();
    sim.plants().place(CellPos{12, 8, 0}, miningDef(sim, "Plant_Grass"), 1.0f);
    sim.submit(DesignateCommand{CellRect{12, 8, 1, 1, 0}, DesignationKind::CutPlant});
    sim.step();
    CHECK(sim.designations().count() == 1);
    sim.plants().remove(CellPos{12, 8, 0});
    sim.stepMany(kSecondsPerHour);
    CHECK(sim.designations().count() == 0);
}

TEST_CASE("Mining: designations, rock and a half-done pick come back from a save") {
    const auto defs = miningTestDefs();
    Sim original(SimSettings{5, 32, 0, 0, true}, defs);
    for (int x = 12; x < 16; ++x) {
        miningPlaceRock(original, CellPos{x, 8, 0}, x == 13 ? "Ore_Steel" : "Rock_Granite");
    }
    original.plants().place(CellPos{20, 20, 0}, miningDef(original, "Plant_TreeOak"), 1.0f);
    const Entity pawn = miningPawn(original, CellPos{11, 8, 0});
    original.submit(DesignateCommand{CellRect{12, 8, 4, 1, 0}, DesignationKind::Mine});
    original.submit(DesignateCommand{CellRect{20, 20, 1, 1, 0}, DesignationKind::CutPlant});
    MiningLog log;
    MINING_REQUIRE(miningRunUntil(original, log, 600, [&] { return log.actions(PawnAction::Mine) >= 1; }));
    original.stepMany(40);
    CHECK(original.mineables().hitPoints(CellPos{12, 8, 0}) == 80);
    CHECK(original.things().registry().get<Worker>(pawn).job.progress > 0.0f);

    SaveLoadResult loaded = loadSim(saveSim(original), defs);
    MINING_REQUIRE(loaded.sim != nullptr);
    Sim& copy = *loaded.sim;
    CHECK(copy.stateHash() == original.stateHash());
    CHECK(copy.designations().count() == 5);
    CHECK(copy.mineables().hitPoints(CellPos{12, 8, 0}) == 80);
    CHECK(saveSim(copy) == saveSim(original));

    // On to the end of the work in both: one mined out cell stays mined in the copy.
    original.stepMany(3000);
    copy.stepMany(3000);
    CHECK(original.mineables().count() == 0);
    CHECK(copy.stateHash() == original.stateHash());
    CHECK(copy.map().terrain(CellPos{13, 8, 0}) == Terrain::RoughStone);

    SaveLoadResult again = loadSim(saveSim(original), defs);
    MINING_REQUIRE(again.sim != nullptr);
    CHECK(again.sim->mineables().count() == 0);
    CHECK(again.sim->stateHash() == original.stateHash());
}

TEST_CASE("Mining: the mined map of a real seed saves and loads the same") {
    SimSettings settings;
    settings.seed = 42;
    settings.mapSize = 128;
    settings.checkpointEvery = 0;
    Sim sim(settings, miningRealDefs());
    // Dig the first few rock cells by hand and pick at one more.
    int dug = 0;
    for (const MineableRecord& r : sim.mineables().records()) {
        const CellPos c = sim.map().cellAt(r.cell);
        if (dug < 5) {
            sim.mineables().hit(c, 100000);
            ++dug;
        } else {
            sim.mineables().hit(c, 10);
            break;
        }
    }
    MINING_REQUIRE(dug == 5);
    sim.designations().set(CellPos{3, 3, 0}, DesignationKind::HarvestPlant);
    const auto defs = miningRealDefs();
    SaveLoadResult loaded = loadSim(saveSim(sim), defs);
    MINING_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->mineables().hash() == sim.mineables().hash());
    CHECK(loaded.sim->mineables().count() == sim.mineables().count());
    CHECK(loaded.sim->stateHash() == sim.stateHash());
    CHECK(loaded.sim->designations().count() == 1);
    size_t mined = 0;
    for (int i = 0; i < 128 * 128; ++i) {
        const CellPos c = sim.map().cellAt(i);
        CHECK(loaded.sim->map().terrain(c) == sim.map().terrain(c));
        mined += sim.map().terrain(c) == Terrain::RoughStone ? 1 : 0;
    }
    CHECK(mined == 5);
    // Mined ground is bare for good: it was rock, so it never had any fertility.
    for (const MineableRecord& r : loaded.sim->mineables().records()) {
        CHECK(loaded.sim->plants().fertility(loaded.sim->map().cellAt(r.cell)) == 0.0f);
    }
    for (int i = 0; i < 128 * 128; ++i) {
        const CellPos c = sim.map().cellAt(i);
        if (sim.map().terrain(c) == Terrain::RoughStone) {
            CHECK(loaded.sim->plants().fertility(c) == 0.0f);
            CHECK_FALSE(loaded.sim->plants().has(c));
        }
    }
}

TEST_CASE("Mining: a recorded session with designations replays to the same state") {
    const auto defs = miningRealDefs();
    SimSettings settings;
    settings.seed = 42;
    settings.mapSize = 128;
    settings.scenario = "Crashlanded";
    settings.checkpointEvery = 600;
    Sim original(settings, defs);
    MINING_REQUIRE(original.setupErrors().empty());
    const CellRect whole{0, 0, 128, 128, 0};
    original.submit(DesignateCommand{whole, DesignationKind::CutPlant});
    original.submit(DesignateCommand{whole, DesignationKind::Mine});
    original.submit(DesignateCommand{whole, DesignationKind::HarvestPlant});
    original.stepMany(2500);
    const size_t marked = original.designations().count();
    CHECK(marked > 0);
    original.submit(CancelDesignationsCommand{CellRect{0, 0, 64, 128, 0}, DesignationKind::CutPlant});
    original.stepMany(500);
    CHECK(original.designations().count() < marked);

    const Replay replay = recordReplay(original);
    const std::string text = writeReplay(replay);
    CHECK(text.find("\"Designate\"") != std::string::npos);
    CHECK(text.find("\"CancelDesignations\"") != std::string::npos);
    Replay parsed;
    std::string error;
    MINING_REQUIRE(readReplay(text, parsed, error));
    CHECK(writeReplay(parsed) == text);
    Sim replayed(parsed.settings, defs);
    playReplay(parsed, replayed, parsed.endStep);
    CHECK(replayed.stateHash() == original.stateHash());
}
