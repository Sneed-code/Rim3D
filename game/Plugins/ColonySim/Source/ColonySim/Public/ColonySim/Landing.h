#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Grid.h"

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace colony {

class Map;
class Plants;
class Rng;
class StateHasher;
class Things;
struct GameTime;

// Shuttles and the landing zone (sneed, 2026-11-17). See docs/design/landing.md.
//
// The player paints a landing zone. A shuttle (a 12 m by 6 m VTOL, the Things def in LandingRules) that comes to the
// colony sets down inside the zone where its whole footprint fits; with no zone, or no room in it, it picks a clear spot
// near the colony at random. Small plants under it (grass, bushes, crops) are flattened when it touches down; trees,
// buildings, items, deep water, rock, roofs and steep ground stop it. While it stands it is an ordinary impassable
// building, so pawns walk round it and raiders can shoot it.
//
// A shuttle goes through three phases on the step clock (real time): Descending, Landed, Ascending. Its passengers step
// out of the side door when it lands; the trade system brings visitors and traders this way (docs/design/trade.md).
// Unreal reads `Snapshot::shuttles` and draws the flight, thrusters and dust from the phase and its progress.

enum class ShuttlePhase : uint8_t { Descending, Landed, Ascending };

COLONY_API const char* shuttlePhaseName(ShuttlePhase phase);

// Someone aboard who steps out on landing: a PawnKinds defName and a VisitorRole as a number.
struct ShuttlePassenger {
    std::string kind;
    uint8_t role = 0;
};

// One shuttle. Saved as a whole.
struct Shuttle {
    uint32_t id = 0;
    Entity building = kNoEntity;  // the Things building that stands on the footprint
    CellPos origin;               // footprint corner with the lowest x and y
    Rot4 rot = Rot4::North;       // North: the nose points to +y and the door is on the -x side
    ShuttlePhase phase = ShuttlePhase::Descending;
    int64_t phaseStep = 0;        // the step its phase began
    uint32_t visit = 0;           // the trade visit it brought, 0 for none
    std::vector<ShuttlePassenger> passengers;  // still aboard, to step out on landing
};

struct LandingSaved {
    uint32_t nextId = 1;
    std::vector<int> zone;            // ground cell indices, ascending
    std::vector<Shuttle> shuttles;    // by id
};

// LandingRules (data/defs/Rules). All ours.
struct COLONY_API LandingRules {
    bool present = false;
    std::string shuttleDef = "Shuttle_VTOL";
    int descendSteps = 360;      // 12 real seconds at 1x
    int ascendSteps = 300;
    float maxSlopeM = 1.5f;      // highest minus lowest ground under the footprint
    float searchMinM = 14.0f;    // a random spot is this far from the colony's middle, or more
    float searchMaxM = 60.0f;
    int searchTries = 300;
    float visitShuttleChance = 0.5f;  // share of visitor groups and trade caravans that come by shuttle

    static LandingRules read(const DefDatabase& defs);
};

class COLONY_API LandingSystem {
public:
    LandingSystem(Things& things, Map& map, Plants& plants, const DefDatabase& defs, const GameTime& time, Rng& rng);

    const LandingRules& rules() const { return rules_; }
    // Whether shuttles can come at all: the data has LandingRules and the shuttle def.
    bool available() const { return shuttleDef_.valid(); }

    // ---- The landing zone ----
    // Adds the rectangle's ground cells (on the map) to the zone or takes them off; returns how many changed.
    int paintZone(const CellRect& rect, bool add);
    bool inZone(CellPos c) const;
    std::vector<CellPos> zoneCells() const;  // in row order
    uint64_t zoneChanges() const { return zoneChanges_; }

    // ---- Shuttles ----
    // Finds where a shuttle can set down: in the zone if it has room (the spot nearest the zone's middle), else a clear
    // spot at random near (nearX, nearY). False if there is none.
    bool findSite(float nearX, float nearY, CellPos& origin, Rot4& rot) const;
    // Whether a shuttle's footprint with that corner and facing can be landed on now (`inZone`: pawns are moved aside
    // rather than counted against it).
    bool siteClear(CellPos origin, Rot4 rot, bool allowPawns) const;
    // Sends a shuttle down near (nearX, nearY) with these passengers for a visit (0 for none). It claims its footprint at
    // once (pawns standing there step aside). False with `error` if there is nowhere to land.
    bool land(uint32_t visit, std::vector<ShuttlePassenger> passengers, float nearX, float nearY, uint32_t& id, std::string& error);
    // Sends a landed shuttle up; it leaves the map when the ascent ends. False if it is not landed.
    bool depart(uint32_t id);

    const Shuttle* find(uint32_t id) const;
    // The shuttle that brought a visit, or null.
    const Shuttle* ofVisit(uint32_t visit) const;
    // True while a visit's shuttle is still coming down (its people are not out yet).
    bool awaiting(uint32_t visit) const;
    const std::vector<Shuttle>& shuttles() const { return shuttles_; }
    // The cell just outside the door, where passengers step out and board.
    CellPos door(const Shuttle& s) const;
    // The footprint and its middle in metres.
    CellRect footprint(const Shuttle& s) const;
    float centreX(const Shuttle& s) const;
    float centreY(const Shuttle& s) const;
    // 0 to 1 through the shuttle's phase (1 while landed).
    float progress(const Shuttle& s) const;

    // Called when a shuttle touches down, with its passengers still listed; the trade system puts them on the map.
    void setOnLanded(std::function<void(const Shuttle&)> onLanded) { onLanded_ = std::move(onLanded); }
    // Every step: descents and ascents move on; a shuttle whose building is gone (destroyed) is dropped.
    void step();

    LandingSaved saved() const;
    bool restore(const LandingSaved& saved, std::string& error);
    void hashState(StateHasher& h) const;

private:
    // A footprint cell's world position for a local cell (lx across, ly along, at North).
    CellPos local(CellPos origin, Rot4 rot, int lx, int ly) const;
    void flattenPlants(const CellRect& rect);
    void moveAside(const CellRect& rect);
    int64_t steps() const;

    Things& things_;
    Map& map_;
    Plants& plants_;
    const DefDatabase& defs_;
    const GameTime& time_;
    Rng& rng_;
    LandingRules rules_;
    DefId shuttleDef_;
    int sizeX_ = 6;
    int sizeY_ = 12;
    std::set<int> zone_;
    uint64_t zoneChanges_ = 0;
    std::vector<Shuttle> shuttles_;
    uint32_t nextId_ = 1;
    std::function<void(const Shuttle&)> onLanded_;
};

}  // namespace colony
