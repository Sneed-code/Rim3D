#include "ColonySim/World.h"

#include "ColonySim/Hash.h"
#include "ColonySim/Random.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <queue>
#include <set>
#include <utility>

namespace colony {

const char* hillinessName(Hilliness h) {
    switch (h) {
        case Hilliness::Flat: return "Flat";
        case Hilliness::SmallHills: return "SmallHills";
        case Hilliness::LargeHills: return "LargeHills";
        case Hilliness::Mountainous: return "Mountainous";
        case Hilliness::Impassable: return "Impassable";
    }
    return "Flat";
}

const char* riverSizeName(RiverSize r) {
    switch (r) {
        case RiverSize::None: return "None";
        case RiverSize::Creek: return "Creek";
        case RiverSize::River: return "River";
        case RiverSize::LargeRiver: return "LargeRiver";
        case RiverSize::HugeRiver: return "HugeRiver";
    }
    return "None";
}

const char* roadKindName(RoadKind r) {
    switch (r) {
        case RoadKind::None: return "None";
        case RoadKind::Dirt: return "Dirt";
        case RoadKind::Stone: return "Stone";
        case RoadKind::Asphalt: return "Asphalt";
    }
    return "None";
}

namespace {

// Only + - * / and sqrt are used on floats (no pow, exp or trig), so the world is the same on
// every compiler and platform, like the rest of the sim.

float worldLattice(uint64_t seed, int ix, int iy) {
    const uint64_t h = splitMix64(seed ^ splitMix64(static_cast<uint64_t>(ix) * 0x9E3779B1ULL + static_cast<uint64_t>(iy) * 0x85EBCA6BULL));
    return static_cast<float>(h >> 40) * (1.0f / 16777216.0f);
}

// Value noise in [0, 1), wrapping east to west. u and v run over the whole map (0..1).
float worldNoise(uint64_t seed, float u, float v, int cellsX, int cellsY) {
    const float fx = u * static_cast<float>(cellsX);
    const float fy = v * static_cast<float>(cellsY);
    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = std::min(static_cast<int>(std::floor(fy)), cellsY - 1);
    float tx = fx - static_cast<float>(x0);
    float ty = fy - static_cast<float>(y0);
    tx = tx * tx * (3.0f - 2.0f * tx);
    ty = ty * ty * (3.0f - 2.0f * ty);
    const int xa = ((x0 % cellsX) + cellsX) % cellsX;
    const int xb = (xa + 1) % cellsX;
    const float a = worldLattice(seed, xa, y0) * (1.0f - tx) + worldLattice(seed, xb, y0) * tx;
    const float b = worldLattice(seed, xa, y0 + 1) * (1.0f - tx) + worldLattice(seed, xb, y0 + 1) * tx;
    return a * (1.0f - ty) + b * ty;
}

float worldFbm(uint64_t seed, float u, float v, int baseCells, int octaves, int height, int width) {
    float sum = 0.0f;
    float amp = 1.0f;
    float total = 0.0f;
    int cx = std::max(1, baseCells);
    for (int o = 0; o < octaves; ++o) {
        const int cy = std::max(1, cx * height / std::max(1, width));
        sum += amp * worldNoise(splitMix64(seed + static_cast<uint64_t>(o)), u, v, cx, cy);
        total += amp;
        amp *= 0.5f;
        cx *= 2;
    }
    return sum / total;
}

float worldClamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

float worldBump(float d, float width) {
    const float q = d / width;
    return 1.0f / (1.0f + q * q);
}

// The k-th smallest of a copy of the values (k in 0..n-1).
float worldQuantile(std::vector<float> values, float q) {
    if (values.empty()) {
        return 0.0f;
    }
    const size_t k = std::min(values.size() - 1, static_cast<size_t>(q * static_cast<float>(values.size())));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(k), values.end());
    return values[k];
}

struct WorldRule {
    std::string biome;
    float tempMin = -1000.0f;  // matches when temperature > tempMin ...
    float tempMax = 1000.0f;   // ... and <= tempMax
    float rainMin = -1.0f;     // rainfall >= rainMin ...
    float rainMax = 1e9f;      // ... and < rainMax
    int swampy = -1;           // -1 don't care, 0 must not be swampy, 1 must be
};

std::vector<WorldRule> worldReadRules(const Def& def, const char* field) {
    std::vector<WorldRule> rules;
    const size_t n = def.count(field);
    for (size_t i = 0; i < n; ++i) {
        const std::string base = std::string(field) + "." + std::to_string(i) + ".";
        WorldRule r;
        r.biome = def.text((base + "biome").c_str());
        r.tempMin = static_cast<float>(def.number((base + "tempMin").c_str(), r.tempMin));
        r.tempMax = static_cast<float>(def.number((base + "tempMax").c_str(), r.tempMax));
        r.rainMin = static_cast<float>(def.number((base + "rainMin").c_str(), r.rainMin));
        r.rainMax = static_cast<float>(def.number((base + "rainMax").c_str(), r.rainMax));
        if (def.has((base + "swampy").c_str())) {
            r.swampy = def.flag((base + "swampy").c_str()) ? 1 : 0;
        }
        rules.push_back(r);
    }
    return rules;
}

bool worldRuleMatches(const WorldRule& r, float t, float rain, bool swampy) {
    return t > r.tempMin && t <= r.tempMax && rain >= r.rainMin && rain < r.rainMax &&
           (r.swampy < 0 || (r.swampy == 1) == swampy);
}

Hilliness worldHillinessFromName(const std::string& name, Hilliness fallback) {
    for (const Hilliness h : {Hilliness::Flat, Hilliness::SmallHills, Hilliness::LargeHills, Hilliness::Mountainous,
                              Hilliness::Impassable}) {
        if (name == hillinessName(h)) {
            return h;
        }
    }
    return fallback;
}

int worldRoadStepCost(const WorldTile& t) {
    if (t.water || t.hilliness == Hilliness::Impassable) {
        return -1;
    }
    int cost = 10;
    switch (t.hilliness) {
        case Hilliness::SmallHills: cost = 15; break;
        case Hilliness::LargeHills: cost = 25; break;
        case Hilliness::Mountainous: cost = 60; break;
        default: break;
    }
    switch (t.river) {
        case RiverSize::River: cost += 10; break;
        case RiverSize::LargeRiver: cost += 30; break;
        case RiverSize::HugeRiver: cost += 60; break;
        default: break;
    }
    return cost;
}

const char* const kWorldSyllablesA[] = {"Ar", "Bel", "Cor", "Dun", "Eld", "Fal", "Gor", "Har", "Ith", "Jor", "Kel", "Lor", "Mar",
                                        "Nor", "Or", "Pel", "Quen", "Ros", "Sar", "Tor", "Ul", "Ver", "Wyn", "Yor", "Zan"};
const char* const kWorldSyllablesB[] = {"ford", "holm", "wick", "stead", "ton", "mere", "gate", "fell", "haven", "burg"};

}  // namespace

