#include "ColonySim/MapRoads.h"

#include "ColonySim/Floors.h"
#include "ColonySim/Map.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Random.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <functional>
#include <limits>
#include <queue>
#include <utility>
#include <vector>

namespace colony {

namespace {

constexpr int kMapRoadMinMap = 256;
constexpr int kMapRoadNode = 4;          // metres: the route is found on a grid of 4 m blocks, then smoothed
constexpr int kMapRoadBase = 10;         // cost of crossing a flat, dry block
constexpr int kMapRoadOnRoad = 3;        // a block already on a road: later roads join it
constexpr int kMapRoadRiseCost = 3;      // per 10 cm of height across a block
constexpr int kMapRoadRockCost = 20;     // per rock cell in a block
constexpr int kMapRoadRockBlocked = 4;   // rock cells that close a block
constexpr int kMapRoadDeepCost = 12;     // per deep-water cell: a bridge is long work
constexpr int kMapRoadShallowCost = 8;
constexpr int kMapRoadWetCost = 1;       // marsh and mud
constexpr int kMapRoadBankCost = 25;     // a dry block within kMapRoadBankBlocks of water
constexpr int kMapRoadBankBlocks = 2;
constexpr int kMapRoadCorridor = 10;     // metres either side of the coarse route the fine one may go
constexpr int kMapRoadThin = 3;          // the fine route keeps every 3rd cell before smoothing
constexpr int kMapRoadFineDeep = 400;    // per metre of deep water crossed
constexpr int kMapRoadFineShallow = 300;
constexpr int kMapRoadFineWet = 6;
constexpr int kMapRoadFineRise = 4;      // per 10 cm step to the next cell
constexpr int kMapRoadFineRiseMax = 80;
constexpr int kMapRoadFineBank = 30;     // dry ground the road's edge would reach water from
constexpr int kMapRoadFineRockSide = 15; // right beside rock
constexpr float kMapRoadCentre = 0.8f;  // metres: a road's middle
constexpr int64_t kMapRoadNoPath = std::numeric_limits<int64_t>::max();

struct MapRoadPoint {
    float x;
    float y;
};

// Width in metres of a road of this kind (ours: RimWorld draws them about this wide).
int mapRoadWidth(RoadKind kind) {
    switch (kind) {
        case RoadKind::Asphalt: return 6;
        case RoadKind::Stone: return 4;
        case RoadKind::Dirt:
        case RoadKind::None: break;
    }
    return 3;
}

const char* mapRoadFloor(RoadKind kind) {
    switch (kind) {
        case RoadKind::Asphalt: return "BrokenAsphalt";
        case RoadKind::Stone: return "Flagstone";
        case RoadKind::Dirt:
        case RoadKind::None: break;
    }
    return "PackedDirt";
}

class MapRoadGrid {
public:
    explicit MapRoadGrid(const Map& map) : map_(map), nx_(map.width() / kMapRoadNode), ny_(map.height() / kMapRoadNode) {
        cost_.assign(static_cast<size_t>(nx_ * ny_), 0);
        wet_.assign(static_cast<size_t>(nx_ * ny_), 0);
        for (int by = 0; by < ny_; ++by) {
            for (int bx = 0; bx < nx_; ++bx) {
                cost_[static_cast<size_t>(by * nx_ + bx)] = blockCost(bx, by);
            }
        }
        // A dry block beside water costs more, so a road crosses a river and doesn't run along its bank.
        for (int by = 0; by < ny_; ++by) {
            for (int bx = 0; bx < nx_; ++bx) {
                const size_t i = static_cast<size_t>(by * nx_ + bx);
                if (cost_[i] == 0 || wet_[i] != 0) {
                    continue;
                }
                bool bank = false;
                for (int dy = -kMapRoadBankBlocks; dy <= kMapRoadBankBlocks && !bank; ++dy) {
                    for (int dx = -kMapRoadBankBlocks; dx <= kMapRoadBankBlocks && !bank; ++dx) {
                        const int x = bx + dx;
                        const int y = by + dy;
                        bank = x >= 0 && y >= 0 && x < nx_ && y < ny_ && wet_[static_cast<size_t>(y * nx_ + x)] != 0;
                    }
                }
                if (bank) {
                    cost_[i] += kMapRoadBankCost;
                }
            }
        }
    }

