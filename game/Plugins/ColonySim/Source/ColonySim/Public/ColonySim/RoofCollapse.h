#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"
#include "ColonySim/Grid.h"
#include "ColonySim/Random.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace colony {

class HealthSystem;
class Map;
class Rooms;
class StateHasher;
class Things;

// Roof support and collapse (task M3, docs/design/roofs.md). A roof is held up by a roof holder (rock,
// a wall, a column, a door) within the roof support radius (RoomRules.roofSupportRadius, 6.9 m).
// When the last holder in reach goes, the roof falls, and what is under it is crushed:
//
// - a built (thin) roof: Rooms reports the cells that lost their holder (`takeUnsupportedRoofs`);
// - the mountain's thick roof over a cell that was mined out: it falls when no rock or wall is in
//   reach any more, which is when a cave is dug wider than about 14 m;
// - an upper floor (M1): it falls, with what stands on it, when nothing holds up the cell under it.
//   Pawns and items drop to the level below and are hurt by the fall; buildings on it are lost.
//
// A fall can take away the holder of another roof or floor, so it goes on until nothing moves.

// The numbers in the RoofRules def (with these values when it is missing).
struct COLONY_API RoofNumbers {
    int thinPawnHits = 2;          // how many hits of Crush a pawn under a falling thin roof takes
    float thinPawnDamage = 25.0f;
    int thickPawnHits = 4;
    float thickPawnDamage = 50.0f;
    int thinThingDamage = 100;     // hit points a building or item loses under a thin roof
    int thickThingDamage = 100000;  // the mountain destroys what is under it
    int fallPawnHits = 2;          // a pawn dropping from a floor that fell
    float fallPawnDamage = 20.0f;
    int fallSearchRadius = 3;      // how far from the spot below a thing lands if that is a wall or full

    static RoofNumbers read(const DefDatabase& defs);
};

// What is saved: the built (thin) roofs, which stay up while a holder is in reach and so are no longer
// worked out from the walls; the thick roofs that fell (mined-out cells, by Map::index); the thin
// roofs waiting to fall (Rooms was asked before the step that drops them) and the counter.
struct RoofCollapseSaved {
    std::vector<CellPos> thin;
    std::vector<int> fallenThick;
    std::vector<CellPos> pending;
    uint64_t collapses = 0;
};

class COLONY_API RoofCollapse {
public:
    RoofCollapse(Map& map, Rooms& rooms, Things& things, HealthSystem& health, const DefDatabase& defs);

    const RoofNumbers& numbers() const { return numbers_; }

    // Where RoofCollapsed events go; null (the default) reports nothing.
    void setEventLog(std::vector<Event>* events) { events_ = events; }
    // The job system ends the job of a pawn that is hurt or dropped (installed by the sim).
    void setInterruptHandler(std::function<void(Entity pawn, const std::string& why)> handler) {
        interrupt_ = std::move(handler);
    }

    // After Rooms::update, once a step. Cheap when no wall, rock or floor changed.
    void step(Rng& rng);

    // Roof cells (thin, thick) and floors that fell, ever. Unreal redraws the roof when it goes up.
    uint64_t collapses() const { return collapses_; }
    // The thick roofs that fell, in order (Map::index of the cell).
    const std::vector<int>& fallenThick() const { return fallenThick_; }

    RoofCollapseSaved state() const;
    // Puts the fallen thick roofs back (clearing them on the map) and the roofs waiting to fall.
    bool restore(const RoofCollapseSaved& saved);
    void hashState(StateHasher& hasher) const;

private:
    enum class Kind { Thin, Thick, Floor };

    bool dropThinRoofs(Rng& rng);
    bool dropThickRoofs(Rng& rng);
    bool dropFloors(Rng& rng);
    // Hurts everything standing, or lying, in the cell by the roof that fell on it.
    void crush(CellPos c, Kind kind, Rng& rng);
    // The floor of the cell goes, with all that stands on it.
    void dropFloor(CellPos c, Rng& rng);
    // A walkable cell on `level` at or near (x, y), nearest first; false if there is none.
    bool landingCell(CellPos around, CellPos* out) const;
    void report(CellPos c, Kind kind, int hurt);

    Map& map_;
    Rooms& rooms_;
    Things& things_;
    HealthSystem& health_;
    RoofNumbers numbers_;
    DefId crushType_;
    std::vector<std::pair<int, int>> landingOffsets_;  // nearest first, within fallSearchRadius
    std::vector<Event>* events_ = nullptr;
    std::function<void(Entity, const std::string&)> interrupt_;

    uint64_t collapses_ = 0;
    std::vector<int> fallenThick_;
    // The map's counters when the thick roofs and the floors were last looked at.
    uint64_t thickBuildings_ = ~uint64_t{0};
    uint64_t thickRock_ = ~uint64_t{0};
    uint64_t floorBuildings_ = ~uint64_t{0};
    uint64_t floorChanges_ = ~uint64_t{0};
};

}  // namespace colony