struct WorldBuilder {
    World& w;
    const DefDatabase& defs;
    const Def& cfg;
    std::vector<float> key;  // elevation with a tiny per-tile jitter: strictly ordered, so water flows without ties

    WorldBuilder(World& world, const DefDatabase& d, const Def& c) : w(world), defs(d), cfg(c) {}

    float num(const char* path, float fallback = 0.0f) const { return static_cast<float>(cfg.number(path, fallback)); }

    void run() {
        const int W = w.width_;
        const int H = w.height_;
        const int n = W * H;
        w.tiles_.assign(static_cast<size_t>(n), WorldTile{});

        // Biome palette: every Biomes def, in id order.
        for (const DefId id : defs.idsOfKind("Biomes")) {
            w.biomes_.push_back(defs.get(id).defName());
        }

        elevation();
        temperature();
        rainfall();
        biomes();
        hilliness();
        rivers();
        settlementsAndRoads();
    }

    float u(int x) const { return (static_cast<float>(x) + 0.5f) / static_cast<float>(w.width_); }
    float v(int y) const { return (static_cast<float>(y) + 0.5f) / static_cast<float>(w.height_); }
    float latitude(int y) const { return v(y) * 2.0f - 1.0f; }  // -1 north pole .. 1 south pole

    uint64_t salt(uint64_t k) const { return splitMix64(w.seed_ ^ (k * 0x1000193ULL)); }

