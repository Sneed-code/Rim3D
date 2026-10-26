#include "ColonySim/Farming.h"

#include "ColonySim/Hash.h"
#include "ColonySim/Lights.h"
#include "ColonySim/Map.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Random.h"
#include "ColonySim/Temperature.h"
#include "ColonySim/Things.h"
#include "ColonySim/Zones.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.
bool farmingCellBefore(const CellPos& a, const CellPos& b) {
    return a.y != b.y ? a.y < b.y : a.x < b.x;
}

// RimWorld's growth rate by temperature: nothing below the minimum or above the maximum, rising
// linearly to the optimal range and falling away from it.
double farmingTemperatureFactor(const Def& crop, double t) {
    const double lo = crop.number("plant.growMinTemperature", 0.0);
    const double optLo = crop.number("plant.growOptimalMinTemperature", 6.0);
    const double optHi = crop.number("plant.growOptimalMaxTemperature", 42.0);
    const double hi = crop.number("plant.growMaxTemperature", 58.0);
    if (t <= lo || t >= hi) {
        return 0.0;
    }
    if (t < optLo) {
        return (t - lo) / std::max(1e-9, optLo - lo);
    }
    if (t > optHi) {
        return (hi - t) / std::max(1e-9, hi - optHi);
    }
    return 1.0;
}

}  // namespace

Farming::Farming(const Map& map, Plants& plants, const Zones& stockpiles, const DefDatabase& defs, std::vector<Event>& events)
    : map_(map),
      plants_(plants),
      stockpiles_(stockpiles),
      defs_(defs),
      events_(events),
      zoneOf_(static_cast<size_t>(map.width()) * static_cast<size_t>(map.height()), kNoGrowingZone) {
    if (const Def* rules = defs.find("Rules", "PlantRules")) {
        frostKillPerDegree_ = rules->number("killChancePerDegree", frostKillPerDegree_);
        minGlow_ = rules->number("growthNeedsLight", minGlow_);
    }
}

size_t Farming::slot(CellPos c) const {
    return static_cast<size_t>(map_.index(c));
}

bool Farming::isCrop(DefId def) const {
    if (!def.valid() || def.value >= defs_.size()) {
        return false;
    }
    const Def& d = defs_.get(def);
    return d.text("category") == "Plant" && d.has("plant.sowWork") && d.has("plant.growDays") && !d.flag("plant.isTree");
}

std::vector<DefId> Farming::crops() const {
    std::vector<DefId> out;
    for (const DefId id : defs_.idsOfKind("Things")) {
        if (isCrop(id)) {
            out.push_back(id);
        }
    }
    return out;
}

GrowingZoneId Farming::createZone(const CellRect& area, DefId crop, std::string& error) {
    if (!isCrop(crop)) {
        error = "that can't be sown";
        return kNoGrowingZone;
    }
    GrowingZone zone;
    for (int y = area.minY; y < area.minY + area.sizeY; ++y) {
        for (int x = area.minX; x < area.minX + area.sizeX; ++x) {
            const CellPos c{x, y, 0};
            if (area.level == 0 && map_.onGround(c) && map_.walkable(c) && (map_.buildingAt(c) == kNoEntity || spotFertility(slot(c)) > 0.0f) &&
                zoneOf_[slot(c)] == kNoGrowingZone && stockpiles_.zoneAt(c) == kNoZone && plants_.fertility(c) > 0.0f) {
                zone.cells.push_back(c);
            }
        }
    }
    if (zone.cells.empty()) {
        error = "no free soil in that area for a growing zone";
        return kNoGrowingZone;
    }
    zone.id = nextZone_++;
    zone.crop = crop;
    for (const CellPos& c : zone.cells) {
        zoneOf_[slot(c)] = zone.id;
    }
    zones_.push_back(std::move(zone));
    ++changeCount_;
    Event changed;
    changed.type = EventType::DesignationsChanged;
    events_.push_back(std::move(changed));
    return zones_.back().id;
}

int Farming::removeCells(const CellRect& area) {
    int removed = 0;
    for (GrowingZone& zone : zones_) {
        size_t kept = 0;
        for (const CellPos& c : zone.cells) {
            if (area.contains(c)) {
                zoneOf_[slot(c)] = kNoGrowingZone;
                ++removed;
            } else {
                zone.cells[kept++] = c;
            }
        }
        zone.cells.resize(kept);
    }
    zones_.erase(std::remove_if(zones_.begin(), zones_.end(), [](const GrowingZone& z) { return z.cells.empty(); }),
                       zones_.end());
    if (removed > 0) {
        ++changeCount_;
        Event changed;
        changed.type = EventType::DesignationsChanged;
        events_.push_back(std::move(changed));
    }
    return removed;
}

