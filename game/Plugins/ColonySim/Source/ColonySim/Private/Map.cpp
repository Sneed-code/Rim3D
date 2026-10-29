#include "ColonySim/Map.h"

#include "ColonySim/Cover.h"
#include "ColonySim/Random.h"
#include "ColonySim/Regions.h"

#include <algorithm>
#include <cmath>
#include <tuple>

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
constexpr float kDeepBelow = 0.195f;       // fractions of max height
constexpr float kWaterBelow = 0.22f;       // below this: water, shallow near the shore
constexpr float kSandBelow = 0.26f;
constexpr float kRockAbove = 0.62f;

// Rivers (docs/design/mapgen.md). Distances are metres from the river's middle line.
constexpr float kRiverDeepBed = 0.17f;     // fractions of max height
constexpr float kRiverShallowBed = 0.20f;
constexpr float kRiverBank = 6.0f;         // the ground slopes up to its old height over this far
constexpr int kMapGenWorldMin = 192;       // maps smaller than this (test maps) are made as before S8: no
                                           // shallow shores, rivers, marsh or mud
constexpr float kRiverFordHalf = 8.0f;     // along the river: no deep water this far from the ford
constexpr float kRiverFordFade = 12.0f;

// Marsh and mud: low ground where a slow noise is high, and the shores of water.
constexpr float kMarshBelow = 0.34f;       // fractions of max height
constexpr float kMarshAbove = 0.58f;       // wetness times the biome's marsh, from here it is marsh
constexpr float kMudAbove = 0.72f;
constexpr float kShoreMarshAbove = 0.35f;
constexpr float kShoreMudAbove = 0.55f;

