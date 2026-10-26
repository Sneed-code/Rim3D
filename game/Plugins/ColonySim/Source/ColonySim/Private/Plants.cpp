#include "ColonySim/Plants.h"

#include "ColonySim/Map.h"
#include "ColonySim/Random.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.
constexpr float kPlantsNoiseCell = 40.0f;   // metres between soil patches
constexpr float kPlantsForestCell = 48.0f;  // metres between forest and meadow patches
constexpr float kPlantsGravelBelow = 0.28f;
constexpr float kPlantsRichAbove = 0.78f;

float plantsLattice(uint64_t seed, int64_t x, int64_t y) {
    uint64_t h = splitMix64(seed);
    h = splitMix64(h ^ static_cast<uint64_t>(x));
    h = splitMix64(h ^ static_cast<uint64_t>(y));
    return static_cast<float>(h >> 40) * (1.0f / 16777216.0f);
}

// Value noise in [0, 1) with lattice points every `cellSize` metres.
float plantsNoise(uint64_t seed, int x, int y, float cellSize) {
    const float gx = static_cast<float>(x) / cellSize;
    const float gy = static_cast<float>(y) / cellSize;
    const float x0f = std::floor(gx);
    const float y0f = std::floor(gy);
    const int64_t x0 = static_cast<int64_t>(x0f);
    const int64_t y0 = static_cast<int64_t>(y0f);
    const float tx = (gx - x0f) * (gx - x0f) * (3.0f - 2.0f * (gx - x0f));
    const float ty = (gy - y0f) * (gy - y0f) * (3.0f - 2.0f * (gy - y0f));
    const float a = plantsLattice(seed, x0, y0);
    const float b = plantsLattice(seed, x0 + 1, y0);
    const float c = plantsLattice(seed, x0, y0 + 1);
    const float d = plantsLattice(seed, x0 + 1, y0 + 1);
    const float top = a + (b - a) * tx;
    const float bottom = c + (d - c) * tx;
    return top + (bottom - top) * ty;
}

double plantsTerrainFertility(const DefDatabase& defs, const char* name) {
    const Def* def = defs.find("Terrain", name);
    return def != nullptr ? def->number("fertility", 0.0) : 0.0;
}

}  // namespace

Plants::Plants(const Map& map, const DefDatabase& defs, uint64_t seed, const std::string& biome)
    : map_(map),
      fertility_(static_cast<size_t>(map.width()) * static_cast<size_t>(map.height()), 0.0f),
      kind_(fertility_.size(), 0),
      growth_(fertility_.size(), 0),
      age_(fertility_.size(), 0) {
    growthTuning_ = defs.tuning("plantGrowth");
    if (const Def* rules = defs.find("Rules", "PlantRules")) {
        growStartHour_ = static_cast<int>(rules->numberAt("growHours", 0, growStartHour_));
        growEndHour_ = static_cast<int>(rules->numberAt("growHours", 1, growEndHour_));
    }
    const Def* biomeDef = defs.find("Biomes", biome);
    kindOfDef_.assign(defs.size(), -1);
    for (const DefId id : defs.idsOfKind("Things")) {
        const Def& def = defs.get(id);
        if (def.text("category") != "Plant" || !def.has("plant.growDays")) {
            continue;
        }
        Kind k;
        k.def = id;
        const double growDays = def.number("plant.growDays", 1.0);
        // RimWorld's grow days are for a plant that grows round the clock; it grows only in the
        // growing hours of a day (docs/design/starting-data.md).
        k.growthPerHour = static_cast<double>(kPlantFullGrowth) / (growDays * 24.0);
        k.fertilityMin = static_cast<float>(def.number("plant.fertilityMin", 0.0));
        k.fertilitySensitivity = static_cast<float>(def.number("plant.fertilitySensitivity", 0.0));
        k.lifespanHours = static_cast<uint32_t>(std::lround(growDays * def.number("plant.lifespanDaysPerGrowDays", 8.0) * 24.0));
        k.isTree = def.flag("plant.isTree");
        if (biomeDef != nullptr) {
            k.commonality = biomeDef->number(("wildPlants." + def.defName()).c_str(), 0.0);
        }
        kindOfDef_[id.value] = static_cast<int>(kinds_.size());
        kinds_.push_back(k);
    }

    // Fertility from the terrain defs. Soil comes in three kinds laid out in patches.
    const double soil = plantsTerrainFertility(defs, "Soil");
    const double gravel = plantsTerrainFertility(defs, "Gravel");
    const double rich = plantsTerrainFertility(defs, "SoilRich");
    const double sand = plantsTerrainFertility(defs, "Sand");
    const uint64_t soilSeed = splitMix64(seed ^ 0x736F696CULL);
    for (int y = 0; y < map.height(); ++y) {
        for (int x = 0; x < map.width(); ++x) {
            const CellPos c{x, y, 0};
            double f = 0.0;
            switch (map.terrain(c)) {
                case Terrain::Sand: f = sand; break;
                case Terrain::Soil: {
                    const float n = plantsNoise(soilSeed, x, y, kPlantsNoiseCell);
                    f = n < kPlantsGravelBelow ? gravel : (n > kPlantsRichAbove ? rich : soil);
                    break;
                }
                case Terrain::Water:
                case Terrain::Rock:
                case Terrain::RoughStone:
                case Terrain::Air:
                case Terrain::UpperFloor: break;
            }
            fertility_[static_cast<size_t>(map.index(c))] = static_cast<float>(f);
        }
    }

    if (biomeDef != nullptr && !kinds_.empty()) {
        generate(seed, biomeDef->number("plantDensity", 0.0));
    }
}

