#include "ColonySim/Defs.h"
#include "ColonySim/World.h"

#include <doctest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
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

TEST_CASE("every world has tropical swamps, wet and hot") {
    // World 42 had none: its swamp noise missed the thin wet band at the equator. Each swampy rule
    // now makes at least `minRuleShare` of the land fitting its temperature and rainfall swampy.
    for (const uint64_t seed : {42ULL, 1ULL, 3ULL, 5ULL, 7ULL, 14ULL}) {
        const World w = World::generate(worldTestDefs(), seed);
        int swamps = 0;
        int rainforest = 0;
        for (int i = 0; i < w.tileCount(); ++i) {
            const WorldTile& t = w.tile(i);
            if (t.water) {
                continue;
            }
            const std::string& b = w.biomeName(t);
            if (b == "TropicalSwamp") {
                ++swamps;
                CHECK(t.rainfall >= 1500.0f);
                CHECK(t.temperature > 19.0f);
            } else if (b == "TropicalRainforest") {
                ++rainforest;
            }
        }
        INFO("seed " << seed);
        CHECK(swamps > 0);
        CHECK(rainforest > swamps);  // swamps are patches in the rainforest belt, not all of it
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

TEST_CASE("a colony cannot start next to a settlement, two tiles away is fine") {
    const World w = World::generate(worldTestDefs(), 42);
    CHECK(!w.settlements().empty());
    // The six sides of a hexagon are all different tiles, east and west wrap, and the odd rows lean right.
    const int mid = w.index(10, 5);   // odd row
    const int even = w.index(10, 4);  // even row
    CHECK(w.hexNeighbour(mid, 0) == w.index(11, 5));
    CHECK(w.hexNeighbour(mid, 1) == w.index(11, 6));
    CHECK(w.hexNeighbour(mid, 2) == w.index(10, 6));
    CHECK(w.hexNeighbour(mid, 3) == w.index(9, 5));
    CHECK(w.hexNeighbour(mid, 4) == w.index(10, 4));
    CHECK(w.hexNeighbour(mid, 5) == w.index(11, 4));
    CHECK(w.hexNeighbour(even, 1) == w.index(10, 5));
    CHECK(w.hexNeighbour(even, 5) == w.index(10, 3));
    CHECK(w.hexNeighbour(w.index(w.width() - 1, 4), 0) == w.index(0, 4));
    CHECK(w.hexNeighbour(w.index(0, 0), 5) == -1);
    CHECK(w.hexNeighbour(w.index(0, w.height() - 1), 1) == -1);

    int adjacentChecked = 0;
    int farStarts = 0;
    for (const WorldSettlement& s : w.settlements()) {
        const int home = s.tile;
        CHECK(!w.canStartAt(home));
        for (int side = 0; side < 6; ++side) {
            const int n = w.hexNeighbour(home, side);
            if (n < 0) {
                continue;
            }
            const WorldTile& t = w.tile(n);
            if (!t.water && t.settlement == kNoSettlement) {
                CHECK(!w.canStartAt(n));
                CHECK(w.nearSettlement(n) >= 0);
                ++adjacentChecked;
            }
            // The tile beyond, two steps away in a line, when it touches no settlement, may start.
            const int far = w.hexNeighbour(n, side);
            if (far >= 0 && w.nearSettlement(far) < 0 && w.tile(far).settlement == kNoSettlement && w.canStartAt(far)) {
                ++farStarts;
            }
        }
    }
    CHECK(adjacentChecked > 0);
    CHECK(farStarts > 0);
    // No tile that may start touches a settlement.
    for (int i = 0; i < w.tileCount(); ++i) {
        if (w.canStartAt(i)) {
            CHECK(w.nearSettlement(i) < 0);
        }
    }
}

TEST_CASE("tropicsExtraTiles widens the hot, wet belt around the equator") {
    // The game's WorldGen def, with the extra tiles set to 0 for the narrow world.
    auto build = [](const char* replaceFrom, const char* replaceWith) {
        std::vector<DefSource> sources;
        const std::filesystem::path root = std::filesystem::path(COLONY_DATA_DIR) / "defs";
        for (const auto& kindDir : std::filesystem::directory_iterator(root)) {
            if (!kindDir.is_directory()) {
                continue;
            }
            for (const auto& file : std::filesystem::directory_iterator(kindDir.path())) {
                if (file.path().extension() != ".json") {
                    continue;
                }
                std::ifstream in(file.path());
                std::stringstream ss;
                ss << in.rdbuf();
                std::string text = ss.str();
                const size_t at = text.find(replaceFrom);
                if (kindDir.path().filename() == "WorldGen" && at != std::string::npos) {
                    text.replace(at, std::strlen(replaceFrom), replaceWith);
                }
                sources.push_back({kindDir.path().filename().string(), file.path().filename().string(), text});
            }
        }
        auto db = std::make_unique<DefDatabase>();
        std::vector<std::string> errors;
        std::string tuning;
        {
            std::ifstream in(std::filesystem::path(COLONY_DATA_DIR) / "tuning.json");
            std::stringstream ss;
            ss << in.rdbuf();
            tuning = ss.str();
        }
        DefDatabase::build(sources, tuning, *db, errors);
        CHECK(errors.empty());
        return db;
    };
    const auto narrow = build("\"tropicsExtraTiles\": 2", "\"tropicsExtraTiles\": 0");
    const auto wide = build("\"tropicsExtraTiles\": 2", "\"tropicsExtraTiles\": 2");
    const World a = World::generate(*narrow, 42);
    const World b = World::generate(*wide, 42);
    CHECK(!a.empty());
    CHECK(a.width() == b.width());
    if (a.empty() || a.width() != b.width()) {
        return;
    }
    CHECK(a.hash() != b.hash());

    auto tropical = [](const World& w, int& firstRow, int& lastRow) {
        int n = 0;
        firstRow = w.height();
        lastRow = -1;
        for (int i = 0; i < w.tileCount(); ++i) {
            const WorldTile& t = w.tile(i);
            if (t.water) {
                continue;
            }
            const std::string& name = w.biomeName(t);
            if (name == "TropicalRainforest" || name == "TropicalSwamp") {
                ++n;
                firstRow = std::min(firstRow, w.tileY(i));
                lastRow = std::max(lastRow, w.tileY(i));
            }
        }
        return n;
    };
    int aFirst = 0;
    int aLast = 0;
    int bFirst = 0;
    int bLast = 0;
    const int narrowCount = tropical(a, aFirst, aLast);
    const int wideCount = tropical(b, bFirst, bLast);
    CHECK(wideCount > narrowCount);
    CHECK(bFirst <= aFirst);
    CHECK(bLast >= aLast);
    CHECK((aFirst - bFirst) + (bLast - aLast) > 0);

    // The land is the same; only the temperature moves, warmer where the belt grew, never colder.
    bool warmer = false;
    for (int i = 0; i < a.tileCount(); ++i) {
        CHECK(a.tile(i).water == b.tile(i).water);
        CHECK(b.tile(i).temperature >= a.tile(i).temperature - 1e-4f);
        warmer = warmer || b.tile(i).temperature > a.tile(i).temperature + 0.01f;
    }
    CHECK(warmer);
    // On the equator rows nothing changes.
    const int eq = a.height() / 2;
    CHECK(a.tile(a.index(5, eq)).temperature == doctest::Approx(b.tile(b.index(5, eq)).temperature).epsilon(0.5));
}
