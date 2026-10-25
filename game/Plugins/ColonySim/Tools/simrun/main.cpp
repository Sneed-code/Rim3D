// simrun: runs the sim with no graphics, prints a summary and can write a
// top-down PNG of the map.
//   simrun --seed 42 --size 512 --days 1 --png map.png
//   simrun --seed 42 --size 1024 --days 1 --pawns 20 --items 300   (Stage 0 sim speed test)
//   simrun --seed 42 --size 512 --days 0 --regions regions.png   (picture of regions and groups)
//   simrun --seed 42 --size 512 --days 0 --sight sight.png   (what a pawn sees among test walls)
//   simrun --size 64 --days 0 --light light.png --hour 22   (light at that hour round lamps and a roofed room)
//   simrun --replay bug.replay.json            (plays a replay and says whether it matched)
//   simrun --replay scenario.json --record out.replay.json
//   simrun --seed 42 --size 1024 --days 1 --pawns 100 --items 2000 --data game/Plugins/ColonySim/Tests/data
//                                     (pathfinding load test, with the test fixture defs; S3)
//   simrun --seed 42 --days 1 --pawns 10 --items 100 --save game.save   (writes a save after the run)
//   simrun --load game.save --days 1 --save later.save    (continues a save; --days defaults to 0)
//   simrun --load game.save --days 0 --save-json game.json   (readable dump of a save)
//   simrun --scenario colony [--seed 42] [--size 512] [--days 5] [--verbose]   (the colony test, C10;
//                                     exit 1 if a check fails, docs/design/colony-scenario.md)
// --replay takes the settings and commands from the file and runs to its endStep (--days
// overrides). --record writes what this run did as a replay, with its state hashes.
// --check makes it exit with 1 if any job or path search failed.
// Defs load from --data (default: the repo's data folder). The hauling test needs the Things defs
// Human and Steel and the WorkTypes def Hauling, which data/defs provides. Its pawns haul items
// scattered over the map to a stockpile zone in the middle through the job system (C1).
#include "ColonyScenario.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Lights.h"
#include "ColonySim/Mood.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Cover.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Designations.h"
#include "ColonySim/Mineables.h"
#include "ColonySim/Pathfinder.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Random.h"
#include "ColonySim/Regions.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/ThreatMap.h"
#include "ColonySim/Things.h"
#include "ColonySim/Version.h"
#include "ColonySim/Zones.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

struct Options {
    uint64_t seed = 42;
    int size = 512;
    double days = 1.0;
    bool daysGiven = false;
    int pawns = 0;
    int items = 0;
    int pathBudget = colony::Pathfinder::kDefaultBudget;
    std::string start;  // a Scenarios def; prints the colonists it makes
    std::string scenario;  // "colony": the scripted colony test (C10)
    bool verbose = false;  // --scenario: an hourly line on stderr
    bool check = false;  // exit 1 if a job or a path search failed
    int gather = 0;      // mark rock to mine and plants to harvest this many cells round the middle
    std::string png;
    std::string regionsPng;
    std::string replay;
    std::string record;
    std::string sightPng;
    std::string lightPng;
    double lightHour = 22.0;  // --light: the hour of day to draw
    std::string load;
    std::string save;
    std::string saveJson;
#ifdef COLONY_DATA_DIR
    std::string data = COLONY_DATA_DIR;
#else
    std::string data = "data";
#endif
};

bool parseOptions(int argc, char** argv, Options& out) {
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (std::strcmp(arg, "--seed") == 0 && hasValue) {
            out.seed = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(arg, "--size") == 0 && hasValue) {
            out.size = std::atoi(argv[++i]);
        } else if (std::strcmp(arg, "--days") == 0 && hasValue) {
            out.days = std::atof(argv[++i]);
            out.daysGiven = true;
        } else if (std::strcmp(arg, "--pawns") == 0 && hasValue) {
            out.pawns = std::atoi(argv[++i]);
        } else if (std::strcmp(arg, "--items") == 0 && hasValue) {
            out.items = std::atoi(argv[++i]);
        } else if (std::strcmp(arg, "--path-budget") == 0 && hasValue) {
            out.pathBudget = std::atoi(argv[++i]);
        } else if (std::strcmp(arg, "--start") == 0 && hasValue) {
            out.start = argv[++i];
        } else if (std::strcmp(arg, "--scenario") == 0 && hasValue) {
            out.scenario = argv[++i];
        } else if (std::strcmp(arg, "--verbose") == 0) {
            out.verbose = true;
        } else if (std::strcmp(arg, "--gather") == 0 && hasValue) {
            out.gather = std::atoi(argv[++i]);
        } else if (std::strcmp(arg, "--check") == 0) {
            out.check = true;
        } else if (std::strcmp(arg, "--png") == 0 && hasValue) {
            out.png = argv[++i];
        } else if (std::strcmp(arg, "--regions") == 0 && hasValue) {
            out.regionsPng = argv[++i];
        } else if (std::strcmp(arg, "--sight") == 0 && hasValue) {
            out.sightPng = argv[++i];
        } else if (std::strcmp(arg, "--light") == 0 && hasValue) {
            out.lightPng = argv[++i];
        } else if (std::strcmp(arg, "--hour") == 0 && hasValue) {
            out.lightHour = std::atof(argv[++i]);
        } else if (std::strcmp(arg, "--data") == 0 && hasValue) {
            out.data = argv[++i];
        } else if (std::strcmp(arg, "--replay") == 0 && hasValue) {
            out.replay = argv[++i];
        } else if (std::strcmp(arg, "--record") == 0 && hasValue) {
            out.record = argv[++i];
        } else if (std::strcmp(arg, "--load") == 0 && hasValue) {
            out.load = argv[++i];
        } else if (std::strcmp(arg, "--save") == 0 && hasValue) {
            out.save = argv[++i];
        } else if (std::strcmp(arg, "--save-json") == 0 && hasValue) {
            out.saveJson = argv[++i];
        } else {
            std::fprintf(stderr,
                         "usage: simrun [--seed N] [--size CELLS] [--days D] [--pawns N] [--items N] "
                         "[--path-budget N] [--start SCENARIO] [--gather RADIUS] [--scenario colony [--verbose]] [--check] [--png FILE] [--regions FILE] [--sight FILE] [--light FILE [--hour H]] [--data DIR] "
                         "[--replay FILE] [--record FILE] [--load FILE] [--save FILE] [--save-json FILE]\n");
            return false;
        }
    }
    return out.size > 0 && out.size <= 4096 && out.days >= 0.0 && out.pawns >= 0 && out.items >= 0 && out.pathBudget > 0;
}

