#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Grid.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace colony {

class Lights;
class Things;
class Map;
class Plants;
class StateHasher;
class TemperatureSystem;
class Zones;

// Growing zones and farming (task C13, docs/design/farming.md): a growing zone is a set of cells with
// one chosen crop. Colonists sow the crop on empty cells and harvest it when it is fully grown (the
// Growing work type); sown crops grow with the cell's light and temperature, and frost or heat kills them.

using GrowingZoneId = uint32_t;
inline constexpr GrowingZoneId kNoGrowingZone = 0;

struct GrowingZone {
    GrowingZoneId id = kNoGrowingZone;
    DefId crop;                  // the Things def of the plant to sow
    std::vector<CellPos> cells;  // in row order: by y, then x
};

struct FarmingStats {
    uint64_t sown = 0;
    uint64_t harvested = 0;
    uint64_t frostKilled = 0;  // crops killed by cold
    uint64_t heatKilled = 0;   // crops killed by heat
};

// The zones as saved: the crop by `defName`.
struct FarmingSaved {
    struct Zone {
        GrowingZoneId id = kNoGrowingZone;
        std::string crop;
        std::vector<CellPos> cells;
    };
    GrowingZoneId nextZone = 1;
    std::vector<Zone> zones;
    FarmingStats stats;
};

class COLONY_API Farming {
public:
    Farming(const Map& map, Plants& plants, const Zones& stockpiles, const DefDatabase& defs, std::vector<Event>& events);

    // The climate the crops grow in; without them every cell is mild and bright.
    void setClimate(TemperatureSystem* temperature, const Lights* lights) {
        temperature_ = temperature;
        lights_ = lights;
    }

    // What colonists know of the weather ahead: the outdoor temperature at a game second, and the game second
    // now. With them a crop is sown only where it can ripen before the frost (`sowBlocker`).
    void setForecast(std::function<float(int64_t)> outdoorAt, std::function<int64_t()> now) {
        forecast_ = std::move(outdoorAt);
        clock_ = std::move(now);
    }
    // The buildings that can carry a plant (plant pot, hydroponics basin) and whether one is running (powered).
    // A building with `plantSpot` gives its cell that fertility; with `plantSpot.needsPower` the crop on it
    // does not grow, and cannot be sown, while it has no power. Without this no building carries plants.
    void setPlantSpots(const Things* things, std::function<bool(Entity)> running) {
        things_ = things;
        spotRunning_ = std::move(running);
    }
    // The fertility the building on a cell gives (a negative number: none), by the cell's index on level 0.
    float spotFertility(size_t cellIndex) const;
    // Crops in this cell grow through the night: a sun lamp that lights it is within its `plantLightRadius`.
    bool sunLampLit(size_t cellIndex) const;

    // True for a Things def that can be sown: a plant with `plant.sowWork` that is not a tree.
    bool isCrop(DefId def) const;
    // The crops the player can pick, in def order.
    std::vector<DefId> crops() const;

    // A zone of the cells in `area` that are walkable, have fertility above 0 and lie in no zone and no
    // stockpile. Returns its id, or kNoGrowingZone with `error` set.
    GrowingZoneId createZone(const CellRect& area, DefId crop, std::string& error);
    // Takes the cells in `area` out of their zones; a zone left empty is deleted. Returns how many.
    int removeCells(const CellRect& area);
    bool setCrop(GrowingZoneId id, DefId crop, std::string& error);

    GrowingZoneId zoneAt(CellPos c) const;
    const GrowingZone* find(GrowingZoneId id) const;
    // The crop a cell is to grow, or an invalid DefId outside the zones.
    DefId cropAt(CellPos c) const;
    const std::vector<GrowingZone>& zones() const { return zones_; }
    uint64_t changeCount() const { return changeCount_; }
    const FarmingStats& stats() const { return stats_; }

    // Why a crop can't be sown in a cell now (empty if it can): ground too poor, too cold or hot,
    // a tree in the way, or (like RimWorld's growth season check for sowing) frost that would come before the
    // crop could ripen in the open. The plant already there, if it is not a tree, is replaced.
    std::string sowBlocker(CellPos c) const;
    // The plant in a zone cell is fully grown and can be harvested.
    bool readyToHarvest(CellPos c) const;

    // How fast the crop in this cell grows now, 0 to 1: the temperature curve of its def, and 0 in
    // too little light. 1 outside the growing zones.
    double growthFactor(size_t cellIndex) const;
    // One game hour has passed: crops in zones too cold or too hot may die, by the plant def's
    // `coldDamageBelow` and `heatDamageAbove` and the rule's chance per degree.
    void growHour(int64_t now);

    void noteSown() { ++stats_.sown; }
    void noteHarvested() { ++stats_.harvested; }

    // Saves.
    FarmingSaved saved() const;
    bool restore(const FarmingSaved& saved, std::string& error);
    void hashState(StateHasher& h) const;

private:
    float temperatureOf(CellPos c) const;
    // Whether a crop sown in this open-air cell now would ripen before the temperature falls under its
    // `coldDamageBelow`, by the forecast; true where there is no forecast or the cell is under a roof.
    bool ripensBeforeFrost(CellPos c, DefId cropId) const;
    bool spotUnpowered(CellPos c) const;
    size_t slot(CellPos c) const;

    const Map& map_;
    Plants& plants_;
    const Zones& stockpiles_;
    const DefDatabase& defs_;
    std::vector<Event>& events_;
    TemperatureSystem* temperature_ = nullptr;
    const Lights* lights_ = nullptr;
    const Things* things_ = nullptr;
    std::function<bool(Entity)> spotRunning_;
    GrowingZoneId nextZone_ = 1;
    std::vector<GrowingZone> zones_;
    FarmingStats stats_;
    std::vector<GrowingZoneId> zoneOf_;  // per cell of level 0
    uint64_t changeCount_ = 0;
    double frostKillPerDegree_ = 0.1;
    double minGlow_ = 0.51;
    double sowGrowthMargin_ = 0.7;   // PlantRules: sowing takes crops to grow at this share of the forecast rate
    int64_t sowSlackHours_ = 36;     // PlantRules: ... and to need this many frost-free hours after ripening, to be picked
    std::function<float(int64_t)> forecast_;
    std::function<int64_t()> clock_;
    // Answers of ripensBeforeFrost for this game hour, by crop and growth rate; derived, so neither saved nor hashed.
    mutable int64_t ripenHour_ = -1;
    mutable std::map<std::pair<uint32_t, int>, bool> ripenCache_;
};

}  // namespace colony
