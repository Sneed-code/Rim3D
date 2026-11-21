#include "ColonySim/Movement.h"

#include "ColonySim/Water.h"

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
        case Terrain::RoughStone:  // mined rock floor, as the Terrain defs: the same path cost as soil
            return 2.0f;
        case Terrain::Sand:
            return 4.0f;
        case Terrain::Marsh:  // the Terrain defs: marshy soil, mud and shallow water cost 14, 14 and 30
        case Terrain::Mud:
            return 14.0f;
        case Terrain::ShallowWater:
            return 30.0f;
        case Terrain::UpperFloor:  // a built floor: no path cost, like RimWorld's constructed floors
            return 0.0f;
        case Terrain::Water:
        case Terrain::Rock:
        case Terrain::Air:
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

float groundSpeedFactor(const Map& map, float x, float y, float bodyHeightM, int level) {
    const CellPos cell{std::clamp(static_cast<int>(std::floor(x)), 0, map.width() - 1),
                       std::clamp(static_cast<int>(std::floor(y)), 0, map.height() - 1), 0};
    const Terrain t = map.terrain(cell);
    if (level != 0) {
        return terrainSpeedFactor(t);
    }
    if (map.hasDeck(cell)) {
        return terrainSpeedFactor(Terrain::UpperFloor);  // a bridge deck: a built floor, no path cost (RimWorld: move speed 100%)
    }
    // Water by its depth against the body (docs/design/water.md): shallow water's cost to the waist,
    // chest-deep water's beyond, then swimming. Marsh and mud keep their own cost until the water is deeper.
    const WaterBand band = waterBandFor(map.waterDepthAt(x, y, 0), bodyHeightM);
    if (t == Terrain::Water || t == Terrain::ShallowWater) {
        return waterSpeedFactor(band == WaterBand::Dry ? WaterBand::Ankle : band);
    }
    const float own = terrainSpeedFactor(t);
    return band >= WaterBand::Waist ? std::min(own, waterSpeedFactor(band)) : own;
}

