#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Farming.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Plants.h"
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
#define FARMING_REQUIRE(...)   \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// Test defs: two crops, quick sowing and harvest, and the Growing work type.
std::shared_ptr<const DefDatabase> farmingDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        const std::vector<DefSource> sources = {
            {"Things", "test.json", R"([
        {"defName": "Human", "category": "Pawn", "stats": {"moveSpeed": 0.15333}},
        {"defName": "Steel", "category": "Item", "stackLimit": 75},
        {"defName": "RawRice", "category": "Item", "stackLimit": 75},
        {"defName": "RawPotatoes", "category": "Item", "stackLimit": 75},
        {"defName": "Wall", "category": "Building", "passability": "Impassable", "fillPercent": 1.0},
        {"defName": "Plant_TreeOak", "category": "Plant", "plant": {"growDays": 30, "isTree": true, "harvestWork": 200,
            "harvestYield": 46, "harvestedThingDef": "Steel", "fertilityMin": 0.7, "lifespanDaysPerGrowDays": 9, "sowWork": 100}},
        {"defName": "Plant_Grass", "category": "Plant", "plant": {"growDays": 2.5, "harvestWork": 50, "fertilityMin": 0.05,
            "lifespanDaysPerGrowDays": 8}},
        {"defName": "Plant_Rice", "category": "Plant", "plant": {"growDays": 5.8, "harvestWork": 20, "harvestYield": 6,
            "harvestedThingDef": "RawRice", "sowWork": 30, "sowMinSkill": 0, "fertilityMin": 0.0, "fertilitySensitivity": 1.0,
            "lifespanDaysPerGrowDays": 8, "growMinTemperature": 0, "growOptimalMinTemperature": 6,
            "growOptimalMaxTemperature": 42, "growMaxTemperature": 58, "coldDamageBelow": 0, "heatDamageAbove": 58}},
        {"defName": "Plant_Potato", "category": "Plant", "plant": {"growDays": 7.6, "harvestWork": 20, "harvestYield": 11,
            "harvestedThingDef": "RawPotatoes", "sowWork": 30, "sowMinSkill": 4, "fertilityMin": 0.0, "fertilitySensitivity": 1.0,
            "lifespanDaysPerGrowDays": 8}}
    ])"},
            {"Terrain", "test.json", R"([{"defName": "Soil", "fertility": 1.0}, {"defName": "Gravel", "fertility": 1.0},
                                         {"defName": "SoilRich", "fertility": 1.0}])"},
            {"WorkTypes", "test.json", R"([
        {"defName": "Growing", "naturalPriority": 700, "relevantSkills": ["Plants"], "tasks": ["HarvestCrops", "SowCrops"]},
        {"defName": "Hauling", "naturalPriority": 300, "tasks": ["HaulToStockpile"]}
    ])"},
            {"Skills", "test.json", R"([{"defName": "Plants"}])"},
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

// A flat 32 m map of soil, 20 degrees outside.
std::unique_ptr<Sim> farmingSim() {
    auto sim = std::make_unique<Sim>(SimSettings{5, 32, 0, 0, true}, farmingDefs());
    sim->setOutdoorOverride(20.0f);
    return sim;
}

DefId farmingDef(const Sim& sim, const char* name) { return sim.defs().idOf("Things", name); }

Entity farmingColonist(Sim& sim, CellPos cell, int plants) {
    const Entity pawn = sim.things().spawn("Human", cell);
    sim.jobs().addWorker(pawn);
    Colonist colonist;
    colonist.skills.push_back(SkillRecord{"Plants", plants, Passion::None, 0.0f, false});
    sim.things().registry().emplace<Colonist>(pawn, colonist);
    return pawn;
}

int farmingItems(Sim& sim, const char* def) {
    int total = 0;
    const DefId id = farmingDef(sim, def);
    for (const auto [e, thing, stack] : sim.things().registry().view<const Thing, const StackCount>().each()) {
        (void)e;
        total += thing.def == id ? stack.count : 0;
    }
    return total;
}

// Runs whole game days; the clock starts at midnight, so a day holds the 13 growing hours.
void farmingDays(Sim& sim, double days) {
    const int64_t steps = static_cast<int64_t>(days * static_cast<double>(kSecondsPerDay));
    for (int64_t i = 0; i < steps; ++i) {
        sim.step();
    }
}

struct FarmingLog {
    std::vector<Event> events;
    void collect(Sim& sim) { events.insert(events.end(), sim.events().begin(), sim.events().end()); }
    int count(EventType type) const {
        return static_cast<int>(std::count_if(events.begin(), events.end(), [&](const Event& e) { return e.type == type; }));
    }
};

}  // namespace