bool writeMapPng(const colony::Sim& sim, const std::string& path) {
    const colony::Map& map = sim.map();
    const int w = map.width();
    const int h = map.height();
    std::vector<unsigned char> pixels(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float r = 0.0f;
            float g = 0.0f;
            float b = 0.0f;
            switch (map.terrain(x, y)) {
                case colony::Terrain::Water: r = 40; g = 90; b = 170; break;
                case colony::Terrain::Sand: r = 210; g = 195; b = 140; break;
                case colony::Terrain::Soil: r = 90; g = 130; b = 60; break;
                case colony::Terrain::Rock: r = 120; g = 115; b = 110; break;
                case colony::Terrain::RoughStone: r = 150; g = 140; b = 125; break;
            }
            // Ore shows brighter than the rock round it: steel blue, silver white, components orange.
            const colony::DefId rock = sim.mineables().defAt(colony::CellPos{x, y, 0});
            if (rock.valid() && sim.mineables().isOre(rock)) {
                const std::string& ore = sim.defs().get(rock).defName();
                if (ore == "Ore_Steel") { r = 90; g = 150; b = 230; }
                else if (ore == "Ore_Silver") { r = 240; g = 240; b = 250; }
                else { r = 240; g = 140; b = 40; }
            }
            const float shade = 0.6f + 0.4f * (map.groundHeight(x, y) / 60.0f);
            const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 3;
            pixels[i] = static_cast<unsigned char>(r * shade);
            pixels[i + 1] = static_cast<unsigned char>(g * shade);
            pixels[i + 2] = static_cast<unsigned char>(b * shade);
        }
    }
    // Plants: trees dark green, berry bushes purple, other bushes and brambles olive, flowers
    // yellow-white, grass a little greener and darker than the ground. A plant that is still
    // growing is shown fainter.
    const colony::Plants& plants = sim.plants();
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const colony::CellPos c{x, y, 0};
            if (!plants.has(c)) {
                continue;
            }
            const std::string& name = sim.defs().get(plants.defAt(c)).defName();
            float r = 60;
            float g = 125;
            float b = 40;
            if (name.find("Tree") != std::string::npos) {
                r = 15; g = 70; b = 25;
            } else if (name == "Plant_Berry") {
                r = 120; g = 40; b = 130;
            } else if (name == "Plant_Bush" || name == "Plant_Brambles") {
                r = 90; g = 100; b = 30;
            } else if (name == "Plant_Dandelion") {
                r = 235; g = 230; b = 150;
            }
            const float grown = 0.5f + 0.5f * plants.growth(c);
            const size_t i = static_cast<size_t>(map.index(c)) * 3;
            pixels[i] = static_cast<unsigned char>(pixels[i] + (r - pixels[i]) * grown);
            pixels[i + 1] = static_cast<unsigned char>(pixels[i + 1] + (g - pixels[i + 1]) * grown);
            pixels[i + 2] = static_cast<unsigned char>(pixels[i + 2] + (b - pixels[i + 2]) * grown);
        }
    }
    // Built roofs (rooms.md): the ground under them tinted warm.
    for (const colony::CellPos& c : sim.rooms().thinRoofCells()) {
        const size_t i = static_cast<size_t>(map.index(c)) * 3;
        pixels[i] = static_cast<unsigned char>((pixels[i] + 190) / 2);
        pixels[i + 1] = static_cast<unsigned char>((pixels[i + 1] + 110) / 2);
        pixels[i + 2] = static_cast<unsigned char>((pixels[i + 2] + 80) / 2);
    }
    // Buildings dark grey, items yellow, pawns red (drawn last, on top). Ground level only.
    const auto mark = [&](const colony::Position& pos, unsigned char r, unsigned char g, unsigned char b) {
        const colony::CellPos c = colony::cellOf(pos);
        if (!map.inBounds(c)) {
            return;
        }
        const size_t i = static_cast<size_t>(map.index(c)) * 3;
        pixels[i] = r;
        pixels[i + 1] = g;
        pixels[i + 2] = b;
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (map.buildingAt(colony::CellPos{x, y, 0}) != colony::kNoEntity) {
                mark(colony::Position{static_cast<float>(x), static_cast<float>(y), 0}, 60, 60, 60);
            }
        }
    }
    // Stockpile zones in pale blue, under the items stored in them.
    for (const colony::StockpileZone& zone : sim.zones().stockpiles()) {
        for (const colony::CellPos& c : zone.cells) {
            mark(colony::Position{static_cast<float>(c.x), static_cast<float>(c.y), 0}, 150, 190, 230);
        }
    }
    const entt::registry& reg = sim.things().registry();
    for (const colony::ThingCategory category : {colony::ThingCategory::Item, colony::ThingCategory::Pawn}) {
        reg.view<const colony::Thing, const colony::Position>().each(
            [&](const colony::Thing& thing, const colony::Position& pos) {
                if (thing.category != category) {
                    return;
                }
                if (category == colony::ThingCategory::Item) {
                    mark(pos, 255, 220, 0);
                } else {
                    mark(pos, 230, 20, 20);
                }
            });
    }
    return stbi_write_png(path.c_str(), w, h, 3, pixels.data(), w * 3) != 0;
}

