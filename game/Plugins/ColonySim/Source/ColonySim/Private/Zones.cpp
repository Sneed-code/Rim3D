#include "ColonySim/Zones.h"

#include "ColonySim/Hash.h"
#include "ColonySim/Map.h"

#include <algorithm>
#include <utility>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.
bool zonesCellBefore(const CellPos& a, const CellPos& b) {
    return a.level != b.level ? a.level < b.level : a.y != b.y ? a.y < b.y : a.x < b.x;
}

bool zonesDefBefore(DefId a, DefId b) {
    return a.value < b.value;
}

}  // namespace

const char* storagePriorityName(StoragePriority priority) {
    switch (priority) {
        case StoragePriority::Unstored: return "Unstored";
        case StoragePriority::Low: return "Low";
        case StoragePriority::Normal: return "Normal";
        case StoragePriority::Preferred: return "Preferred";
        case StoragePriority::Important: return "Important";
        case StoragePriority::Critical: return "Critical";
    }
    return "?";
}

bool StockpileZone::allows(DefId def) const {
    return !std::binary_search(disallowed.begin(), disallowed.end(), def, zonesDefBefore);
}

Zones::Zones(const Map& map)
    : map_(map), zoneOf_(static_cast<size_t>(map.width()) * static_cast<size_t>(map.height()), kNoZone) {}

// Level after level; upper levels are added to zoneOf_ the first time a zone is made on them.
size_t Zones::slot(CellPos c) const {
    return static_cast<size_t>(c.level) * static_cast<size_t>(map_.width()) * static_cast<size_t>(map_.height()) +
           static_cast<size_t>(map_.index(c));
}

void Zones::sizeFor(int level) {
    const size_t cells = static_cast<size_t>(level + 1) * static_cast<size_t>(map_.width()) * static_cast<size_t>(map_.height());
    if (zoneOf_.size() < cells) {
        zoneOf_.resize(cells, kNoZone);
    }
}

ZoneId Zones::zoneAt(CellPos c) const {
    if (!map_.inBounds(c)) {
        return kNoZone;
    }
    const size_t i = slot(c);
    return i < zoneOf_.size() ? zoneOf_[i] : kNoZone;
}

const StockpileZone* Zones::find(ZoneId id) const {
    const auto it = std::lower_bound(stockpiles_.begin(), stockpiles_.end(), id,
                                     [](const StockpileZone& z, ZoneId value) { return z.id < value; });
    return it != stockpiles_.end() && it->id == id ? &*it : nullptr;
}

StockpileZone* Zones::findMutable(ZoneId id) {
    return const_cast<StockpileZone*>(std::as_const(*this).find(id));
}

const StockpileZone* Zones::stockpileAt(CellPos c) const {
    const ZoneId id = zoneAt(c);
    return id == kNoZone ? nullptr : find(id);
}

ZoneId Zones::createStockpile(const CellRect& area, StoragePriority priority) {
    StockpileZone zone;
    zone.priority = priority == StoragePriority::Unstored ? StoragePriority::Low : priority;
    if (area.level < 0 || area.level >= map_.levels()) {
        return kNoZone;
    }
    sizeFor(area.level);
    for (int y = area.minY; y < area.minY + area.sizeY; ++y) {
        for (int x = area.minX; x < area.minX + area.sizeX; ++x) {
            const CellPos c{x, y, area.level};
            if (map_.walkable(c) && zoneOf_[slot(c)] == kNoZone && !(taken_ && taken_(c))) {
                zone.cells.push_back(c);
            }
        }
    }
    if (zone.cells.empty()) {
        return kNoZone;
    }
    zone.id = nextId_++;
    for (const CellPos& c : zone.cells) {
        zoneOf_[slot(c)] = zone.id;
    }
    stockpiles_.push_back(std::move(zone));
    ++changeCount_;
    return stockpiles_.back().id;
}

int Zones::removeCells(const CellRect& area) {
    int removed = 0;
    for (StockpileZone& zone : stockpiles_) {
        size_t kept = 0;
        for (const CellPos& c : zone.cells) {
            if (area.contains(c)) {
                zoneOf_[slot(c)] = kNoZone;
                ++removed;
            } else {
                zone.cells[kept++] = c;
            }
        }
        zone.cells.resize(kept);
    }
    stockpiles_.erase(std::remove_if(stockpiles_.begin(), stockpiles_.end(),
                                     [](const StockpileZone& z) { return z.cells.empty(); }),
                      stockpiles_.end());
    if (removed > 0) {
        ++changeCount_;
    }
    return removed;
}

bool Zones::setPriority(ZoneId id, StoragePriority priority) {
    StockpileZone* zone = findMutable(id);
    if (zone == nullptr || priority == StoragePriority::Unstored) {
        return false;
    }
    zone->priority = priority;
    ++changeCount_;
    return true;
}

bool Zones::setAllowed(ZoneId id, DefId def, bool allowed) {
    StockpileZone* zone = findMutable(id);
    if (zone == nullptr || !def.valid()) {
        return false;
    }
    auto it = std::lower_bound(zone->disallowed.begin(), zone->disallowed.end(), def, zonesDefBefore);
    const bool listed = it != zone->disallowed.end() && *it == def;
    if (allowed && listed) {
        zone->disallowed.erase(it);
    } else if (!allowed && !listed) {
        zone->disallowed.insert(it, def);
    }
    ++changeCount_;
    return true;
}

bool Zones::restore(std::vector<StockpileZone> stockpiles, ZoneId nextId, std::string& error) {
    std::fill(zoneOf_.begin(), zoneOf_.end(), kNoZone);
    ZoneId last = kNoZone;
    for (StockpileZone& zone : stockpiles) {
        if (zone.id == kNoZone || zone.id <= last || zone.id >= nextId || zone.cells.empty() ||
            zone.priority == StoragePriority::Unstored) {
            error = "the save has a stockpile zone that doesn't add up";
            return false;
        }
        last = zone.id;
        std::sort(zone.disallowed.begin(), zone.disallowed.end(), zonesDefBefore);
        std::sort(zone.cells.begin(), zone.cells.end(), zonesCellBefore);
        for (const CellPos& c : zone.cells) {
            if (!map_.inBounds(c)) {
                error = "the save has a stockpile zone outside the map or on top of another";
                return false;
            }
            sizeFor(c.level);
            if (zoneOf_[slot(c)] != kNoZone) {
                error = "the save has a stockpile zone outside the map or on top of another";
                return false;
            }
            zoneOf_[slot(c)] = zone.id;
        }
    }
    stockpiles_ = std::move(stockpiles);
    nextId_ = nextId;
    ++changeCount_;
    return true;
}

void Zones::hashState(StateHasher& h) const {
    h.add(nextId_);
    h.add(static_cast<uint64_t>(stockpiles_.size()));
    for (const StockpileZone& zone : stockpiles_) {
        h.add(zone.id);
        h.add(static_cast<int>(zone.priority));
        h.add(static_cast<uint64_t>(zone.disallowed.size()));
        for (const DefId def : zone.disallowed) {
            h.add(def.value);
        }
        h.add(static_cast<uint64_t>(zone.cells.size()));
        for (const CellPos& c : zone.cells) {
            h.add(c.x);
            h.add(c.y);
            h.add(c.level);
        }
    }
}

}  // namespace colony