TEST_CASE("Farming: a growing zone takes free soil, not stockpile cells or another zone's") {
    auto sim = farmingSim();
    // A stockpile on the left of the area, then a zone over all of it.
    sim->submit(CreateStockpileCommand{CellRect{4, 4, 2, 4, 0}, StoragePriority::Normal});
    sim->submit(CreateGrowingZoneCommand{CellRect{4, 4, 6, 4, 0}, "Plant_Rice"});
    sim->step();
    FARMING_REQUIRE(sim->farming().zones().size() == 1);
    const GrowingZone& zone = sim->farming().zones()[0];
    CHECK(zone.id == 1);
    CHECK(zone.cells.size() == 16);  // 4 x 4: the two stockpile columns are left out
    CHECK(sim->farming().zoneAt(CellPos{9, 7, 0}) == 1);
    CHECK(sim->farming().zoneAt(CellPos{4, 4, 0}) == kNoGrowingZone);
    CHECK(sim->farming().cropAt(CellPos{9, 7, 0}) == farmingDef(*sim, "Plant_Rice"));

    // A second zone over the same area finds nothing free; a stockpile over a growing zone skips its cells.
    sim->submit(CreateGrowingZoneCommand{CellRect{6, 4, 4, 4, 0}, "Plant_Potato"});
    sim->submit(CreateStockpileCommand{CellRect{8, 4, 4, 4, 0}, StoragePriority::Normal});
    sim->step();
    CHECK(sim->farming().zones().size() == 1);
    const std::vector<LoggedCommand>& log = sim->commandLog();
    CHECK(log.size() == 4);
    CHECK(sim->zones().zoneAt(CellPos{8, 5, 0}) == kNoZone);
    CHECK(sim->zones().zoneAt(CellPos{10, 5, 0}) != kNoZone);

    // Something that can't be sown, or a map edge, makes no zone.
    sim->submit(CreateGrowingZoneCommand{CellRect{20, 20, 3, 3, 0}, "Steel"});
    sim->submit(CreateGrowingZoneCommand{CellRect{20, 20, 3, 3, 0}, "Plant_TreeOak"});
    sim->step();
    CHECK(sim->farming().zones().size() == 1);

    // Taking cells out shrinks the zone; taking them all deletes it.
    sim->submit(RemoveGrowingZoneCellsCommand{CellRect{4, 4, 6, 2, 0}});
    sim->step();
    CHECK(sim->farming().zones()[0].cells.size() == 8);
    sim->submit(RemoveGrowingZoneCellsCommand{CellRect{0, 0, 32, 32, 0}});
    sim->step();
    CHECK(sim->farming().zones().empty());
    CHECK(sim->farming().zoneAt(CellPos{9, 7, 0}) == kNoGrowingZone);

    // The crop of a zone can be changed.
    sim->submit(CreateGrowingZoneCommand{CellRect{20, 20, 2, 2, 0}, "Plant_Rice"});
    sim->step();
    sim->submit(SetGrowingZoneCropCommand{2, "Plant_Potato"});
    sim->step();
    FARMING_REQUIRE(sim->farming().zones().size() == 1);
    CHECK(sim->farming().zones()[0].crop == farmingDef(*sim, "Plant_Potato"));
}

TEST_CASE("Farming: a colonist sows the zone, and a crop that is fully grown is harvested and sown again") {
    auto sim = farmingSim();
    farmingColonist(*sim, CellPos{8, 8, 0}, 8);
    sim->submit(CreateGrowingZoneCommand{CellRect{10, 8, 2, 1, 0}, "Plant_Rice"});
    FarmingLog log;
    for (int i = 0; i < 600; ++i) {
        sim->step();
        log.collect(*sim);
    }
    const DefId rice = farmingDef(*sim, "Plant_Rice");
    CHECK(sim->plants().has(CellPos{10, 8, 0}));
    CHECK(sim->plants().has(CellPos{11, 8, 0}));
    CHECK(sim->plants().defAt(CellPos{10, 8, 0}) == rice);
    CHECK(sim->plants().growth(CellPos{10, 8, 0}) < 0.01f);  // a seedling
    CHECK(log.count(EventType::PlantSown) == 2);
    CHECK(sim->farming().stats().sown == 2);
    CHECK(sim->stats().failedJobs == 0);

    // Not ready: left alone. Fully grown: picked, full yield at skill 8, the plant is gone and sown again.
    sim->plants().setGrowth(CellPos{10, 8, 0}, 0.9f);
    for (int i = 0; i < 300; ++i) {
        sim->step();
    }
    CHECK(farmingItems(*sim, "RawRice") == 0);
    sim->plants().setGrowth(CellPos{10, 8, 0}, 1.0f);
    for (int i = 0; i < 300; ++i) {
        sim->step();
        log.collect(*sim);
    }
    CHECK(farmingItems(*sim, "RawRice") == 6);
    CHECK(sim->farming().stats().harvested == 1);
    CHECK(sim->farming().stats().sown == 3);  // sown again after the harvest
    CHECK(sim->plants().has(CellPos{10, 8, 0}));
    CHECK(sim->plants().growth(CellPos{10, 8, 0}) < 0.01f);
}