// Regions picture: each region in its own colour, so the 16 m blocks and the cuts inside them
// show. Regions outside the biggest reachability group are drawn dark, doors white, and cells
// with no region (water, rock, walls) in a dim version of their terrain colour.
bool writeRegionsPng(colony::Sim& sim, const std::string& path) {
    const colony::Map& map = sim.map();
    colony::Regions& regions = sim.regions();
    const int w = map.width();
    const int h = map.height();
    uint32_t mainGroup = colony::kNoGroup;
    int mainCells = 0;
    for (uint32_t g = 0; g < regions.groupCount(); ++g) {
        if (regions.groupCellCount(g) > mainCells) {
            mainCells = regions.groupCellCount(g);
            mainGroup = g;
        }
    }
    std::vector<unsigned char> pixels(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const colony::CellPos c{x, y, 0};
            const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 3;
            const colony::RegionId id = regions.regionAt(c);
            unsigned char rgb[3] = {0, 0, 0};
            if (id == colony::kNoRegion) {
                switch (map.terrain(c)) {
                    case colony::Terrain::Water: rgb[0] = 20; rgb[1] = 35; rgb[2] = 70; break;
                    case colony::Terrain::Rock: rgb[0] = 45; rgb[1] = 45; rgb[2] = 45; break;
                    default: rgb[0] = 90; rgb[1] = 20; rgb[2] = 20; break;  // a wall or other blocking building
                }
            } else if (regions.region(id).kind == colony::RegionKind::Door) {
                rgb[0] = rgb[1] = rgb[2] = 255;
            } else {
                const uint64_t hash = colony::splitMix64(id);
                const bool inMain = regions.region(id).group == mainGroup;
                for (int k = 0; k < 3; ++k) {
                    const int v = 110 + static_cast<int>((hash >> (k * 8)) & 0x7F);  // 110..237
                    rgb[k] = static_cast<unsigned char>(inMain ? v : v / 3);
                }
            }
            pixels[i] = rgb[0];
            pixels[i + 1] = rgb[1];
            pixels[i + 2] = rgb[2];
        }
    }
    return stbi_write_png(path.c_str(), w, h, 3, pixels.data(), w * 3) != 0;
}

