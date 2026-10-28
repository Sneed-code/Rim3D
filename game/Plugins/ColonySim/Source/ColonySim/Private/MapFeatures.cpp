#include "ColonySim/MapFeatures.h"

#include "ColonySim/Map.h"
#include "ColonySim/Random.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace colony {

namespace {

constexpr int kMapFeatMinMap = 256;
constexpr int kMapFeatEdge = 12;               // metres kept clear at the map's edge
constexpr float kMapFeatCentreClear = 45.0f;   // metres kept clear round the middle, where games start
constexpr float kMapFeatApart = 25.0f;         // metres between two features
constexpr float kMapFeatRuinRise = 1.5f;       // metres of ground height a ruin may stand across
constexpr float kMapFeatGeyserRise = 1.0f;
constexpr int kMapFeatRuinTries = 80;
constexpr int kMapFeatGeyserTries = 120;
constexpr int kMapFeatGeyserRockTries = 80;    // the first tries want rock close by
constexpr int kMapFeatRuinPer = 130000;        // cells of map for each ruin
constexpr int kMapFeatGeyserPer = 260000;      // and each geyser after the first
constexpr int kMapFeatKeepPercent = 68;        // how much of a ruin's wall is still standing

struct MapFeatSpot {
    float x;
    float y;
};

// True if the w x h block at (x0, y0) is open ground (soil or sand, no building, no roof) whose
// height varies by no more than maxRise.
bool mapFeatBlockClear(const Map& map, int x0, int y0, int w, int h, float maxRise) {
    float lo = 1.0e9f;
    float hi = -1.0e9f;
    for (int y = y0; y < y0 + h; ++y) {
        for (int x = x0; x < x0 + w; ++x) {
            const CellPos c{x, y, 0};
            if (!map.inBounds(c) || map.buildingAt(c) != kNoEntity || map.roof(c) != Roof::None) {
                return false;
            }
            const Terrain t = map.terrain(c);
            if (t != Terrain::Soil && t != Terrain::Sand) {
                return false;
            }
            lo = std::min(lo, map.groundHeight(c));
            hi = std::max(hi, map.groundHeight(c));
        }
    }
    return hi - lo <= maxRise;
}

bool mapFeatFarEnough(const Map& map, const std::vector<MapFeatSpot>& placed, float x, float y) {
    const float cx = static_cast<float>(map.width()) * 0.5f;
    const float cy = static_cast<float>(map.height()) * 0.5f;
    if (std::hypot(x - cx, y - cy) < kMapFeatCentreClear) {
        return false;
    }
    for (const MapFeatSpot& s : placed) {
        if (std::hypot(x - s.x, y - s.y) < kMapFeatApart) {
            return false;
        }
    }
    return true;
}

bool mapFeatNearRock(const Map& map, int x, int y) {
    static constexpr int kOffsets[8][2] = {{6, 0}, {-6, 0}, {0, 6}, {0, -6}, {10, 10}, {-10, 10}, {10, -10}, {-10, -10}};
    for (const auto& o : kOffsets) {
        const CellPos c{x + o[0], y + o[1], 0};
        if (map.inBounds(c) && map.terrain(c) == Terrain::Rock) {
            return true;
        }
    }
    return false;
}

}  // namespace

