#include "ColonySim/Defs.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Save.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"
#include "ColonySim/UpperFloors.h"

#include <doctest.h>

#include <memory>
#include <string>
#include <vector>

using namespace colony;

namespace {

std::shared_ptr<const DefDatabase> plantsTestRealDefs() {
    auto db = std::make_shared<DefDatabase>();
    std::vector<std::string> errors;
    DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
    return db;
}

SimSettings plantsTestSettings(uint64_t seed, int size = 128) {
    SimSettings s;
    s.seed = seed;
    s.mapSize = size;
    s.checkpointEvery = 0;
    return s;
}

// Counts of plants by defName on a map.
size_t plantsTestCount(const Sim& sim, const char* name) {
    return sim.plants().countOf(sim.defs().idOf("Things", name));
}

}  // namespace

TEST_CASE("Plants: a new map has wild plants placed by fertility") {
    Sim sim(plantsTestSettings(5, 256), plantsTestRealDefs());
    const Plants& plants = sim.plants();
    CHECK(plants.count() > 10000);
    CHECK(plantsTestCount(sim, "Plant_Grass") > plantsTestCount(sim, "Plant_TreeOak"));
    CHECK(plantsTestCount(sim, "Plant_TreeOak") > 0);
    CHECK(plantsTestCount(sim, "Plant_Berry") > 0);

    const DefId tallGrass = sim.defs().idOf("Things", "Plant_TallGrass");
    const DefId grass = sim.defs().idOf("Things", "Plant_Grass");
    size_t cellsWithPlant = 0;
    size_t cellsWithoutGround = 0;
    for (int y = 0; y < sim.map().height(); ++y) {
        for (int x = 0; x < sim.map().width(); ++x) {
            const CellPos c{x, y, 0};
            const Terrain t = sim.map().terrain(c);
            if (!plants.has(c)) {
                continue;
            }
            ++cellsWithPlant;
            if (t == Terrain::Water || t == Terrain::Rock) {
                ++cellsWithoutGround;
            }
            // Nothing grows where its minimum fertility is not met: sand (0.1) only takes plants
            // that need 0.05.
            if (t == Terrain::Sand) {
                CHECK(plants.defAt(c) != tallGrass);
                CHECK(plants.fertility(c) == doctest::Approx(0.1f));
            }
            CHECK(plants.fertility(c) > 0.0f);
            CHECK(plants.growth(c) >= 0.0f);
            CHECK(plants.growth(c) <= 1.0f);
        }
    }
    CHECK(cellsWithPlant == plants.count());
    CHECK(cellsWithoutGround == 0);
    CHECK(plants.defAt(CellPos{0, 0, 0}).valid() == plants.has(CellPos{0, 0, 0}));
    (void)grass;
}

TEST_CASE("Plants: the soil comes in three fertilities and rich soil grows more plants") {
    Sim sim(plantsTestSettings(9, 256), plantsTestRealDefs());
    size_t cells[3] = {0, 0, 0};
    size_t withPlant[3] = {0, 0, 0};
    for (int y = 0; y < 256; ++y) {
        for (int x = 0; x < 256; ++x) {
            const CellPos c{x, y, 0};
            if (sim.map().terrain(c) != Terrain::Soil) {
                continue;
            }
            const float f = sim.plants().fertility(c);
            const int bucket = f < 0.8f ? 0 : (f < 1.2f ? 1 : 2);
            cells[bucket] += 1;
            withPlant[bucket] += sim.plants().has(c) ? 1 : 0;
        }
    }
    CHECK(cells[0] > 100);
    CHECK(cells[1] > 100);
    CHECK(cells[2] > 100);
    // Stony soil (0.7) holds fewer plants per cell than full soil (1.0); rich soil (1.4) is capped at 1.0.
    CHECK(static_cast<double>(withPlant[0]) / static_cast<double>(cells[0]) <
          static_cast<double>(withPlant[1]) / static_cast<double>(cells[1]));
    CHECK(static_cast<double>(withPlant[2]) / static_cast<double>(cells[2]) ==
          doctest::Approx(static_cast<double>(withPlant[1]) / static_cast<double>(cells[1])).epsilon(0.1));
}

