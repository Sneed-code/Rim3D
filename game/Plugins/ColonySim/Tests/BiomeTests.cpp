#include "ColonySim/Defs.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Weather.h"

#include <cstdint>

#include <doctest.h>

#include <memory>
#include <string>
#include <vector>

using namespace colony;

namespace {

const std::vector<std::string>& biomeTestNames() {
    static const std::vector<std::string> names = {
        "TemperateForest", "BorealForest", "AridShrubland", "Tundra", "IceSheet", "SeaIce",
        "ColdBog", "TemperateSwamp", "TropicalRainforest", "TropicalSwamp", "Desert", "ExtremeDesert"};
    return names;
}

}  // namespace

TEST_CASE("Biomes: all twelve vanilla biomes are in the defs and make a working climate and plant layer") {
    auto db = std::make_shared<DefDatabase>();
    std::vector<std::string> errors;
    DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
    CHECK(errors.empty());
    CHECK(db->idsOfKind("Biomes").size() == biomeTestNames().size());

    SimSettings settings;
    settings.seed = 9;
    settings.mapSize = 128;
    settings.checkpointEvery = 0;
    Sim sim(settings, db);

    for (const std::string& name : biomeTestNames()) {
        CAPTURE(name);
        const Def* biome = db->find("Biomes", name);
        if (biome == nullptr) { CHECK(false); continue; }
        CHECK(biome->number("climate.dailySwing") > 0.0);
        CHECK(biome->number("climate.seasonMeanTemperature.winter") < biome->number("climate.seasonMeanTemperature.summer"));
        CHECK(biome->number("weather.Clear") > 0.0);
        CHECK(biome->count("animals") > 0);

        const Weather weather(sim.map(), *db, 3, name);
        CHECK(weather.growingSeason(1) == (biome->count("climate.growingSeasons") > 0 &&
                                           biome->textAt("climate.growingSeasons", 0) != ""));
        const Plants plants(sim.map(), *db, 3, name);
        if (biome->number("plantDensity") > 0.2) {
            CHECK(plants.count() > 1000);
        }
    }

    // The wiki's climates: ice is far colder than the forest, deserts and rainforests far warmer.
    CHECK(db->find("Biomes", "IceSheet")->number("climate.seasonMeanTemperature.summer") < -5.0);
    CHECK(db->find("Biomes", "ExtremeDesert")->number("climate.seasonMeanTemperature.summer") > 40.0);
    CHECK(db->find("Biomes", "TropicalRainforest")->number("climate.seasonMeanTemperature.winter") > 20.0);
    CHECK(db->find("Biomes", "IceSheet")->count("climate.growingSeasons") == 0);
    CHECK(db->find("Biomes", "TropicalRainforest")->count("climate.growingSeasons") == 4);
    // The wiki lists snow for the tropics too; the weather's own temperature range keeps it away.
    CHECK(db->find("Biomes", "TropicalRainforest")->number("weather.SnowHard") == doctest::Approx(4));
    // More rain in the rainforest than in the desert.
    CHECK(db->find("Biomes", "TropicalRainforest")->number("climate.rainfallMmPerYear") >
          db->find("Biomes", "Desert")->number("climate.rainfallMmPerYear"));
}

TEST_CASE("Biomes: the rainforest is half open ground with few trees and half dense forest (forestPatches)") {
    auto db = std::make_shared<DefDatabase>();
    std::vector<std::string> errors;
    DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
    CHECK(errors.empty());
    const Def* rainforest = db->find("Biomes", "TropicalRainforest");
    if (rainforest == nullptr || !rainforest->has("forestPatches")) {
        FAIL_CHECK("the rainforest has no forestPatches");
        return;
    }

    SimSettings settings;
    settings.seed = 42;
    settings.mapSize = 512;
    settings.checkpointEvery = 0;
    Sim sim(settings, db);
    const Map& map = sim.map();

    for (const uint64_t seed : {3ULL, 42ULL}) {
        CAPTURE(seed);
        const Plants plants(map, *db, seed, "TropicalRainforest");
        // Trees and plants per 16 m block: open blocks have almost no trees but still bushes and grass.
        constexpr int kBlock = 16;
        int blocks = 0, open = 0, dense = 0;
        long openPlants = 0, openCells = 0;
        for (int by = 0; by + kBlock <= map.height(); by += kBlock) {
            for (int bx = 0; bx + kBlock <= map.width(); bx += kBlock) {
                int ground = 0, trees = 0, others = 0;
                for (int y = by; y < by + kBlock; ++y) {
                    for (int x = bx; x < bx + kBlock; ++x) {
                        const CellPos c{x, y, 0};
                        if (plants.fertility(c) <= 0.0f) {
                            continue;
                        }
                        ++ground;
                        const DefId def = plants.defAt(c);
                        if (!def.valid()) {
                            continue;
                        }
                        (db->get(def).flag("plant.isTree") ? trees : others) += 1;
                    }
                }
                if (ground < kBlock * kBlock / 2) {
                    continue;  // mostly water or rock
                }
                ++blocks;
                const double treeShare = static_cast<double>(trees) / ground;
                if (treeShare < 0.04) {
                    ++open;
                    openPlants += others;
                    openCells += ground;
                } else if (treeShare > 0.12) {
                    ++dense;
                }
            }
        }
        CHECK(blocks > 300);
        if (blocks == 0) {
            continue;
        }
        const double openShare = static_cast<double>(open) / blocks;
        const double denseShare = static_cast<double>(dense) / blocks;
        CAPTURE(openShare);
        CAPTURE(denseShare);
        CHECK(openShare > 0.3);
        CHECK(openShare < 0.65);
        CHECK(denseShare > 0.25);
        // Open ground is not bare: bushes and grass cover most of it.
        CHECK(static_cast<double>(openPlants) / static_cast<double>(openCells) > 0.6);
    }
}
