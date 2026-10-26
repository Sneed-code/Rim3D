#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Grid.h"
#include "ColonySim/Random.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace colony {

class Floors;
class Map;
class StateHasher;
class Things;

// Filth: dirt tracked in from outside, blood, ash and vomit lying on the ground (task C17,
// docs/design/room-stats.md). It is a layer of piles in cells, not things: a cell holds at most one
// pile of each kind, each 1 to `maxThickness` layers thick. Piles lower the beauty and cleanliness of
// their cell, and the Cleaning work type's CleanFilth job removes them one layer at a time.
//
// Kinds are the Filth defs (data/defs/Filth); the numbers are in FilthRules. Without Filth defs the
// layer is inert.

struct FilthKind {
    DefId def;
    std::string name;
    float beauty = 0.0f;       // indoors; outdoors it counts FilthRules.outdoorBeautyFactor of it
    float cleanliness = 0.0f;  // added to the cell's own
    bool trackable = false;    // pawns carry it to other cells
    float cleanSeconds = 0.0f; // game seconds of work per layer at 100% speed
    int maxThickness = 3;
};

struct FilthPile {
    uint16_t kind = 0;  // index in Filth::kinds()
    uint8_t thickness = 1;
};

// The numbers from the FilthRules def (these values when it is missing).
struct COLONY_API FilthNumbers {
    float outdoorBeautyFactor = 0.3f;
    float ticksToSeconds = 1.44f;
    int maxCarried = 3;
    float pickupChancePerCell = 0.05f;
    float dropChancePerCell = 0.05f;
    float bloodPerBleedDay = 4.0f;   // drops an hour for each point of blood loss a day
    float cleaningSpeed = 1.0f;

    static FilthNumbers read(const DefDatabase& defs);
};

// What a pawn is carrying on its shoes, by kind index. Pawns that have walked get one.
inline constexpr size_t kFilthTrackedKinds = 8;
struct FilthCarrier {
    int lastCell = -1;  // the cell index it was in at the last step
    std::array<uint8_t, kFilthTrackedKinds> carried{};
};

// Everything saved (docs/design/saves.md, section "roomstats").
struct FilthState {
    struct Entry {
        int cell = 0;
        std::string kind;  // Filth defName
        int thickness = 1;
    };
    struct Carrier {
        uint32_t pawn = 0;
        int lastCell = -1;
        std::vector<std::pair<std::string, int>> carried;  // kind defName, count
    };
    std::vector<Entry> piles;
    std::vector<Carrier> carriers;
    uint64_t rngState = 0;
    uint64_t rngIncrement = 0;
};

class COLONY_API Filth {
public:
    Filth(Map& map, Things& things, const Floors& floors, const DefDatabase& defs, uint64_t seed);

    const FilthNumbers& numbers() const { return numbers_; }
    const std::vector<FilthKind>& kinds() const { return kinds_; }
    // Index in kinds(), or -1.
    int kindIndex(const std::string& name) const;

    // Adds `layers` to the cell's pile of that kind (making it), up to the kind's thickness limit.
    // False if nothing was added: no such kind, a cell that is off the map, rock, water or blocked
    // by a building, or a pile already at its limit.
    bool add(CellPos c, int kind, int layers = 1);
    bool add(CellPos c, const std::string& kindName, int layers = 1) { return add(c, kindIndex(kindName), layers); }
    // Takes one layer off the cell's first pile. False if there is none.
    bool clean(CellPos c);
    // Takes every pile off the cell.
    void clear(CellPos c);

    bool has(CellPos c) const;
    // The piles in a cell, by kind; empty for a clean cell. The reference is good until the next change.
    const std::vector<FilthPile>& at(CellPos c) const;
    // Game seconds a pawn at 100% speed needs for one layer of the cell's first pile; 0 for a clean cell.
    float cleanSeconds(CellPos c) const;
    // The beauty and cleanliness the piles in a cell add (outdoors: the beauty is cut by the outdoor factor).
    float beautyAt(CellPos c, bool outdoors) const;
    float cleanlinessAt(CellPos c) const;

    size_t cellCount() const { return piles_.size(); }
    size_t totalLayers() const;
    // Every pile of every cell by cell index, in cell order.
    const std::map<int, std::vector<FilthPile>>& all() const { return piles_; }
    // Goes up whenever a pile is added, thinned or removed.
    uint64_t changes() const { return changes_; }

    // One game second: pawns pick dirt up on bare ground and drop it on floors, track filth from
    // cell to cell, and bleeding pawns drop blood.
    void step();

    void hashState(StateHasher& h) const;
    FilthState state() const;
    // Loading a save; false, with `error`, on a kind or cell that does not exist.
    bool restore(const FilthState& state, std::string& error);

private:
    bool bareGround(CellPos c) const;

    Map& map_;
    Things& things_;
    const Floors& floors_;
    const DefDatabase& defs_;
    FilthNumbers numbers_;
    std::vector<FilthKind> kinds_;
    std::vector<float> filthRate_;  // per def id: stats.filthRate
    int dirtKind_ = -1;
    std::map<int, std::vector<FilthPile>> piles_;
    std::map<uint32_t, FilthCarrier> carriers_;  // by entity id
    Rng rng_;
    uint64_t changes_ = 0;
};

}  // namespace colony
