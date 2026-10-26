#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Grid.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace colony {

class Map;
class StateHasher;

// Stockpile zones, as RimWorld's. See docs/design/jobs.md.

// How much a stockpile wants things, as RimWorld's StoragePriority. `Unstored` means "not in
// storage". Pawns haul an item only to a cell of a higher priority than the one it lies in.
enum class StoragePriority : uint8_t { Unstored, Low, Normal, Preferred, Important, Critical };

COLONY_API const char* storagePriorityName(StoragePriority priority);

using ZoneId = uint32_t;
inline constexpr ZoneId kNoZone = 0;

// A set of cells that store items. Each cell holds one stack (RimWorld's rule): items of one
// def, up to its `stackLimit`. The filter allows every item def except the ones listed.
struct StockpileZone {
    ZoneId id = kNoZone;
    StoragePriority priority = StoragePriority::Normal;
    std::vector<DefId> disallowed;  // sorted by value
    std::vector<CellPos> cells;     // in row order: by y, then x

    bool allows(DefId def) const;
};

// Every zone on the map and the zone of each cell. Zone ids count up from 1 in the order zones
// are made and are never used again, so commands and replays can name them.
class COLONY_API Zones {
public:
    explicit Zones(const Map& map);

    // The zone a cell is in; kNoZone off the map or outside every zone.
    ZoneId zoneAt(CellPos c) const;
    const StockpileZone* find(ZoneId id) const;
    // The stockpile a cell is in, or null.
    const StockpileZone* stockpileAt(CellPos c) const;
    // In id order.
    const std::vector<StockpileZone>& stockpiles() const { return stockpiles_; }
    // Goes up on every change, so a cache of zones can tell it is stale.
    uint64_t changeCount() const { return changeCount_; }

    // A new stockpile of the cells in `area` that can hold one: walkable ground and no zone yet.
    // Returns its id, or kNoZone (and changes nothing) if no cell qualifies.
    // Cells another kind of zone has taken (a growing zone) can't be stockpile cells. Not saved.
    void setCellTaken(std::function<bool(CellPos)> taken) { taken_ = std::move(taken); }
    ZoneId createStockpile(const CellRect& area, StoragePriority priority = StoragePriority::Normal);
    // Takes the cells in `area` out of their zones; a zone left with no cells is deleted.
    // Returns how many cells were taken out.
    int removeCells(const CellRect& area);
    bool setPriority(ZoneId id, StoragePriority priority);
    bool setAllowed(ZoneId id, DefId def, bool allowed);

    // Saves (docs/design/saves.md).
    ZoneId nextId() const { return nextId_; }
    // Puts saved zones back. Returns false, with `error`, if they don't add up.
    bool restore(std::vector<StockpileZone> stockpiles, ZoneId nextId, std::string& error);
    void hashState(StateHasher& h) const;

private:
    StockpileZone* findMutable(ZoneId id);
    size_t slot(CellPos c) const;

    const Map& map_;
    std::vector<StockpileZone> stockpiles_;
    std::vector<ZoneId> zoneOf_;  // per cell of level 0
    ZoneId nextId_ = 1;
    uint64_t changeCount_ = 0;
    std::function<bool(CellPos)> taken_;
};

}  // namespace colony