    void elevation() {
        const int W = w.width_;
        const int H = w.height_;
        const int n = W * H;
        const int cells = static_cast<int>(num("elevation.baseNoiseCells", 4));
        const int oct = static_cast<int>(num("elevation.octaves", 5));
        std::vector<float> e(static_cast<size_t>(n));
        key.assign(static_cast<size_t>(n), 0.0f);
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                e[static_cast<size_t>(w.index(x, y))] = worldFbm(salt(1), u(x), v(y), cells, oct, H, W);
            }
        }
        const float sea = worldQuantile(e, 1.0f - num("landFraction", 0.4f));
        const float lo = *std::min_element(e.begin(), e.end());
        const float hi = *std::max_element(e.begin(), e.end());
        const float peak = num("elevation.highestPeak", 4500);
        const float deep = num("elevation.seaFloorDepth", 4000);
        elev01_.assign(static_cast<size_t>(n), 0.0f);
        for (int i = 0; i < n; ++i) {
            WorldTile& t = w.tiles_[static_cast<size_t>(i)];
            const float ev = e[static_cast<size_t>(i)];
            key[static_cast<size_t>(i)] = ev + static_cast<float>(splitMix64(w.seed_ + static_cast<uint64_t>(i)) % 100000ULL) * 1e-9f;
            if (ev > sea) {
                const float f = (ev - sea) / std::max(1e-6f, hi - sea);
                elev01_[static_cast<size_t>(i)] = f;
                t.elevation = static_cast<int16_t>(f * std::sqrt(f) * peak + 1.0f);
                t.water = false;
            } else {
                const float f = (sea - ev) / std::max(1e-6f, sea - lo);
                t.elevation = static_cast<int16_t>(-(f * deep) - 1.0f);
                t.water = true;
            }
        }
    }

    void temperature() {
        const int W = w.width_;
        const int H = w.height_;
        for (int y = 0; y < H; ++y) {
            const float lat = std::fabs(latitude(y));
            const float drop = lat * std::sqrt(lat) * num("temperature.poleDrop", 52);
            for (int x = 0; x < W; ++x) {
                WorldTile& t = w.tiles_[static_cast<size_t>(w.index(x, y))];
                const float noise = worldFbm(salt(2), u(x), v(y), 3, 3, H, W) * 2.0f - 1.0f;
                float temp = num("temperature.equator", 29) - drop + noise * num("temperature.noiseSwing", 3);
                if (t.water) {
                    temp *= num("temperature.waterModeration", 0.9f);
                } else {
                    temp -= static_cast<float>(t.elevation) / 1000.0f * num("temperature.lapsePerKm", 6.5f);
                }
                t.temperature = temp;
            }
        }
    }

    void rainfall() {
        const int W = w.width_;
        const int H = w.height_;
        const int radius = static_cast<int>(num("rainfall.coastRadius", 3));
        const int noiseCells = static_cast<int>(num("rainfall.noiseCells", 5));
        for (int y = 0; y < H; ++y) {
            const float lat = std::fabs(latitude(y));
            const float zonal = num("rainfall.equatorBand", 0.55f) * worldBump(lat, num("rainfall.equatorWidth", 0.12f)) +
                                num("rainfall.midLatitudeBand", 0.35f) *
                                    worldBump(lat - num("rainfall.midLatitude", 0.65f), num("rainfall.midLatitudeWidth", 0.15f));
            for (int x = 0; x < W; ++x) {
                WorldTile& t = w.tiles_[static_cast<size_t>(w.index(x, y))];
                int wet = 0;
                int seen = 0;
                for (int dy = -radius; dy <= radius; ++dy) {
                    for (int dx = -radius; dx <= radius; ++dx) {
                        const int yy = y + dy;
                        if (yy < 0 || yy >= H) {
                            continue;
                        }
                        ++seen;
                        wet += w.tiles_[static_cast<size_t>(w.index(((x + dx) % W + W) % W, yy))].water ? 1 : 0;
                    }
                }
                const float coastal = seen > 0 ? static_cast<float>(wet) / static_cast<float>(seen) : 0.0f;
                const float noise = worldFbm(salt(3), u(x), v(y), noiseCells, 3, H, W);
                float rain = num("rainfall.base", 150) + num("rainfall.scale", 2400) * zonal * (0.4f + 1.2f * noise) +
                             num("rainfall.coastBonus", 400) * coastal;
                const float cold = worldClamp((t.temperature + 20.0f) / 40.0f, num("rainfall.coldDryFactor", 0.3f), 1.0f);
                t.rainfall = rain * cold;
            }
        }
    }

    int paletteIndex(const std::string& name) const {
        for (size_t i = 0; i < w.biomes_.size(); ++i) {
            if (w.biomes_[i] == name) {
                return static_cast<int>(i);
            }
        }
        for (size_t i = 0; i < w.biomes_.size(); ++i) {
            if (w.biomes_[i] == "TemperateForest") {
                return static_cast<int>(i);
            }
        }
        return w.biomes_.empty() ? kNoBiome : 0;
    }

    void biomes() {
        const int W = w.width_;
        const int H = w.height_;
        const std::vector<WorldRule> land = worldReadRules(cfg, "biomeRules");
        const std::vector<WorldRule> sea = worldReadRules(cfg, "waterRules");
        const float swampLimit = num("swampiness.threshold", 0.62f);
        const int swampCells = static_cast<int>(num("swampiness.noiseCells", 6));
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                WorldTile& t = w.tiles_[static_cast<size_t>(w.index(x, y))];
                const bool swampy = worldFbm(salt(4), u(x), v(y), swampCells, 3, H, W) > swampLimit;
                const std::vector<WorldRule>& rules = t.water ? sea : land;
                for (const WorldRule& r : rules) {
                    if (worldRuleMatches(r, t.temperature, t.rainfall, swampy)) {
                        t.biome = static_cast<uint8_t>(paletteIndex(r.biome));
                        break;
                    }
                }
            }
        }
    }

    void hilliness() {
        const int W = w.width_;
        const int H = w.height_;
        const int n = W * H;
        std::vector<float> score(static_cast<size_t>(n), 0.0f);
        std::vector<float> landScores;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const int i = w.index(x, y);
                if (w.tiles_[static_cast<size_t>(i)].water) {
                    continue;
                }
                const float s = num("hilliness.elevationWeight", 0.6f) * elev01_[static_cast<size_t>(i)] +
                                num("hilliness.ruggednessWeight", 0.6f) * worldFbm(salt(5), u(x), v(y), 8, 3, H, W);
                score[static_cast<size_t>(i)] = s;
                landScores.push_back(s);
            }
        }
        float cut[4];
        for (size_t k = 0; k < 4; ++k) {
            cut[k] = worldQuantile(landScores, static_cast<float>(cfg.numberAt("hilliness.quantiles", k, 0.5)));
        }
        for (int i = 0; i < n; ++i) {
            WorldTile& t = w.tiles_[static_cast<size_t>(i)];
            if (t.water) {
                continue;
            }
            const float s = score[static_cast<size_t>(i)];
            t.hilliness = s < cut[0]   ? Hilliness::Flat
                          : s < cut[1] ? Hilliness::SmallHills
                          : s < cut[2] ? Hilliness::LargeHills
                          : s < cut[3] ? Hilliness::Mountainous
                                       : Hilliness::Impassable;
        }
    }

    // Water runs from every land tile to its lowest neighbour; a tile's river size is how much
    // water passes through it. A big pit with no way out becomes a lake.
    void rivers() {
        const int n = w.width_ * w.height_;
        std::vector<int> order;
        for (int i = 0; i < n; ++i) {
            if (!w.tiles_[static_cast<size_t>(i)].water) {
                order.push_back(i);
            }
        }
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            const float ka = key[static_cast<size_t>(a)];
            const float kb = key[static_cast<size_t>(b)];
            return ka != kb ? ka > kb : a < b;
        });
        const float unit = std::max(1.0f, std::sqrt(static_cast<float>(order.size())) * num("rivers.sizeUnit", 0.25f));
        std::vector<float> flow(static_cast<size_t>(n), 0.0f);
        std::vector<int> down(static_cast<size_t>(n), -1);
        std::vector<uint8_t> downDir(static_cast<size_t>(n), kNoDirection);
        const float perFlow = num("rivers.rainfallPerFlow", 1500);
        for (const int i : order) {
            flow[static_cast<size_t>(i)] += 1.0f + w.tiles_[static_cast<size_t>(i)].rainfall / perFlow;
            int best = -1;
            int bestDir = 0;
            for (int d = 0; d < 8; ++d) {
                const int j = w.neighbour(i, d);
                if (j < 0 || key[static_cast<size_t>(j)] >= key[static_cast<size_t>(i)]) {
                    continue;
                }
                if (best < 0 || key[static_cast<size_t>(j)] < key[static_cast<size_t>(best)]) {
                    best = j;
                    bestDir = d;
                }
            }
            if (best >= 0) {
                down[static_cast<size_t>(i)] = best;
                downDir[static_cast<size_t>(i)] = static_cast<uint8_t>(bestDir);
                flow[static_cast<size_t>(best)] += flow[static_cast<size_t>(i)];
            }
        }
        for (const int i : order) {
            WorldTile& t = w.tiles_[static_cast<size_t>(i)];
            const float f = flow[static_cast<size_t>(i)] / unit;
            if (down[static_cast<size_t>(i)] < 0) {
                if (f >= num("rivers.lakeSinkAtLeast", 3)) {
                    t.water = true;
                    t.biome = kNoBiome;
                    t.hilliness = Hilliness::Flat;
                }
                continue;
            }
            t.river = f >= num("rivers.hugeRiver", 20)        ? RiverSize::HugeRiver
                      : f >= num("rivers.largeRiver", 8)      ? RiverSize::LargeRiver
                      : f >= num("rivers.river", 3)           ? RiverSize::River
                      : f >= num("rivers.creek", 1)           ? RiverSize::Creek
                                                              : RiverSize::None;
            if (t.river != RiverSize::None) {
                t.riverFlowsTo = downDir[static_cast<size_t>(i)];
            }
        }
        // A river into a lake that was just made ends there; one that flows from a lake tile has no bed.
        for (WorldTile& t : w.tiles_) {
            if (t.water) {
                t.river = RiverSize::None;
                t.riverFlowsTo = kNoDirection;
            }
        }
    }

    int distance(int a, int b) const {
        int dx = std::abs(w.tileX(a) - w.tileX(b));
        dx = std::min(dx, w.width_ - dx);
        return std::max(dx, std::abs(w.tileY(a) - w.tileY(b)));
    }

    void settlementsAndRoads() {
        // Factions that live in settlements: the humanlike ones.
        struct Slot {
            std::string name;
            float weight = 1.0f;
            int home = 0;
        };
        std::vector<Slot> slots;
        for (const DefId id : defs.idsOfKind("Factions")) {
            const Def& f = defs.get(id);
            if (f.flag("humanlike", true)) {
                slots.push_back(Slot{f.defName(), f.flag("permanentEnemy") ? num("settlements.permanentEnemyWeight", 0.5f) : 1.0f, 0});
            }
        }
        const int minTemp = static_cast<int>(num("settlements.minTemperature", -10));
        std::vector<int> candidates;
        for (int i = 0; i < w.tileCount(); ++i) {
            const WorldTile& t = w.tiles_[static_cast<size_t>(i)];
            if (!t.water && t.hilliness != Hilliness::Impassable && t.temperature > static_cast<float>(minTemp)) {
                candidates.push_back(i);
            }
        }
        if (slots.empty() || candidates.empty()) {
            return;
        }
        Rng rng(w.seed_, 11);
        for (size_t i = candidates.size(); i > 1; --i) {  // Fisher-Yates
            std::swap(candidates[i - 1], candidates[rng.below(static_cast<uint32_t>(i))]);
        }
        for (Slot& s : slots) {
            s.home = candidates[rng.below(static_cast<uint32_t>(candidates.size()))];
        }
        const int want = static_cast<int>(static_cast<float>(w.landTileCount()) * num("settlements.per100Land", 3) / 100.0f + 0.5f);
        const int spacing = static_cast<int>(num("settlements.minSpacing", 3));
        std::set<std::string> used;
        for (const int c : candidates) {
            if (static_cast<int>(w.settlements_.size()) >= want) {
                break;
            }
            bool near = false;
            for (const WorldSettlement& s : w.settlements_) {
                if (distance(s.tile, c) < spacing) {
                    near = true;
                    break;
                }
            }
            if (near) {
                continue;
            }
            size_t best = 0;
            float bestScore = 1e30f;
            for (size_t k = 0; k < slots.size(); ++k) {
                const float score = static_cast<float>(distance(c, slots[k].home) + 1) * (0.6f + 0.8f * rng.unit()) / slots[k].weight;
                if (score < bestScore) {
                    bestScore = score;
                    best = k;
                }
            }
            WorldSettlement s;
            s.tile = c;
            s.faction = slots[best].name;
            do {
                s.name = std::string(kWorldSyllablesA[rng.below(25)]) + kWorldSyllablesB[rng.below(10)];
            } while (!used.insert(s.name).second);
            w.tiles_[static_cast<size_t>(c)].settlement = static_cast<int32_t>(w.settlements_.size());
            w.settlements_.push_back(s);
        }
        roads();
    }

    void roads() {
        const int n = w.tileCount();
        const int limit = static_cast<int>(num("settlements.roadCostLimit", 400));
        const int perSettlement = static_cast<int>(num("settlements.roadsPerSettlement", 2));
        std::map<std::pair<int, int>, int> uses;
        std::set<std::pair<int, int>> linked;
        std::vector<int> dist(static_cast<size_t>(n));
        std::vector<int> parent(static_cast<size_t>(n));
        for (size_t si = 0; si < w.settlements_.size(); ++si) {
            std::fill(dist.begin(), dist.end(), INT32_MAX);
            std::fill(parent.begin(), parent.end(), -1);
            using Node = std::pair<int, int>;  // cost, tile
            std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;
            const int start = w.settlements_[si].tile;
            dist[static_cast<size_t>(start)] = 0;
            open.push({0, start});
            while (!open.empty()) {
                const auto [cost, at] = open.top();
                open.pop();
                if (cost > dist[static_cast<size_t>(at)] || cost > limit) {
                    continue;
                }
                for (int d = 0; d < 8; ++d) {
                    const int to = w.neighbour(at, d);
                    if (to < 0) {
                        continue;
                    }
                    const int step = worldRoadStepCost(w.tiles_[static_cast<size_t>(to)]);
                    if (step < 0) {
                        continue;
                    }
                    const int total = cost + ((d % 2 == 1) ? step * 14 / 10 : step);
                    if (total < dist[static_cast<size_t>(to)]) {
                        dist[static_cast<size_t>(to)] = total;
                        parent[static_cast<size_t>(to)] = at;
                        open.push({total, to});
                    }
                }
            }
            std::vector<std::pair<int, size_t>> reached;  // cost, settlement
            for (size_t sj = 0; sj < w.settlements_.size(); ++sj) {
                const int c = dist[static_cast<size_t>(w.settlements_[sj].tile)];
                if (sj != si && c != INT32_MAX && c <= limit) {
                    reached.push_back({c, sj});
                }
            }
            std::sort(reached.begin(), reached.end());
            for (size_t k = 0; k < reached.size() && static_cast<int>(k) < perSettlement; ++k) {
                const size_t sj = reached[k].second;
                if (!linked.insert({static_cast<int>(std::min(si, sj)), static_cast<int>(std::max(si, sj))}).second) {
                    continue;
                }
                for (int at = w.settlements_[sj].tile; parent[static_cast<size_t>(at)] >= 0; at = parent[static_cast<size_t>(at)]) {
                    const int p = parent[static_cast<size_t>(at)];
                    ++uses[{std::min(at, p), std::max(at, p)}];
                }
            }
        }
        const int stoneUses = static_cast<int>(num("settlements.roadStoneUses", 2));
        const int asphaltUses = static_cast<int>(num("settlements.roadAsphaltUses", 4));
        for (const auto& [edge, count] : uses) {
            WorldRoad r;
            r.tileA = edge.first;
            r.tileB = edge.second;
            r.kind = count >= asphaltUses ? RoadKind::Asphalt : (count >= stoneUses ? RoadKind::Stone : RoadKind::Dirt);
            w.roads_.push_back(r);
            for (const int t : {r.tileA, r.tileB}) {
                WorldTile& tile = w.tiles_[static_cast<size_t>(t)];
                if (static_cast<uint8_t>(r.kind) > static_cast<uint8_t>(tile.road)) {
                    tile.road = r.kind;
                }
            }
        }
    }

    std::vector<float> elev01_;
};

