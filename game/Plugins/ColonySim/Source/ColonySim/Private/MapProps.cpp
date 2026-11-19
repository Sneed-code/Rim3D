#include "ColonySim/MapProps.h"

#include "ColonySim/Floors.h"
#include "ColonySim/Loot.h"
#include "ColonySim/Map.h"
#include "ColonySim/Mineables.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Random.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace colony {

namespace {

constexpr int kMapPropMinMap = 256;
constexpr int kMapPropTries = 60;
constexpr float kMapPropPi = 3.14159265f;

// The MapPropRules def's numbers (these values when it or a field is missing).
struct MapPropRules {
    int edge = 12;
    float centreClear = 20.0f;
    float bigCentreClear = 45.0f;
    float bigApart = 25.0f;
    float maxRise = 1.5f;
    int chunkClusterCells = 16000;
    int chunkClusterMin = 2;
    int chunkClusterMax = 6;
    int chunkClusterRadius = 4;
    float chunkNearRockShare = 0.6f;
    int boulderGroupCells = 14000;
    int boulderGroupMin = 1;
    int boulderGroupMax = 3;
    float boulderNearRockShare = 0.6f;
    int fallenLogCells = 9000;
    int fallenLogTreeRadius = 5;
    int wreckCells = 350000;
    int wreckSlagMin = 2;
    int wreckSlagMax = 5;
    float wreckSecondChunkChance = 0.35f;
    int dangerRoomCells = 600000;
    int dangerWidthMin = 9;
    int dangerWidthMax = 15;
    int dangerDepthMin = 7;
    int dangerDepthMax = 11;
    int dangerCasketsMin = 2;
    int dangerCasketsMax = 4;
    int roadWidthDirt = 3;
    int roadWidthStone = 4;
    int roadWidthAsphalt = 5;
};

MapPropRules mapPropReadRules(const DefDatabase& defs) {
    MapPropRules r;
    const Def* d = defs.find("Rules", "MapPropRules");
    if (d == nullptr) {
        return r;
    }
    const auto num = [d](const char* path, float fallback) { return static_cast<float>(d->number(path, fallback)); };
    const auto whole = [d](const char* path, int fallback) { return static_cast<int>(d->integer(path, fallback)); };
    const auto at = [d](const char* path, size_t i, int fallback) { return static_cast<int>(d->numberAt(path, i, fallback)); };
    r.edge = whole("edge", r.edge);
    r.centreClear = num("centreClear", r.centreClear);
    r.bigCentreClear = num("bigCentreClear", r.bigCentreClear);
    r.bigApart = num("bigApart", r.bigApart);
    r.maxRise = num("maxRise", r.maxRise);
    r.chunkClusterCells = std::max(1000, whole("chunkClusterCells", r.chunkClusterCells));
    r.chunkClusterMin = at("chunkClusterSize", 0, r.chunkClusterMin);
    r.chunkClusterMax = at("chunkClusterSize", 1, r.chunkClusterMax);
    r.chunkClusterRadius = whole("chunkClusterRadius", r.chunkClusterRadius);
    r.chunkNearRockShare = num("chunkNearRockShare", r.chunkNearRockShare);
    r.boulderGroupCells = std::max(1000, whole("boulderGroupCells", r.boulderGroupCells));
    r.boulderGroupMin = at("boulderGroupSize", 0, r.boulderGroupMin);
    r.boulderGroupMax = at("boulderGroupSize", 1, r.boulderGroupMax);
    r.boulderNearRockShare = num("boulderNearRockShare", r.boulderNearRockShare);
    r.fallenLogCells = std::max(1000, whole("fallenLogCells", r.fallenLogCells));
    r.fallenLogTreeRadius = whole("fallenLogTreeRadius", r.fallenLogTreeRadius);
    r.wreckCells = std::max(1000, whole("wreckCells", r.wreckCells));
    r.wreckSlagMin = at("wreckSlag", 0, r.wreckSlagMin);
    r.wreckSlagMax = at("wreckSlag", 1, r.wreckSlagMax);
    r.wreckSecondChunkChance = num("wreckSecondChunkChance", r.wreckSecondChunkChance);
    r.dangerRoomCells = std::max(1000, whole("dangerRoomCells", r.dangerRoomCells));
    r.dangerWidthMin = at("dangerRoomWidth", 0, r.dangerWidthMin);
    r.dangerWidthMax = at("dangerRoomWidth", 1, r.dangerWidthMax);
    r.dangerDepthMin = at("dangerRoomDepth", 0, r.dangerDepthMin);
    r.dangerDepthMax = at("dangerRoomDepth", 1, r.dangerDepthMax);
    r.dangerCasketsMin = at("dangerCaskets", 0, r.dangerCasketsMin);
    r.dangerCasketsMax = at("dangerCaskets", 1, r.dangerCasketsMax);
    r.roadWidthDirt = whole("roadWidth.Dirt", r.roadWidthDirt);
    r.roadWidthStone = whole("roadWidth.Stone", r.roadWidthStone);
    r.roadWidthAsphalt = whole("roadWidth.Asphalt", r.roadWidthAsphalt);
    return r;
}

// lo..hi inclusive.
int mapPropBetween(Rng& rng, int lo, int hi) {
    if (hi <= lo) {
        return lo;
    }
    return lo + static_cast<int>(rng.below(static_cast<uint32_t>(hi - lo + 1)));
}

class MapPropPlacer {
public:
    MapPropPlacer(const MapPropInput& in, const MapPropRules& rules)
        : map_(*in.map), things_(*in.things), defs_(in.things->defs()), in_(in), rules_(rules), rng_(in.seed ^ 0x70726F7073ULL) {}

