#include "ColonySim/Mineables.h"

#include "ColonySim/Map.h"
#include "ColonySim/Random.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.
constexpr float kMineNoiseCell = 96.0f;  // metres between rock-type patches

float mineLattice(uint64_t seed, int64_t x, int64_t y) {
    uint64_t h = splitMix64(seed);
    h = splitMix64(h ^ static_cast<uint64_t>(x));
    h = splitMix64(h ^ static_cast<uint64_t>(y));
    return static_cast<float>(h >> 40) * (1.0f / 16777216.0f);
}

// Value noise in [0, 1) with lattice points every `cellSize` metres.
float mineNoise(uint64_t seed, int x, int y, float cellSize) {
    const float gx = static_cast<float>(x) / cellSize;
    const float gy = static_cast<float>(y) / cellSize;
    const float x0f = std::floor(gx);
    const float y0f = std::floor(gy);
    const int64_t x0 = static_cast<int64_t>(x0f);
    const int64_t y0 = static_cast<int64_t>(y0f);
    const float fx = gx - x0f;
    const float fy = gy - y0f;
    const float tx = fx * fx * (3.0f - 2.0f * fx);
    const float ty = fy * fy * (3.0f - 2.0f * fy);
    const float a = mineLattice(seed, x0, y0);
    const float b = mineLattice(seed, x0 + 1, y0);
    const float c = mineLattice(seed, x0, y0 + 1);
    const float d = mineLattice(seed, x0 + 1, y0 + 1);
    const float top = a + (b - a) * tx;
    const float bottom = c + (d - c) * tx;
    return top + (bottom - top) * ty;
}

}  // namespace

Mineables::Mineables(Map& map, const DefDatabase& defs, uint64_t seed)
    : map_(map),
      seed_(seed),
      kind_(static_cast<size_t>(map.width()) * static_cast<size_t>(map.height()), 0),
      hp_(kind_.size(), 0) {
    kindOfDef_.assign(defs.size(), -1);
    for (const DefId id : defs.idsOfKind("Things")) {
        const Def& def = defs.get(id);
        if (def.text("category") != "Mineable" || !def.has("mineable.hitPoints")) {
            continue;
        }
        Kind k;
        k.def = id;
        k.hitPoints = static_cast<int>(std::clamp<int64_t>(def.integer("mineable.hitPoints", 1), 1, 65535));
        k.dropDef = defs.idOf("Things", def.text("mineable.dropDef"));
        k.dropCount = static_cast<int>(std::max<int64_t>(1, def.integer("mineable.dropCount", 1)));
        k.dropChance = def.number("mineable.dropChance", 1.0);
        k.isOre = def.flag("mineable.isOre");
        k.commonality = def.number("mineable.commonality", 1.0);
        k.lumpMin = static_cast<int>(std::max<int64_t>(1, static_cast<int64_t>(def.numberAt("mineable.lumpSize", 0, 1.0))));
        k.lumpMax = static_cast<int>(std::max<int64_t>(k.lumpMin, static_cast<int64_t>(def.numberAt("mineable.lumpSize", 1, k.lumpMin))));
        kindOfDef_[id.value] = static_cast<int>(kinds_.size());
        defIds_.push_back(id);
        kinds_.push_back(k);
    }
    if (!kinds_.empty()) {
        generate(defs);
    }
}

bool Mineables::map_inBounds(CellPos c) const {
    return map_.onGround(c);
}

size_t Mineables::indexOf(CellPos c) const {
    return static_cast<size_t>(map_.index(c));
}

DefId Mineables::defAt(CellPos c) const {
    if (!map_inBounds(c)) {
        return DefId{};
    }
    const uint8_t k = kind_[indexOf(c)];
    return k == 0 ? DefId{} : kinds_[static_cast<size_t>(k) - 1].def;
}

int Mineables::kindOfDef(DefId def) const {
    return def.valid() && def.value < kindOfDef_.size() ? kindOfDef_[def.value] : -1;
}

int Mineables::maxHitPoints(DefId def) const {
    const int k = kindOfDef(def);
    return k < 0 ? 0 : kinds_[static_cast<size_t>(k)].hitPoints;
}

bool Mineables::isOre(DefId def) const {
    const int k = kindOfDef(def);
    return k >= 0 && kinds_[static_cast<size_t>(k)].isOre;
}

size_t Mineables::countOf(DefId def) const {
    const int k = kindOfDef(def);
    return k < 0 ? 0 : static_cast<size_t>(std::count(kind_.begin(), kind_.end(), static_cast<uint8_t>(k + 1)));
}

