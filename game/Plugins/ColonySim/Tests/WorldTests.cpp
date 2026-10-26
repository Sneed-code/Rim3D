#include "ColonySim/Defs.h"
#include "ColonySim/World.h"

#include <doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

namespace {

// The game's own defs: the world is data (WorldGen, Biomes, Factions).
const DefDatabase& worldTestDefs() {
    static std::unique_ptr<DefDatabase> db;
    if (db == nullptr) {
        db = std::make_unique<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
    }
    return *db;
}

}  // namespace

TEST_CASE("the same seed makes the same world, another seed another") {
    const World a = World::generate(worldTestDefs(), 42);
    const World b = World::generate(worldTestDefs(), 42);
    const World c = World::generate(worldTestDefs(), 43);
    CHECK(!a.empty());
    CHECK(a.hash() == b.hash());
    CHECK(a.hash() != c.hash());
}

TEST_CASE("a world of the def's size has land, sea, poles that are cold and an equator that is warm") {
    const World w = World::generate(worldTestDefs(), 42);
    CHECK(w.width() == 120);
    CHECK(w.height() == 60);
    const double landShare = static_cast<double>(w.landTileCount()) / w.tileCount();
    CHECK(landShare == doctest::Approx(0.4).epsilon(0.05));
    double pole = 0.0;
    double equator = 0.0;
    for (int x = 0; x < w.width(); ++x) {
        pole += w.tile(w.index(x, 0)).temperature;
        equator += w.tile(w.index(x, w.height() / 2)).temperature;
    }
    CHECK(equator / w.width() > pole / w.width() + 30.0);
}

TEST_CASE("the map wraps east to west and stops at the poles") {
    const World w = World::generate(worldTestDefs(), 1, 40, 20);
    CHECK(w.neighbour(w.index(0, 5), 6) == w.index(39, 5));
    CHECK(w.neighbour(w.index(39, 5), 2) == w.index(0, 5));
    CHECK(w.neighbour(w.index(3, 0), 0) == -1);
    CHECK(w.neighbour(w.index(3, 19), 4) == -1);
}

TEST_CASE("land tiles have a biome and water has none unless it is frozen") {
    const World w = World::generate(worldTestDefs(), 5);
    for (int i = 0; i < w.tileCount(); ++i) {
        const WorldTile& t = w.tile(i);
        if (!t.water) {
            CHECK(t.biome != kNoBiome);
        } else if (t.biome != kNoBiome) {
            CHECK(w.biomeName(t) == "SeaIce");
        }
    }
}

TEST_CASE("biomes follow temperature and rainfall") {
    const World w = World::generate(worldTestDefs(), 42);
    double iceTemp = -1000.0;
    double desertRain = 0.0;
    double rainforestRain = 1e9;
    for (int i = 0; i < w.tileCount(); ++i) {
        const WorldTile& t = w.tile(i);
        if (t.water) {
            continue;
        }
        const std::string& b = w.biomeName(t);
        if (b == "IceSheet") {
            iceTemp = std::max(iceTemp, static_cast<double>(t.temperature));
        } else if (b == "Desert" || b == "ExtremeDesert") {
            desertRain = std::max(desertRain, static_cast<double>(t.rainfall));
        } else if (b == "TropicalRainforest") {
            rainforestRain = std::min(rainforestRain, static_cast<double>(t.rainfall));
        }
    }
    CHECK(iceTemp <= -10.0);
    CHECK(desertRain < 400.0);
    if (rainforestRain < 1e9) {
        CHECK(rainforestRain >= 1300.0);
    }
}

TEST_CASE("rivers flow downhill into the sea or a lake and get bigger") {
    const World w = World::generate(worldTestDefs(), 42);
    int rivers = 0;
    for (int i = 0; i < w.tileCount(); ++i) {
        const WorldTile& t = w.tile(i);
        if (t.river == RiverSize::None) {
            continue;
        }
        ++rivers;
        CHECK(!t.water);
        CHECK(t.riverFlowsTo < 8);
        const int to = w.neighbour(i, t.riverFlowsTo);
        CHECK(to >= 0);
        if (to < 0) {
            continue;
        }
        const WorldTile& next = w.tile(to);
        CHECK(next.elevation <= t.elevation);
        CHECK((next.water || static_cast<int>(next.river) >= static_cast<int>(t.river)));
    }
    CHECK(rivers > 20);
}