    MapPropCounts run() {
        road();
        dangerRooms();
        wrecks();
        boulders();
        fallenLogs();
        chunks();
        return counts_;
    }

private:
    Map& map_;
    Things& things_;
    const DefDatabase& defs_;
    const MapPropInput& in_;
    MapPropRules rules_;
    Rng rng_;
    MapPropCounts counts_;
    std::vector<std::pair<float, float>> big_;  // middles of the danger rooms and wrecks

    int area() const { return map_.width() * map_.height(); }

    // How many of a thing the map gets: one for every `cells` cells, rounded.
    int countFor(int cells) const { return (area() + cells / 2) / cells; }

    float distToMiddle(float x, float y) const {
        return std::hypot(x - static_cast<float>(map_.width()) * 0.5f, y - static_cast<float>(map_.height()) * 0.5f);
    }

    // A random cell at least `edge` cells from the map's edge, with room for a w x h block.
    CellPos randomCell(int w = 1, int h = 1) {
        const int spanX = std::max(1, map_.width() - w - 2 * rules_.edge);
        const int spanY = std::max(1, map_.height() - h - 2 * rules_.edge);
        return CellPos{rules_.edge + static_cast<int>(rng_.below(static_cast<uint32_t>(spanX))),
                       rules_.edge + static_cast<int>(rng_.below(static_cast<uint32_t>(spanY))), 0};
    }

    bool hasFloor(CellPos c) const { return in_.floors != nullptr && in_.floors->at(c).valid(); }

    // Open natural ground: soil or sand, no building, no item, no floor (roads), no roof.
    bool open(CellPos c) const {
        if (!map_.onGround(c) || map_.buildingAt(c) != kNoEntity || map_.roof(c) != Roof::None || hasFloor(c)) {
            return false;
        }
        const Terrain t = map_.terrain(c);
        if (t != Terrain::Soil && t != Terrain::Sand) {
            return false;
        }
        return things_.itemsAt(c).empty();
    }

    // Every cell of the block is open and its height varies by no more than `rise`.
    bool blockOpen(int x0, int y0, int w, int h, float rise) const {
        float lo = 1.0e9f;
        float hi = -1.0e9f;
        for (int y = y0; y < y0 + h; ++y) {
            for (int x = x0; x < x0 + w; ++x) {
                const CellPos c{x, y, 0};
                if (!open(c)) {
                    return false;
                }
                lo = std::min(lo, map_.groundHeight(c));
                hi = std::max(hi, map_.groundHeight(c));
            }
        }
        return hi - lo <= rise;
    }

    // No building at all within `margin` cells of the block (keeps big things off the ruins and geysers).
    bool noBuildingsNear(int x0, int y0, int w, int h, int margin) const {
        for (int y = y0 - margin; y < y0 + h + margin; ++y) {
            for (int x = x0 - margin; x < x0 + w + margin; ++x) {
                const CellPos c{x, y, 0};
                if (map_.inBounds(c) && map_.buildingAt(c) != kNoEntity) {
                    return false;
                }
            }
        }
        return true;
    }