World World::generate(const DefDatabase& defs, uint64_t seed, int width, int height) {
    World w;
    const Def* cfg = defs.find("WorldGen", "Default");
    if (cfg == nullptr) {
        return w;
    }
    w.seed_ = seed;
    w.width_ = width > 0 ? width : static_cast<int>(cfg->integer("width", 120));
    w.height_ = height > 0 ? height : static_cast<int>(cfg->integer("height", 60));
    w.maxStartHilliness_ = static_cast<int>(worldHillinessFromName(cfg->text("startTile.maxHilliness"), Hilliness::Mountainous));
    WorldBuilder builder(w, defs, *cfg);
    builder.run();
    return w;
}

int World::neighbour(int index, int dir) const {
    const int y = tileY(index) + kNeighbourDy[dir];
    if (y < 0 || y >= height_) {
        return -1;
    }
    const int x = ((tileX(index) + kNeighbourDx[dir]) % width_ + width_) % width_;
    return this->index(x, y);
}

const std::string& World::biomeName(const WorldTile& t) const {
    static const std::string kNone;
    return t.biome < biomes_.size() ? biomes_[t.biome] : kNone;
}

int World::landTileCount() const {
    int n = 0;
    for (const WorldTile& t : tiles_) {
        n += t.water ? 0 : 1;
    }
    return n;
}