TEST_CASE("Plants: the same seed gives the same plants, another seed other plants") {
    const auto defs = plantsTestRealDefs();
    Sim a(plantsTestSettings(3), defs);
    Sim b(plantsTestSettings(3), defs);
    Sim c(plantsTestSettings(4), defs);
    CHECK(a.plants().hash() == b.plants().hash());
    CHECK(a.stateHash() == b.stateHash());
    CHECK(a.plants().hash() != c.plants().hash());
}

TEST_CASE("Plants: no plant defs, no plants") {
    Sim sim(plantsTestSettings(3));
    CHECK(sim.plants().count() == 0);
}

TEST_CASE("Plants: growth happens only in the growing hours and takes growDays x 24 / 13 days") {
    Sim sim(plantsTestSettings(1, 32), plantsTestRealDefs());
    Plants& plants = sim.plants();
    const DefId grass = sim.defs().idOf("Things", "Plant_Grass");
    const CellPos soil = [&] {
        for (int i = 0; i < 32 * 32; ++i) {
            const CellPos c = sim.map().cellAt(i);
            if (sim.map().terrain(c) == Terrain::Soil && plants.fertility(c) == doctest::Approx(1.0f)) {
                return c;
            }
        }
        return CellPos{-1, -1, 0};
    }();
    if (soil.x < 0) { CHECK(false); return; }
    plants.remove(soil);
    CHECK_FALSE(plants.has(soil));
    CHECK(plants.place(soil, grass, 0.0f));
    CHECK_FALSE(plants.place(soil, grass));  // occupied

    // Night hours do nothing but add age.
    plants.growHour(3);
    plants.growHour(22);
    CHECK(plants.growthPpm(soil) == 0);
    CHECK(plants.ageHours(soil) == 2);
    // A growing hour adds 1 / (growDays x 24) of full growth (grass: 2.5 days, so 1/60).
    plants.growHour(10);
    CHECK(plants.growthPpm(soil) == doctest::Approx(1000000.0 / 60.0).epsilon(0.001));

    // A day has 13 growing hours (06:00 to 19:00), so grass takes 60 / 13 days to grow up.
    plants.remove(soil);
    plants.place(soil, grass, 0.0f);
    int hours = 0;
    while (!plants.mature(soil) && hours < 24 * 10) {
        plants.growHour(hours % 24);
        ++hours;
    }
    CHECK(plants.mature(soil));
    CHECK(hours == doctest::Approx(60.0 / 13.0 * 24.0).epsilon(0.03));
}

TEST_CASE("Plants: fertility changes the growth rate and low fertility stops it") {
    Sim sim(plantsTestSettings(2, 256), plantsTestRealDefs());
    Plants& plants = sim.plants();
    const DefId oak = sim.defs().idOf("Things", "Plant_TreeOak");
    const DefId grass = sim.defs().idOf("Things", "Plant_Grass");
    CellPos rich{-1, -1, 0};
    CellPos stony{-1, -1, 0};
    CellPos sand{-1, -1, 0};
    for (int i = 0; i < 256 * 256; ++i) {
        const CellPos c = sim.map().cellAt(i);
        const float f = plants.fertility(c);
        if (f == doctest::Approx(1.4f) && rich.x < 0) rich = c;
        if (f == doctest::Approx(0.7f) && stony.x < 0) stony = c;
        if (f == doctest::Approx(0.1f) && sand.x < 0) sand = c;
    }
    if (rich.x < 0 || stony.x < 0 || sand.x < 0) { CHECK(false); return; }
    for (const CellPos c : {rich, stony, sand}) {
        plants.remove(c);
    }
    plants.place(rich, oak);
    plants.place(stony, oak);  // oak needs 0.7: just enough
    plants.place(sand, oak);   // placing is allowed, growing is not
    plants.growHour(12);
    const double base = 1000000.0 / (30.0 * 24.0);
    CHECK(plants.growthPpm(rich) == doctest::Approx(base * (1.0 + 0.4 * 0.5)).epsilon(0.001));  // sensitivity 0.5
    CHECK(plants.growthPpm(stony) == doctest::Approx(base * (1.0 - 0.3 * 0.5)).epsilon(0.001));
    CHECK(plants.growthPpm(sand) == 0);

    plants.remove(sand);
    plants.place(sand, grass);
    plants.growHour(12);
    CHECK(plants.growthPpm(sand) > 0);  // grass needs only 0.05
}