// Sight picture: scatters test walls (red) and sandbags (orange) around the middle of the map,
// then shows what a standing pawn (white) there can see within 60 m (bright) and what a crouching
// pawn can see (medium); the rest is dark. Also times the cover build and a threat map of 40
// armed pawns, which is what the fighter AI will ask for a few times a second.
bool writeSightPng(colony::Sim& sim, const std::string& path, uint64_t seed) {
    colony::Map& map = const_cast<colony::Map&>(sim.map());
    const int w = map.width();
    const int h = map.height();
    colony::Cover& cover = sim.cover();
    colony::Rng rng(seed ^ 0x5163ULL);

    // The pawn stands where the most ground around it can be walked (nearest the middle on a tie),
    // so the picture isn't mostly mountain.
    colony::CellPos centre{w / 2, h / 2, 0};
    int bestOpen = -1;
    for (int y = 16; y < h - 16; y += 8) {
        for (int x = 16; x < w - 16; x += 8) {
            int open = 0;
            for (int dy = -16; dy <= 16; dy += 4) {
                for (int dx = -16; dx <= 16; dx += 4) {
                    open += map.walkable(colony::CellPos{x + dx, y + dy, 0}) ? 1 : 0;
                }
            }
            const int far = std::abs(x - w / 2) + std::abs(y - h / 2);
            const int score = open * 4096 - far;
            if (map.walkable(colony::CellPos{x, y, 0}) && score > bestOpen) {
                bestOpen = score;
                centre = colony::CellPos{x, y, 0};
            }
        }
    }
    const int radius = std::min(60, std::min(w, h) / 2 - 1);
    for (int i = 0; i < 60; ++i) {
        const bool wall = i % 3 != 0;
        const bool horizontal = rng.below(2) == 0;
        const int length = 3 + static_cast<int>(rng.below(8));
        const int x = centre.x - radius + static_cast<int>(rng.below(static_cast<uint32_t>(2 * radius)));
        const int y = centre.y - radius + static_cast<int>(rng.below(static_cast<uint32_t>(2 * radius)));
        for (int k = 0; k < length; ++k) {
            const colony::CellPos c{x + (horizontal ? k : 0), y + (horizontal ? 0 : k), 0};
            if (map.walkable(c) && !(c == centre)) {
                map.setBuilding(c, colony::kNoEntity, wall ? colony::Passage::Blocked : colony::Passage::Open,
                                wall ? colony::kObstacleTallDm : colony::obstacleFromFill(0.55));
            }
        }
    }

    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    cover.update();
    const auto t1 = Clock::now();
    colony::ThreatMap& threat = sim.threatMap(1);
    int placed = 0;
    for (int tries = 0; tries < 4000 && placed < 40; ++tries) {
        const int x = centre.x - 100 + static_cast<int>(rng.below(200));
        const int y = centre.y - 100 + static_cast<int>(rng.below(200));
        if (map.walkable(colony::CellPos{x, y, 0})) {
            threat.addSource(colony::Viewpoint::at(colony::CellPos{x, y, 0}), 40.0f, 1.0f);
            ++placed;
        }
    }
    const auto t2 = Clock::now();
    int exposed = 0;
    int walkable = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const colony::CellPos c{x, y, 0};
            walkable += map.walkable(c) ? 1 : 0;
            exposed += threat.threatAt(c) > 0.0f ? 1 : 0;
        }
    }
    std::printf("{\"coverBuildMs\":%.2f,\"threatSources\":%d,\"threatMs\":%.2f,\"exposedCells\":%d,\"walkableCells\":%d}\n",
                std::chrono::duration<double, std::milli>(t1 - t0).count(), placed,
                std::chrono::duration<double, std::milli>(t2 - t1).count(), exposed, walkable);

    std::vector<unsigned char> pixels(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
    const colony::Viewpoint standing = colony::Viewpoint::at(centre, colony::Stance::Standing);
    const colony::Viewpoint crouching = colony::Viewpoint::at(centre, colony::Stance::Crouching);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const colony::CellPos c{x, y, 0};
            const size_t i = (static_cast<size_t>(h - 1 - y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 3;
            unsigned char rgb[3];
            const uint8_t dm = map.obstacleDm(c);
            const float dx = static_cast<float>(x - centre.x);
            const float dy = static_cast<float>(y - centre.y);
            const bool inRange = dx * dx + dy * dy <= static_cast<float>(radius * radius);
            if (map.terrain(c) == colony::Terrain::Rock) {
                rgb[0] = rgb[1] = rgb[2] = 60;
            } else if (dm == colony::kObstacleTallDm) {
                rgb[0] = 200; rgb[1] = 40; rgb[2] = 40;
            } else if (dm > 0) {
                rgb[0] = 230; rgb[1] = 140; rgb[2] = 30;
            } else if (map.terrain(c) == colony::Terrain::Water) {
                rgb[0] = 20; rgb[1] = 35; rgb[2] = 70;
            } else if (inRange && cover.lineOfSight(standing, colony::Viewpoint::at(c, colony::Stance::Standing))) {
                const bool low = cover.lineOfSight(crouching, colony::Viewpoint::at(c, colony::Stance::Crouching));
                rgb[0] = low ? 235 : 160; rgb[1] = low ? 235 : 190; rgb[2] = low ? 140 : 90;
            } else {
                rgb[0] = 35; rgb[1] = 50; rgb[2] = 35;
            }
            if (colony::CellPos{x, y, 0} == centre) {
                rgb[0] = rgb[1] = rgb[2] = 255;
            }
            pixels[i] = rgb[0];
            pixels[i + 1] = rgb[1];
            pixels[i + 2] = rgb[2];
        }
    }
    return stbi_write_png(path.c_str(), w, h, 3, pixels.data(), w * 3) != 0;
}