    bool farFromBig(float x, float y) const {
        for (const auto& b : big_) {
            if (std::hypot(x - b.first, y - b.second) < rules_.bigApart) {
                return false;
            }
        }
        return true;
    }

    void clearPlants(int x0, int y0, int w, int h) {
        if (in_.plants == nullptr) {
            return;
        }
        for (int y = y0; y < y0 + h; ++y) {
            for (int x = x0; x < x0 + w; ++x) {
                const CellPos c{x, y, 0};
                if (map_.inBounds(c) && in_.plants->has(c)) {
                    in_.plants->remove(c);
                }
            }
        }
    }

    // A building put down where it was checked to fit; its cells lose their plants.
    bool putBuilding(DefId def, CellPos at, Rot4 rot = Rot4::North) {
        if (!def.valid()) {
            return false;
        }
        const Entity e = things_.spawn(def, at, 1, rot);
        if (e == kNoEntity) {
            return false;
        }
        const Def& d = defs_.get(def);
        int sx = static_cast<int>(d.numberAt("size", 0, 1.0));
        int sy = static_cast<int>(d.numberAt("size", 1, 1.0));
        const CellRect r = footprintRect(at, sx, sy, rot);
        clearPlants(r.minX, r.minY, r.sizeX, r.sizeY);
        return true;
    }

    // The mineable rock def nearest to `c` within `radius` cells (looked at on a sparse ring), or invalid.
    DefId rockNear(CellPos c, int radius) const {
        if (in_.mineables == nullptr) {
            return DefId{};
        }
        for (int r = 1; r <= radius; r += 2) {
            for (int k = 0; k < 8; ++k) {
                const float a = static_cast<float>(k) * kMapPropPi * 0.25f;
                const CellPos p{c.x + static_cast<int>(std::lround(std::cos(a) * static_cast<float>(r))),
                                c.y + static_cast<int>(std::lround(std::sin(a) * static_cast<float>(r))), 0};
                if (!map_.inBounds(p) || map_.terrain(p) != Terrain::Rock) {
                    continue;
                }
                const DefId d = in_.mineables->defAt(p);
                if (d.valid() && !in_.mineables->isOre(d)) {
                    return d;
                }
            }
        }
        return DefId{};
    }

    // Which rock a loose boulder or chunk at `c` is: the rock next to it, else one of the map's rock types.
    DefId rockFor(CellPos c, DefId near) {
        if (near.valid()) {
            return near;
        }
        if (in_.mineables != nullptr && !in_.mineables->rockTypes().empty()) {
            const auto& types = in_.mineables->rockTypes();
            return types[rng_.below(static_cast<uint32_t>(types.size()))];
        }
        (void)c;
        return defs_.idOf("Things", "Rock_Granite");
    }

    // "Rock_Granite" -> "<prefix>Granite".
    DefId fromRock(DefId rock, const char* prefix) const {
        if (!rock.valid()) {
            return DefId{};
        }
        const std::string& name = defs_.get(rock).defName();
        const size_t cut = name.find('_');
        return defs_.idOf("Things", std::string(prefix) + (cut == std::string::npos ? name : name.substr(cut + 1)));
    }

    // A spot for a small prop: open, away from the middle; the first `nearRockTries` tries want rock or hills close by.
    bool smallSpot(int nearRockTries, CellPos& out, DefId& rock) {
        for (int t = 0; t < kMapPropTries; ++t) {
            const CellPos c = randomCell();
            if (!open(c) || distToMiddle(static_cast<float>(c.x), static_cast<float>(c.y)) < rules_.centreClear) {
                continue;
            }
            rock = rockNear(c, 12);
            // The first tries want rock close by or hill ground (Map::hillGround: over 18 m on generated maps).
            if (t < nearRockTries && !rock.valid() && !map_.hillGround(c)) {
                continue;
            }
            out = c;
            return true;
        }
        return false;
    }