bool Farming::setCrop(GrowingZoneId id, DefId crop, std::string& error) {
    const auto it = std::find_if(zones_.begin(), zones_.end(), [&](const GrowingZone& z) { return z.id == id; });
    if (it == zones_.end()) {
        error = "no such growing zone";
        return false;
    }
    if (!isCrop(crop)) {
        error = "that can't be sown";
        return false;
    }
    it->crop = crop;
    ++changeCount_;
    return true;
}

GrowingZoneId Farming::zoneAt(CellPos c) const {
    return map_.onGround(c) ? zoneOf_[slot(c)] : kNoGrowingZone;
}

const GrowingZone* Farming::find(GrowingZoneId id) const {
    const auto it = std::lower_bound(zones_.begin(), zones_.end(), id,
                                     [](const GrowingZone& z, GrowingZoneId value) { return z.id < value; });
    return it != zones_.end() && it->id == id ? &*it : nullptr;
}

DefId Farming::cropAt(CellPos c) const {
    const GrowingZone* zone = find(zoneAt(c));
    return zone != nullptr ? zone->crop : DefId{};
}

float Farming::temperatureOf(CellPos c) const {
    return temperature_ != nullptr ? temperature_->temperatureAt(c) : 20.0f;
}

float Farming::spotFertility(size_t cellIndex) const {
    if (things_ == nullptr) {
        return -1.0f;
    }
    const Entity building = map_.buildingAt(map_.cellAt(static_cast<int>(cellIndex)));
    if (building == kNoEntity) {
        return -1.0f;
    }
    const Thing& thing = things_->registry().get<Thing>(building);
    return static_cast<float>(defs_.get(thing.def).number("plantSpot.fertility", -1.0));
}

bool Farming::spotUnpowered(CellPos c) const {
    if (things_ == nullptr) {
        return false;
    }
    const Entity building = map_.buildingAt(c);
    if (building == kNoEntity) {
        return false;
    }
    const Thing& thing = things_->registry().get<Thing>(building);
    return defs_.get(thing.def).flag("plantSpot.needsPower") && spotRunning_ && !spotRunning_(building);
}

bool Farming::sunLampLit(size_t cellIndex) const {
    if (lights_ == nullptr || things_ == nullptr) {
        return false;
    }
    const CellPos c = map_.cellAt(static_cast<int>(cellIndex));
    const double glowNeeded = minGlow_;
    if (static_cast<double>(lights_->lampLightAt(c)) < glowNeeded) {
        return false;
    }
    for (const LightSource& source : lights_->sources()) {
        if (source.cell.level != c.level) {
            continue;
        }
        const Def& def = defs_.get(things_->registry().get<Thing>(source.building).def);
        if (!def.flag("glower.growsPlants")) {
            continue;
        }
        const double radius = def.number("glower.plantLightRadius", 0.0);
        const double dx = static_cast<double>(source.cell.x - c.x);
        const double dy = static_cast<double>(source.cell.y - c.y);
        if (dx * dx + dy * dy <= radius * radius) {
            return true;
        }
    }
    return false;
}

std::string Farming::sowBlocker(CellPos c) const {
    const DefId crop = cropAt(c);
    if (!crop.valid()) {
        return "not in a growing zone";
    }
    const Def& def = defs_.get(crop);
    if (plants_.has(c)) {
        const DefId there = plants_.defAt(c);
        if (there == crop) {
            return "already sown";
        }
        if (defs_.get(there).flag("plant.isTree")) {
            return "a tree is in the way";
        }
    }
    if (plants_.fertility(c) < static_cast<float>(def.number("plant.fertilityMin", 0.0)) || plants_.fertility(c) <= 0.0f) {
        return "the soil is too poor";
    }
    if (spotUnpowered(c)) {
        return "its basin has no power";
    }
    if (farmingTemperatureFactor(def, temperatureOf(c)) <= 0.0) {
        return "the weather is wrong for it";
    }
    return std::string();
}

bool Farming::readyToHarvest(CellPos c) const {
    const DefId crop = cropAt(c);
    return crop.valid() && plants_.has(c) && plants_.defAt(c) == crop && plants_.mature(c);
}

