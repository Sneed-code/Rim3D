#include "ColonySim/Map.h"

#include "ColonySim/Cover.h"
#include "ColonySim/Random.h"
#include "ColonySim/Regions.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.
uint64_t mapGenHash(uint64_t seed, int64_t x, int64_t y, uint64_t octave) {
    uint64_t h = splitMix64(seed ^ (octave * 0xD1B54A32D192ED03ULL));
    h = splitMix64(h ^ static_cast<uint64_t>(x));
    return splitMix64(h ^ static_cast<uint64_t>(y));
}

float mapGenLattice(uint64_t seed, int64_t x, int64_t y, uint64_t octave) {
    return static_cast<float>(mapGenHash(seed, x, y, octave) >> 40) * (1.0f / 16777216.0f);
}

float mapGenSmooth(float t) { return t * t * (3.0f - 2.0f * t); }

// Value noise in [0, 1), with lattice points every cellSize metres.
float mapGenValueNoise(uint64_t seed, float fx, float fy, float cellSize, uint64_t octave) {
    const float gx = fx / cellSize;
    const float gy = fy / cellSize;
    const float x0f = std::floor(gx);
    const float y0f = std::floor(gy);
    const int64_t x0 = static_cast<int64_t>(x0f);
    const int64_t y0 = static_cast<int64_t>(y0f);
    const float tx = mapGenSmooth(gx - x0f);
    const float ty = mapGenSmooth(gy - y0f);
    const float a = mapGenLattice(seed, x0, y0, octave);
    const float b = mapGenLattice(seed, x0 + 1, y0, octave);
    const float c = mapGenLattice(seed, x0, y0 + 1, octave);
    const float d = mapGenLattice(seed, x0 + 1, y0 + 1, octave);
    const float top = a + (b - a) * tx;
    const float bottom = c + (d - c) * tx;
    return top + (bottom - top) * ty;
}

constexpr int kMapGenOctaves = 5;
constexpr float kMapGenBaseCell = 128.0f;  // metres between the largest hills
constexpr float kMapGenMaxHeight = 60.0f;  // metres
constexpr float kWaterBelow = 0.22f;       // fractions of max height
constexpr float kSandBelow = 0.26f;
constexpr float kRockAbove = 0.62f;

}  // namespace

Map::Map(const MapSettings& settings)
    : width_(settings.width),
      height_(settings.height),
      seed_(settings.seed),
      groundHeight_(static_cast<size_t>(settings.width) * static_cast<size_t>(settings.height), 0.0f),
      terrain_(static_cast<size_t>(settings.width) * static_cast<size_t>(settings.height), Terrain::Soil),
      building_(static_cast<size_t>(settings.width) * static_cast<size_t>(settings.height), kNoEntity),
      passage_(static_cast<size_t>(settings.width) * static_cast<size_t>(settings.height), Passage::Open),
      obstacle_(static_cast<size_t>(settings.width) * static_cast<size_t>(settings.height), 0),
      regions_(std::make_unique<Regions>(*this)),
      cover_(std::make_unique<Cover>(*this)) {
    if (!settings.flat) {
        generate();
    }
}

Map::~Map() = default;

Regions& Map::regions() {
    return *regions_;
}

Cover& Map::cover() {
    return *cover_;
}

void Map::setBuilding(CellPos c, Entity building, Passage passage, uint8_t obstacleDm) {
    const size_t i = static_cast<size_t>(index(c));
    building_[i] = building;
    if (obstacle_[i] != obstacleDm) {
        obstacle_[i] = obstacleDm;
        cover_->markDirty(c);
    }
    if (passage_[i] != passage) {
        passage_[i] = passage;
        regions_->markDirty(c);
    }
}

void Map::clearBuilding(CellPos c) {
    setBuilding(c, kNoEntity, Passage::Open, 0);
}

void Map::setTerrain(CellPos c, Terrain terrain) {
    const size_t i = static_cast<size_t>(index(c));
    if (terrain_[i] == terrain) {
        return;
    }
    const bool walkableBefore = groundWalkable(c);
    const uint8_t obstacleBefore = obstacleDm(c);
    terrain_[i] = terrain;
    if (groundWalkable(c) != walkableBefore) {
        regions_->markDirty(c);
    }
    if (obstacleDm(c) != obstacleBefore) {
        cover_->markDirty(c);
    }
}

void Map::generate() {
    for (int y = 0; y < height_; ++y) {
        for (int x = 0; x < width_; ++x) {
            float sum = 0.0f;
            float amplitude = 1.0f;
            float total = 0.0f;
            float cell = kMapGenBaseCell;
            for (int octave = 0; octave < kMapGenOctaves; ++octave) {
                sum += amplitude * mapGenValueNoise(seed_, static_cast<float>(x), static_cast<float>(y), cell,
                                                    static_cast<uint64_t>(octave));
                total += amplitude;
                amplitude *= 0.5f;
                cell *= 0.5f;
            }
            const float h = sum / total;
            const size_t i = static_cast<size_t>(index(CellPos{x, y, 0}));
            groundHeight_[i] = h * kMapGenMaxHeight;
            if (h < kWaterBelow) {
                terrain_[i] = Terrain::Water;
            } else if (h < kSandBelow) {
                terrain_[i] = Terrain::Sand;
            } else if (h > kRockAbove) {
                terrain_[i] = Terrain::Rock;
            } else {
                terrain_[i] = Terrain::Soil;
            }
        }
    }
}

float Map::heightAt(float x, float y) const {
    x = std::clamp(x, 0.0f, static_cast<float>(width_ - 1));
    y = std::clamp(y, 0.0f, static_cast<float>(height_ - 1));
    const int x0 = static_cast<int>(x);
    const int y0 = static_cast<int>(y);
    const int x1 = std::min(x0 + 1, width_ - 1);
    const int y1 = std::min(y0 + 1, height_ - 1);
    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);
    const auto h = [this](int cx, int cy) { return groundHeight_[static_cast<size_t>(cy * width_ + cx)]; };
    const float top = h(x0, y0) + (h(x1, y0) - h(x0, y0)) * fx;
    const float bottom = h(x0, y1) + (h(x1, y1) - h(x0, y1)) * fx;
    return top + (bottom - top) * fy;
}

uint64_t Map::contentHash() const {
    // FNV-1a over heights (to the centimetre), terrain and the building layer.
    uint64_t hash = 0xCBF29CE484222325ULL;
    const auto mix = [&hash](uint64_t value) {
        hash ^= value;
        hash *= 0x100000001B3ULL;
    };
    mix(static_cast<uint64_t>(width_));
    mix(static_cast<uint64_t>(height_));
    for (size_t i = 0; i < groundHeight_.size(); ++i) {
        mix(static_cast<uint64_t>(static_cast<int64_t>(std::lround(groundHeight_[i] * 100.0f))));
        mix(static_cast<uint64_t>(terrain_[i]));
        mix(static_cast<uint64_t>(entt::to_integral(building_[i])));
        mix(static_cast<uint64_t>(passage_[i]));
        mix(static_cast<uint64_t>(obstacle_[i]));
    }
    return hash;
}

}  // namespace colony