TEST_CASE("Plants: plants die at the end of their lifespan") {
    Sim sim(plantsTestSettings(1, 32), plantsTestRealDefs());
    Plants& plants = sim.plants();
    const DefId grass = sim.defs().idOf("Things", "Plant_Grass");
    CHECK(plants.lifespanHours(grass) == 20 * 24);  // 2.5 grow days x 8
    CHECK(plants.lifespanHours(sim.defs().idOf("Things", "Plant_TreeOak")) == 270 * 24);
    const CellPos c = [&] {
        for (int i = 0; i < 32 * 32; ++i) {
            if (plants.fertility(sim.map().cellAt(i)) > 0.0f) return sim.map().cellAt(i);
        }
        return CellPos{-1, -1, 0};
    }();
    if (c.x < 0) { CHECK(false); return; }
    plants.remove(c);
    plants.place(c, grass, 1.0f);
    for (uint32_t h = 0; h + 1 < plants.lifespanHours(grass); ++h) {
        plants.growHour(0);
    }
    CHECK(plants.has(c));
    plants.growHour(0);
    CHECK_FALSE(plants.has(c));
}

TEST_CASE("Plants: placing, removing and resetting growth keep the hash and count right") {
    Sim sim(plantsTestSettings(1, 32), plantsTestRealDefs());
    Plants& plants = sim.plants();
    const DefId berry = sim.defs().idOf("Things", "Plant_Berry");
    CellPos c{-1, -1, 0};
    for (int i = 0; i < 32 * 32 && c.x < 0; ++i) {
        const CellPos cell = sim.map().cellAt(i);
        if (plants.fertility(cell) >= 1.0f && !plants.has(cell)) c = cell;
    }
    if (c.x < 0) { CHECK(false); return; }
    const uint64_t before = plants.hash();
    const size_t count = plants.count();
    CHECK(plants.place(c, berry, 1.0f));
    CHECK(plants.mature(c));
    CHECK(plants.count() == count + 1);
    CHECK(plants.hash() != before);
    // Harvest: back to 30% (plant.harvestAfterGrowth).
    plants.setGrowth(c, 0.3f);
    CHECK(plants.growth(c) == doctest::Approx(0.3f));
    CHECK_FALSE(plants.mature(c));
    plants.remove(c);
    CHECK(plants.count() == count);
    CHECK(plants.hash() == before);
    CHECK_FALSE(plants.place(CellPos{-1, 0, 0}, berry));      // off the map
    CHECK_FALSE(plants.place(c, sim.defs().idOf("Things", "Steel")));  // not a plant
}

TEST_CASE("Plants: the sim grows them hourly, in its state hash, and a save brings them back") {
    const auto defs = plantsTestRealDefs();
    Sim sim(plantsTestSettings(5), defs);
    CHECK(sim.plants().count() > 1000);
    const uint64_t start = sim.stateHash();
    sim.stepMany(kSecondsPerHour * 9 + 5);  // 00:00 to 09:00: three growing hours (06, 07, 08)
    CHECK(sim.stateHash() != start);

    const std::vector<uint8_t> bytes = saveSim(sim);
    SaveLoadResult loaded = loadSim(bytes, defs);
    if (!(loaded.sim != nullptr)) { MESSAGE(loaded.error); CHECK(false); return; }
    CHECK(loaded.sim->plants().count() == sim.plants().count());
    CHECK(loaded.sim->plants().hash() == sim.plants().hash());
    CHECK(loaded.sim->stateHash() == sim.stateHash());
    CHECK(saveSim(*loaded.sim) == bytes);
    // On from the save, both grow the same.
    sim.stepMany(kSecondsPerHour * 4);
    loaded.sim->stepMany(kSecondsPerHour * 4);
    CHECK(loaded.sim->stateHash() == sim.stateHash());

    // A wild map grows up: after a few days plants have aged.
    sim.stepMany(kSecondsPerDay * 3);
    size_t mature = 0;
    for (const PlantRecord& r : sim.plants().records()) {
        mature += r.growth >= kPlantFullGrowth ? 1 : 0;
    }
    CHECK(mature > 0);
}

TEST_CASE("Plants: a damaged plant list in a save is refused") {
    const auto defs = plantsTestRealDefs();
    Sim sim(plantsTestSettings(5, 256), defs);
    CHECK(sim.plants().count() > 0);
    // Loading with data that lacks the plant defs fails clearly rather than dropping plants.
    const std::vector<uint8_t> bytes = saveSim(sim);
    auto other = std::make_shared<DefDatabase>();
    std::vector<std::string> errors;
    DefDatabase::build({{"Things", "t.json", R"([{"defName": "Human", "category": "Pawn"}])"}}, "", *other, errors);
    SaveLoadResult loaded = loadSim(bytes, other);
    CHECK(loaded.sim == nullptr);
    CHECK_FALSE(loaded.error.empty());
}