TEST_CASE("Farming: the yield follows the Plants skill, and a crop needs its minimum skill to be sown") {
    auto unskilled = farmingSim();
    farmingColonist(*unskilled, CellPos{8, 8, 0}, 0);
    unskilled->submit(CreateGrowingZoneCommand{CellRect{10, 8, 1, 1, 0}, "Plant_Rice"});
    for (int i = 0; i < 1500; ++i) {
        unskilled->step();
    }
    FARMING_REQUIRE(unskilled->plants().has(CellPos{10, 8, 0}));
    unskilled->plants().setGrowth(CellPos{10, 8, 0}, 1.0f);
    for (int i = 0; i < 600; ++i) {
        unskilled->step();
    }
    CHECK(farmingItems(*unskilled, "RawRice") == 4);  // 6 x 60% rounds to 4

    auto skilled = farmingSim();
    farmingColonist(*skilled, CellPos{8, 8, 0}, 20);
    skilled->submit(CreateGrowingZoneCommand{CellRect{10, 8, 1, 1, 0}, "Plant_Rice"});
    for (int i = 0; i < 300; ++i) {
        skilled->step();
    }
    skilled->plants().setGrowth(CellPos{10, 8, 0}, 1.0f);
    for (int i = 0; i < 600; ++i) {
        skilled->step();
    }
    CHECK(farmingItems(*skilled, "RawRice") == 7);  // 6 x 113% rounds to 7

    // Potatoes want Plants 4: the unskilled colonist leaves the cell bare, the skilled one sows it.
    auto bare = farmingSim();
    farmingColonist(*bare, CellPos{8, 8, 0}, 2);
    bare->submit(CreateGrowingZoneCommand{CellRect{10, 8, 1, 1, 0}, "Plant_Potato"});
    for (int i = 0; i < 600; ++i) {
        bare->step();
    }
    CHECK_FALSE(bare->plants().has(CellPos{10, 8, 0}));
    farmingColonist(*bare, CellPos{8, 9, 0}, 6);
    for (int i = 0; i < 600; ++i) {
        bare->step();
    }
    CHECK(bare->plants().has(CellPos{10, 8, 0}));
}

TEST_CASE("Farming: crops grow by the temperature curve, and not at all in the cold") {
    // One growing day (13 growing hours) at each temperature; the cell is sown by hand.
    auto growthAfterOneDay = [](float outdoor) {
        auto sim = farmingSim();
        sim->setOutdoorOverride(outdoor);
        sim->submit(CreateGrowingZoneCommand{CellRect{10, 8, 1, 1, 0}, "Plant_Rice"});
        sim->step();
        sim->plants().place(CellPos{10, 8, 0}, farmingDef(*sim, "Plant_Rice"), 0.0f);
        farmingDays(*sim, 1.0);
        return sim->plants().growth(CellPos{10, 8, 0});
    };
    const float warm = growthAfterOneDay(20.0f);
    const float cool = growthAfterOneDay(3.0f);   // halfway up the curve between 0 and 6 degrees
    const float cold = growthAfterOneDay(-1.0f);  // frost kills it, but at least it doesn't grow
    const float hot = growthAfterOneDay(70.0f);
    // 13 growing hours of 1 / (5.8 x 24) at fertility 1.
    CHECK(warm == doctest::Approx(13.0f / (5.8f * 24.0f)).epsilon(0.03));
    CHECK(cool == doctest::Approx(warm * 0.5f).epsilon(0.05));
    CHECK(cold == 0.0f);
    CHECK(hot == 0.0f);

    // A plant outside a growing zone is not slowed by the cold.
    auto sim = farmingSim();
    sim->setOutdoorOverride(-1.0f);
    sim->plants().place(CellPos{10, 8, 0}, farmingDef(*sim, "Plant_Rice"), 0.0f);
    farmingDays(*sim, 1.0);
    CHECK(sim->plants().growth(CellPos{10, 8, 0}) == doctest::Approx(warm).epsilon(0.03));
}

