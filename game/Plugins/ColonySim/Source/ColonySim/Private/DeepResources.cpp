#include "ColonySim/DeepResources.h"

#include "ColonySim/Hash.h"
#include "ColonySim/Map.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

constexpr int kDeepFindTries = 200;
const int kDeepDirX[4] = {1, -1, 0, 0};
const int kDeepDirY[4] = {0, 0, 1, -1};

}  // namespace

DeepResources::DeepResources(Map& map, const DefDatabase& defs, uint64_t seed) : map_(map), rng_(seed, 0x4445u) {
    for (const DefId id : defs.idsOfKind("Things")) {
        const Def& def = defs.get(id);
        if (!def.has("deep")) {
            continue;
        }
        Kind kind;
        kind.def = id;
        kind.lumpMin = std::max(1, static_cast<int>(def.numberAt("deep.lumpSize", 0, 1.0)));
        kind.lumpMax = std::max(kind.lumpMin, static_cast<int>(def.numberAt("deep.lumpSize", 1, kind.lumpMin)));
        kind.commonality = std::max(0.0, def.number("deep.commonality", 1.0));
        kind.portion = std::max(1, static_cast<int>(def.integer("deep.portion", 10)));
        info_.push_back(kind);
        kinds_.push_back(id);
    }
    if (const Def* rules = defs.find("Rules", "DeepRules")) {
        unitsPerCell_ = std::max(1.0, rules->number("unitsPerCell", unitsPerCell_));
        drillCycleSeconds_ = std::max(1.0, rules->number("drillCycleSeconds", drillCycleSeconds_));
        scanAverageSeconds_ = std::max(1.0, rules->number("scanAverageSeconds", scanAverageSeconds_));
        scanGuaranteedSeconds_ = std::max(1.0, rules->number("scanGuaranteedSeconds", scanGuaranteedSeconds_));
        scanSpeedBase_ = rules->number("scanSpeedBase", scanSpeedBase_);
        scanSpeedPerLevel_ = rules->number("scanSpeedPerLevel", scanSpeedPerLevel_);
        defaultSkillLevel_ = static_cast<int>(rules->integer("defaultSkillLevel", defaultSkillLevel_));
        scanXpPerSecond_ = rules->number("scanXpPerSecond", scanXpPerSecond_);
        drillXpPerSecond_ = rules->number("drillXpPerSecond", drillXpPerSecond_);
    }
}

const DeepResources::Kind* DeepResources::kindOf(DefId def) const {
    for (const Kind& k : info_) {
        if (k.def == def) {
            return &k;
        }
    }
    return nullptr;
}

int DeepResources::portionOf(DefId def) const {
    const Kind* k = kindOf(def);
    return k != nullptr ? k->portion : 0;
}

bool DeepResources::has(CellPos c) const {
    return c.level == 0 && map_.inBounds(c) && index_.count(map_.index(c)) != 0;
}

DefId DeepResources::defAt(CellPos c) const {
    if (c.level != 0 || !map_.inBounds(c)) {
        return DefId{};
    }
    const auto it = index_.find(map_.index(c));
    return it != index_.end() ? cells_[it->second].def : DefId{};
}

int DeepResources::unitsAt(CellPos c) const {
    if (c.level != 0 || !map_.inBounds(c)) {
        return 0;
    }
    const auto it = index_.find(map_.index(c));
    return it != index_.end() ? cells_[it->second].units : 0;
}

int DeepResources::totalUnits() const {
    int total = 0;
    for (const DeepCell& c : cells_) {
        total += c.units;
    }
    return total;
}

bool DeepResources::buildable(CellPos c) const {
    if (!map_.inBounds(c)) {
        return false;
    }
    const Terrain t = map_.terrain(c);
    return t != Terrain::Water && t != Terrain::Air;
}