TEST_CASE("settlements sit on land, apart from each other, with a faction and a name") {
    const World w = World::generate(worldTestDefs(), 42);
    CHECK(w.settlements().size() > 20);
    std::vector<std::string> names;
    for (size_t i = 0; i < w.settlements().size(); ++i) {
        const WorldSettlement& s = w.settlements()[i];
        const WorldTile& t = w.tile(s.tile);
        CHECK(!t.water);
        CHECK(t.hilliness != Hilliness::Impassable);
        CHECK(t.settlement == static_cast<int>(i));
        CHECK(!s.faction.empty());
        CHECK(worldTestDefs().find("Factions", s.faction) != nullptr);
        CHECK(s.faction != "Remnant");
        CHECK(s.faction != "Chitinid");
        names.push_back(s.name);
        for (size_t j = 0; j < i; ++j) {
            int dx = std::abs(w.tileX(s.tile) - w.tileX(w.settlements()[j].tile));
            dx = std::min(dx, w.width() - dx);
            const int dy = std::abs(w.tileY(s.tile) - w.tileY(w.settlements()[j].tile));
            CHECK(std::max(dx, dy) >= 3);
        }
    }
    std::sort(names.begin(), names.end());
    CHECK(std::adjacent_find(names.begin(), names.end()) == names.end());
}

TEST_CASE("roads join neighbouring land tiles and busy ones are better") {
    const World w = World::generate(worldTestDefs(), 42);
    CHECK(!w.roads().empty());
    bool better = false;
    for (const WorldRoad& r : w.roads()) {
        CHECK(!w.tile(r.tileA).water);
        CHECK(!w.tile(r.tileB).water);
        bool next = false;
        for (int d = 0; d < 8; ++d) {
            next = next || w.neighbour(r.tileA, d) == r.tileB;
        }
        CHECK(next);
        CHECK(static_cast<int>(w.tile(r.tileA).road) >= static_cast<int>(r.kind));
        better = better || r.kind != RoadKind::Dirt;
    }
    CHECK(better);
}

TEST_CASE("choosing a start tile") {
    World w = World::generate(worldTestDefs(), 42);
    CHECK(w.startTile() == -1);
    int water = -1;
    int settlement = -1;
    for (int i = 0; i < w.tileCount(); ++i) {
        water = w.tile(i).water ? i : water;
        settlement = w.tile(i).settlement != kNoSettlement ? i : settlement;
    }
    CHECK(!w.chooseStartTile(water));
    CHECK(!w.chooseStartTile(settlement));
    CHECK(!w.chooseStartTile(-1));
    CHECK(!w.chooseStartTile(w.tileCount()));
    CHECK(w.startTile() == -1);

    StartCriteria c;
    c.biome = "TemperateForest";
    c.needRiver = true;
    const int pick = w.suggestStartTile(c);
    CHECK(pick >= 0);
    if (pick < 0) {
        return;
    }
    CHECK(w.biomeName(w.tile(pick)) == "TemperateForest");
    CHECK(w.tile(pick).river != RiverSize::None);
    CHECK(w.suggestStartTile(c) == pick);
    CHECK(w.chooseStartTile(pick));
    CHECK(w.startTile() == pick);

    c.minTemperature = 500.0f;
    CHECK(w.suggestStartTile(c) == -1);
}

TEST_CASE("a site tells the map generator where it is, and its seed depends on world and tile") {
    const World a = World::generate(worldTestDefs(), 42);
    const World b = World::generate(worldTestDefs(), 43);
    const int i = a.suggestStartTile(StartCriteria{});
    CHECK(i >= 0);
    if (i < 0) {
        return;
    }
    const WorldSite s = a.siteFor(i);
    CHECK(s.biome == a.biomeName(a.tile(i)));
    CHECK(s.temperature == a.tile(i).temperature);
    CHECK(s.mapSeed != a.siteFor(i + 1).mapSeed);
    CHECK(s.mapSeed != b.siteFor(i).mapSeed);
    CHECK(s.mapSeed == a.siteFor(i).mapSeed);
}

TEST_CASE("without a WorldGen def the world is empty") {
    DefDatabase db;
    std::vector<std::string> errors;
    DefDatabase::build({}, "", db, errors);
    CHECK(World::generate(db, 1).empty());
}
