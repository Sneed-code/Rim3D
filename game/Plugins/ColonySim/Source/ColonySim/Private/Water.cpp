#include "ColonySim/Water.h"

#include <algorithm>
#include <cmath>

namespace colony {

WaterBand waterBandFor(float depthM, float bodyHeightM) {
    const float height = std::max(bodyHeightM, 0.1f);
    const float share = depthM / height;
    if (share >= kWaterSwimShare) {
        return WaterBand::Swimming;
    }
    if (share >= kWaterChestShare) {
        return WaterBand::Chest;
    }
    if (share >= kWaterWaistShare) {
        return WaterBand::Waist;
    }
    if (share >= kWaterAnkleShare) {
        return WaterBand::Ankle;
    }
    return WaterBand::Dry;
}

WaterContact waterContactAt(const Map& map, float x, float y, int level, float bodyHeightM) {
    WaterContact out;
    if (level != 0) {
        return out;
    }
    out.depth = map.waterDepthAt(x, y, level);
    out.band = waterBandFor(out.depth, bodyHeightM);
    if (out.band == WaterBand::Dry) {
        return out;
    }
    const CellPos c{std::clamp(static_cast<int>(std::floor(x)), 0, map.width() - 1),
                    std::clamp(static_cast<int>(std::floor(y)), 0, map.height() - 1), 0};
    map.flowAt(c, out.flowX, out.flowY);
    switch (out.band) {
        case WaterBand::Swimming: out.carry = kWaterCarrySwim; break;
        case WaterBand::Chest: out.carry = kWaterCarryChest; break;
        case WaterBand::Waist: out.carry = kWaterCarryWaist; break;
        default: out.carry = 0.0f; break;
    }
    return out;
}

float waterSpeedFactor(WaterBand band) {
    constexpr float kWaterCostSlowing = 0.077f;  // as Movement.cpp: each point of path cost adds 7.7% to the time
    switch (band) {
        case WaterBand::Swimming: return kSwimSpeedFactor;
        case WaterBand::Chest: return 1.0f / (1.0f + kWaterChestPathCost * kWaterCostSlowing);
        case WaterBand::Waist:
        case WaterBand::Ankle: return 1.0f / (1.0f + kWaterShallowPathCost * kWaterCostSlowing);
        case WaterBand::Dry: break;
    }
    return 1.0f;
}

bool swimmerFitsAt(const Map& map, float x, float y, float radius, int level) {
    const int minX = static_cast<int>(std::floor(x - radius));
    const int maxX = static_cast<int>(std::floor(x + radius));
    const int minY = static_cast<int>(std::floor(y - radius));
    const int maxY = static_cast<int>(std::floor(y + radius));
    for (int cy = minY; cy <= maxY; ++cy) {
        for (int cx = minX; cx <= maxX; ++cx) {
            const CellPos c{cx, cy, level};
            if (map.walkable(c) ||
                (map.inBounds(c) && map.terrain(c) == Terrain::Water && map.passage(c) != Passage::Blocked)) {
                continue;
            }
            const float nx = std::clamp(x, static_cast<float>(cx), static_cast<float>(cx + 1));
            const float ny = std::clamp(y, static_cast<float>(cy), static_cast<float>(cy + 1));
            const float dx = x - nx;
            const float dy = y - ny;
            if (dx * dx + dy * dy < radius * radius) {
                return false;
            }
        }
    }
    return true;
}

bool nearestShore(const Map& map, float x, float y, int level, int radius, CellPos& out) {
    const int cx = static_cast<int>(std::floor(x));
    const int cy = static_cast<int>(std::floor(y));
    float best = static_cast<float>(radius * radius) + 1.0f;
    bool found = false;
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            const CellPos c{cx + dx, cy + dy, level};
            if (!map.walkable(c)) {
                continue;
            }
            const float ex = static_cast<float>(c.x) + 0.5f - x;
            const float ey = static_cast<float>(c.y) + 0.5f - y;
            const float d = ex * ex + ey * ey;
            if (d < best) {  // rows then columns in order, so the first of equals is kept
                best = d;
                out = c;
                found = true;
            }
        }
    }
    return found;
}

}  // namespace colony