bool World::canStartAt(int index) const {
    if (index < 0 || index >= tileCount()) {
        return false;
    }
    const WorldTile& t = tiles_[static_cast<size_t>(index)];
    return !t.water && t.biome != kNoBiome && static_cast<int>(t.hilliness) <= maxStartHilliness_ &&
           t.hilliness != Hilliness::Impassable && t.settlement == kNoSettlement;
}

bool World::chooseStartTile(int index) {
    if (!canStartAt(index)) {
        return false;
    }
    startTile_ = index;
    return true;
}

bool World::canVisit(int index) const {
    if (index < 0 || index >= tileCount()) {
        return false;
    }
    const WorldTile& t = tiles_[static_cast<size_t>(index)];
    return !t.water && t.biome != kNoBiome && t.hilliness != Hilliness::Impassable;
}

bool World::chooseVisitTile(int index) {
    if (!canVisit(index)) {
        return false;
    }
    startTile_ = index;
    return true;
}

int World::suggestStartTile(const StartCriteria& c) const {
    std::vector<int> matches;
    for (int i = 0; i < tileCount(); ++i) {
        const WorldTile& t = tiles_[static_cast<size_t>(i)];
        if (!canStartAt(i) || t.temperature < c.minTemperature || t.temperature > c.maxTemperature ||
            (c.needRiver && t.river == RiverSize::None) || (c.needRoad && t.road == RoadKind::None) ||
            (!c.biome.empty() && biomeName(t) != c.biome)) {
            continue;
        }
        matches.push_back(i);
    }
    if (matches.empty()) {
        return -1;
    }
    Rng rng(c.seed, 12);
    return matches[rng.below(static_cast<uint32_t>(matches.size()))];
}