float mapGenClamp01(float v) { return std::min(1.0f, std::max(0.0f, v)); }

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
      roof_(static_cast<size_t>(settings.width) * static_cast<size_t>(settings.height), Roof::None),
      chunksX_((settings.width + kUpperChunk - 1) / kUpperChunk),
      chunksY_((settings.height + kUpperChunk - 1) / kUpperChunk),
      regions_(std::make_unique<Regions>(*this)),
      cover_(std::make_unique<Cover>(*this)),
      river_(settings.river),
      marsh_(settings.marsh) {
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

Map::UpperChunk& Map::upperChunkFor(CellPos c) {
    if (upper_.empty()) {
        upper_.resize(static_cast<size_t>(kLevels - 1) * static_cast<size_t>(chunksX_) * static_cast<size_t>(chunksY_));
    }
    std::unique_ptr<UpperChunk>& chunk = upper_[upperChunkIndex(c)];
    if (!chunk) {
        chunk = std::make_unique<UpperChunk>();
    }
    return *chunk;
}

Roof& Map::roofSlot(CellPos c) {
    return c.level == 0 ? roof_[static_cast<size_t>(index(c))] : upperChunkFor(c).roof[upperSlot(c)];
}

void Map::setGeyser(CellPos c, bool present) {
    if (!onGround(c)) {
        return;
    }
    if (geyser_.empty()) {
        geyser_.assign(static_cast<size_t>(width_) * static_cast<size_t>(height_), 0);
    }
    geyser_[static_cast<size_t>(index(c))] = present ? 1 : 0;
}

void Map::setFlowing(CellPos c, bool present) {
    if (!onGround(c)) {
        return;
    }
    if (flowing_.empty()) {
        flowing_.assign(static_cast<size_t>(width_) * static_cast<size_t>(height_), 0);
    }
    flowing_[static_cast<size_t>(index(c))] = present ? 1 : 0;
}

void Map::dryWater(CellPos c) {
    if (!onGround(c) || terrain(c) != Terrain::Water || flowing(c)) {
        return;
    }
    setTerrain(c, Terrain::Soil);
    dried_.push_back(static_cast<uint32_t>(index(c)));
}

void Map::restoreDried(const std::vector<uint32_t>& cells) {
    dried_.clear();
    for (const uint32_t i : cells) {
        if (i < static_cast<uint32_t>(width_) * static_cast<uint32_t>(height_)) {
            dryWater(cellAt(static_cast<int>(i)));
        }
    }
}

void Map::setBuilding(CellPos c, Entity building, Passage passage, uint8_t obstacleDm) {
    ++buildingChanges_;
    if (c.level != 0) {
        UpperChunk& chunk = upperChunkFor(c);
        const size_t i = upperSlot(c);
        chunk.building[i] = building;
        chunk.obstacle[i] = obstacleDm;  // cover is worked out for the ground only so far
        if (chunk.passage[i] != passage) {
            chunk.passage[i] = passage;
            regions_->markDirty(c);
        }
        return;
    }
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

void Map::setObstacle(CellPos c, uint8_t obstacleDm) {
    if (c.level != 0) {
        upperChunkFor(c).obstacle[upperSlot(c)] = obstacleDm;
        return;
    }
    const size_t i = static_cast<size_t>(index(c));
    if (obstacle_[i] != obstacleDm) {
        obstacle_[i] = obstacleDm;
        cover_->markDirty(c);
    }
}

void Map::addLevelLink(const LevelLink& link) {
    const auto before = [](const LevelLink& a, const LevelLink& b) {
        const auto key = [](CellPos c) { return std::make_tuple(c.level, c.y, c.x); };
        return key(a.low) != key(b.low) ? key(a.low) < key(b.low) : key(a.high) < key(b.high);
    };
    levelLinks_.insert(std::upper_bound(levelLinks_.begin(), levelLinks_.end(), link, before), link);
    regions_->markDirty(link.low);
    regions_->markDirty(link.high);
}

void Map::removeLevelLinks(Entity building) {
    for (auto it = levelLinks_.begin(); it != levelLinks_.end();) {
        if (it->building == building) {
            regions_->markDirty(it->low);
            regions_->markDirty(it->high);
            it = levelLinks_.erase(it);
        } else {
            ++it;
        }
    }
}

const LevelLink* Map::levelLinkBetween(CellPos a, CellPos b) const {
    for (const LevelLink& link : levelLinks_) {
        if ((link.low == a && link.high == b) || (link.low == b && link.high == a)) {
            return &link;
        }
    }
    return nullptr;
}

void Map::clearBuilding(CellPos c) {
    setBuilding(c, kNoEntity, Passage::Open, 0);
}

void Map::setGroundHeight(CellPos c, float metres) {
    if (c.level != 0 || !inBounds(c)) {
        return;
    }
    groundHeight_[static_cast<size_t>(index(c))] = metres;
    cover_->markDirty(c);
}

void Map::setTerrain(CellPos c, Terrain terrain) {
    if (c.level != 0 || terrain == Terrain::Air || terrain == Terrain::UpperFloor) {
        return;  // upper floors are laid with setUpperFloor
    }
    const size_t i = static_cast<size_t>(index(c));
    if (terrain_[i] == terrain) {
        return;
    }
    const bool walkableBefore = groundWalkable(c);
    const uint8_t obstacleBefore = obstacleDm(c);
    const bool wasRock = terrain_[i] == Terrain::Rock;
    terrain_[i] = terrain;
    if (wasRock && terrain != Terrain::Rock && roof_[i] == Roof::Thick) {
        hollow_.push_back(static_cast<int>(i));  // mined out under the mountain's roof
        ++rockChanges_;
    } else if (!wasRock && terrain == Terrain::Rock) {
        hollow_.erase(std::remove(hollow_.begin(), hollow_.end(), static_cast<int>(i)), hollow_.end());
    }
    if (groundWalkable(c) != walkableBefore) {
        regions_->markDirty(c);
    }
    if (obstacleDm(c) != obstacleBefore) {
        cover_->markDirty(c);
    }
}

void Map::setThickRoof(CellPos c, bool thick) {
    if (c.level != 0 || !inBounds(c)) {
        return;
    }
    const size_t i = static_cast<size_t>(index(c));
    if ((roof_[i] == Roof::Thick) == thick) {
        return;
    }
    roof_[i] = thick ? Roof::Thick : Roof::None;
    ++rockChanges_;
    if (terrain_[i] != Terrain::Rock) {
        if (thick) {
            hollow_.push_back(static_cast<int>(i));
        } else {
            hollow_.erase(std::remove(hollow_.begin(), hollow_.end(), static_cast<int>(i)), hollow_.end());
        }
    }
}

bool Map::setUpperFloor(CellPos c, bool floor) {
    if (c.level < 1 || !inBounds(c)) {
        return false;
    }
    if (upperFloor(c) == floor) {
        return true;
    }
    upperChunkFor(c).floor[upperSlot(c)] = floor ? 1 : 0;
    ++upperFloorChanges_;
    regions_->markDirty(c);
    // The cell below gained or lost its roof; rooms below see it through buildingChanges.
    ++buildingChanges_;
    return true;
}

std::vector<CellPos> Map::upperFloorCells() const {
    std::vector<CellPos> out;
    for (size_t k = 0; k < upper_.size(); ++k) {
        const UpperChunk* chunk = upper_[k].get();
        if (chunk == nullptr) {
            continue;
        }
        const int perLevel = chunksX_ * chunksY_;
        const int level = static_cast<int>(k) / perLevel + 1;
        const int x0 = static_cast<int>(k) % perLevel % chunksX_ * kUpperChunk;
        const int y0 = static_cast<int>(k) % perLevel / chunksX_ * kUpperChunk;
        for (int i = 0; i < UpperChunk::kCells; ++i) {
            if (chunk->floor[static_cast<size_t>(i)] != 0) {
                out.push_back(CellPos{x0 + i % kUpperChunk, y0 + i / kUpperChunk, level});
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const CellPos& a, const CellPos& b) {
        return a.level != b.level ? a.level < b.level : (a.y != b.y ? a.y < b.y : a.x < b.x);
    });
    return out;
}

uint64_t Map::upperFloorHash() const {
    uint64_t hash = 0xCBF29CE484222325ULL;
    for (size_t k = 0; k < upper_.size(); ++k) {
        const UpperChunk* chunk = upper_[k].get();
        if (chunk == nullptr) {
            continue;
        }
        for (int i = 0; i < UpperChunk::kCells; ++i) {
            if (chunk->floor[static_cast<size_t>(i)] != 0) {
                hash ^= static_cast<uint64_t>(k) * static_cast<uint64_t>(UpperChunk::kCells) + static_cast<uint64_t>(i);
                hash *= 0x100000001B3ULL;
            }
        }
    }
    return hash;
}

void Map::generate() {
    const bool world = std::min(width_, height_) >= kMapGenWorldMin;
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
            if (h < kDeepBelow || (h < kWaterBelow && !world)) {
                terrain_[i] = Terrain::Water;
            } else if (h < kWaterBelow) {
                terrain_[i] = Terrain::ShallowWater;
            } else if (h < kSandBelow) {
                terrain_[i] = Terrain::Sand;
            } else if (h > kRockAbove) {
                terrain_[i] = Terrain::Rock;
                roof_[i] = Roof::Thick;
            } else {
                terrain_[i] = Terrain::Soil;
            }
        }
    }
    if (world) {
        carveRivers();
        addWetlands();
    }
}

void Map::carveRivers() {
    const int shortSide = std::min(width_, height_);
    if (river_ <= 0.0f) {
        return;
    }
    const int count = shortSide >= 768 ? 2 : 1;
    const bool alongY = (mapGenHash(seed_, 0, 0, 100) & 1) == 0;  // all rivers of a map run the same way
    const int length = alongY ? height_ : width_;
    const int across = alongY ? width_ : height_;
    std::vector<float> centre(static_cast<size_t>(length));
    std::vector<float> deepHalf(static_cast<size_t>(length));
    std::vector<float> shallowHalf(static_cast<size_t>(length));
    float firstShare = 0.5f;
    for (int r = 0; r < count; ++r) {
        const uint64_t id = 100 + static_cast<uint64_t>(r) * 10;
        const float jitter = mapGenLattice(seed_, r, 1, id);
        float share = 0.2f + 0.6f * jitter;
        if (r == 0) {
            firstShare = share;
        } else {
            // The second river keeps well away from the first.
            share = firstShare < 0.5f ? firstShare + 0.3f + 0.1f * jitter : firstShare - 0.3f - 0.1f * jitter;
        }
        const float fordAt = static_cast<float>(length) * (0.25f + 0.5f * mapGenLattice(seed_, r, 2, id));
        const float acrossF = static_cast<float>(across);
        for (int a = 0; a < length; ++a) {
            const float af = static_cast<float>(a);
            const float meander = (mapGenValueNoise(seed_, af, 0.0f, 200.0f, id + 1) - 0.5f) * 0.30f * acrossF +
                                  (mapGenValueNoise(seed_, af, 0.0f, 50.0f, id + 2) - 0.5f) * 40.0f;
            centre[static_cast<size_t>(a)] = std::min(acrossF - 13.0f, std::max(12.0f, acrossF * share + meander));
            const float width = mapGenValueNoise(seed_, af, 0.0f, 90.0f, id + 3);
            const float ford = mapGenClamp01((std::fabs(af - fordAt) - kRiverFordHalf) / kRiverFordFade);
            const float deep = width < 0.38f ? 0.0f : (width - 0.38f) * 6.0f * river_ * ford;
            deepHalf[static_cast<size_t>(a)] = deep;
            shallowHalf[static_cast<size_t>(a)] = deep + (2.0f + 2.0f * mapGenValueNoise(seed_, af, 0.0f, 60.0f, id + 4)) * river_;
        }
        for (int a = 0; a < length; ++a) {
            const float mid = centre[static_cast<size_t>(a)];
            const float deepH = deepHalf[static_cast<size_t>(a)];
            const float shallowH = shallowHalf[static_cast<size_t>(a)];
            const float reach = shallowH + kRiverBank;
            const int from = std::max(0, static_cast<int>(std::floor(mid - reach)));
            const int to = std::min(across - 1, static_cast<int>(std::ceil(mid + reach)));
            for (int c = from; c <= to; ++c) {
                const float d = std::fabs(static_cast<float>(c) + 0.5f - mid);
                if (d > reach) {
                    continue;
                }
                const int x = alongY ? c : a;
                const int y = alongY ? a : c;
                const size_t i = static_cast<size_t>(y * width_ + x);
                const float old = groundHeight_[i];
                if (d <= shallowH) {
                    const bool deep = d <= deepH;
                    const float bed = (deep ? kRiverDeepBed : kRiverShallowBed) * kMapGenMaxHeight;
                    groundHeight_[i] = std::min(old, bed);
                    terrain_[i] = deep ? Terrain::Water : Terrain::ShallowWater;
                    roof_[i] = Roof::None;
                } else {
                    const float t = 1.0f - mapGenSmooth(mapGenClamp01((d - shallowH) / kRiverBank));
                    const float bed = kRiverShallowBed * kMapGenMaxHeight;
                    groundHeight_[i] = old + (std::min(old, bed) - old) * t;
                    if (terrain_[i] == Terrain::Rock && t > 0.2f) {
                        terrain_[i] = Terrain::Soil;  // the river cuts a valley through the mountain
                        roof_[i] = Roof::None;
                    }
                }
            }
        }
    }
}

void Map::addWetlands() {
    if (marsh_ <= 0.0f) {
        return;
    }
    const std::vector<Terrain> before = terrain_;
    const auto wet = [this](int x, int y) {
        const float fx = static_cast<float>(x);
        const float fy = static_cast<float>(y);
        return 0.65f * mapGenValueNoise(seed_, fx, fy, 48.0f, 30) + 0.35f * mapGenValueNoise(seed_, fx, fy, 16.0f, 31);
    };
    const auto isWater = [&before](Terrain t) { return t == Terrain::Water || t == Terrain::ShallowWater; };
    for (int y = 0; y < height_; ++y) {
        for (int x = 0; x < width_; ++x) {
            const size_t i = static_cast<size_t>(y * width_ + x);
            if (before[i] != Terrain::Soil && before[i] != Terrain::Sand) {
                continue;
            }
            const float w = wet(x, y) * marsh_;
            if (before[i] == Terrain::Soil && groundHeight_[i] < kMarshBelow * kMapGenMaxHeight) {
                if (w > kMudAbove) {
                    terrain_[i] = Terrain::Mud;
                    continue;
                }
                if (w > kMarshAbove) {
                    terrain_[i] = Terrain::Marsh;
                    continue;
                }
            }
            if (w <= kShoreMarshAbove) {
                continue;
            }
            bool shore = false;
            for (int dy = -1; dy <= 1 && !shore; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx;
                    const int ny = y + dy;
                    if (nx >= 0 && ny >= 0 && nx < width_ && ny < height_ &&
                        isWater(before[static_cast<size_t>(ny * width_ + nx)])) {
                        shore = true;
                        break;
                    }
                }
            }
            if (shore) {
                terrain_[i] = w > kShoreMudAbove ? Terrain::Mud : Terrain::Marsh;
            }
        }
    }
}

float Map::heightAt(float x, float y, int level) const {
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
    return top + (bottom - top) * fy + static_cast<float>(level) * kLevelHeightM;
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
        mix(static_cast<uint64_t>(roof_[i]));
    }
    return hash;
}

}  // namespace colony