double Farming::growthFactor(size_t cellIndex) const {
    const GrowingZoneId id = zoneOf_[cellIndex];
    if (id == kNoGrowingZone) {
        return 1.0;
    }
    const GrowingZone* zone = find(id);
    const CellPos c = map_.cellAt(static_cast<int>(cellIndex));
    if (zone == nullptr || !plants_.has(c) || plants_.defAt(c) != zone->crop) {
        return 1.0;
    }
    if (spotUnpowered(c)) {
        return 0.0;
    }
    const Def& def = defs_.get(zone->crop);
    const double glowNeeded = def.number("plant.growMinGlow", minGlow_);
    if (lights_ != nullptr && static_cast<double>(lights_->lightAt(c)) < glowNeeded) {
        return 0.0;
    }
    return farmingTemperatureFactor(def, static_cast<double>(temperatureOf(c)));
}

void Farming::growHour(int64_t now) {
    for (const GrowingZone& zone : zones_) {
        const Def& def = defs_.get(zone.crop);
        const double cold = def.number("plant.coldDamageBelow", 0.0);
        const double heat = def.number("plant.heatDamageAbove", 58.0);
        for (const CellPos& c : zone.cells) {
            if (!plants_.has(c) || plants_.defAt(c) != zone.crop) {
                continue;
            }
            const double t = static_cast<double>(temperatureOf(c));
            const double degrees = t < cold ? cold - t : (t > heat ? t - heat : 0.0);
            if (degrees <= 0.0) {
                continue;
            }
            // A hash of the cell and the hour stands in for a random draw, so the layer needs no stream of its own.
            const uint64_t draw = splitMix64(static_cast<uint64_t>(slot(c)) * 0x9E3779B97F4A7C15ULL ^ static_cast<uint64_t>(now));
            const double roll = static_cast<double>(draw >> 11) / static_cast<double>(1ULL << 53);
            if (roll >= std::min(1.0, degrees * frostKillPerDegree_)) {
                continue;
            }
            plants_.remove(c);
            if (t < cold) {
                ++stats_.frostKilled;
            } else {
                ++stats_.heatKilled;
            }
            Event event;
            event.type = EventType::PlantRemoved;
            event.cell = c;
            event.def = zone.crop;
            event.message = t < cold ? "frost" : "heat";
            events_.push_back(std::move(event));
        }
    }
}

bool Farming::restore(const FarmingSaved& saved, std::string& error) {
    std::fill(zoneOf_.begin(), zoneOf_.end(), kNoGrowingZone);
    std::vector<GrowingZone> next;
    GrowingZoneId last = kNoGrowingZone;
    for (const FarmingSaved::Zone& z : saved.zones) {
        const DefId crop = defs_.idOf("Things", z.crop);
        if (z.id == kNoGrowingZone || z.id <= last || z.id >= saved.nextZone || z.cells.empty() || !isCrop(crop)) {
            error = "the save has a growing zone that doesn't add up";
            std::fill(zoneOf_.begin(), zoneOf_.end(), kNoGrowingZone);
            zones_.clear();
            return false;
        }
        last = z.id;
        GrowingZone zone;
        zone.id = z.id;
        zone.crop = crop;
        zone.cells = z.cells;
        std::sort(zone.cells.begin(), zone.cells.end(), farmingCellBefore);
        for (const CellPos& c : zone.cells) {
            if (!map_.onGround(c) || zoneOf_[slot(c)] != kNoGrowingZone) {
                error = "the save has a growing zone cell that doesn't add up";
                std::fill(zoneOf_.begin(), zoneOf_.end(), kNoGrowingZone);
                zones_.clear();
                return false;
            }
            zoneOf_[slot(c)] = zone.id;
        }
        next.push_back(std::move(zone));
    }
    nextZone_ = saved.nextZone;
    stats_ = saved.stats;
    zones_ = std::move(next);
    ++changeCount_;
    return true;
}

FarmingSaved Farming::saved() const {
    FarmingSaved out;
    out.nextZone = nextZone_;
    out.stats = stats_;
    for (const GrowingZone& zone : zones_) {
        out.zones.push_back(FarmingSaved::Zone{zone.id, defs_.get(zone.crop).defName(), zone.cells});
    }
    return out;
}

void Farming::hashState(StateHasher& h) const {
    h.add(nextZone_);
    h.add(static_cast<uint64_t>(zones_.size()));
    for (const GrowingZone& zone : zones_) {
        h.add(zone.id);
        h.add(zone.crop.value);
        h.add(static_cast<uint64_t>(zone.cells.size()));
        for (const CellPos& c : zone.cells) {
            h.add(c.x);
            h.add(c.y);
        }
    }
    h.add(stats_.sown);
    h.add(stats_.harvested);
    h.add(stats_.frostKilled);
    h.add(stats_.heatKilled);
}

}  // namespace colony