    int nx() const { return nx_; }
    int ny() const { return ny_; }
    bool open(int node) const { return cost_[static_cast<size_t>(node)] > 0; }
    void markRoad(int node) {
        if (open(node)) {
            cost_[static_cast<size_t>(node)] = kMapRoadOnRoad;
        }
    }

    // The open node nearest `node` along the map's edge or anywhere (a short ring search); -1 if none.
    int nearestOpen(int node) const {
        if (open(node)) {
            return node;
        }
        const int x0 = node % nx_;
        const int y0 = node / nx_;
        for (int r = 1; r < 16; ++r) {
            for (int dy = -r; dy <= r; ++dy) {
                for (int dx = -r; dx <= r; ++dx) {
                    if (std::max(std::abs(dx), std::abs(dy)) != r) {
                        continue;
                    }
                    const int x = x0 + dx;
                    const int y = y0 + dy;
                    if (x >= 0 && y >= 0 && x < nx_ && y < ny_ && open(y * nx_ + x)) {
                        return y * nx_ + x;
                    }
                }
            }
        }
        return -1;
    }

    // Cheapest route between two nodes (8-connected), from `from` to `to`; empty if none.
    std::vector<int> route(int from, int to) const {
        const size_t n = cost_.size();
        std::vector<int64_t> best(n, kMapRoadNoPath);
        std::vector<int> prev(n, -1);
        using Item = std::pair<int64_t, int>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
        best[static_cast<size_t>(from)] = 0;
        open.push(Item{0, from});
        static constexpr int kDx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
        static constexpr int kDy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
        while (!open.empty()) {
            const Item top = open.top();
            open.pop();
            if (top.first != best[static_cast<size_t>(top.second)]) {
                continue;
            }
            if (top.second == to) {
                break;
            }
            const int x = top.second % nx_;
            const int y = top.second / nx_;
            for (int d = 0; d < 8; ++d) {
                const int ax = x + kDx[d];
                const int ay = y + kDy[d];
                if (ax < 0 || ay < 0 || ax >= nx_ || ay >= ny_) {
                    continue;
                }
                const int next = ay * nx_ + ax;
                if (!this->open(next)) {
                    continue;
                }
                const int64_t step = static_cast<int64_t>(cost_[static_cast<size_t>(top.second)] + cost_[static_cast<size_t>(next)]) *
                                     (d < 4 ? 10 : 14) / 20;
                const int64_t total = top.first + step;
                if (total < best[static_cast<size_t>(next)]) {
                    best[static_cast<size_t>(next)] = total;
                    prev[static_cast<size_t>(next)] = top.second;
                    open.push(Item{total, next});
                }
            }
        }
        std::vector<int> path;
        if (best[static_cast<size_t>(to)] == kMapRoadNoPath) {
            return path;
        }
        for (int at = to; at != -1; at = prev[static_cast<size_t>(at)]) {
            path.push_back(at);
        }
        std::reverse(path.begin(), path.end());
        return path;
    }

private:
    const Map& map_;
    int nx_;
    int ny_;
    std::vector<int> cost_;  // 0: closed
    std::vector<uint8_t> wet_;  // 1 where the block has deep or shallow water