float walkSpeedFactor(const Map& map, float x, float y, float dirX, float dirY, float bodyHeightM, int level) {
    const float rise = map.heightAt(x + dirX * 0.5f, y + dirY * 0.5f) - map.heightAt(x - dirX * 0.5f, y - dirY * 0.5f);
    // A swimmer floats: the bottom's slope does not slow it.
    const float ground = groundSpeedFactor(map, x, y, bodyHeightM, level);
    if (level == 0 && waterBandFor(map.waterDepthAt(x, y, 0), bodyHeightM) == WaterBand::Swimming) {
        return ground;
    }
    return ground * slopeSpeedFactor(rise);
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

bool trunksClearOfLine(const Map& map, float x0, float y0, float x1, float y1, float radius, int level) {
    if (level != 0 || map.trunkCount() == 0) {
        return true;
    }
    // A trunk stands within 0.35 m of its cell's centre: cells more than a metre off the line's box can't reach it.
    const int minX = static_cast<int>(std::floor(std::min(x0, x1) - radius - 1.0f));
    const int maxX = static_cast<int>(std::floor(std::max(x0, x1) + radius + 1.0f));
    const int minY = static_cast<int>(std::floor(std::min(y0, y1) - radius - 1.0f));
    const int maxY = static_cast<int>(std::floor(std::max(y0, y1) + radius + 1.0f));
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float length2 = dx * dx + dy * dy;
    for (int cy = minY; cy <= maxY; ++cy) {
        for (int cx = minX; cx <= maxX; ++cx) {
            float tx = 0.0f;
            float ty = 0.0f;
            float tr = 0.0f;
            if (!map.trunkAt(CellPos{cx, cy, 0}, tx, ty, tr)) {
                continue;
            }
            const float reach = radius + tr;
            const float sx = x0 - tx;
            const float sy = y0 - ty;
            if (sx * sx + sy * sy < reach * reach) {
                continue;  // already beside it: walking away is allowed
            }
            const float t = length2 > 1.0e-8f ? std::clamp(((tx - x0) * dx + (ty - y0) * dy) / length2, 0.0f, 1.0f) : 0.0f;
            const float nx = x0 + dx * t - tx;
            const float ny = y0 + dy * t - ty;
            if (nx * nx + ny * ny < reach * reach) {
                return false;
            }
        }
    }
    return true;
}

bool clearOfTrunks(const Map& map, float x, float y, float radius, int level) {
    if (level != 0 || map.trunkCount() == 0) {
        return true;
    }
    const int cx = static_cast<int>(std::floor(x));
    const int cy = static_cast<int>(std::floor(y));
    const int span = 1 + static_cast<int>(std::ceil(radius));
    for (int ty = cy - span; ty <= cy + span; ++ty) {
        for (int tx = cx - span; tx <= cx + span; ++tx) {
            float sx = 0.0f;
            float sy = 0.0f;
            float tr = 0.0f;
            if (map.trunkAt(CellPos{tx, ty, 0}, sx, sy, tr)) {
                const float reach = radius + tr;
                if ((x - sx) * (x - sx) + (y - sy) * (y - sy) < reach * reach) {
                    return false;
                }
            }
        }
    }
    return true;
}

void cellWalkPoint(const Map& map, CellPos c, float& x, float& y) {
    x = static_cast<float>(c.x) + 0.5f;
    y = static_cast<float>(c.y) + 0.5f;
    if (c.level != 0 || map.trunkCount() == 0) {
        return;
    }
    const float lowX = static_cast<float>(c.x) + 0.05f;
    const float highX = static_cast<float>(c.x) + 0.95f;
    const float lowY = static_cast<float>(c.y) + 0.05f;
    const float highY = static_cast<float>(c.y) + 0.95f;
    // Pushed out of each trunk near enough to matter (its own and the neighbours'), twice over for a point between two.
    for (int pass = 0; pass < 2; ++pass) {
        bool moved = false;
        for (int ny = c.y - 1; ny <= c.y + 1; ++ny) {
            for (int nx = c.x - 1; nx <= c.x + 1; ++nx) {
                float tx = 0.0f;
                float ty = 0.0f;
                float tr = 0.0f;
                if (!map.trunkAt(CellPos{nx, ny, 0}, tx, ty, tr)) {
                    continue;
                }
                const float reach = kTrunkBodyRadius + tr + kTrunkMargin;
                float ox = x - tx;
                float oy = y - ty;
                const float d2 = ox * ox + oy * oy;
                if (d2 >= reach * reach) {
                    continue;
                }
                const float d = std::sqrt(d2);
                if (d < 1.0e-4f) {
                    ox = 1.0f;  // right on it: out along +x, the same way every time
                    oy = 0.0f;
                } else {
                    ox /= d;
                    oy /= d;
                }
                x = std::clamp(tx + ox * reach, lowX, highX);
                y = std::clamp(ty + oy * reach, lowY, highY);
                moved = true;
            }
        }
        if (!moved) {
            return;
        }
    }
    // Still too close (a thick trunk in the middle of the cell): the inset corner farthest from the trunks.
    float best = -1.0e9f;
    float bestX = x;
    float bestY = y;
    for (const float cornerY : {lowY + 0.1f, highY - 0.1f}) {
        for (const float cornerX : {lowX + 0.1f, highX - 0.1f}) {
            float nearest = 1.0e9f;
            for (int ny = c.y - 1; ny <= c.y + 1; ++ny) {
                for (int nx = c.x - 1; nx <= c.x + 1; ++nx) {
                    float tx = 0.0f;
                    float ty = 0.0f;
                    float tr = 0.0f;
                    if (map.trunkAt(CellPos{nx, ny, 0}, tx, ty, tr)) {
                        nearest = std::min(nearest, std::sqrt((cornerX - tx) * (cornerX - tx) + (cornerY - ty) * (cornerY - ty)) - tr);
                    }
                }
            }
            if (nearest > best) {
                best = nearest;
                bestX = cornerX;
                bestY = cornerY;
            }
        }
    }
    x = bestX;
    y = bestY;
}

void refreshTrunkSteps(Map& map, int x0, int y0, int x1, int y1) {
    if (map.trunkCount() == 0 && map.groundTightSteps() == nullptr) {
        return;  // no trees, and never were: no steps to work out
    }
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::min(x1, map.width() - 1);
    y1 = std::min(y1, map.height() - 1);
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const CellPos c{x, y, 0};
            // Nothing to work out with no trunk within 2 cells (all a step's line and walk points can meet).
            bool near = false;
            for (int ty = y - 2; ty <= y + 2 && !near; ++ty) {
                for (int tx = x - 2; tx <= x + 2 && !near; ++tx) {
                    near = map.hasTrunk(CellPos{tx, ty, 0});
                }
            }
            uint8_t steps = 0;
            if (near && map.walkable(c)) {
                float fromX = 0.0f;
                float fromY = 0.0f;
                cellWalkPoint(map, c, fromX, fromY);
                for (int d = 0; d < 8; ++d) {
                    const CellPos n{x + Map::kStepDx[d], y + Map::kStepDy[d], 0};
                    if (!map.inBounds(n)) {
                        continue;
                    }
                    float toX = 0.0f;
                    float toY = 0.0f;
                    cellWalkPoint(map, n, toX, toY);
                    if (!trunksClearOfLine(map, fromX, fromY, toX, toY, kTrunkBodyRadius, 0)) {
                        steps = static_cast<uint8_t>(steps | (1u << d));
                    }
                }
            }
            map.setTightSteps(c, steps);
        }
    }
}

