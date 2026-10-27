#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Grid.h"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace colony {

class StateHasher;

// Allowed areas, as RimWorld's: named sets of cells, and a pawn can be limited to one. A limited pawn takes no
// work whose target is outside its area and hauls only to cells inside it. See docs/design/areas.md.
using AreaId = uint32_t;
inline constexpr AreaId kNoArea = 0;

struct Area {
    AreaId id = kNoArea;
    std::string name;
    std::vector<CellPos> cells;  // by level, then y, then x
};

// An area and the pawns limited to it, as saved.
struct AreasSaved {
    AreaId nextId = 1;
    std::vector<Area> areas;
    std::vector<std::pair<Entity, AreaId>> assigned;  // by pawn id
};

class COLONY_API Areas {
public:
    // A new empty area. A blank name becomes "Area <id>". Ids count 1, 2, 3 and are never used again.
    AreaId create(const std::string& name);
    // Adds the rectangle's cells to the area or takes them off; returns how many cells changed. 0 for an unknown area.
    int paint(AreaId id, const CellRect& rect, bool add);
    // Deletes the area; pawns limited to it are free again. False if there is no such area.
    bool remove(AreaId id);
    // Limits `pawn` to the area, or frees it with kNoArea. False (with `error`) for an unknown area or pawn.
    bool assign(Entity pawn, AreaId id, std::string& error);

    const Area* find(AreaId id) const;
    const std::vector<Area>& areas() const { return areas_; }  // `cells` is empty here: ask cellsIn
    // The area's cells by level, then y, then x.
    std::vector<CellPos> cellsIn(AreaId id) const;
    AreaId areaOf(Entity pawn) const;
    // True if the pawn may work at the cell: it has no area, or the cell is in it.
    bool allows(Entity pawn, CellPos cell) const;
    bool contains(AreaId id, CellPos cell) const;
    // True once any pawn is limited, so callers can skip the lookup otherwise.
    bool anyAssigned() const { return !assigned_.empty(); }
    uint64_t changeCount() const { return changeCount_; }

    AreasSaved saved() const;
    bool restore(const AreasSaved& saved, std::string& error);
    void hashState(StateHasher& h) const;

private:
    static uint64_t key(CellPos c);
    Area* findMutable(AreaId id);
    const std::set<uint64_t>* cellsOf(AreaId id) const;

    AreaId nextId_ = 1;
    std::vector<Area> areas_;                    // by id
    std::vector<std::set<uint64_t>> members_;    // parallel to areas_, for fast lookup
    std::vector<std::pair<Entity, AreaId>> assigned_;  // by pawn id
    uint64_t changeCount_ = 0;
};

}  // namespace colony