bool DeepResources::placeDeposit(DefId def, CellPos center, int cells, DeepFind* out) {
    const Kind* kind = kindOf(def);
    center.level = 0;
    if (kind == nullptr || cells <= 0 || !buildable(center)) {
        return false;
    }
    std::vector<CellPos> frontier{center};
    std::vector<int> seen{map_.index(center)};
    int placed = 0;
    const uint16_t units = static_cast<uint16_t>(std::min(65535.0, unitsPerCell_));
    // Grows from the centre over a random neighbour at a time; cells that are not ground are skipped
    // and the lump goes round them. Cells that already hold a deposit are skipped the same way.
    while (placed < cells && !frontier.empty()) {
        const size_t pick = rng_.below(static_cast<uint32_t>(frontier.size()));
        const CellPos c = frontier[pick];
        frontier[pick] = frontier.back();
        frontier.pop_back();
        const int index = map_.index(c);
        if (index_.count(index) == 0) {
            index_[index] = cells_.size();
            cells_.push_back(DeepCell{index, def, units});
            ++placed;
        }
        for (int d = 0; d < 4; ++d) {
            const CellPos n{c.x + kDeepDirX[d], c.y + kDeepDirY[d], 0};
            if (!buildable(n)) {
                continue;
            }
            const int ni = map_.index(n);
            if (std::find(seen.begin(), seen.end(), ni) == seen.end()) {
                seen.push_back(ni);
                frontier.push_back(n);
            }
        }
    }
    if (placed == 0) {
        return false;
    }
    ++changes_;
    ++found_;
    if (out != nullptr) {
        out->def = def;
        out->center = center;
        out->cells = placed;
    }
    return true;
}

bool DeepResources::findDeposit(DefId def, DeepFind& out) {
    if (info_.empty()) {
        return false;
    }
    const Kind* kind = def.valid() ? kindOf(def) : nullptr;
    if (def.valid() && kind == nullptr) {
        return false;
    }
    if (kind == nullptr) {
        double total = 0.0;
        for (const Kind& k : info_) {
            total += k.commonality;
        }
        double roll = static_cast<double>(rng_.unit()) * total;
        kind = &info_.back();
        for (const Kind& k : info_) {
            if (roll < k.commonality) {
                kind = &k;
                break;
            }
            roll -= k.commonality;
        }
    }
    const int cells = kind->lumpMin + static_cast<int>(rng_.below(static_cast<uint32_t>(kind->lumpMax - kind->lumpMin + 1)));
    for (int i = 0; i < kDeepFindTries; ++i) {
        const CellPos c{static_cast<int>(rng_.below(static_cast<uint32_t>(map_.width()))),
                        static_cast<int>(rng_.below(static_cast<uint32_t>(map_.height()))), 0};
        if (buildable(c) && placeDeposit(kind->def, c, cells, &out)) {
            return true;
        }
    }
    return false;
}

DeepWork* DeepResources::workEntry(CellPos building, bool create) {
    for (DeepWork& w : work_) {
        if (w.at == building) {
            return &w;
        }
    }
    if (!create) {
        return nullptr;
    }
    work_.push_back(DeepWork{building, 0.0});
    return &work_.back();
}

double DeepResources::workAt(CellPos building) const {
    for (const DeepWork& w : work_) {
        if (w.at == building) {
            return w.work;
        }
    }
    return 0.0;
}

void DeepResources::forgetWork(CellPos building) {
    work_.erase(std::remove_if(work_.begin(), work_.end(), [&](const DeepWork& w) { return w.at == building; }), work_.end());
}

bool DeepResources::scanStep(CellPos building, double speed, DeepFind& out) {
    if (!(speed > 0.0)) {
        return false;
    }
    DeepWork* w = workEntry(building, true);
    w->work += speed;
    const bool guaranteed = w->work >= scanGuaranteedSeconds_;
    if (!guaranteed && !(static_cast<double>(rng_.unit()) < speed / scanAverageSeconds_)) {
        return false;
    }
    forgetWork(building);
    // A find that cannot be placed (a map with no room) still resets the scanner.
    return findDeposit(DefId{}, out);
}

