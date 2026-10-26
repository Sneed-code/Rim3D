#include "ColonySim/Defs.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Weather.h"

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