    int blockCost(int bx, int by) {
        int rock = 0;
        int deep = 0;
        int shallow = 0;
        int wet = 0;
        float lo = 1.0e9f;
        float hi = -1.0e9f;
        for (int y = by * kMapRoadNode; y < (by + 1) * kMapRoadNode; ++y) {
            for (int x = bx * kMapRoadNode; x < (bx + 1) * kMapRoadNode; ++x) {
                const CellPos c{x, y, 0};
                switch (map_.terrain(c)) {
                    case Terrain::Rock: ++rock; break;
                    case Terrain::Water: ++deep; break;
                    case Terrain::ShallowWater: ++shallow; break;
                    case Terrain::Marsh:
                    case Terrain::Mud: ++wet; break;
                    default: break;
                }
                if (map_.terrain(c) != Terrain::Water && map_.terrain(c) != Terrain::ShallowWater) {
                    lo = std::min(lo, map_.groundHeight(c));
                    hi = std::max(hi, map_.groundHeight(c));
                }
            }
        }
        wet_[static_cast<size_t>(by * nx_ + bx)] = deep + shallow > 0 ? 1 : 0;
        if (rock >= kMapRoadRockBlocked) {
            return 0;
        }
        const int rise = hi > lo ? static_cast<int>((hi - lo) * 10.0f) : 0;
        return kMapRoadBase + rise * kMapRoadRiseCost + rock * kMapRoadRockCost + deep * kMapRoadDeepCost +
               shallow * kMapRoadShallowCost + wet * kMapRoadWetCost;
    }
};

// Where a road from direction `dir` (World::kNeighbourDx/Dy) comes onto the map: the middle stretch of that side,
// or near the corner for a diagonal. Map y grows to the south, like the world's rows.
MapRoadPoint mapRoadEntry(const Map& map, int dir, Rng& rng) {
    const float w = static_cast<float>(map.width());
    const float h = static_cast<float>(map.height());
    const int dx = World::kNeighbourDx[dir];
    const int dy = World::kNeighbourDy[dir];
    if (dx != 0 && dy != 0) {
        const float along = rng.range(0.08f, 0.2f);
        const bool onSide = rng.below(2) == 0;  // on the east/west side, else on the north/south side
        const float x = onSide ? (dx > 0 ? w - 0.5f : 0.5f) : (dx > 0 ? w * (1.0f - along) : w * along);
        const float y = onSide ? (dy > 0 ? h * (1.0f - along) : h * along) : (dy > 0 ? h - 0.5f : 0.5f);
        return MapRoadPoint{x, y};
    }
    const float along = rng.range(0.3f, 0.7f);
    if (dx != 0) {
        return MapRoadPoint{dx > 0 ? w - 0.5f : 0.5f, h * along};
    }
    return MapRoadPoint{w * along, dy > 0 ? h - 0.5f : 0.5f};
}

int mapRoadNodeAt(const MapRoadGrid& grid, MapRoadPoint p) {
    const int x = std::clamp(static_cast<int>(p.x) / kMapRoadNode, 0, grid.nx() - 1);
    const int y = std::clamp(static_cast<int>(p.y) / kMapRoadNode, 0, grid.ny() - 1);
    return y * grid.nx() + x;
}

// Chaikin's corner cutting, keeping the two ends: a smooth line through the blocks' middles.
std::vector<MapRoadPoint> mapRoadSmooth(std::vector<MapRoadPoint> line, int passes) {
    for (int p = 0; p < passes && line.size() > 2; ++p) {
        std::vector<MapRoadPoint> out;
        out.reserve(line.size() * 2);
        out.push_back(line.front());
        for (size_t i = 0; i + 1 < line.size(); ++i) {
            const MapRoadPoint a = line[i];
            const MapRoadPoint b = line[i + 1];
            out.push_back(MapRoadPoint{a.x * 0.75f + b.x * 0.25f, a.y * 0.75f + b.y * 0.25f});
            out.push_back(MapRoadPoint{a.x * 0.25f + b.x * 0.75f, a.y * 0.25f + b.y * 0.75f});
        }
        out.push_back(line.back());
        line = std::move(out);
    }
    return line;
}

// Marks every cell whose middle is within half the road's width of the line.
// `centre` gets the cells within kMapRoadCentre of the line: a crossing is bridged only where the road's middle is in the water.
void mapRoadPaint(const Map& map, const std::vector<MapRoadPoint>& line, RoadKind kind, std::vector<uint8_t>& marks,
                  std::vector<uint8_t>& centre) {
    const float half = static_cast<float>(mapRoadWidth(kind)) * 0.5f;
    const float halfSq = half * half;
    const int reach = static_cast<int>(half) + 1;
    const auto stamp = [&](float px, float py) {
        const int cx = static_cast<int>(px);
        const int cy = static_cast<int>(py);
        for (int y = cy - reach; y <= cy + reach; ++y) {
            for (int x = cx - reach; x <= cx + reach; ++x) {
                if (x < 0 || y < 0 || x >= map.width() || y >= map.height()) {
                    continue;
                }
                const float ddx = static_cast<float>(x) + 0.5f - px;
                const float ddy = static_cast<float>(y) + 0.5f - py;
                const float dSq = ddx * ddx + ddy * ddy;
                if (dSq <= halfSq) {
                    uint8_t& m = marks[static_cast<size_t>(y * map.width() + x)];
                    m = std::max(m, static_cast<uint8_t>(kind));
                }
                if (dSq <= kMapRoadCentre * kMapRoadCentre) {
                    centre[static_cast<size_t>(y * map.width() + x)] = 1;
                }
            }
        }
    };
    for (size_t i = 0; i + 1 < line.size(); ++i) {
        const MapRoadPoint a = line[i];
        const MapRoadPoint b = line[i + 1];
        const float lx = b.x - a.x;
        const float ly = b.y - a.y;
        const int steps = std::max(1, static_cast<int>(std::max(std::abs(lx), std::abs(ly)) * 2.0f));
        for (int s = 0; s <= steps; ++s) {
            const float t = static_cast<float>(s) / static_cast<float>(steps);
            stamp(a.x + lx * t, a.y + ly * t);
        }
    }
}

// The road's own route at 1 m, inside a corridor kMapRoadCorridor metres either side of the coarse route: rock is
// closed, water costs a lot (so a crossing is short and square), dry ground within reach of the road's edge from
// water costs more (so the road keeps off a bank it runs beside), cells already on a road cost little. Returns the
// cells' middles from the first coarse point to the last, thinned to every kMapRoadThin-th cell; empty if no route.
std::vector<MapRoadPoint> mapRoadRefine(const Map& map, const std::vector<MapRoadPoint>& coarse, RoadKind kind,
                                        const std::vector<uint8_t>& roads) {
    const int w = map.width();
    const int h = map.height();
    std::vector<uint8_t> corridor(static_cast<size_t>(w * h), 0);
    {
        std::vector<uint8_t> stamp(static_cast<size_t>(w * h), 0);
        // A wide "road" painted along the coarse route marks the corridor.
        const float half = static_cast<float>(kMapRoadCorridor);
        for (size_t i = 0; i + 1 < coarse.size(); ++i) {
            const MapRoadPoint a = coarse[i];
            const MapRoadPoint b = coarse[i + 1];
            const int steps = std::max(1, static_cast<int>(std::max(std::abs(b.x - a.x), std::abs(b.y - a.y))));
            for (int st = 0; st <= steps; ++st) {
                const float t = static_cast<float>(st) / static_cast<float>(steps);
                const int cx = static_cast<int>(a.x + (b.x - a.x) * t);
                const int cy = static_cast<int>(a.y + (b.y - a.y) * t);
                for (int y = std::max(0, cy - kMapRoadCorridor); y <= std::min(h - 1, cy + kMapRoadCorridor); ++y) {
                    for (int x = std::max(0, cx - kMapRoadCorridor); x <= std::min(w - 1, cx + kMapRoadCorridor); ++x) {
                        const float ddx = static_cast<float>(x - cx);
                        const float ddy = static_cast<float>(y - cy);
                        if (ddx * ddx + ddy * ddy <= half * half) {
                            corridor[static_cast<size_t>(y * w + x)] = 1;
                        }
                    }
                }
            }
        }
    }
    const int reach = mapRoadWidth(kind) / 2 + 2;  // metres from water a road's edge keeps, where it can
    const auto isWater = [&map](int x, int y) {
        const Terrain t = map.terrain(CellPos{x, y, 0});
        return t == Terrain::Water || t == Terrain::ShallowWater;
    };
    const auto cellCost = [&](int x, int y) -> int {
        const CellPos c{x, y, 0};
        const Terrain t = map.terrain(c);
        if (t == Terrain::Rock) {
            return 0;
        }
        if (roads[static_cast<size_t>(y * w + x)] != 0) {
            return kMapRoadOnRoad;
        }
        if (t == Terrain::Water) {
            return kMapRoadFineDeep;
        }
        if (t == Terrain::ShallowWater) {
            return kMapRoadFineShallow;
        }
        int cost = kMapRoadBase;
        if (t == Terrain::Marsh || t == Terrain::Mud) {
            cost += kMapRoadFineWet;
        }
        const float here = map.groundHeight(c);
        float rise = 0.0f;
        if (x + 1 < w) {
            rise = std::max(rise, std::abs(map.groundHeight(CellPos{x + 1, y, 0}) - here));
        }
        if (y + 1 < h) {
            rise = std::max(rise, std::abs(map.groundHeight(CellPos{x, y + 1, 0}) - here));
        }
        cost += std::min(static_cast<int>(rise * 10.0f) * kMapRoadFineRise, kMapRoadFineRiseMax);  // a steep bank still beats wading up the river
        bool rockNear = false;
        bool waterNear = false;
        for (int dy = -reach; dy <= reach && !(rockNear && waterNear); ++dy) {
            for (int dx = -reach; dx <= reach; ++dx) {
                const int ax = x + dx;
                const int ay = y + dy;
                if (ax < 0 || ay < 0 || ax >= w || ay >= h) {
                    continue;
                }
                waterNear = waterNear || isWater(ax, ay);
                rockNear = rockNear || (std::abs(dx) <= 1 && std::abs(dy) <= 1 && map.terrain(CellPos{ax, ay, 0}) == Terrain::Rock);
            }
        }
        return cost + (waterNear ? kMapRoadFineBank : 0) + (rockNear ? kMapRoadFineRockSide : 0);
    };
    const auto cellOfPoint = [&](MapRoadPoint p) {
        return std::clamp(static_cast<int>(p.y), 0, h - 1) * w + std::clamp(static_cast<int>(p.x), 0, w - 1);
    };
    int from = cellOfPoint(coarse.front());
    int to = cellOfPoint(coarse.back());
    std::vector<int32_t> best(static_cast<size_t>(w * h), std::numeric_limits<int32_t>::max());
    std::vector<int32_t> prev(static_cast<size_t>(w * h), -1);
    std::vector<int32_t> costOf(static_cast<size_t>(w * h), -1);  // -1: not worked out yet
    const auto costAt = [&](int i) {
        int32_t& c = costOf[static_cast<size_t>(i)];
        if (c < 0) {
            c = corridor[static_cast<size_t>(i)] != 0 ? cellCost(i % w, i / w) : 0;
        }
        return c;
    };
    // An end on rock moves to the nearest open cell close by.
    const auto openNear = [&](int cell) {
        for (int r = 0; r <= kMapRoadNode; ++r) {
            for (int dy = -r; dy <= r; ++dy) {
                for (int dx = -r; dx <= r; ++dx) {
                    const int x = cell % w + dx;
                    const int y = cell / w + dy;
                    if (std::max(std::abs(dx), std::abs(dy)) == r && x >= 0 && y >= 0 && x < w && y < h && costAt(y * w + x) != 0) {
                        return y * w + x;
                    }
                }
            }
        }
        return -1;
    };
    from = openNear(from);
    to = openNear(to);
    if (from < 0 || to < 0) {
        return {};
    }
    using Item = std::pair<int32_t, int32_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
    best[static_cast<size_t>(from)] = 0;
    open.push(Item{0, from});
    static constexpr int kFineDx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    static constexpr int kFineDy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    while (!open.empty()) {
        const Item top = open.top();
        open.pop();
        if (top.first != best[static_cast<size_t>(top.second)]) {
            continue;
        }
        if (top.second == to) {
            break;
        }
        const int x = top.second % w;
        const int y = top.second / w;
        for (int d = 0; d < 8; ++d) {
            const int ax = x + kFineDx[d];
            const int ay = y + kFineDy[d];
            if (ax < 0 || ay < 0 || ax >= w || ay >= h) {
                continue;
            }
            const int next = ay * w + ax;
            const int32_t c = costAt(next);
            if (c == 0) {
                continue;
            }
            const int32_t total = top.first + (costAt(top.second) + c) * (d < 4 ? 10 : 14) / 20;
            if (total < best[static_cast<size_t>(next)]) {
                best[static_cast<size_t>(next)] = total;
                prev[static_cast<size_t>(next)] = top.second;
                open.push(Item{total, next});
            }
        }
    }
    if (best[static_cast<size_t>(to)] == std::numeric_limits<int32_t>::max()) {
        return {};
    }
    std::vector<int> cells;
    for (int at = to; at != -1; at = prev[static_cast<size_t>(at)]) {
        cells.push_back(at);
    }
    std::reverse(cells.begin(), cells.end());
    std::vector<MapRoadPoint> line;
    line.push_back(coarse.front());
    for (size_t i = kMapRoadThin; i + 1 < cells.size(); i += kMapRoadThin) {
        line.push_back(MapRoadPoint{static_cast<float>(cells[i] % w) + 0.5f, static_cast<float>(cells[i] / w) + 0.5f});
    }
    line.push_back(MapRoadPoint{static_cast<float>(to % w) + 0.5f, static_cast<float>(to / w) + 0.5f});
    return line;
}

}  // namespace

MapRoadCounts placeMapRoads(Map& map, Things& things, Plants& plants, Floors& floors, const std::array<RoadKind, 8>& roadEdges,
                            uint64_t seed) {
    MapRoadCounts counts;
    const DefDatabase& defs = things.defs();
    const DefId bridgeDef = defs.idOf("Things", "AncientBridge");
    if (std::min(map.width(), map.height()) < kMapRoadMinMap || !bridgeDef.valid()) {
        return counts;
    }
    Rng rng(seed ^ 0x726F616473ULL);
    MapRoadGrid grid(map);
    // Where the roads meet: near the middle (the middle itself is where the colony lands).
    const float w = static_cast<float>(map.width());
    const float h = static_cast<float>(map.height());
    const MapRoadPoint meet{w * rng.range(0.4f, 0.6f), h * rng.range(0.4f, 0.6f)};
    const int meetNode = grid.nearestOpen(mapRoadNodeAt(grid, meet));
    std::vector<uint8_t> marks(static_cast<size_t>(map.width() * map.height()), 0);
    std::vector<uint8_t> centre(static_cast<size_t>(map.width() * map.height()), 0);
    // The best road first, so lesser roads join it.
    std::vector<int> dirs;
    for (int d = 0; d < 8; ++d) {
        if (roadEdges[static_cast<size_t>(d)] != RoadKind::None) {
            dirs.push_back(d);
        }
    }
    std::stable_sort(dirs.begin(), dirs.end(),
                     [&](int a, int b) { return roadEdges[static_cast<size_t>(a)] > roadEdges[static_cast<size_t>(b)]; });
    for (const int dir : dirs) {
        const RoadKind kind = roadEdges[static_cast<size_t>(dir)];
        MapRoadPoint entry = mapRoadEntry(map, dir, rng);
        int from = mapRoadNodeAt(grid, entry);
        if (!grid.open(from)) {
            // Rock where it would come in: slide along the map's edge to the nearest open block, and come in there.
            const int ex = from % grid.nx();
            const int ey = from / grid.nx();
            const bool alongX = ey == 0 || ey == grid.ny() - 1;
            int found = -1;
            for (int r = 1; r < std::max(grid.nx(), grid.ny()) && found < 0; ++r) {
                for (const int sign : {-1, 1}) {
                    const int x = alongX ? ex + sign * r : ex;
                    const int y = alongX ? ey : ey + sign * r;
                    if (found < 0 && x >= 0 && y >= 0 && x < grid.nx() && y < grid.ny() && grid.open(y * grid.nx() + x)) {
                        found = y * grid.nx() + x;
                    }
                }
            }
            if (found >= 0) {
                const float nx = static_cast<float>((found % grid.nx()) * kMapRoadNode) + kMapRoadNode * 0.5f;
                const float ny = static_cast<float>((found / grid.nx()) * kMapRoadNode) + kMapRoadNode * 0.5f;
                entry = alongX ? MapRoadPoint{nx, entry.y} : MapRoadPoint{entry.x, ny};
                from = found;
            } else {
                from = grid.nearestOpen(from);
            }
        }
        if (from < 0 || meetNode < 0) {
            continue;
        }
        const std::vector<int> nodes = grid.route(from, meetNode);
        if (nodes.empty()) {
            continue;
        }
        std::vector<MapRoadPoint> coarse;
        coarse.push_back(entry);
        for (const int node : nodes) {
            coarse.push_back(MapRoadPoint{static_cast<float>((node % grid.nx()) * kMapRoadNode) + kMapRoadNode * 0.5f,
                                          static_cast<float>((node / grid.nx()) * kMapRoadNode) + kMapRoadNode * 0.5f});
            grid.markRoad(node);
        }
        // The fine route, metre by metre, in a corridor round the coarse one: it keeps off banks and crosses water square on.
        std::vector<MapRoadPoint> line = mapRoadRefine(map, coarse, kind, marks);
        if (line.size() < 2) {
            line = std::move(coarse);
        }
        mapRoadPaint(map, mapRoadSmooth(std::move(line), 3), kind, marks, centre);
        ++counts.roads;
    }

    DefId floorDefs[4];
    for (int k = 1; k < 4; ++k) {
        floorDefs[k] = defs.idOf("Terrain", mapRoadFloor(static_cast<RoadKind>(k)));
    }
    // Water under the road is bridged within the road's half width of water under the road's middle: a crossing. A
    // strip of the road's edge over water it runs beside (its middle on dry land) is not, and is left as water.
    const auto roadWater = [&](int i) {
        const Terrain t = map.terrain(map.cellAt(i));
        return marks[static_cast<size_t>(i)] != 0 && (t == Terrain::Water || t == Terrain::ShallowWater);
    };
    std::vector<uint8_t> crossing(marks.size(), 0);
    for (int i = 0; i < map.width() * map.height(); ++i) {
        if (centre[static_cast<size_t>(i)] == 0 || !roadWater(i)) {
            continue;
        }
        const int kind = marks[static_cast<size_t>(i)];
        const float half = static_cast<float>(mapRoadWidth(static_cast<RoadKind>(kind))) * 0.5f + 0.5f;
        const int r = static_cast<int>(half) + 1;
        const int cx = i % map.width();
        const int cy = i / map.width();
        for (int y = std::max(0, cy - r); y <= std::min(map.height() - 1, cy + r); ++y) {
            for (int x = std::max(0, cx - r); x <= std::min(map.width() - 1, cx + r); ++x) {
                const float ddx = static_cast<float>(x - cx);
                const float ddy = static_cast<float>(y - cy);
                if (ddx * ddx + ddy * ddy <= half * half && roadWater(y * map.width() + x)) {
                    crossing[static_cast<size_t>(y * map.width() + x)] = 1;
                }
            }
        }
    }
    for (int i = 0; i < map.width() * map.height(); ++i) {
        const uint8_t m = marks[static_cast<size_t>(i)];
        if (m == 0) {
            continue;
        }
        const CellPos c = map.cellAt(i);
        if (map.buildingAt(c) != kNoEntity || map.terrain(c) == Terrain::Rock) {
            continue;
        }
        const Terrain t = map.terrain(c);
        if (t == Terrain::Water || t == Terrain::ShallowWater) {
            if (crossing[static_cast<size_t>(i)] != 0 && things.spawn(bridgeDef, c) != kNoEntity) {
                ++counts.bridgeCells;
                if (plants.has(c)) {
                    plants.remove(c);
                }
            }
            continue;
        }
        if (floorDefs[m].valid() && floors.set(c, floorDefs[m])) {
            ++counts.roadCells;
            if (plants.has(c)) {
                plants.remove(c);
            }
        }
    }
    return counts;
}

}  // namespace colony