    // ---- Roads --------------------------------------------------------------------------------
    void road() {
        if (in_.road == RoadKind::None || in_.floors == nullptr) {
            return;
        }
        DefId floor;
        const char* kind = roadKindName(in_.road);
        for (const DefId id : defs_.idsOfKind("Terrain")) {
            if (defs_.get(id).text("mapRoad") == kind) {
                floor = id;
            }
        }
        if (in_.road == RoadKind::Stone && !floor.valid()) {
            floor = defs_.idOf("Terrain", "Flagstone");
        }
        if (!floor.valid()) {
            return;
        }
        const int width = in_.road == RoadKind::Dirt ? rules_.roadWidthDirt
                          : in_.road == RoadKind::Stone ? rules_.roadWidthStone : rules_.roadWidthAsphalt;
        const bool alongX = rng_.below(2) == 0;
        const int length = alongX ? map_.width() : map_.height();
        const int across = alongX ? map_.height() : map_.width();
        // The middle line wanders gently: two slow waves, from 25% to 75% of the way across.
        const float base = static_cast<float>(across) * rng_.range(0.3f, 0.7f);
        const float amp1 = static_cast<float>(across) * rng_.range(0.04f, 0.1f);
        const float amp2 = static_cast<float>(across) * rng_.range(0.01f, 0.03f);
        const float wave1 = rng_.range(1.5f, 3.0f) * 2.0f * kMapPropPi / static_cast<float>(length);
        const float wave2 = rng_.range(5.0f, 9.0f) * 2.0f * kMapPropPi / static_cast<float>(length);
        const float phase1 = rng_.range(0.0f, 2.0f * kMapPropPi);
        const float phase2 = rng_.range(0.0f, 2.0f * kMapPropPi);
        for (int i = 0; i < length; ++i) {
            const float s = static_cast<float>(i);
            const float mid = base + amp1 * std::sin(s * wave1 + phase1) + amp2 * std::sin(s * wave2 + phase2);
            const int lo = static_cast<int>(std::floor(mid - static_cast<float>(width) * 0.5f));
            for (int j = lo; j < lo + width; ++j) {
                const CellPos c = alongX ? CellPos{i, j, 0} : CellPos{j, i, 0};
                if (!map_.inBounds(c) || map_.buildingAt(c) != kNoEntity) {
                    continue;
                }
                const Terrain t = map_.terrain(c);
                if (t != Terrain::Soil && t != Terrain::Sand && t != Terrain::Marsh && t != Terrain::Mud) {
                    continue;  // a road stops at water and rock
                }
                if (in_.floors->set(c, floor)) {
                    ++counts_.roadCells;
                    if (in_.plants != nullptr && in_.plants->has(c)) {
                        in_.plants->remove(c);
                    }
                }
            }
        }
    }

    // ---- Ancient danger -----------------------------------------------------------------------
    void dangerRooms() {
        const DefId wall = defs_.idOf("Things", "AncientWall");
        const DefId casket = defs_.idOf("Things", "AncientCryptosleepCasket");
        if (!wall.valid()) {
            return;
        }
        const int count = std::max(1, countFor(rules_.dangerRoomCells));
        for (int n = 0; n < count; ++n) {
            for (int t = 0; t < kMapPropTries * 2; ++t) {
                int w = mapPropBetween(rng_, rules_.dangerWidthMin, rules_.dangerWidthMax);
                int h = mapPropBetween(rng_, rules_.dangerDepthMin, rules_.dangerDepthMax);
                if (rng_.below(2) == 0) {
                    std::swap(w, h);
                }
                const CellPos o = randomCell(w, h);
                const float cx = static_cast<float>(o.x) + static_cast<float>(w) * 0.5f;
                const float cy = static_cast<float>(o.y) + static_cast<float>(h) * 0.5f;
                if (distToMiddle(cx, cy) < rules_.bigCentreClear || !farFromBig(cx, cy) ||
                    !blockOpen(o.x - 1, o.y - 1, w + 2, h + 2, rules_.maxRise) || !noBuildingsNear(o.x, o.y, w, h, 6)) {
                    continue;
                }
                buildDangerRoom(o, w, h, wall, casket);
                big_.emplace_back(cx, cy);
                break;
            }
        }
    }

