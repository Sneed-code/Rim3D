#include "ColonySim/Filth.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Floors.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Health.h"
#include "ColonySim/Map.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

const std::vector<FilthPile> kFilthNone;

}  // namespace

FilthNumbers FilthNumbers::read(const DefDatabase& defs) {
    FilthNumbers n;
    const Def* def = defs.find("Rules", "FilthRules");
    if (def == nullptr) {
        return n;
    }
    n.outdoorBeautyFactor = static_cast<float>(def->number("outdoorBeautyFactor", n.outdoorBeautyFactor));
    n.ticksToSeconds = static_cast<float>(def->number("ticksToSeconds", n.ticksToSeconds));
    n.maxCarried = std::clamp(static_cast<int>(def->integer("maxCarried", n.maxCarried)), 0, 255);
    n.pickupChancePerCell = static_cast<float>(def->number("pickupChancePerCell", n.pickupChancePerCell));
    n.dropChancePerCell = static_cast<float>(def->number("dropChancePerCell", n.dropChancePerCell));
    n.bloodPerBleedDay = static_cast<float>(def->number("bloodPerBleedDay", n.bloodPerBleedDay));
    n.cleaningSpeed = static_cast<float>(def->number("cleaningSpeed", n.cleaningSpeed));
    n.rainWashFactor = static_cast<float>(def->number("rainWashFactor", n.rainWashFactor));
    return n;
}

Filth::Filth(Map& map, Things& things, const Floors& floors, const DefDatabase& defs, uint64_t seed)
    : map_(map),
      things_(things),
      floors_(floors),
      defs_(defs),
      numbers_(FilthNumbers::read(defs)),
      rng_(splitMix64(seed ^ 0x66696C7468ULL)) {
    for (const DefId id : defs.idsOfKind("Filth")) {
        const Def& def = defs.get(id);
        FilthKind kind;
        kind.def = id;
        kind.name = def.defName();
        kind.beauty = static_cast<float>(def.number("stats.beauty", 0.0));
        kind.cleanliness = static_cast<float>(def.number("stats.cleanliness", 0.0));
        kind.trackable = def.flag("trackable", false);
        kind.cleanSeconds = static_cast<float>(def.number("cleanTicks", 35.0)) * numbers_.ticksToSeconds;
        kind.maxThickness = std::clamp(static_cast<int>(def.integer("maxThickness", 3)), 1, 255);
        if (def.count("filth.disappearsInDays") >= 2) {
            kind.fadeHours = static_cast<float>((def.numberAt("filth.disappearsInDays", 0, 0.0) + def.numberAt("filth.disappearsInDays", 1, 0.0)) * 0.5 *
                                                24.0);
        }
        kind.rainWashes = def.flag("filth.rainWashes", false);
        kinds_.push_back(std::move(kind));
    }
    dirtKind_ = kindIndex("Dirt");
    filthRate_.assign(defs.size(), 0.0f);
    for (DefId id : defs.idsOfKind("Things")) {
        filthRate_[id.value] = static_cast<float>(defs.get(id).number("stats.filthRate", 0.0));
    }
}

