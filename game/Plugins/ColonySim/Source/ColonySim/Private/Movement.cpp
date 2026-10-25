#include "ColonySim/Movement.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

// Tobler's hiking function exp(-3.5 * |slope + 0.05|) divided by its value on the flat, at slopes
// -0.5, -0.45 ... 0.5. Written out so no platform's exp() can make two machines disagree.
constexpr float kMoveSlopeFactors[21] = {0.2466f, 0.2938f, 0.3499f, 0.4169f, 0.4966f, 0.5916f, 0.7047f,
                                         0.8395f, 1.0000f, 1.1912f, 1.0000f, 0.8395f, 0.7047f, 0.5916f,
                                         0.4966f, 0.4169f, 0.3499f, 0.2938f, 0.2466f, 0.2070f, 0.1738f};
constexpr float kMoveMaxSlope = 0.5f;
constexpr float kMoveSlopeSteps = 20.0f;  // table entries per unit of slope (every 0.05)

constexpr float kMovePathCostSlowing = 0.077f;  // RimWorld: each point of path cost adds 7.7% to the time
// A line is tested every kMoveSampleStep metres, so one can pass a corner between two tests a
// little closer than the pawn's radius. Testing with this much extra room makes up for that.
constexpr float kMoveSmoothRadius = kPawnRadius + 0.05f;
constexpr float kMoveSampleStep = 0.25f;        // metres between the spots a straight walk is tested at
constexpr float kMoveEffortStep = 0.5f;         // metres between the spots a walk's effort is measured at
constexpr float kMoveEffortSlack = 1.001f;      // a shortcut may cost this much of what it replaces

}  // namespace

float terrainPathCost(Terrain t) {
    switch (t) {
        case Terrain::Soil:
            return 2.0f;
        case Terrain::Sand:
            return 4.0f;
        case Terrain::Water:
        case Terrain::Rock:
            break;  // can't be walked
    }
    return 0.0f;
}

float terrainSpeedFactor(Terrain t) {
    return 1.0f / (1.0f + terrainPathCost(t) * kMovePathCostSlowing);
}

float slopeSpeedFactor(float slope) {
    const float pos = (std::clamp(slope, -kMoveMaxSlope, kMoveMaxSlope) + kMoveMaxSlope) * kMoveSlopeSteps;
    const int i = std::min(static_cast<int>(pos), 19);
    const float f = pos - static_cast<float>(i);
    return kMoveSlopeFactors[i] + (kMoveSlopeFactors[i + 1] - kMoveSlopeFactors[i]) * f;
}

float walkSpeedFactor(const Map& map, float x, float y, float dirX, float dirY) {
    const CellPos cell{std::clamp(static_cast<int>(std::floor(x)), 0, map.width() - 1),
                       std::clamp(static_cast<int>(std::floor(y)), 0, map.height() - 1), 0};
    const float rise = map.heightAt(x + dirX * 0.5f, y + dirY * 0.5f) - map.heightAt(x - dirX * 0.5f, y - dirY * 0.5f);
    return terrainSpeedFactor(map.terrain(cell)) * slopeSpeedFactor(rise);
}

float walkEffort(const Map& map, float x0, float y0, float x1, float y1) {
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length <= 0.0f) {
        return 0.0f;
    }
    const int n = std::max(1, static_cast<int>(std::ceil(length / kMoveEffortStep)));
    const float ds = length / static_cast<float>(n);
    const float ux = dx / length;
    const float uy = dy / length;
    float effort = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
        effort += ds / walkSpeedFactor(map, x0 + dx * t, y0 + dy * t, ux, uy);
    }
    return effort;
}

bool pawnFitsAt(const Map& map, float x, float y, float radius, int level) {
    const int minX = static_cast<int>(std::floor(x - radius));
    const int maxX = static_cast<int>(std::floor(x + radius));
    const int minY = static_cast<int>(std::floor(y - radius));
    const int maxY = static_cast<int>(std::floor(y + radius));
    for (int cy = minY; cy <= maxY; ++cy) {
        for (int cx = minX; cx <= maxX; ++cx) {
            if (map.walkable(CellPos{cx, cy, level})) {
                continue;
            }
            // The point of the blocked cell nearest the pawn's centre must be out of reach.
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

bool straightWalkIsClear(const Map& map, float x0, float y0, float x1, float y1, float radius, int level) {
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float length = std::sqrt(dx * dx + dy * dy);
    const int n = std::max(1, static_cast<int>(std::ceil(length / kMoveSampleStep)));
    for (int i = 1; i <= n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(n);
        if (!pawnFitsAt(map, x0 + dx * t, y0 + dy * t, radius, level)) {
            return false;
        }
    }
    return true;
}

void smoothPath(const Map& map, float startX, float startY, std::vector<CellPos>& path) {
    const int n = static_cast<int>(path.size());
    if (n <= 1) {
        return;
    }
    const auto px = [&path](int k) { return static_cast<float>(path[static_cast<size_t>(k)].x) + 0.5f; };
    const auto py = [&path](int k) { return static_cast<float>(path[static_cast<size_t>(k)].y) + 0.5f; };
    const int level = path[0].level;

    // effort[k]: the effort of walking from the start along the cell path to its cell k.
    std::vector<float> effort(static_cast<size_t>(n));
    effort[0] = walkEffort(map, startX, startY, px(0), py(0));
    for (int k = 1; k < n; ++k) {
        effort[static_cast<size_t>(k)] =
            effort[static_cast<size_t>(k - 1)] + walkEffort(map, px(k - 1), py(k - 1), px(k), py(k));
    }

    // From the cell the walk has reached (-1: the start), whether walking straight to cell j is
    // allowed: nothing in the way, and not more effort than the path's own cells.
    float curX = startX;
    float curY = startY;
    int cur = -1;
    const auto canSkipTo = [&](int j) {
        if (j == cur + 1) {
            return true;  // the path's own step
        }
        if (!straightWalkIsClear(map, curX, curY, px(j), py(j), kMoveSmoothRadius, level)) {
            return false;
        }
        const float before = cur < 0 ? 0.0f : effort[static_cast<size_t>(cur)];
        const float replaced = effort[static_cast<size_t>(j)] - before;
        return walkEffort(map, curX, curY, px(j), py(j)) <= replaced * kMoveEffortSlack + 0.001f;
    };

    std::vector<CellPos> out;
    while (cur < n - 1) {
        // Look further ahead by doubling until a cell can't be reached, then settle between the
        // last one that could and the first that couldn't. (If reachability isn't smooth the walk
        // is a little less straight than it could be, never wrong: every cell taken was tested.)
        int ok = cur + 1;
        int bad = -1;
        for (int reach = 1; bad < 0; reach *= 2) {
            const int j = std::min(cur + 1 + reach, n - 1);
            if (canSkipTo(j)) {
                ok = j;
                if (j == n - 1) {
                    break;
                }
            } else {
                bad = j;
            }
        }
        while (bad >= 0 && bad - ok > 1) {
            const int mid = ok + (bad - ok) / 2;
            if (canSkipTo(mid)) {
                ok = mid;
            } else {
                bad = mid;
            }
        }
        out.push_back(path[static_cast<size_t>(ok)]);
        cur = ok;
        curX = px(ok);
        curY = py(ok);
    }
    path.swap(out);
}

}  // namespace colony