size_t Plants::indexOf(CellPos c) const {
    return static_cast<size_t>(map_.index(c));
}

float Plants::fertility(CellPos c) const {
    return map_.onGround(c) ? fertility_[indexOf(c)] : 0.0f;
}

DefId Plants::defAt(CellPos c) const {
    const uint16_t k = kind_[indexOf(c)];
    return k == 0 ? DefId{} : kinds_[static_cast<size_t>(k) - 1].def;
}

int Plants::kindOfDef(DefId def) const {
    return def.valid() && def.value < kindOfDef_.size() ? kindOfDef_[def.value] : -1;
}

uint32_t Plants::lifespanHours(DefId def) const {
    const int k = kindOfDef(def);
    return k < 0 ? 0 : kinds_[static_cast<size_t>(k)].lifespanHours;
}

uint64_t Plants::cellHash(size_t i) const {
    return splitMix64(splitMix64((static_cast<uint64_t>(i) << 16) | kind_[i]) ^
                      ((static_cast<uint64_t>(growth_[i]) << 32) | age_[i]));
}

void Plants::set(size_t i, uint16_t kind, uint32_t growth, uint32_t age) {
    if (kind_[i] != 0) {
        hash_ ^= cellHash(i);
        --count_;
    }
    kind_[i] = kind;
    growth_[i] = kind != 0 ? growth : 0;
    age_[i] = kind != 0 ? age : 0;
    if (kind != 0) {
        hash_ ^= cellHash(i);
        ++count_;
    }
}

bool Plants::place(CellPos c, DefId def, float growth) {
    const int k = kindOfDef(def);
    if (k < 0 || !map_.onGround(c)) {
        return false;
    }
    const size_t i = indexOf(c);
    if (kind_[i] != 0 || fertility_[i] <= 0.0f || underUpperFloor(c)) {
        return false;
    }
    const float g = std::clamp(growth, 0.0f, 1.0f);
    set(i, static_cast<uint16_t>(k + 1), static_cast<uint32_t>(std::lround(g * static_cast<float>(kPlantFullGrowth))), 0);
    return true;
}

bool Plants::underUpperFloor(CellPos c) const {
    for (int level = 1; level < Map::kLevels; ++level) {
        if (map_.upperFloor(CellPos{c.x, c.y, level})) {
            return true;
        }
    }
    return false;
}

void Plants::remove(CellPos c) {
    if (map_.onGround(c)) {
        set(indexOf(c), 0, 0, 0);
    }
}

void Plants::setGrowth(CellPos c, float growth) {
    if (!map_.onGround(c) || !has(c)) {
        return;
    }
    const size_t i = indexOf(c);
    const float g = std::clamp(growth, 0.0f, 1.0f);
    set(i, kind_[i], static_cast<uint32_t>(std::lround(g * static_cast<float>(kPlantFullGrowth))), age_[i]);
}

size_t Plants::countOf(DefId def) const {
    const int k = kindOfDef(def);
    return k < 0 ? 0 : static_cast<size_t>(std::count(kind_.begin(), kind_.end(), static_cast<uint16_t>(k + 1)));
}

void Plants::growHour(int hourOfDay) {
    const bool growing = hourOfDay >= growStartHour_ && hourOfDay < growEndHour_;
    for (size_t i = 0; i < kind_.size(); ++i) {
        if (kind_[i] == 0) {
            continue;
        }
        // No light under an upper floor: whatever was there dies.
        if (underUpperFloor(CellPos{static_cast<int>(i % static_cast<size_t>(map_.width())),
                                    static_cast<int>(i / static_cast<size_t>(map_.width())), 0})) {
            set(i, 0, 0, 0);
            continue;
        }
        const Kind& k = kinds_[static_cast<size_t>(kind_[i]) - 1];
        const uint32_t age = age_[i] + 1;
        if (age >= k.lifespanHours) {
            set(i, 0, 0, 0);
            continue;
        }
        uint32_t growth = growth_[i];
        if (growing && growth < kPlantFullGrowth && fertility_[i] >= k.fertilityMin) {
            double rate = std::max(0.0, 1.0 + (static_cast<double>(fertility_[i]) - 1.0) * k.fertilitySensitivity);
            if (modifier_) {
                rate *= modifier_(i);
            }
            const uint64_t step = static_cast<uint64_t>(std::llround(k.growthPerHour * rate * growthTuning_));
            growth = static_cast<uint32_t>(std::min<uint64_t>(kPlantFullGrowth, growth + step));
        }
        set(i, kind_[i], growth, age);
    }
}