int Filth::kindIndex(const std::string& name) const {
    for (size_t i = 0; i < kinds_.size(); ++i) {
        if (kinds_[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool Filth::bareGround(CellPos c) const {
    if (floors_.at(c).valid()) {
        return false;
    }
    const Terrain t = map_.terrain(c);
    return t == Terrain::Soil || t == Terrain::Sand || t == Terrain::RoughStone;
}

bool Filth::add(CellPos c, int kind, int layers) {
    if (kind < 0 || static_cast<size_t>(kind) >= kinds_.size() || layers <= 0 || !map_.onGround(c) ||
        !map_.groundWalkable(c) || map_.passage(c) == Passage::Blocked) {
        return false;
    }
    std::vector<FilthPile>& cell = piles_[map_.index(c)];
    const int limit = kinds_[static_cast<size_t>(kind)].maxThickness;
    auto it = std::find_if(cell.begin(), cell.end(), [&](const FilthPile& p) { return p.kind == kind; });
    if (it == cell.end()) {
        FilthPile pile;
        pile.kind = static_cast<uint16_t>(kind);
        pile.thickness = static_cast<uint8_t>(std::min(layers, limit));
        cell.insert(std::find_if(cell.begin(), cell.end(), [&](const FilthPile& p) { return p.kind > kind; }), pile);
        ++changes_;
        return true;
    }
    if (it->thickness >= limit) {
        return false;
    }
    it->thickness = static_cast<uint8_t>(std::min(limit, it->thickness + layers));
    ++changes_;
    return true;
}

bool Filth::clean(CellPos c) {
    if (!map_.onGround(c)) {
        return false;
    }
    const auto it = piles_.find(map_.index(c));
    if (it == piles_.end() || it->second.empty()) {
        return false;
    }
    FilthPile& pile = it->second.front();
    if (pile.thickness > 1) {
        --pile.thickness;
    } else {
        it->second.erase(it->second.begin());
        if (it->second.empty()) {
            piles_.erase(it);
        }
    }
    ++changes_;
    return true;
}

void Filth::clear(CellPos c) {
    if (map_.onGround(c) && piles_.erase(map_.index(c)) > 0) {
        ++changes_;
    }
}

bool Filth::has(CellPos c) const {
    return map_.onGround(c) && piles_.count(map_.index(c)) > 0;
}

const std::vector<FilthPile>& Filth::at(CellPos c) const {
    if (!map_.onGround(c)) {
        return kFilthNone;
    }
    const auto it = piles_.find(map_.index(c));
    return it == piles_.end() ? kFilthNone : it->second;
}

float Filth::cleanSeconds(CellPos c) const {
    const std::vector<FilthPile>& cell = at(c);
    return cell.empty() ? 0.0f : kinds_[cell.front().kind].cleanSeconds;
}

float Filth::beautyAt(CellPos c, bool outdoors) const {
    float total = 0.0f;
    for (const FilthPile& pile : at(c)) {
        total += kinds_[pile.kind].beauty;
    }
    return outdoors ? total * numbers_.outdoorBeautyFactor : total;
}

float Filth::cleanlinessAt(CellPos c) const {
    float total = 0.0f;
    for (const FilthPile& pile : at(c)) {
        total += kinds_[pile.kind].cleanliness;
    }
    return total;
}

size_t Filth::totalLayers() const {
    size_t total = 0;
    for (const auto& [index, cell] : piles_) {
        for (const FilthPile& pile : cell) {
            total += pile.thickness;
        }
    }
    return total;
}

void Filth::step() {
    if (kinds_.empty()) {
        return;
    }
    entt::registry& reg = things_.registry();
    // Pawns in id order: the registry's own order depends on its history.
    std::vector<Entity> pawns;
    for (const auto [entity, thing, pos] : reg.view<const Thing, const Position>().each()) {
        (void)pos;
        if (thing.category == ThingCategory::Pawn && filthRate_[thing.def.value] > 0.0f) {
            pawns.push_back(entity);
        }
    }
    std::sort(pawns.begin(), pawns.end());
    const float bloodPerSecond = numbers_.bloodPerBleedDay / static_cast<float>(kSecondsPerHour);
    for (const Entity pawn : pawns) {
        const CellPos cell = cellOf(reg.get<Position>(pawn));
        if (!map_.onGround(cell)) {
            continue;
        }
        const float rate = filthRate_[reg.get<Thing>(pawn).def.value];
        FilthCarrier& carrier = carriers_[entt::to_integral(pawn)];
        const int index = map_.index(cell);
        if (carrier.lastCell != index) {
            carrier.lastCell = index;
            // Entered a new cell: dirt sticks to shoes on bare ground and falls off on a floor;
            // filth lying in the cell sticks as well.
            if (bareGround(cell)) {
                if (dirtKind_ >= 0 && static_cast<size_t>(dirtKind_) < kFilthTrackedKinds &&
                    carrier.carried[static_cast<size_t>(dirtKind_)] < numbers_.maxCarried &&
                    rng_.unit() < numbers_.pickupChancePerCell * rate) {
                    ++carrier.carried[static_cast<size_t>(dirtKind_)];
                }
            } else {
                for (size_t k = 0; k < kFilthTrackedKinds && k < kinds_.size(); ++k) {
                    if (carrier.carried[k] > 0 && rng_.unit() < numbers_.dropChancePerCell) {
                        if (add(cell, static_cast<int>(k), 1)) {
                            --carrier.carried[k];
                        }
                        break;
                    }
                }
            }
            for (const FilthPile& pile : at(cell)) {
                if (pile.kind < kFilthTrackedKinds && kinds_[pile.kind].trackable &&
                    carrier.carried[pile.kind] < numbers_.maxCarried && rng_.unit() < numbers_.pickupChancePerCell * rate) {
                    ++carrier.carried[pile.kind];
                }
            }
        }
        const Health* health = reg.try_get<Health>(pawn);
        if (health != nullptr && !health->dead && health->bleedPerDay > 0.0f) {
            const int blood = kindIndex("Blood");
            if (blood >= 0 && rng_.unit() < bloodPerSecond * health->bleedPerDay) {
                add(cell, blood, 1);
            }
        }
    }
    // Forget pawns that are gone.
    for (auto it = carriers_.begin(); it != carriers_.end();) {
        const Entity e = static_cast<Entity>(it->first);
        it = reg.valid(e) ? std::next(it) : carriers_.erase(it);
    }
}

void Filth::fadeHour(float rainRate) {
    const bool raining = rainRate > 0.001f;
    for (auto it = piles_.begin(); it != piles_.end();) {
        const CellPos cell = map_.cellAt(it->first);
        std::vector<FilthPile>& cellPiles = it->second;
        if (map_.roof(cell) == Roof::None) {
            for (size_t i = 0; i < cellPiles.size();) {
                FilthPile& pile = cellPiles[i];
                const FilthKind& kind = kinds_[pile.kind];
                if (kind.fadeHours <= 0.0f) {
                    ++i;
                    continue;
                }
                const float perLayer = (raining && kind.rainWashes ? numbers_.rainWashFactor : 1.0f) / kind.fadeHours;
                if (rng_.unit() < perLayer * static_cast<float>(pile.thickness)) {
                    ++changes_;
                    if (pile.thickness > 1) {
                        --pile.thickness;
                    } else {
                        cellPiles.erase(cellPiles.begin() + static_cast<std::ptrdiff_t>(i));
                        continue;
                    }
                }
                ++i;
            }
        }
        it = cellPiles.empty() ? piles_.erase(it) : std::next(it);
    }
}

void Filth::hashState(StateHasher& h) const {
    h.add(static_cast<uint64_t>(piles_.size()));
    for (const auto& [index, cell] : piles_) {
        h.add(index);
        for (const FilthPile& pile : cell) {
            h.add(static_cast<int>(pile.kind));
            h.add(static_cast<int>(pile.thickness));
        }
    }
    h.add(static_cast<uint64_t>(carriers_.size()));
    for (const auto& [id, carrier] : carriers_) {
        h.add(id);
        h.add(carrier.lastCell);
        for (const uint8_t n : carrier.carried) {
            h.add(static_cast<int>(n));
        }
    }
    h.add(rng_.state());
    h.add(rng_.increment());
}

FilthState Filth::state() const {
    FilthState out;
    for (const auto& [index, cell] : piles_) {
        for (const FilthPile& pile : cell) {
            out.piles.push_back(FilthState::Entry{index, kinds_[pile.kind].name, pile.thickness});
        }
    }
    for (const auto& [id, carrier] : carriers_) {
        FilthState::Carrier c;
        c.pawn = id;
        c.lastCell = carrier.lastCell;
        for (size_t k = 0; k < kFilthTrackedKinds && k < kinds_.size(); ++k) {
            if (carrier.carried[k] > 0) {
                c.carried.emplace_back(kinds_[k].name, carrier.carried[k]);
            }
        }
        out.carriers.push_back(std::move(c));
    }
    out.rngState = rng_.state();
    out.rngIncrement = rng_.increment();
    return out;
}

bool Filth::restore(const FilthState& state, std::string& error) {
    piles_.clear();
    carriers_.clear();
    ++changes_;
    for (const FilthState::Entry& e : state.piles) {
        const int kind = kindIndex(e.kind);
        if (kind < 0 || e.cell < 0 || e.cell >= map_.width() * map_.height() || e.thickness < 1 ||
            e.thickness > kinds_[static_cast<size_t>(kind)].maxThickness) {
            error = "the save file has filth that does not fit the data";
            piles_.clear();
            return false;
        }
        std::vector<FilthPile>& cell = piles_[e.cell];
        FilthPile pile;
        pile.kind = static_cast<uint16_t>(kind);
        pile.thickness = static_cast<uint8_t>(e.thickness);
        cell.insert(std::find_if(cell.begin(), cell.end(), [&](const FilthPile& p) { return p.kind > kind; }), pile);
    }
    for (const FilthState::Carrier& c : state.carriers) {
        if (!things_.registry().valid(static_cast<Entity>(c.pawn))) {
            error = "the save file has a pawn carrying filth that is not there";
            carriers_.clear();
            return false;
        }
        FilthCarrier carrier;
        carrier.lastCell = c.lastCell;
        for (const auto& [name, count] : c.carried) {
            const int kind = kindIndex(name);
            if (kind < 0 || static_cast<size_t>(kind) >= kFilthTrackedKinds || count < 0 || count > 255) {
                error = "the save file has a pawn carrying filth that does not fit the data";
                carriers_.clear();
                return false;
            }
            carrier.carried[static_cast<size_t>(kind)] = static_cast<uint8_t>(count);
        }
        carriers_[c.pawn] = carrier;
    }
    rng_.restore(state.rngState, state.rngIncrement);
    return true;
}

}  // namespace colony