uint64_t Mineables::cellHash(size_t i) const {
    return splitMix64(splitMix64((static_cast<uint64_t>(i) << 16) | kind_[i]) ^ hp_[i]);
}

void Mineables::set(size_t i, uint8_t kind, uint16_t hp) {
    if (kind_[i] != 0) {
        hash_ ^= cellHash(i);
        --count_;
    }
    kind_[i] = kind;
    hp_[i] = kind != 0 ? hp : 0;
    if (kind != 0) {
        hash_ ^= cellHash(i);
        ++count_;
    }
}

bool Mineables::place(CellPos c, DefId def) {
    const int k = kindOfDef(def);
    if (k < 0 || !map_inBounds(c)) {
        return false;
    }
    map_.setTerrain(c, Terrain::Rock);
    set(indexOf(c), static_cast<uint8_t>(k + 1), static_cast<uint16_t>(kinds_[static_cast<size_t>(k)].hitPoints));
    return true;
}

bool Mineables::hit(CellPos c, int damage) {
    if (!has(c)) {
        return false;
    }
    const size_t i = indexOf(c);
    if (damage < static_cast<int>(hp_[i])) {
        set(i, kind_[i], static_cast<uint16_t>(hp_[i] - std::max(0, damage)));
        return false;
    }
    set(i, 0, 0);
    map_.setTerrain(c, Terrain::RoughStone);
    return true;
}

MineDrop Mineables::dropOf(CellPos c) const {
    MineDrop drop;
    if (!has(c)) {
        return drop;
    }
    const Kind& k = kinds_[static_cast<size_t>(kind_[indexOf(c)]) - 1];
    if (!k.dropDef.valid()) {
        return drop;
    }
    // One draw per cell: whether this cell holds a chunk is a fact about the cell.
    const uint64_t h = splitMix64(splitMix64(seed_ ^ 0x6368756E6BULL) ^ static_cast<uint64_t>(map_.index(c)));
    const double roll = static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0);
    if (k.dropChance < 1.0 && roll >= k.dropChance) {
        return drop;
    }
    drop.def = k.dropDef;
    drop.count = k.dropCount;
    return drop;
}

void Mineables::generate(const DefDatabase& defs) {
    std::vector<size_t> rockKinds;
    std::vector<size_t> oreKinds;
    for (size_t k = 0; k < kinds_.size(); ++k) {
        (kinds_[k].isOre ? oreKinds : rockKinds).push_back(k);
    }

    const Def* rules = defs.find("Rules", "WorkRules");
    const int typesMin = rules ? static_cast<int>(rules->numberAt("mining.rockTypesPerMap", 0, 2)) : 2;
    const int typesMax = rules ? static_cast<int>(rules->numberAt("mining.rockTypesPerMap", 1, 3)) : 3;
    const double orePer10k = rules ? rules->number("mining.orePerTenThousandRockCells", 6.0) : 6.0;

    Rng rng(splitMix64(seed_ ^ 0x726F636BULL), 11);
    // Which rock types this map has: a shuffle of the rock defs, the first few kept.
    for (size_t i = rockKinds.size(); i > 1; --i) {
        std::swap(rockKinds[i - 1], rockKinds[rng.below(static_cast<uint32_t>(i))]);
    }
    const size_t wanted = static_cast<size_t>(typesMin + static_cast<int>(rng.below(static_cast<uint32_t>(std::max(1, typesMax - typesMin + 1)))));
    rockKinds.resize(std::min(rockKinds.size(), std::max<size_t>(1, wanted)));
    for (const size_t k : rockKinds) {
        rockTypes_.push_back(kinds_[k].def);
    }

    std::vector<int> rockCells;
    const uint64_t typeSeed = splitMix64(seed_ ^ 0x7479706573ULL);
    for (int y = 0; y < map_.height(); ++y) {
        for (int x = 0; x < map_.width(); ++x) {
            const CellPos c{x, y, 0};
            if (map_.terrain(c) != Terrain::Rock) {
                continue;
            }
            const size_t i = indexOf(c);
            if (rockKinds.empty()) {
                continue;
            }
            const float n = mineNoise(typeSeed, x, y, kMineNoiseCell);
            const size_t pick = std::min(rockKinds.size() - 1, static_cast<size_t>(n * static_cast<float>(rockKinds.size())));
            const Kind& k = kinds_[rockKinds[pick]];
            set(i, static_cast<uint8_t>(rockKinds[pick] + 1), static_cast<uint16_t>(k.hitPoints));
            rockCells.push_back(static_cast<int>(i));
        }
    }

    // Ore lumps: a start cell in the rock, then random growth through neighbouring rock cells.
    if (rockCells.empty()) {
        return;
    }
    std::vector<int> frontier;
    for (const size_t k : oreKinds) {
        const Kind& ore = kinds_[k];
        const double expected = static_cast<double>(rockCells.size()) / 10000.0 * orePer10k * ore.commonality;
        int lumps = static_cast<int>(expected);
        if (static_cast<double>(rng.next()) / 4294967296.0 < expected - static_cast<double>(lumps)) {
            ++lumps;
        }
        for (int lump = 0; lump < lumps; ++lump) {
            const int size = ore.lumpMin + static_cast<int>(rng.below(static_cast<uint32_t>(ore.lumpMax - ore.lumpMin + 1)));
            const int start = rockCells[rng.below(static_cast<uint32_t>(rockCells.size()))];
            frontier.assign(1, start);
            for (int placed = 0; placed < size && !frontier.empty();) {
                const size_t pickAt = rng.below(static_cast<uint32_t>(frontier.size()));
                const int i = frontier[pickAt];
                frontier[pickAt] = frontier.back();
                frontier.pop_back();
                const Kind& here = kinds_[static_cast<size_t>(kind_[static_cast<size_t>(i)]) - 1];
                if (here.isOre) {
                    continue;
                }
                set(static_cast<size_t>(i), static_cast<uint8_t>(k + 1), static_cast<uint16_t>(ore.hitPoints));
                ++placed;
                const CellPos c = map_.cellAt(i);
                const CellPos around[4] = {{c.x + 1, c.y, 0}, {c.x - 1, c.y, 0}, {c.x, c.y + 1, 0}, {c.x, c.y - 1, 0}};
                for (const CellPos& n : around) {
                    if (map_.onGround(n) && kind_[indexOf(n)] != 0 && !kinds_[static_cast<size_t>(kind_[indexOf(n)]) - 1].isOre) {
                        frontier.push_back(static_cast<int>(indexOf(n)));
                    }
                }
            }
        }
    }
}