WorldSite World::siteFor(int index) const {
    WorldSite s;
    const WorldTile& t = tiles_[static_cast<size_t>(index)];
    s.tile = index;
    s.biome = biomeName(t);
    s.hilliness = t.hilliness;
    s.river = t.river;
    s.road = t.road;
    s.temperature = t.temperature;
    s.rainfall = t.rainfall;
    s.elevation = t.elevation;
    s.mapSeed = splitMix64(seed_ ^ (static_cast<uint64_t>(index) + 1) * 0x9E3779B97F4A7C15ULL);
    return s;
}

uint64_t World::hash() const {
    StateHasher h;
    h.add(seed_);
    h.add(width_);
    h.add(height_);
    for (const WorldTile& t : tiles_) {
        h.add(static_cast<int>(t.elevation));
        h.add(t.temperature);
        h.add(t.rainfall);
        h.add(static_cast<int>(t.biome));
        h.add(static_cast<int>(t.hilliness));
        h.add(static_cast<int>(t.river));
        h.add(static_cast<int>(t.riverFlowsTo));
        h.add(static_cast<int>(t.road));
        h.add(t.water);
        h.add(static_cast<int>(t.settlement));
    }
    for (const WorldSettlement& s : settlements_) {
        h.add(s.tile);
        h.add(s.faction.data(), s.faction.size());
        h.add(s.name.data(), s.name.size());
    }
    for (const WorldRoad& r : roads_) {
        h.add(r.tileA);
        h.add(r.tileB);
        h.add(static_cast<int>(r.kind));
    }
    return h.value();
}

}  // namespace colony
