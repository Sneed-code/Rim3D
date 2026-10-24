// simrun: runs the sim with no graphics, prints a summary and can write a
// top-down PNG of the map.
//   simrun --seed 42 --size 512 --days 1 --png map.png
//   simrun --seed 42 --size 1024 --days 1 --pawns 20 --items 300   (Stage 0 sim speed test)
#include "ColonySim/Sim.h"
#include "ColonySim/Version.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
        } else {
            std::fprintf(stderr, "usage: simrun [--seed N] [--size CELLS] [--days D] [--pawns N] [--items N] [--png FILE]\n");
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
    // Items left on the ground in yellow, pawns in red.
    const auto mark = [&pixels](int cell, unsigned char r, unsigned char g, unsigned char b) {
        const size_t i = static_cast<size_t>(cell) * 3;
        pixels[i] = r;
        pixels[i + 1] = g;
        pixels[i + 2] = b;
    };
    for (const colony::Item& item : sim.items()) {
        if (!item.stored) {
            mark(item.cell, 255, 220, 0);
        }
    }
    for (const colony::Pawn& pawn : sim.pawns()) {
        mark(static_cast<int>(pawn.y) * w + static_cast<int>(pawn.x), 230, 20, 20);
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

    const auto start = Clock::now();
    colony::Sim sim(colony::SimSettings{options.seed, options.size, options.pawns, options.items});
    const auto generated = Clock::now();

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
                "\"p99StepMs\":%.3f,\"maxStepMs\":%.3f,\"msPerFrameAt3x\":%.3f}\n",
                colony::kSimVersion, static_cast<unsigned long long>(options.seed), options.size,
                static_cast<long long>(sim.time().day()), static_cast<unsigned long long>(sim.map().contentHash()),
                ms(start, generated), ms(generated, finished), options.pawns, options.items,
                static_cast<unsigned long long>(sim.stats().hauls), sim.itemsLeft(),
                static_cast<unsigned long long>(sim.stats().failedJobs),
                static_cast<unsigned long long>(sim.pathfinder().searches()),
                static_cast<unsigned long long>(sim.pathfinder().cellsExpanded()), avgMs, percentile(0.99),
                percentile(1.0), msPerFrameAt3x);

    if (!options.png.empty() && !writeMapPng(sim, options.png)) {
        std::fprintf(stderr, "could not write %s\n", options.png.c_str());
        return 1;
    }
    return 0;
}