    void buildDangerRoom(CellPos o, int w, int h, DefId wall, DefId casket) {
        clearPlants(o.x, o.y, w, h);
        for (int dy = 0; dy < h; ++dy) {
            for (int dx = 0; dx < w; ++dx) {
                if (dx == 0 || dy == 0 || dx == w - 1 || dy == h - 1) {
                    if (things_.spawn(wall, CellPos{o.x + dx, o.y + dy, 0}) != kNoEntity) {
                        ++counts_.dangerWalls;
                    }
                }
            }
        }
        ++counts_.dangerRooms;
        // Caskets in a row along the long side, one cell in from the wall, a cell apart.
        const bool wide = w >= h;
        const int inner = (wide ? w : h) - 2;
        const int caskets = std::min(mapPropBetween(rng_, rules_.dangerCasketsMin, rules_.dangerCasketsMax), std::max(0, (inner + 1) / 2));
        if (casket.valid()) {
            const int start = 1 + std::max(0, (inner - (caskets * 2 - 1)) / 2);
            for (int k = 0; k < caskets; ++k) {
                // A 1x2 casket stands across the long side: north-facing in a wide room, east-facing in a deep one.
                const CellPos at = wide ? CellPos{o.x + start + 2 * k, o.y + 1, 0} : CellPos{o.x + 1, o.y + start + 2 * k, 0};
                if (things_.spawn(casket, at, 1, wide ? Rot4::North : Rot4::East) != kNoEntity) {
                    ++counts_.caskets;
                }
            }
        }
        // The loot lies on the floor of the far half.
        if (in_.loot == nullptr || !in_.loot->hasTable("AncientDanger")) {
            return;
        }
        const std::vector<LootItem> items = in_.loot->roll("AncientDanger", 0.0f, rng_);
        std::vector<CellPos> spots;
        for (int dy = 1; dy < h - 1; ++dy) {
            for (int dx = 1; dx < w - 1; ++dx) {
                const CellPos c{o.x + dx, o.y + dy, 0};
                if ((wide ? dy : dx) >= 3 && map_.buildingAt(c) == kNoEntity) {
                    spots.push_back(c);
                }
            }
        }
        size_t next = 0;
        for (const LootItem& item : items) {
            const int limit = std::max<int>(1, static_cast<int>(defs_.get(item.def).integer("stackLimit", 1)));
            int left = item.count;
            while (left > 0 && next < spots.size()) {
                const int n = std::min(left, limit);
                if (things_.spawn(item.def, spots[next], n) != kNoEntity) {
                    ++counts_.lootStacks;
                    left -= n;
                }
                ++next;
            }
        }
    }

    // ---- Wreckage -----------------------------------------------------------------------------
    void wrecks() {
        const DefId chunk = defs_.idOf("Things", "ShipChunk");
        const DefId slag = defs_.idOf("Things", "ChunkSlagSteel");
        if (!chunk.valid()) {
            return;
        }
        const int count = std::max(1, countFor(rules_.wreckCells));
        for (int n = 0; n < count; ++n) {
            for (int t = 0; t < kMapPropTries; ++t) {
                const CellPos o = randomCell(2, 2);
                const float cx = static_cast<float>(o.x) + 1.0f;
                const float cy = static_cast<float>(o.y) + 1.0f;
                if (distToMiddle(cx, cy) < rules_.bigCentreClear || !farFromBig(cx, cy) || !blockOpen(o.x, o.y, 2, 2, 1.0f) ||
                    !noBuildingsNear(o.x, o.y, 2, 2, 3) || !putBuilding(chunk, o)) {
                    continue;
                }
                ++counts_.wrecks;
                big_.emplace_back(cx, cy);
                if (rng_.unit() < rules_.wreckSecondChunkChance) {
                    for (int k = 0; k < 12; ++k) {
                        const CellPos p{o.x + mapPropBetween(rng_, -6, 6), o.y + mapPropBetween(rng_, -6, 6), 0};
                        if (std::abs(p.x - o.x) >= 3 || std::abs(p.y - o.y) >= 3) {
                            if (blockOpen(p.x, p.y, 2, 2, 1.0f) && putBuilding(chunk, p)) {
                                ++counts_.wrecks;
                                break;
                            }
                        }
                    }
                }
                if (slag.valid()) {
                    const int pieces = mapPropBetween(rng_, rules_.wreckSlagMin, rules_.wreckSlagMax);
                    for (int k = 0, put = 0; k < pieces * 6 && put < pieces; ++k) {
                        const CellPos p{o.x + mapPropBetween(rng_, -4, 5), o.y + mapPropBetween(rng_, -4, 5), 0};
                        if (open(p) && things_.spawn(slag, p) != kNoEntity) {
                            ++counts_.slag;
                            ++put;
                        }
                    }
                }
                break;
            }
        }
    }