// Light picture (docs/design/light.md): builds a walled, roofed room with a torch lamp inside, a torch
// lamp and a campfire outside (one of them behind a wall) on the nearest open ground to the middle,
// runs the game to `hour` of the day and draws the light of every cell, six pixels a cell. Cells under
// a roof get a blue tint, lamps are red dots.
bool writeLightPng(colony::Sim& sim, const std::string& path, double hour) {
    const colony::Map& map = sim.map();
    const int w = map.width();
    const int h = map.height();
    constexpr int kAreaW = 30;
    constexpr int kAreaH = 20;
    colony::CellPos origin{-1, -1, 0};
    int bestFar = 1 << 30;
    for (int y = 1; y + kAreaH < h - 1; y += 2) {
        for (int x = 1; x + kAreaW < w - 1; x += 2) {
            const int far = std::abs(x + kAreaW / 2 - w / 2) + std::abs(y + kAreaH / 2 - h / 2);
            if (far >= bestFar) {
                continue;
            }
            bool open = true;
            for (int dy = 0; dy < kAreaH && open; ++dy) {
                for (int dx = 0; dx < kAreaW; ++dx) {
                    open = open && map.walkable(colony::CellPos{x + dx, y + dy, 0});
                }
            }
            if (open) {
                bestFar = far;
                origin = colony::CellPos{x, y, 0};
            }
        }
    }
    if (origin.x < 0) {
        std::fprintf(stderr, "no open ground of %d x %d cells for the light picture (try --size 128)\n", kAreaW, kAreaH);
        return false;
    }
    colony::Things& things = sim.things();
    for (int y = 2; y <= 10; ++y) {
        for (int x = 2; x <= 12; ++x) {
            if (x == 2 || x == 12 || y == 2 || y == 10) {
                things.spawn("Wall", colony::CellPos{origin.x + x, origin.y + y, 0});
            }
        }
    }
    things.spawn("TorchLamp", colony::CellPos{origin.x + 7, origin.y + 6, 0});   // inside the room
    things.spawn("TorchLamp", colony::CellPos{origin.x + 16, origin.y + 6, 0});  // outside, east of the room
    for (int y = 12; y <= 16; ++y) {
        things.spawn("Wall", colony::CellPos{origin.x + 22, origin.y + y, 0});   // a wall casting a shadow
    }
    things.spawn("Campfire", colony::CellPos{origin.x + 20, origin.y + 14, 0});
    sim.stepMany(static_cast<int64_t>(hour * static_cast<double>(colony::kSecondsPerHour)));

    const colony::Lights& lights = sim.lights();
    constexpr int kScale = 6;
    const int pw = w * kScale;
    const int ph = h * kScale;
    std::vector<unsigned char> pixels(static_cast<size_t>(pw) * static_cast<size_t>(ph) * 3);
    int lit = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const colony::CellPos c{x, y, 0};
            const float light = lights.lightAt(c);
            lit += light >= 0.3f ? 1 : 0;
            const bool roofed = map.roof(c) != colony::Roof::None;
            const float v = 30.0f + 225.0f * light;
            unsigned char rgb[3] = {static_cast<unsigned char>(v * (roofed ? 0.8f : 1.0f)),
                                    static_cast<unsigned char>(v * (roofed ? 0.85f : 1.0f)),
                                    static_cast<unsigned char>(roofed ? std::min(255.0f, v * 1.15f + 10.0f) : v * 0.9f)};
            if (map.passage(c) == colony::Passage::Blocked) {
                rgb[0] = rgb[1] = rgb[2] = static_cast<unsigned char>(40.0f + 60.0f * light);
            }
            for (const colony::LightSource& s : lights.sources()) {
                if (s.cell == c) {
                    rgb[0] = 255; rgb[1] = 40; rgb[2] = 40;
                }
            }
            for (int py = 0; py < kScale; ++py) {
                for (int px = 0; px < kScale; ++px) {
                    const size_t i = (static_cast<size_t>(y * kScale + py) * static_cast<size_t>(pw) + static_cast<size_t>(x * kScale + px)) * 3;
                    pixels[i] = rgb[0];
                    pixels[i + 1] = rgb[1];
                    pixels[i + 2] = rgb[2];
                }
            }
        }
    }
    std::printf("{\"hour\":%.1f,\"skyGlow\":%.2f,\"lightSources\":%zu,\"litCells\":%d,\"lightRebuilds\":%llu}\n", hour,
                static_cast<double>(lights.skyGlow()), lights.sources().size(), lit,
                static_cast<unsigned long long>(lights.rebuilds()));
    return stbi_write_png(path.c_str(), pw, ph, 3, pixels.data(), pw * 3) != 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) {
        return 2;
    }

    using Clock = std::chrono::steady_clock;
    const auto ms = [](auto from, auto to) { return std::chrono::duration<double, std::milli>(to - from).count(); };

    // Defs: a broken data folder is an error; a missing one is fine for map-only runs.
    std::shared_ptr<const colony::DefDatabase> defs;
    std::error_code ec;
    if (std::filesystem::exists(std::filesystem::path(options.data) / "defs", ec)) {
        auto loaded = std::make_shared<colony::DefDatabase>();
        std::vector<std::string> errors;
        if (!colony::DefDatabase::loadDirectory(options.data, *loaded, errors)) {
            for (const std::string& e : errors) {
                std::fprintf(stderr, "%s\n", e.c_str());
            }
            return 1;
        }
        defs = std::move(loaded);
    }

    // The colony scenario (C10) runs its own script on the Crashlanded start; 5 days unless --days says.
    if (!options.scenario.empty()) {
        if (options.scenario != "colony" || !defs) {
            std::fprintf(stderr, "--scenario takes 'colony' and needs the game's defs\n");
            return 2;
        }
        return colonyscenario::run(options.seed, options.size, options.daysGiven ? options.days : 5.0, defs, options.verbose);
    }

    // A replay brings its own settings and commands; otherwise they come from the options.
    colony::Replay replay;
    colony::SimSettings settings{options.seed, options.size, options.pawns, options.items};
    settings.pathBudget = options.pathBudget;
    settings.scenario = options.start;
    int64_t steps = static_cast<int64_t>(options.days * static_cast<double>(colony::kSecondsPerDay));
    if (!options.replay.empty()) {
        std::string error;
        if (!colony::loadReplayFile(options.replay, replay, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        settings = replay.settings;
        if (!options.daysGiven && replay.endStep >= 0) {
            steps = replay.endStep;
        }
        const uint64_t defsHash = defs ? defs->contentHash() : colony::DefDatabase().contentHash();
        if (replay.defsHash != 0 && replay.defsHash != defsHash) {
            std::fprintf(stderr, "warning: the data files differ from the ones this replay was recorded with\n");
        }
        if (!replay.simVersion.empty() && replay.simVersion != colony::kSimVersion) {
            std::fprintf(stderr, "warning: replay recorded by sim %s, this is %s\n", replay.simVersion.c_str(),
                         colony::kSimVersion);
        }
    }

    const auto start = Clock::now();
    std::unique_ptr<colony::Sim> simOwner;
    if (!options.load.empty()) {
        if (!options.replay.empty()) {
            std::fprintf(stderr, "--load and --replay can't be used together\n");
            return 2;
        }
        std::vector<uint8_t> bytes;
        std::string error;
        if (!colony::readSaveFile(options.load, bytes, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        colony::SaveLoadResult loaded = colony::loadSim(bytes, defs);
        if (!loaded.sim) {
            std::fprintf(stderr, "%s: %s\n", options.load.c_str(), loaded.error.c_str());
            return 1;
        }
        for (const std::string& w : loaded.warnings) {
            std::fprintf(stderr, "warning: %s\n", w.c_str());
        }
        simOwner = std::move(loaded.sim);
        settings = simOwner->settings();
        if (!options.daysGiven) {
            steps = 0;
        }
    } else {
        simOwner = std::make_unique<colony::Sim>(settings, defs);
    }
    colony::Sim& sim = *simOwner;
    const auto generated = Clock::now();
    if (!sim.setupErrors().empty()) {
        for (const std::string& e : sim.setupErrors()) {
            std::fprintf(stderr, "%s\n", e.c_str());
        }
        return 1;
    }
    if (!settings.scenario.empty()) {
        // The colonists the scenario made, one line each.
        sim.things().registry().view<const colony::Colonist>().each([](const colony::Colonist& c) {
            std::printf("%s%s \"%s\" %s, %d, %s / %s; traits:", c.protagonist ? "* " : "  ", c.firstName.c_str(),
                        c.nickName.c_str(), c.lastName.c_str(), c.age, c.childhood.c_str(), c.adulthood.c_str());
            for (const colony::TraitRecord& t : c.traits) {
                std::printf(" %s(%d)", t.def.c_str(), t.degree);
            }
            std::printf("; skills:");
            for (const colony::SkillRecord& s : c.skills) {
                std::printf(" %s %d%s%s", s.def.c_str(), s.level, s.passion == colony::Passion::Major ? "++" : s.passion == colony::Passion::Minor ? "+" : "",
                            s.disabled ? "x" : "");
            }
            std::printf("\n");
        });
    }

    // --gather: mark the rock to mine and the plants to harvest in a square round the middle (C3).
    size_t gatherMarked = 0;
    if (options.gather > 0) {
        const int mid = sim.map().width() / 2;
        const colony::CellRect area{mid - options.gather, mid - options.gather, 2 * options.gather, 2 * options.gather, 0};
        sim.submit(colony::DesignateCommand{area, colony::DesignationKind::Mine});
        sim.submit(colony::DesignateCommand{area, colony::DesignationKind::HarvestPlant});
    }

    // Time every step so spikes (long path searches) show up, not just the average.
    colony::ReplayFeeder feeder(replay);
    uint64_t events = 0;
    uint64_t commandsFailed = 0;
    uint64_t ate = 0;     // PawnAte events
    uint64_t wokeUp = 0;  // PawnWokeUp events
    uint64_t died = 0;    // PawnDied events
    uint64_t mined = 0;   // CellMined events
    uint64_t felled = 0;  // PlantRemoved events
    uint64_t picked = 0;  // PlantHarvested events
    std::vector<float> stepMs(static_cast<size_t>(steps));
    for (int64_t i = 0; i < steps; ++i) {
        feeder.submitDue(sim);
        const auto before = Clock::now();
        sim.step();
        if (i == 0) {
            gatherMarked = sim.designations().count();
        }
        stepMs[static_cast<size_t>(i)] = static_cast<float>(ms(before, Clock::now()));
        events += sim.events().size();
        for (const colony::Event& e : sim.events()) {
            ate += e.type == colony::EventType::PawnAte ? 1 : 0;
            wokeUp += e.type == colony::EventType::PawnWokeUp ? 1 : 0;
            died += e.type == colony::EventType::PawnDied ? 1 : 0;
            mined += e.type == colony::EventType::CellMined ? 1 : 0;
            felled += e.type == colony::EventType::PlantRemoved ? 1 : 0;
            picked += e.type == colony::EventType::PlantHarvested ? 1 : 0;
            if (e.type == colony::EventType::CommandFailed) {
                ++commandsFailed;
                std::fprintf(stderr, "step %lld: command %llu failed: %s\n", static_cast<long long>(i),
                             static_cast<unsigned long long>(e.command), e.message.c_str());
            }
        }
    }
    const auto finished = Clock::now();

    double totalMs = 0.0;
    for (const float v : stepMs) {
        totalMs += v;
    }
    const double avgMs = steps > 0 ? totalMs / static_cast<double>(steps) : 0.0;
    std::vector<float> sorted = stepMs;
    std::sort(sorted.begin(), sorted.end());
    const auto percentile = [&sorted](double p) {
        return sorted.empty() ? 0.0 : static_cast<double>(sorted[static_cast<size_t>(p * static_cast<double>(sorted.size() - 1))]);
    };
    // At 3x the sim runs 90 steps per real second; at 60 fps that is 1.5 steps per frame.
    const double msPerFrameAt3x = avgMs * 3.0 * colony::kStepsPerRealSecondAt1x / 60.0;

    const colony::ReplayCheck check = colony::checkReplay(replay, sim);
    const char* replayResult = options.replay.empty() ? "none" : check.compared == 0 ? "unchecked"
                               : check.matched()      ? "matched"
                                                      : "mismatch";

    std::printf("{\"version\":\"%s\",\"seed\":%llu,\"size\":%d,\"day\":%lld,\"mapHash\":\"%016llx\","
                "\"mapGenMs\":%.1f,\"stepMs\":%.1f,\"pawns\":%d,\"items\":%d,\"hauls\":%llu,\"itemsLeft\":%d,"
                "\"failedJobs\":%llu,\"avgStepMs\":%.4f,\"p99StepMs\":%.3f,\"maxStepMs\":%.3f,"
                "\"msPerFrameAt3x\":%.3f,\"defs\":%zu,\"things\":%zu,"
                "\"commands\":%zu,\"commandsFailed\":%llu,\"events\":%llu,\"stateHash\":\"%016llx\","
                "\"replay\":\"%s\",\"hashesCompared\":%d,\"firstMismatch\":%lld}\n",
                colony::kSimVersion, static_cast<unsigned long long>(settings.seed), settings.mapSize,
                static_cast<long long>(sim.time().day()), static_cast<unsigned long long>(sim.map().contentHash()),
                ms(start, generated), ms(generated, finished), settings.pawns, settings.items,
                static_cast<unsigned long long>(sim.stats().hauls), sim.itemsLeft(),
                static_cast<unsigned long long>(sim.stats().failedJobs), avgMs, percentile(0.99), percentile(1.0),
                msPerFrameAt3x, defs ? defs->size() : static_cast<size_t>(0), sim.things().count(),
                sim.commandLog().size(), static_cast<unsigned long long>(commandsFailed),
                static_cast<unsigned long long>(events), static_cast<unsigned long long>(sim.stateHash()), replayResult,
                check.compared, static_cast<long long>(check.firstMismatch));

    {
        const colony::Plants& plants = sim.plants();
        size_t mature = 0;
        size_t trees = 0;
        for (const colony::PlantRecord& r : plants.records()) {
            mature += r.growth >= colony::kPlantFullGrowth ? 1 : 0;
            trees += sim.defs().get(r.def).flag("plant.isTree") ? 1 : 0;
        }
        std::printf("{\"plants\":%zu,\"trees\":%zu,\"maturePlants\":%zu}\n", plants.count(), trees, mature);
    }
    {
        // Enclosed rooms by role, and the built roof (docs/design/rooms.md).
        int enclosed = 0;
        int open = 0;
        int roles[5] = {0, 0, 0, 0, 0};
        for (const colony::Room& room : sim.rooms().rooms()) {
            if (!room.enclosed()) {
                continue;
            }
            ++enclosed;
            open += room.outdoors() ? 1 : 0;
            ++roles[static_cast<int>(room.role)];
        }
        std::printf("{\"rooms\":{\"enclosed\":%d,\"unroofed\":%d,\"bedrooms\":%d,\"barracks\":%d,\"kitchens\":%d,"
                    "\"diningRooms\":%d,\"thinRoofCells\":%zu}}\n",
                    enclosed, open, roles[static_cast<int>(colony::RoomRole::Bedroom)],
                    roles[static_cast<int>(colony::RoomRole::Barracks)], roles[static_cast<int>(colony::RoomRole::Kitchen)],
                    roles[static_cast<int>(colony::RoomRole::DiningRoom)], sim.rooms().thinRoofCells().size());
    }

    if (!options.record.empty()) {
        std::string error;
        if (!colony::saveReplayFile(options.record, colony::recordReplay(sim), error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
    }

    // Pathfinding queue (docs/design/pathfinding.md).
    const colony::PathStats& paths = sim.pathfinder().stats();
    std::printf("{\"pathBudget\":%d,\"pathRequests\":%llu,\"pathRefused\":%llu,\"pathSearches\":%llu,"
                "\"pathFailedSearches\":%llu,\"pathRestarts\":%llu,\"regionsExpanded\":%llu,\"cellsExpanded\":%llu,"
                "\"maxPathWorkInStep\":%llu,\"maxPathQueue\":%u,\"maxPathWaitSteps\":%u}\n",
                settings.pathBudget, static_cast<unsigned long long>(paths.requests),
                static_cast<unsigned long long>(paths.refused), static_cast<unsigned long long>(paths.searches),
                static_cast<unsigned long long>(paths.failedSearches), static_cast<unsigned long long>(paths.restarts),
                static_cast<unsigned long long>(paths.regionsExpanded),
                static_cast<unsigned long long>(paths.cellsExpanded),
                static_cast<unsigned long long>(paths.maxWorkInStep), paths.maxQueue, paths.maxWaitSteps);

    // Needs (docs/design/needs.md): only when some pawn has food and rest.
    {
        int withNeeds = 0;
        int asleep = 0;
        float minFood = 1.0f;
        float minRest = 1.0f;
        for (const auto [pawn, needs] : sim.things().registry().view<const colony::Needs>().each()) {
            ++withNeeds;
            minFood = std::min(minFood, needs.food);
            minRest = std::min(minRest, needs.rest);
            const colony::Worker* worker = sim.things().registry().try_get<colony::Worker>(pawn);
            asleep += worker != nullptr && colony::jobIsLyingDown(worker->job) ? 1 : 0;
        }
        if (withNeeds > 0) {
            std::printf("{\"needs\":{\"pawns\":%d,\"ate\":%llu,\"wokeUp\":%llu,\"died\":%llu,\"asleepNow\":%d,"
                        "\"minFood\":%.3f,\"minRest\":%.3f}}\n",
                        withNeeds, static_cast<unsigned long long>(ate), static_cast<unsigned long long>(wokeUp),
                        static_cast<unsigned long long>(died), asleep, static_cast<double>(minFood), static_cast<double>(minRest));
        }
        // Mood (docs/design/mood.md): the lowest level, the worst break band and how many thoughts count now.
        {
            int withMood = 0;
            int worstBand = 0;
            int thoughtsNow = 0;
            float minMood = 100.0f;
            for (const auto [pawn, mood] : sim.things().registry().view<const colony::Mood>().each()) {
                ++withMood;
                minMood = std::min(minMood, mood.level);
                worstBand = std::max(worstBand, static_cast<int>(mood.band));
                thoughtsNow += static_cast<int>(sim.mood().thoughts(pawn, sim.time().seconds).size());
            }
            if (withMood > 0) {
                std::printf("{\"mood\":{\"pawns\":%d,\"minMood\":%.1f,\"worstBand\":\"%s\",\"thoughtsNow\":%d}}\n", withMood,
                            static_cast<double>(minMood), colony::moodBandName(static_cast<colony::MoodBand>(worstBand)), thoughtsNow);
            }
            if (options.check && withMood != withNeeds) {
                std::fprintf(stderr, "check failed: %d pawns have needs but %d have a mood\n", withNeeds, withMood);
                return 1;
            }
        }
        if (options.check && died > 0) {
            std::fprintf(stderr, "check failed: %llu pawn(s) died\n", static_cast<unsigned long long>(died));
            return 1;
        }
    }

    // Mining, chopping and harvesting (docs/design/mining.md): only when something was marked.
    if (options.gather > 0) {
        std::printf("{\"gather\":{\"marked\":%zu,\"mined\":%llu,\"felled\":%llu,\"picked\":%llu,\"stillMarked\":%zu,"
                    "\"rockCellsLeft\":%zu}}\n",
                    gatherMarked, static_cast<unsigned long long>(mined), static_cast<unsigned long long>(felled),
                    static_cast<unsigned long long>(picked), sim.designations().count(), sim.mineables().count());
        if (options.check && gatherMarked > 0 && mined + felled + picked == 0) {
            std::fprintf(stderr, "check failed: %zu cells marked and nothing was mined, felled or picked\n", gatherMarked);
            return 1;
        }
    }

    // Movement (docs/design/movement.md): walks that were given up and asked for again.
    std::printf("{\"repaths\":%llu,\"hauls\":%llu}\n", static_cast<unsigned long long>(sim.stats().repaths),
                static_cast<unsigned long long>(sim.stats().hauls));

    // A walk held up for good is rare (it was a rock corner once); many mean the movement is broken.
    if (options.check && sim.stats().repaths > 5 + sim.stats().hauls / 100) {
        std::fprintf(stderr, "check failed: %llu repaths for %llu hauls\n",
                     static_cast<unsigned long long>(sim.stats().repaths), static_cast<unsigned long long>(sim.stats().hauls));
        return 1;
    }
    if (options.check && (sim.stats().failedJobs > 0 || paths.failedSearches > 0)) {
        std::fprintf(stderr, "check failed: %llu failed jobs, %llu failed path searches\n",
                     static_cast<unsigned long long>(sim.stats().failedJobs),
                     static_cast<unsigned long long>(paths.failedSearches));
        return 1;
    }

    if (!options.save.empty() || !options.saveJson.empty()) {
        std::string error;
        if (!options.save.empty()) {
            const auto before = Clock::now();
            const std::vector<uint8_t> bytes = colony::saveSim(sim);
            const auto saved = Clock::now();
            if (!colony::writeSaveFile(options.save, bytes, error)) {
                std::fprintf(stderr, "%s\n", error.c_str());
                return 1;
            }
            std::printf("{\"saveBytes\":%zu,\"saveMs\":%.2f}\n", bytes.size(), ms(before, saved));
        }
        if (!options.saveJson.empty()) {
            std::ofstream file(options.saveJson, std::ios::binary | std::ios::trunc);
            const std::string json = colony::saveToJson(sim);
            file.write(json.data(), static_cast<std::streamsize>(json.size()));
            if (!file) {
                std::fprintf(stderr, "could not write %s\n", options.saveJson.c_str());
                return 1;
            }
        }
    }

    if (!options.regionsPng.empty()) {
        const size_t regionCount = sim.regions().regionCount();  // built with the sim, so in mapGenMs
        const size_t groups = sim.regions().groupCount();
        int biggest = 0;
        for (uint32_t g = 0; g < groups; ++g) {
            biggest = std::max(biggest, sim.regions().groupCellCount(g));
        }
        std::printf("{\"regions\":%zu,\"groups\":%zu,\"biggestGroupCells\":%d}\n", regionCount, groups, biggest);
        if (!writeRegionsPng(sim, options.regionsPng)) {
            std::fprintf(stderr, "could not write %s\n", options.regionsPng.c_str());
            return 1;
        }
    }
    if (!options.sightPng.empty() && !writeSightPng(sim, options.sightPng, options.seed)) {
        std::fprintf(stderr, "could not write %s\n", options.sightPng.c_str());
        return 1;
    }
    if (!options.lightPng.empty() && !writeLightPng(sim, options.lightPng, options.lightHour)) {
        std::fprintf(stderr, "could not write %s\n", options.lightPng.c_str());
        return 1;
    }
    if (!options.png.empty() && !writeMapPng(sim, options.png)) {
        std::fprintf(stderr, "could not write %s\n", options.png.c_str());
        return 1;
    }
    if (!check.matched()) {
        std::fprintf(stderr, "replay mismatch: the state first differed at game second %lld\n",
                     static_cast<long long>(check.firstMismatch));
        return 3;
    }
    return 0;
}