MapFeatureCounts placeMapFeatures(Map& map, Things& things, uint64_t seed) {
    MapFeatureCounts counts;
    const int shortSide = std::min(map.width(), map.height());
    const DefDatabase& defs = things.defs();
    const DefId wallDef = defs.idOf("Things", "AncientWall");
    const DefId geyserDef = defs.idOf("Things", "SteamGeyser");
    if (shortSide < kMapFeatMinMap || !wallDef.valid() || !geyserDef.valid()) {
        return counts;
    }
    Rng rng(seed ^ 0x72756E73ULL);
    const int area = map.width() * map.height();
    std::vector<MapFeatSpot> placed;

    const int ruinCount = std::max(1, (area + kMapFeatRuinPer / 2) / kMapFeatRuinPer);
    for (int r = 0; r < ruinCount; ++r) {
        for (int attempt = 0; attempt < kMapFeatRuinTries; ++attempt) {
            const int w = 7 + static_cast<int>(rng.below(7));
            const int h = 6 + static_cast<int>(rng.below(5));
            const int x0 = kMapFeatEdge + static_cast<int>(rng.below(static_cast<uint32_t>(map.width() - w - 2 * kMapFeatEdge)));
            const int y0 = kMapFeatEdge + static_cast<int>(rng.below(static_cast<uint32_t>(map.height() - h - 2 * kMapFeatEdge)));
            const float cx = static_cast<float>(x0) + static_cast<float>(w) * 0.5f;
            const float cy = static_cast<float>(y0) + static_cast<float>(h) * 0.5f;
            if (!mapFeatFarEnough(map, placed, cx, cy) || !mapFeatBlockClear(map, x0 - 1, y0 - 1, w + 2, h + 2, kMapFeatRuinRise)) {
                continue;
            }
            // Two gaps in the outline, where the doorways were; the rest is standing most of the time.
            const bool gapOnX[2] = {rng.below(2) == 0, rng.below(2) == 0};
            const int gapAt[2] = {2 + static_cast<int>(rng.below(static_cast<uint32_t>(std::max(1, (gapOnX[0] ? w : h) - 4)))),
                                  2 + static_cast<int>(rng.below(static_cast<uint32_t>(std::max(1, (gapOnX[1] ? w : h) - 4))))};
            const bool gapLow[2] = {rng.below(2) == 0, rng.below(2) == 0};
            const bool partition = rng.below(2) == 0;
            const int partitionAt = 3 + static_cast<int>(rng.below(static_cast<uint32_t>(std::max(1, w - 6))));
            int walls = 0;
            for (int dy = 0; dy < h; ++dy) {
                for (int dx = 0; dx < w; ++dx) {
                    const bool outline = dx == 0 || dy == 0 || dx == w - 1 || dy == h - 1;
                    const bool inner = partition && dx == partitionAt && !outline;
                    if (!outline && !inner) {
                        continue;
                    }
                    bool gap = false;
                    for (int g = 0; g < 2; ++g) {
                        // A gap two cells wide on one side of the outline.
                        if (gapOnX[g] ? ((gapLow[g] ? dy == 0 : dy == h - 1) && (dx == gapAt[g] || dx == gapAt[g] + 1))
                                      : ((gapLow[g] ? dx == 0 : dx == w - 1) && (dy == gapAt[g] || dy == gapAt[g] + 1))) {
                            gap = true;
                        }
                    }
                    const bool standing = static_cast<int>(rng.below(100)) < kMapFeatKeepPercent;
                    if (gap || !standing) {
                        continue;
                    }
                    if (things.spawn(wallDef, CellPos{x0 + dx, y0 + dy, 0}) != kNoEntity) {
                        ++walls;
                    }
                }
            }
            counts.ruinWalls += walls;
            ++counts.ruins;
            placed.push_back(MapFeatSpot{cx, cy});
            break;
        }
    }

    const int geyserCount = 1 + area / kMapFeatGeyserPer;
    for (int g = 0; g < geyserCount; ++g) {
        for (int attempt = 0; attempt < kMapFeatGeyserTries; ++attempt) {
            const int x0 = kMapFeatEdge + static_cast<int>(rng.below(static_cast<uint32_t>(map.width() - 2 - 2 * kMapFeatEdge)));
            const int y0 = kMapFeatEdge + static_cast<int>(rng.below(static_cast<uint32_t>(map.height() - 2 - 2 * kMapFeatEdge)));
            if (attempt < kMapFeatGeyserRockTries && !mapFeatNearRock(map, x0, y0)) {
                continue;
            }
            if (!mapFeatFarEnough(map, placed, static_cast<float>(x0) + 1.0f, static_cast<float>(y0) + 1.0f) ||
                !mapFeatBlockClear(map, x0, y0, 2, 2, kMapFeatGeyserRise)) {
                continue;
            }
            if (things.spawn(geyserDef, CellPos{x0, y0, 0}) != kNoEntity) {
                ++counts.geysers;
                placed.push_back(MapFeatSpot{static_cast<float>(x0) + 1.0f, static_cast<float>(y0) + 1.0f});
                break;
            }
        }
    }
    return counts;
}

}  // namespace colony