bool DeepResources::hasResourceNear(CellPos building, double radius) const {
    const int r = static_cast<int>(std::ceil(radius));
    for (int dy = -r; dy <= r; ++dy) {
        for (int dx = -r; dx <= r; ++dx) {
            if (static_cast<double>(dx * dx + dy * dy) <= radius * radius &&
                has(CellPos{building.x + dx, building.y + dy, 0})) {
                return true;
            }
        }
    }
    return false;
}

bool DeepResources::drillStep(CellPos building, double amount) {
    if (!(amount > 0.0)) {
        return false;
    }
    DeepWork* w = workEntry(building, true);
    w->work += amount;
    if (w->work < drillCycleSeconds_) {
        return false;
    }
    w->work -= drillCycleSeconds_;
    return true;
}

DeepPortion DeepResources::takePortion(CellPos building, double radius) {
    DeepPortion portion;
    portion.from = building;
    const int r = static_cast<int>(std::ceil(radius));
    int bestDist = 0;
    size_t best = cells_.size();
    for (int dy = -r; dy <= r; ++dy) {
        for (int dx = -r; dx <= r; ++dx) {
            const int dist = dx * dx + dy * dy;
            if (static_cast<double>(dist) > radius * radius) {
                continue;
            }
            const CellPos c{building.x + dx, building.y + dy, 0};
            if (!has(c)) {
                continue;
            }
            if (best == cells_.size() || dist < bestDist) {  // ties: the first in row order
                best = index_.at(map_.index(c));
                bestDist = dist;
            }
        }
    }
    ++portions_;
    if (best == cells_.size()) {
        portion.chunk = true;
        portion.count = 1;
        return portion;
    }
    DeepCell& cell = cells_[best];
    const int take = std::min<int>(cell.units, portionOf(cell.def));
    portion.def = cell.def;
    portion.count = take;
    portion.from = map_.cellAt(cell.cell);
    cell.units = static_cast<uint16_t>(cell.units - take);
    if (cell.units == 0) {
        // Swap-remove, keeping the index right.
        index_.erase(cell.cell);
        if (best + 1 != cells_.size()) {
            cells_[best] = cells_.back();
            index_[cells_[best].cell] = best;
        }
        cells_.pop_back();
    }
    ++changes_;
    return portion;
}

void DeepResources::hashState(StateHasher& hasher) const {
    uint64_t sum = 0;
    for (const DeepCell& c : cells_) {  // order does not matter, removal swaps cells about
        sum += splitMix64(static_cast<uint64_t>(static_cast<uint64_t>(c.cell) * 1000003ULL + c.def.value * 131ULL + c.units));
    }
    hasher.add(sum);
    hasher.add(static_cast<uint64_t>(cells_.size()));
    hasher.add(rng_.state());
    for (const DeepWork& w : work_) {
        hasher.add(static_cast<uint64_t>(static_cast<uint64_t>(map_.index(CellPos{w.at.x, w.at.y, 0})) + static_cast<uint64_t>(w.at.level) * 7919ULL));
        hasher.add(static_cast<float>(w.work));
    }
}

DeepResourcesSaved DeepResources::saved() const {
    DeepResourcesSaved s;
    s.cells = cells_;
    std::sort(s.cells.begin(), s.cells.end(), [](const DeepCell& a, const DeepCell& b) { return a.cell < b.cell; });
    s.work = work_;
    s.rngState = rng_.state();
    s.rngIncrement = rng_.increment();
    s.found = found_;
    s.portions = portions_;
    return s;
}

bool DeepResources::restore(const DeepResourcesSaved& state) {
    std::vector<DeepCell> cells;
    std::unordered_map<int, size_t> index;
    const int total = map_.width() * map_.height();
    for (const DeepCell& c : state.cells) {
        if (c.cell < 0 || c.cell >= total || kindOf(c.def) == nullptr || index.count(c.cell) != 0) {
            return false;
        }
        index[c.cell] = cells.size();
        cells.push_back(c);
    }
    cells_ = std::move(cells);
    index_ = std::move(index);
    work_ = state.work;
    rng_.restore(state.rngState, state.rngIncrement);
    found_ = state.found;
    portions_ = state.portions;
    ++changes_;
    return true;
}

}  // namespace colony