std::vector<MineableRecord> Mineables::records() const {
    std::vector<MineableRecord> out;
    out.reserve(count_);
    for (size_t i = 0; i < kind_.size(); ++i) {
        if (kind_[i] != 0) {
            out.push_back(MineableRecord{static_cast<int>(i), kinds_[static_cast<size_t>(kind_[i]) - 1].def, hp_[i]});
        }
    }
    return out;
}

bool Mineables::restore(const std::vector<MineableRecord>& records) {
    // Rock cells with no record were mined (when the map has rock types at all, else the rock is
    // plain solid ground that was never in the layer); cells with one are rock (a test may have
    // placed it).
    std::vector<uint8_t> wasRock(kind_.size(), 0);
    for (size_t i = 0; i < kind_.size(); ++i) {
        wasRock[i] = map_.terrain(map_.cellAt(static_cast<int>(i))) == Terrain::Rock ? 1 : 0;
    }
    std::fill(kind_.begin(), kind_.end(), uint8_t{0});
    std::fill(hp_.begin(), hp_.end(), uint16_t{0});
    count_ = 0;
    hash_ = 0;
    for (const MineableRecord& r : records) {
        const int k = kindOfDef(r.def);
        if (k < 0 || r.cell < 0 || static_cast<size_t>(r.cell) >= kind_.size() || kind_[static_cast<size_t>(r.cell)] != 0 ||
            r.hitPoints == 0 || r.hitPoints > kinds_[static_cast<size_t>(k)].hitPoints) {
            std::fill(kind_.begin(), kind_.end(), uint8_t{0});
            std::fill(hp_.begin(), hp_.end(), uint16_t{0});
            count_ = 0;
            hash_ = 0;
            return false;
        }
        set(static_cast<size_t>(r.cell), static_cast<uint8_t>(k + 1), r.hitPoints);
    }
    for (size_t i = 0; i < kind_.size(); ++i) {
        const CellPos c = map_.cellAt(static_cast<int>(i));
        if (kind_[i] != 0) {
            map_.setTerrain(c, Terrain::Rock);
        } else if (wasRock[i] != 0 && !rockTypes_.empty()) {
            map_.setTerrain(c, Terrain::RoughStone);  // every rock cell had a record once: this one was mined
        }
    }
    return true;
}

}  // namespace colony