void Plants::generate(uint64_t seed, double density) {
    Rng rng(splitMix64(seed ^ 0x706C616E74ULL), 7);
    const uint64_t forestSeed = splitMix64(seed ^ 0x666F72657374ULL);
    std::vector<double> weight(kinds_.size());
    for (int y = 0; y < map_.height(); ++y) {
        for (int x = 0; x < map_.width(); ++x) {
            const size_t i = indexOf(CellPos{x, y, 0});
            const float fertility = fertility_[i];
            if (fertility <= 0.0f) {
                continue;
            }
            // One draw decides whether the cell has a plant, so what grows where doesn't shift
            // when a def is added.
            const double chance = density * std::min(1.0, static_cast<double>(fertility));
            if (static_cast<double>(rng.next()) >= chance * 4294967296.0) {
                continue;
            }
            // Trees come in forests and meadows: the share of trees swings between 0.2 and 1.8
            // times the biome's.
            const double forest = 0.2 + 1.6 * static_cast<double>(plantsNoise(forestSeed, x, y, kPlantsForestCell));
            double total = 0.0;
            for (size_t k = 0; k < kinds_.size(); ++k) {
                const Kind& kind = kinds_[k];
                weight[k] = fertility >= kind.fertilityMin ? kind.commonality * (kind.isTree ? forest : 1.0) : 0.0;
                total += weight[k];
            }
            if (total <= 0.0) {
                continue;
            }
            double pick = static_cast<double>(rng.next()) / 4294967296.0 * total;
            size_t chosen = kinds_.size();
            for (size_t k = 0; k < kinds_.size(); ++k) {
                if (weight[k] <= 0.0) {
                    continue;
                }
                chosen = k;
                if (pick < weight[k]) {
                    break;
                }
                pick -= weight[k];
            }
            if (chosen == kinds_.size()) {
                continue;
            }
            // A start state a few seasons into the plants' lives: a third fully grown (and older
            // than that), the rest anywhere on the way up.
            const Kind& kind = kinds_[chosen];
            const double growDaysHours = static_cast<double>(kPlantFullGrowth) / kind.growthPerHour;  // growDays x 24
            const double hoursToGrow = growDaysHours * 24.0 / 13.0;  // growing 13 hours a day
            uint32_t growth = 0;
            uint32_t age = 0;
            if (rng.below(3) == 0) {
                growth = kPlantFullGrowth;
                const double spare = std::max(0.0, static_cast<double>(kind.lifespanHours) * 0.7 - hoursToGrow);
                age = static_cast<uint32_t>(hoursToGrow + spare * static_cast<double>(rng.next()) / 4294967296.0);
            } else {
                growth = rng.below(kPlantFullGrowth);
                age = static_cast<uint32_t>(static_cast<double>(growth) / static_cast<double>(kPlantFullGrowth) * hoursToGrow);
            }
            set(i, static_cast<uint16_t>(chosen + 1), growth, age);
        }
    }
}

std::vector<PlantRecord> Plants::records() const {
    std::vector<PlantRecord> out;
    out.reserve(count_);
    for (size_t i = 0; i < kind_.size(); ++i) {
        if (kind_[i] != 0) {
            out.push_back(PlantRecord{static_cast<int>(i), kinds_[static_cast<size_t>(kind_[i]) - 1].def, growth_[i], age_[i]});
        }
    }
    return out;
}

bool Plants::restore(const std::vector<PlantRecord>& records) {
    std::fill(kind_.begin(), kind_.end(), uint16_t{0});
    std::fill(growth_.begin(), growth_.end(), 0u);
    std::fill(age_.begin(), age_.end(), 0u);
    count_ = 0;
    hash_ = 0;
    for (const PlantRecord& r : records) {
        const int k = kindOfDef(r.def);
        if (k < 0 || r.cell < 0 || static_cast<size_t>(r.cell) >= kind_.size() || kind_[static_cast<size_t>(r.cell)] != 0 ||
            r.growth > kPlantFullGrowth) {
            restore({});
            return false;
        }
        set(static_cast<size_t>(r.cell), static_cast<uint16_t>(k + 1), r.growth, r.ageHours);
    }
    return true;
}

}  // namespace colony