TEST_CASE("Plants: nothing grows under an upper floor, and laying one clears the cell") {
    Sim sim(plantsTestSettings(1, 32), plantsTestRealDefs());
    Plants& plants = sim.plants();
    const DefId grass = sim.defs().idOf("Things", "Plant_Grass");
    const DefId oak = sim.defs().idOf("Things", "Plant_TreeOak");
    // A column of ground with walls round it to hold a floor up.
    CellPos soil{-1, -1, 0};
    for (int i = 0; i < 32 * 32 && soil.x < 0; ++i) {
        const CellPos c = sim.map().cellAt(i);
        if (c.x >= 5 && c.x < 25 && c.y >= 5 && c.y < 25 && plants.fertility(c) > 0.5f && sim.map().terrain(c) == Terrain::Soil) {
            soil = c;
        }
    }
    if (soil.x < 0) { CHECK(false); return; }
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const CellPos c{soil.x + dx, soil.y + dy, 0};
            plants.remove(c);
            if (dx != 0 || dy != 0) {
                sim.things().spawn("Wall", c);
            }
        }
    }
    sim.rooms().update();
    CHECK(plants.place(soil, oak, 1.0f));
    const CellPos up{soil.x, soil.y, 1};
    if (!sim.upperFloors().lay(up)) { CHECK(false); return; }
    CHECK(plants.underUpperFloor(soil));
    CHECK_FALSE(plants.has(soil));
    CHECK_FALSE(plants.place(soil, grass, 0.0f));
    // A plant that gets there another way (e.g. a save from before) dies in the next hour.
    CHECK(sim.upperFloors().remove(up));
    CHECK(plants.place(soil, grass, 0.5f));
    CHECK(const_cast<Map&>(sim.map()).setUpperFloor(up, true));
    CHECK(plants.has(soil));
    plants.growHour(8);
    CHECK_FALSE(plants.has(soil));
}

TEST_CASE("Plants: the hourly pass in slices grows every plant once, as the whole pass does (G3)") {
    Sim whole(plantsTestSettings(9, 128), plantsTestRealDefs());
    Sim sliced(plantsTestSettings(9, 128), plantsTestRealDefs());
    whole.plants().growHour(10);
    for (int slice = 0; slice < 60; ++slice) {
        sliced.plants().growHourSlice(10, slice, 60);
    }
    size_t differ = 0;
    for (int y = 0; y < whole.map().height(); ++y) {
        for (int x = 0; x < whole.map().width(); ++x) {
            const CellPos c{x, y, 0};
            differ += whole.plants().growthPpm(c) != sliced.plants().growthPpm(c) || whole.plants().ageHours(c) != sliced.plants().ageHours(c) ? 1 : 0;
        }
    }
    CHECK(differ == 0);
}

TEST_CASE("Plants: the sim spreads the hourly pass over the next hour's first minute (G3)") {
    Sim sim(plantsTestSettings(9, 128), plantsTestRealDefs());
    // A plant in the first row ages on the hour; one in the last row only a minute later.
    CellPos first{-1, -1, 0};
    CellPos last{-1, -1, 0};
    for (int x = 0; x < sim.map().width() && (first.x < 0 || last.x < 0); ++x) {
        if (first.x < 0 && sim.plants().has(CellPos{x, 0, 0})) {
            first = CellPos{x, 0, 0};
        }
        if (last.x < 0 && sim.plants().has(CellPos{x, sim.map().height() - 1, 0})) {
            last = CellPos{x, sim.map().height() - 1, 0};
        }
    }
    if (first.x < 0 || last.x < 0) { CHECK(false); return; }
    const uint32_t firstAge = sim.plants().ageHours(first);
    const uint32_t lastAge = sim.plants().ageHours(last);
    while (sim.time().seconds < kSecondsPerHour) {
        sim.step();
    }
    CHECK(sim.plants().ageHours(first) == firstAge + 1);
    CHECK(sim.plants().ageHours(last) == lastAge);
    while (sim.time().seconds < kSecondsPerHour + 60) {
        sim.step();
    }
    CHECK(sim.plants().ageHours(last) == lastAge + 1);
}
