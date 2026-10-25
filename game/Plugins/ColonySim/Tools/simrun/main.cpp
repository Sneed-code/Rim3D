// simrun: runs the sim with no graphics, prints a summary and can write a
// top-down PNG of the map.
//   simrun --seed 42 --size 512 --days 1 --png map.png
//   simrun --seed 42 --size 1024 --days 1 --pawns 20 --items 300   (Stage 0 sim speed test)
//   simrun --seed 42 --size 512 --days 0 --regions regions.png   (picture of regions and groups)
// Defs load from --data (default: the repo's data folder). The hauling test needs the Things defs
// Human and Steel, which the RimWorld import provides.
#include "ColonySim/Defs.h"
#include "ColonySim/Random.h"
#include "ColonySim/Regions.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"
#include "ColonySim/Version.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

struct Options {
    uint64_t seed = 42;
    int size = 512;
    double days = 1.0;
    int pawns = 0;
    int items = 0;
    std::string png;
    std::string regionsPng;
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
        } else if (std::strcmp(arg, "--pawns") == 0 && hasValue) {
            out.pawns = std::atoi(argv[++i]);
        } else if (std::strcmp(arg, "--items") == 0 && hasValue) {
            out.items = std::atoi(argv[++i]);
        } else if (std::strcmp(arg, "--png") == 0 && hasValue) {
            out.png = argv[++i];
        } else if (std::strcmp(arg, "--regions") == 0 && hasValue) {
            out.regionsPng = argv[++i];
        } else if (std::strcmp(arg, "--data") == 0 && hasValue) {
            out.data = argv[++i];
        } else {
            std::fprintf(stderr, "usage: simrun [--seed N] [--size CELLS] [--days D] [--pawns N] [--items N] [--png FILE] [--regions FILE] [--data DIR]\n");
            return false;
        }
    }
    return out.size > 0 && out.size <= 4096 && out.days >= 0.0 && out.pawns >= 0 && out.items >= 0;
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
            }
            const float shade = 0.6f + 0.4f * (map.groundHeight(x, y) / 60.0f);
            const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 3;
            pixels[i] = static_cast<unsigned char>(r * shade);
            pixels[i + 1] = static_cast<unsigned char>(g * shade);
            pixels[i + 2] = static_cast<unsigned char>(b * shade);
        }
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
    const entt::registry& reg = sim.things().registry();
    for (const colony::ThingCategory category : {colony::ThingCategory::Item, colony::ThingCategory::Pawn}) {
        reg.view<const colony::Thing, const colony::Position>().each(
            [&](const colony::Thing& thing, const colony::Position& pos) {
                if (thing.category != category || sim.stockpileCell() == colony::cellOf(pos)) {
                    return;  // stored items pile up on the stockpile; leave it showing the ground
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

    const auto start = Clock::now();
    colony::SimSettings settings{options.seed, options.size, options.pawns, options.items};
    colony::Sim sim(settings, defs);
    const auto generated = Clock::now();
    if (!sim.setupErrors().empty()) {
        for (const std::string& e : sim.setupErrors()) {
            std::fprintf(stderr, "%s\n", e.c_str());
        }
        return 1;
    }

    // Time every step so spikes (long path searches) show up, not just the average.
    const int64_t steps = static_cast<int64_t>(options.days * static_cast<double>(colony::kSecondsPerDay));
    std::vector<float> stepMs(static_cast<size_t>(steps));
    for (int64_t i = 0; i < steps; ++i) {
        const auto before = Clock::now();
        sim.step();
        stepMs[static_cast<size_t>(i)] = static_cast<float>(ms(before, Clock::now()));
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

    std::printf("{\"version\":\"%s\",\"seed\":%llu,\"size\":%d,\"day\":%lld,\"mapHash\":\"%016llx\","
                "\"mapGenMs\":%.1f,\"stepMs\":%.1f,\"pawns\":%d,\"items\":%d,\"hauls\":%llu,\"itemsLeft\":%d,"
                "\"failedJobs\":%llu,\"pathSearches\":%llu,\"cellsExpanded\":%llu,\"avgStepMs\":%.4f,"
                "\"p99StepMs\":%.3f,\"maxStepMs\":%.3f,\"msPerFrameAt3x\":%.3f,\"defs\":%zu,\"things\":%zu}\n",
                colony::kSimVersion, static_cast<unsigned long long>(options.seed), options.size,
                static_cast<long long>(sim.time().day()), static_cast<unsigned long long>(sim.map().contentHash()),
                ms(start, generated), ms(generated, finished), options.pawns, options.items,
                static_cast<unsigned long long>(sim.stats().hauls), sim.itemsLeft(),
                static_cast<unsigned long long>(sim.stats().failedJobs),
                static_cast<unsigned long long>(sim.pathfinder().searches()),
                static_cast<unsigned long long>(sim.pathfinder().cellsExpanded()), avgMs, percentile(0.99),
                percentile(1.0), msPerFrameAt3x, defs ? defs->size() : static_cast<size_t>(0), sim.things().count());

    if (!options.regionsPng.empty()) {
        const auto before = Clock::now();
        const size_t regionCount = sim.regions().regionCount();  // the first query builds every block
        const auto built = Clock::now();
        const size_t groups = sim.regions().groupCount();
        int biggest = 0;
        for (uint32_t g = 0; g < groups; ++g) {
            biggest = std::max(biggest, sim.regions().groupCellCount(g));
        }
        std::printf("{\"regions\":%zu,\"groups\":%zu,\"biggestGroupCells\":%d,\"regionBuildMs\":%.2f}\n", regionCount,
                    groups, biggest, ms(before, built));
        if (!writeRegionsPng(sim, options.regionsPng)) {
            std::fprintf(stderr, "could not write %s\n", options.regionsPng.c_str());
            return 1;
        }
    }
    if (!options.png.empty() && !writeMapPng(sim, options.png)) {
        std::fprintf(stderr, "could not write %s\n", options.png.c_str());
        return 1;
    }
    return 0;
}