#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Grid.h"

#include <string>

namespace colony {

class Map;
class Rooms;
class Things;

// Upper floors (task M1, docs/design/levels.md): the floor of a cell on level 1 or above, which
// pawns walk and build on and which is the roof of the cell below. It needs holding up like a
// roof: a wall or column (or rock, under level 1) within the roof support radius on the level
// below, and no mountain roof under it. It can only be taken up when nothing stands on it.
//
// Laying one is the PlaceUpperFloorCommand for now (instant, like floors); the work to build one
// comes with stairs (M2), and floors that fall when their walls go with M3.
class COLONY_API UpperFloors {
public:
    UpperFloors(Map& map, Rooms& rooms, Things& things);

    // Why an upper floor can't be laid on the cell, or empty if it can (or is there already).
    std::string whyNotLay(CellPos c) const;
    // Why the upper floor on the cell can't be taken up, or empty if it can (or there is none).
    std::string whyNotRemove(CellPos c) const;
    // Lays or takes up the floor of one cell. False, with the reason in `why`, if it can't be.
    bool lay(CellPos c, std::string* why = nullptr);
    bool remove(CellPos c, std::string* why = nullptr);

private:
    Map& map_;
    Rooms& rooms_;
    Things& things_;
};

}  // namespace colony