namespace {

void moveSmoothLevel(const Map& map, float startX, float startY, std::vector<CellPos>& path);

}  // namespace

void smoothPath(const Map& map, float startX, float startY, std::vector<CellPos>& path, int startLevel) {
    if (path.empty()) {
        return;
    }
    if (startLevel < 0) {
        startLevel = path[0].level;
    }
    // A path that climbs stairs or a ladder is straightened a level at a time: the cell in front of
    // a climb and the cell it comes out at are always kept (MovementSystem climbs between them).
    size_t end = 0;
    while (end < path.size() && path[end].level == startLevel) {
        ++end;
    }
    if (end == path.size()) {
        moveSmoothLevel(map, startX, startY, path);
        return;
    }
    std::vector<CellPos> out;
    size_t begin = 0;
    float x = startX;
    float y = startY;
    while (begin < path.size()) {
        end = begin;
        while (end < path.size() && path[end].level == path[begin].level) {
            ++end;
        }
        // The first cell after a climb stays: the pawn arrives there by climbing.
        if (path[begin].level != (begin == 0 ? startLevel : path[begin - 1].level)) {
            out.push_back(path[begin]);
            x = static_cast<float>(path[begin].x) + 0.5f;
            y = static_cast<float>(path[begin].y) + 0.5f;
            ++begin;
        }
        std::vector<CellPos> part(path.begin() + static_cast<std::ptrdiff_t>(begin), path.begin() + static_cast<std::ptrdiff_t>(end));
        moveSmoothLevel(map, x, y, part);
        out.insert(out.end(), part.begin(), part.end());
        begin = end;
    }
    path.swap(out);
}

namespace {

void moveSmoothLevel(const Map& map, float startX, float startY, std::vector<CellPos>& path) {
    const int n = static_cast<int>(path.size());
    if (n <= 1) {
        return;
    }
    // Each cell's walk point (its centre unless a trunk is in the way), worked out once.
    std::vector<float> walkX(static_cast<size_t>(n));
    std::vector<float> walkY(static_cast<size_t>(n));
    for (int k = 0; k < n; ++k) {
        cellWalkPoint(map, path[static_cast<size_t>(k)], walkX[static_cast<size_t>(k)], walkY[static_cast<size_t>(k)]);
    }
    const auto px = [&walkX](int k) { return walkX[static_cast<size_t>(k)]; };
    const auto py = [&walkY](int k) { return walkY[static_cast<size_t>(k)]; };
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
        if (!straightWalkIsClear(map, curX, curY, px(j), py(j), kMoveSmoothRadius, level) ||
            !trunksClearOfLine(map, curX, curY, px(j), py(j), kMoveSmoothRadius, level)) {
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

}  // namespace

}  // namespace colony