TEST_CASE("Farming: frost and heat kill crops in a zone, mild weather does not") {
    auto sim = farmingSim();
    sim->submit(CreateGrowingZoneCommand{CellRect{10, 8, 4, 1, 0}, "Plant_Rice"});
    sim->step();
    for (int x = 10; x < 14; ++x) {
        sim->plants().place(CellPos{x, 8, 0}, farmingDef(*sim, "Plant_Rice"), 0.5f);
    }
    farmingDays(*sim, 1.0);
    CHECK(sim->plants().count() == 4);  // 20 degrees is mild

    FarmingLog log;
    sim->setOutdoorOverride(-20.0f);  // 20 degrees under: certain death within the hour
    for (int i = 0; i < kSecondsPerHour + 1; ++i) {
        sim->step();
        log.collect(*sim);
    }
    CHECK(sim->plants().count() == 0);
    CHECK(sim->farming().stats().frostKilled == 4);
    CHECK(log.count(EventType::PlantRemoved) == 4);

    auto hot = farmingSim();
    hot->submit(CreateGrowingZoneCommand{CellRect{10, 8, 2, 1, 0}, "Plant_Rice"});
    hot->step();
    hot->plants().place(CellPos{10, 8, 0}, farmingDef(*hot, "Plant_Rice"), 0.5f);
    hot->setOutdoorOverride(80.0f);
    for (int i = 0; i < kSecondsPerHour + 1; ++i) {
        hot->step();
    }
    CHECK(hot->plants().count() == 0);
    CHECK(hot->farming().stats().heatKilled == 1);
}

TEST_CASE("Farming: it is too cold to sow, and the zone waits for the weather") {
    auto sim = farmingSim();
    sim->setOutdoorOverride(-5.0f);
    farmingColonist(*sim, CellPos{8, 8, 0}, 8);
    sim->submit(CreateGrowingZoneCommand{CellRect{10, 8, 1, 1, 0}, "Plant_Rice"});
    for (int i = 0; i < 400; ++i) {
        sim->step();
    }
    CHECK_FALSE(sim->plants().has(CellPos{10, 8, 0}));
    sim->setOutdoorOverride(15.0f);
    for (int i = 0; i < 400; ++i) {
        sim->step();
    }
    CHECK(sim->plants().has(CellPos{10, 8, 0}));
}

TEST_CASE("Farming: sowing clears a wild plant, but a tree in the way stays") {
    auto sim = farmingSim();
    farmingColonist(*sim, CellPos{8, 8, 0}, 8);
    sim->plants().place(CellPos{10, 8, 0}, farmingDef(*sim, "Plant_Grass"), 1.0f);
    sim->plants().place(CellPos{11, 8, 0}, farmingDef(*sim, "Plant_TreeOak"), 1.0f);
    sim->submit(CreateGrowingZoneCommand{CellRect{10, 8, 2, 1, 0}, "Plant_Rice"});
    for (int i = 0; i < 600; ++i) {
        sim->step();
    }
    CHECK(sim->plants().defAt(CellPos{10, 8, 0}) == farmingDef(*sim, "Plant_Rice"));
    CHECK(sim->plants().defAt(CellPos{11, 8, 0}) == farmingDef(*sim, "Plant_TreeOak"));
}

TEST_CASE("Farming: zones are in the snapshot, the save and the replay") {
    auto sim = farmingSim();
    farmingColonist(*sim, CellPos{8, 8, 0}, 8);
    sim->submit(CreateGrowingZoneCommand{CellRect{10, 8, 3, 2, 0}, "Plant_Rice"});
    sim->submit(CreateGrowingZoneCommand{CellRect{10, 12, 2, 2, 0}, "Plant_Potato"});
    for (int i = 0; i < 200; ++i) {
        sim->step();
    }
    Snapshot snapshot;
    sim->writeSnapshot(snapshot);
    FARMING_REQUIRE(snapshot.growingZones.size() == 2);
    CHECK(snapshot.growingZones[0].cellCount == 6);
    CHECK(snapshot.growingZones[0].crop == farmingDef(*sim, "Plant_Rice"));
    CHECK(snapshot.growingZoneCells.size() == 10);

    const std::vector<uint8_t> bytes = saveSim(*sim);
    SaveLoadResult loaded = loadSim(bytes, farmingDefs());
    FARMING_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.error.empty());
    CHECK(loaded.sim->stateHash() == sim->stateHash());
    CHECK(loaded.sim->farming().zones().size() == 2);
    CHECK(loaded.sim->farming().zones()[1].crop == farmingDef(*loaded.sim, "Plant_Potato"));
    CHECK(loaded.sim->farming().stats().sown == sim->farming().stats().sown);
    CHECK(saveSim(*loaded.sim) == bytes);
    // The loaded game keeps growing the same: crops still feel their zone, and both stay in step.
    for (int i = 0; i < 3000; ++i) {
        sim->step();
        loaded.sim->step();
    }
    CHECK(loaded.sim->stateHash() == sim->stateHash());

    // The commands go through the replay file and back.
    const std::string json = writeReplay(recordReplay(*sim));
    CHECK(json.find("CreateGrowingZone") != std::string::npos);
    Replay back;
    std::string error;
    CHECK(readReplay(json, back, error));
    CHECK(error.empty());
}
