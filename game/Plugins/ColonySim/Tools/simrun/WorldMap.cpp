#include "WorldMap.h"

#include "ColonySim/World.h"

#include <stb_image_write.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace worldmap {

namespace {

struct WorldRgb {
    unsigned char r, g, b;
};

WorldRgb worldBiomeColour(const std::string& biome) {
    if (biome == "IceSheet") return {240, 245, 250};
    if (biome == "SeaIce") return {200, 225, 240};
    if (biome == "Tundra") return {170, 180, 160};
    if (biome == "BorealForest") return {60, 110, 80};
    if (biome == "ColdBog") return {90, 110, 100};
    if (biome == "TemperateForest") return {70, 150, 70};
    if (biome == "TemperateSwamp") return {80, 120, 70};
    if (biome == "AridShrubland") return {170, 160, 90};
    if (biome == "Desert") return {220, 190, 110};
    if (biome == "ExtremeDesert") return {235, 200, 140};
    if (biome == "TropicalRainforest") return {20, 120, 50};
    if (biome == "TropicalSwamp") return {50, 100, 60};
    return {255, 0, 255};
}

}  // namespace

int run(unsigned long long seed, int width, int height, const std::string& png, bool check,
        std::shared_ptr<const colony::DefDatabase> defs) {
    if (defs == nullptr) {
        std::fprintf(stderr, "--world needs the data folder\n");
        return 2;
    }
    colony::World world = colony::World::generate(*defs, seed, width, height);
    if (world.empty()) {
        std::fprintf(stderr, "no WorldGen def\n");
        return 2;
    }
    const int land = world.landTileCount();
    int rivers = 0;
    int lakes = 0;
    int peaks = 0;
    std::vector<int> perBiome(world.biomeNames().size(), 0);
    for (int i = 0; i < world.tileCount(); ++i) {
        const colony::WorldTile& t = world.tile(i);
        rivers += t.river != colony::RiverSize::None ? 1 : 0;
        peaks += t.hilliness == colony::Hilliness::Mountainous ? 1 : 0;
        if (!t.water && t.biome < perBiome.size()) {
            ++perBiome[t.biome];
        }
    }
    (void)lakes;
    colony::StartCriteria want;
    want.biome = "TemperateForest";
    want.needRiver = true;
    want.seed = seed;
    const int start = world.suggestStartTile(want);
    std::printf("world seed %llu  %dx%d tiles  land %d (%.0f%%)  river tiles %d  mountain tiles %d  settlements %zu  road edges %zu  hash %016llx\n",
                seed, world.width(), world.height(), land, 100.0 * land / world.tileCount(), rivers, peaks,
                world.settlements().size(), world.roads().size(), static_cast<unsigned long long>(world.hash()));
    for (size_t b = 0; b < perBiome.size(); ++b) {
        std::printf("  %-20s %d\n", world.biomeNames()[b].c_str(), perBiome[b]);
    }
    if (start >= 0) {
        const colony::WorldSite site = world.siteFor(start);
        std::printf("suggested start: tile %d (%d,%d) %s, %s, %.1f C, %.0f mm, river %s\n", start, world.tileX(start), world.tileY(start),
                    site.biome.c_str(), colony::hillinessName(site.hilliness), site.temperature, site.rainfall,
                    colony::riverSizeName(site.river));
    }

    if (!png.empty()) {
        const int scale = std::max(1, 960 / world.width());
        const int w = world.width() * scale;
        const int h = world.height() * scale;
        std::vector<unsigned char> px(static_cast<size_t>(w) * static_cast<size_t>(h) * 3, 0);
        const auto paint = [&](int x, int y, WorldRgb c) {
            if (x >= 0 && y >= 0 && x < w && y < h) {
                unsigned char* p = &px[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 3];
                p[0] = c.r;
                p[1] = c.g;
                p[2] = c.b;
            }
        };
        const auto centre = [&](int tile, int& cx, int& cy) {
            cx = world.tileX(tile) * scale + scale / 2;
            cy = world.tileY(tile) * scale + scale / 2;
        };
        const auto dot = [&](int tile, int radius, WorldRgb c) {
            int cx = 0;
            int cy = 0;
            centre(tile, cx, cy);
            for (int dy = -radius; dy <= radius; ++dy) {
                for (int dx = -radius; dx <= radius; ++dx) {
                    paint(cx + dx, cy + dy, c);
                }
            }
        };
        for (int i = 0; i < world.tileCount(); ++i) {
            const colony::WorldTile& t = world.tile(i);
            WorldRgb c{};
            if (t.water) {
                const float depth = std::min(1.0f, static_cast<float>(-t.elevation) / 4000.0f);
                c = {static_cast<unsigned char>(40 - 25 * depth), static_cast<unsigned char>(90 - 50 * depth),
                     static_cast<unsigned char>(170 - 60 * depth)};
                if (t.biome != colony::kNoBiome) {
                    c = worldBiomeColour(world.biomeName(t));
                }
            } else {
                c = worldBiomeColour(world.biomeName(t));
                const float shade = t.hilliness == colony::Hilliness::Flat ? 1.0f
                                    : t.hilliness == colony::Hilliness::SmallHills ? 0.9f
                                    : t.hilliness == colony::Hilliness::LargeHills ? 0.78f
                                    : t.hilliness == colony::Hilliness::Mountainous ? 0.6f : 0.4f;
                c = {static_cast<unsigned char>(c.r * shade), static_cast<unsigned char>(c.g * shade), static_cast<unsigned char>(c.b * shade)};
            }
            const int x0 = world.tileX(i) * scale;
            const int y0 = world.tileY(i) * scale;
            for (int dy = 0; dy < scale; ++dy) {
                for (int dx = 0; dx < scale; ++dx) {
                    paint(x0 + dx, y0 + dy, c);
                }
            }
        }
        const auto line = [&](int a, int b, WorldRgb c) {
            int ax = 0, ay = 0, bx = 0, by = 0;
            centre(a, ax, ay);
            centre(b, bx, by);
            if (std::abs(ax - bx) > w / 2) {  // across the seam: skip
                return;
            }
            const int steps = std::max(std::abs(bx - ax), std::abs(by - ay));
            for (int s = 0; s <= steps; ++s) {
                paint(ax + (bx - ax) * s / std::max(1, steps), ay + (by - ay) * s / std::max(1, steps), c);
            }
        };
        for (int i = 0; i < world.tileCount(); ++i) {
            const colony::WorldTile& t = world.tile(i);
            if (t.river != colony::RiverSize::None && t.riverFlowsTo != colony::kNoDirection) {
                line(i, world.neighbour(i, t.riverFlowsTo), {60, 130, 240});
            }
        }
        for (const colony::WorldRoad& r : world.roads()) {
            line(r.tileA, r.tileB, r.kind == colony::RoadKind::Asphalt ? WorldRgb{30, 30, 30}
                                   : r.kind == colony::RoadKind::Stone ? WorldRgb{110, 90, 70} : WorldRgb{180, 140, 90});
        }
        for (const colony::WorldSettlement& s : world.settlements()) {
            dot(s.tile, std::max(1, scale / 3), {230, 30, 30});
        }
        if (start >= 0) {
            dot(start, std::max(2, scale / 2), {255, 255, 0});
        }
        if (stbi_write_png(png.c_str(), w, h, 3, px.data(), w * 3) == 0) {
            std::fprintf(stderr, "could not write %s\n", png.c_str());
            return 1;
        }
    }

    if (!check) {
        return 0;
    }
    int failures = 0;
    const auto expect = [&](bool ok, const char* what) {
        if (!ok) {
            std::fprintf(stderr, "WORLD CHECK FAILED: %s\n", what);
            ++failures;
        }
    };
    const double landShare = static_cast<double>(land) / world.tileCount();
    expect(landShare > 0.2 && landShare < 0.6, "land share between 20% and 60%");
    expect(rivers > 0, "some rivers");
    expect(!world.settlements().empty(), "some settlements");
    expect(!world.roads().empty(), "some roads");
    expect(start >= 0, "a temperate forest start tile beside a river exists");
    int biomesUsed = 0;
    for (const int n : perBiome) {
        biomesUsed += n > 0 ? 1 : 0;
    }
    expect(biomesUsed >= 6, "at least 6 different biomes");
    expect(colony::World::generate(*defs, seed, width, height).hash() == world.hash(), "same seed, same world");
    return failures == 0 ? 0 : 1;
}

}  // namespace worldmap
