#pragma once

#include "ColonySim/Api.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace colony {

// Placeholder terrain types until terrain defs are loaded from data/defs.
enum class Terrain : uint8_t { Water, Sand, Soil, Rock };

struct MapSettings {
    int width = 512;   // cells, 1 m each
    int height = 512;  // cells, 1 m each
    uint64_t seed = 0;
};

// The map grid: one flat array per layer, generated from a seed.
class COLONY_API Map {
public:
    explicit Map(const MapSettings& settings);

    int width() const { return width_; }
    int height() const { return height_; }
    bool inBounds(int x, int y) const { return x >= 0 && y >= 0 && x < width_ && y < height_; }
    int index(int x, int y) const { return y * width_ + x; }

    // Ground height in metres.
    float groundHeight(int x, int y) const { return groundHeight_[static_cast<size_t>(index(x, y))]; }
    Terrain terrain(int x, int y) const { return terrain_[static_cast<size_t>(index(x, y))]; }

    // Hash of every layer, for determinism tests.
    uint64_t contentHash() const;

private:
    void generate();

    int width_;
    int height_;
    uint64_t seed_;
    std::vector<float> groundHeight_;
    std::vector<Terrain> terrain_;
};

}  // namespace colony
