// simrun: runs the sim with no graphics, prints a summary and can write a
// top-down PNG of the map.
//   simrun --seed 42 --size 512 --days 1 --png map.png
#include "ColonySim/Sim.h"
#include "ColonySim/Version.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

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
        } else if (std::strcmp(arg, "--png") == 0 && hasValue) {
            out.png = argv[++i];
        } else {
            std::fprintf(stderr, "usage: simrun [--seed N] [--size CELLS] [--days D] [--png FILE]\n");
            return false;
        }
    }
    return out.size > 0 && out.size <= 4096 && out.days >= 0.0;
}

bool writeMapPng(const colony::Map& map, const std::string& path) {
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
    return stbi_write_png(path.c_str(), w, h, 3, pixels.data(), w * 3) != 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) {
        return 2;
    }

    const auto start = std::chrono::steady_clock::now();
    colony::Sim sim(colony::SimSettings{options.seed, options.size});
    const auto generated = std::chrono::steady_clock::now();
    sim.stepMany(static_cast<int64_t>(options.days * static_cast<double>(colony::kSecondsPerDay)));
    const auto finished = std::chrono::steady_clock::now();

    const auto ms = [](auto from, auto to) {
        return std::chrono::duration<double, std::milli>(to - from).count();
    };
    std::printf("{\"version\":\"%s\",\"seed\":%llu,\"size\":%d,\"day\":%lld,\"mapHash\":\"%016llx\","
                "\"mapGenMs\":%.1f,\"stepMs\":%.1f}\n",
                colony::kSimVersion, static_cast<unsigned long long>(options.seed), options.size,
                static_cast<long long>(sim.time().day()), static_cast<unsigned long long>(sim.map().contentHash()),
                ms(start, generated), ms(generated, finished));

    if (!options.png.empty() && !writeMapPng(sim.map(), options.png)) {
        std::fprintf(stderr, "could not write %s\n", options.png.c_str());
        return 1;
    }
    return 0;
}