    // ---- Boulders, logs, chunks ---------------------------------------------------------------
    void boulders() {
        const int groups = countFor(rules_.boulderGroupCells);
        const int nearTries = static_cast<int>(static_cast<float>(kMapPropTries) * rules_.boulderNearRockShare);
        for (int g = 0; g < groups; ++g) {
            CellPos c;
            DefId near;
            if (!smallSpot(rng_.unit() < rules_.boulderNearRockShare ? nearTries : 0, c, near)) {
                continue;
            }
            const DefId boulder = fromRock(rockFor(c, near), "Boulder");
            const int size = mapPropBetween(rng_, rules_.boulderGroupMin, rules_.boulderGroupMax);
            for (int k = 0, put = 0; k < size * 4 && put < size; ++k) {
                const CellPos p = k == 0 ? c : CellPos{c.x + mapPropBetween(rng_, -2, 2), c.y + mapPropBetween(rng_, -2, 2), 0};
                if (open(p) && putBuilding(boulder, p)) {
                    ++counts_.boulders;
                    ++put;
                }
            }
        }
    }

    bool treeNear(CellPos c, int radius) const {
        if (in_.plants == nullptr) {
            return false;
        }
        for (int y = c.y - radius; y <= c.y + radius; ++y) {
            for (int x = c.x - radius; x <= c.x + radius; ++x) {
                const CellPos p{x, y, 0};
                if (map_.inBounds(p) && in_.plants->has(p) && defs_.get(in_.plants->defAt(p)).flag("plant.isTree")) {
                    return true;
                }
            }
        }
        return false;
    }

    void fallenLogs() {
        const DefId log = defs_.idOf("Things", "FallenLog");
        if (!log.valid() || in_.plants == nullptr) {
            return;
        }
        const int count = countFor(rules_.fallenLogCells);
        for (int n = 0; n < count; ++n) {
            for (int t = 0; t < kMapPropTries / 3; ++t) {
                const bool alongX = rng_.below(2) == 0;
                const CellPos o = randomCell(3, 3);
                const int w = alongX ? 3 : 1;
                const int h = alongX ? 1 : 3;
                if (distToMiddle(static_cast<float>(o.x), static_cast<float>(o.y)) < rules_.centreClear ||
                    !blockOpen(o.x, o.y, w, h, 0.8f) || map_.terrain(o) != Terrain::Soil ||
                    !treeNear(o, rules_.fallenLogTreeRadius)) {
                    continue;
                }
                // A 3x1 def facing East lies along y (Rot4 swaps width and depth).
                if (putBuilding(log, o, alongX ? Rot4::North : Rot4::East)) {
                    ++counts_.fallenLogs;
                    break;
                }
            }
        }
    }

    void chunks() {
        const int clusters = countFor(rules_.chunkClusterCells);
        const int nearTries = static_cast<int>(static_cast<float>(kMapPropTries) * rules_.chunkNearRockShare);
        for (int g = 0; g < clusters; ++g) {
            CellPos c;
            DefId near;
            if (!smallSpot(rng_.unit() < rules_.chunkNearRockShare ? nearTries : 0, c, near)) {
                continue;
            }
            const DefId chunk = fromRock(rockFor(c, near), "Chunk");
            if (!chunk.valid()) {
                continue;
            }
            const int size = mapPropBetween(rng_, rules_.chunkClusterMin, rules_.chunkClusterMax);
            const int r = rules_.chunkClusterRadius;
            for (int k = 0, put = 0; k < size * 4 && put < size; ++k) {
                const CellPos p = k == 0 ? c : CellPos{c.x + mapPropBetween(rng_, -r, r), c.y + mapPropBetween(rng_, -r, r), 0};
                if (open(p) && things_.spawn(chunk, p) != kNoEntity) {
                    ++counts_.chunks;
                    ++put;
                }
            }
        }
    }
};

}  // namespace

MapPropCounts placeMapProps(const MapPropInput& in) {
    if (in.map == nullptr || in.things == nullptr || std::min(in.map->width(), in.map->height()) < kMapPropMinMap) {
        return MapPropCounts{};
    }
    MapPropPlacer placer(in, mapPropReadRules(in.things->defs()));
    return placer.run();
}

}  // namespace colony
