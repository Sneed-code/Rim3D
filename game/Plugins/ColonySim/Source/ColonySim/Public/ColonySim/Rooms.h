#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Grid.h"
#include "ColonySim/Map.h"
#include "ColonySim/Regions.h"

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace colony {

class DefDatabase;
class Things;

// Rooms and roofs, as RimWorld's. See docs/design/rooms.md.
//
// A room is the regions (docs/design/regions.md) joined across every border except doors: a
// door is not in any room, it is where two rooms (or a room and the outdoors) meet. A room that
// reaches the map edge, or is huge, is the outdoors. A room that does not is enclosed and gets a
// thin roof over every cell that has a roof holder (a wall, a door, rock) within the roof support
// radius; a room with a cell left unroofed counts as outdoors. A room's role comes from what is
// in it.
//
// Every level has its own rooms (docs/design/levels.md). On an upper level a room that reaches the
// edge of its floor (a cell beside open air) is outdoors too, like one that reaches the map edge.
// A cell under an upper floor has that floor for a roof, so Rooms never puts a thin roof there.

using RoomId = uint32_t;
inline constexpr RoomId kNoRoom = 0xFFFFFFFFu;

enum class RoomRole : uint8_t { None, Bedroom, Barracks, Kitchen, DiningRoom };

COLONY_API const char* roomRoleName(RoomRole role);

// The numbers in the RoomRules def (with these values when the def is missing).
struct COLONY_API RoomNumbers {
    struct RoleRule {
        RoomRole role = RoomRole::None;
        int minBeds = 0;
        int minTables = 0;
        int minChairs = 0;
        int minStoves = 0;
    };

    float roofSupportRadius = 6.9f;  // metres from a roof holder that a roof can span
    int hugeRoomCells = 2000;        // a bigger room is the outdoors and is never roofed
    std::vector<RoleRule> roles;     // in priority order: the first whose minimums are met

    static RoomNumbers read(const DefDatabase& defs);
};

struct Room {
    RoomId id = kNoRoom;
    int cellCount = 0;
    int regionCount = 0;
    CellRect bounds;     // smallest rectangle round the room's cells
    CellPos anchor;      // the room's first cell in row order, one of the room's cells
    bool touchesEdge = false;
    bool huge = false;
    bool openToAir = false;  // on an upper level: a cell beside one with no floor

    // Cells with no roof. An outdoor room that was not looked at has them all.
    int openRoofCount = 0;
    RoomRole role = RoomRole::None;
    // Buildings in the room, each counted once however many cells it covers.
    int beds = 0;
    int tables = 0;
    int chairs = 0;
    int stoves = 0;
    uint32_t firstCell = 0;  // into Rooms::cellsOf; only enclosed rooms list their cells
    uint32_t listedCells = 0;

    bool enclosed() const { return !touchesEdge && !huge && !openToAir; }
    // Under the open sky: reaches the map edge or open air, is huge, or has a cell with no roof.
    bool outdoors() const { return !enclosed() || openRoofCount > 0; }
};

class COLONY_API Rooms {
public:
    Rooms(Map& map, Things& things, const DefDatabase& defs);

    const RoomNumbers& numbers() const { return numbers_; }

    // Rebuilds rooms, roofs and roles if a cell's passage or any building changed since the last
    // call. Cheap when nothing changed. The sim calls it at the end of every step; the queries
    // below that are not const do it first.
    void update();

    // The room a walkable cell is in; kNoRoom for a door, a wall or a cell with no region.
    RoomId roomAt(CellPos c);
    const Room* roomOf(CellPos c);
    // Every room, the outdoors (edge and huge ones) included, in id order. Ids are good
    // until the next change, like region ids.
    const std::vector<Room>& rooms() const { return rooms_; }
    const Room& room(RoomId id) const { return rooms_[id]; }
    // The cells of a room that lists them, in region and row order.
    std::span<const CellPos> cellsOf(const Room& room) const;
    // The distinct rooms on the sides of a door cell, in id order (a closed door between a room
    // and the outdoors gives both).
    std::vector<RoomId> roomsBesideDoor(CellPos door);

    // Role of the room a cell is in; None for a cell with no room.
    RoomRole roleAt(CellPos c);
    // Under a roof and not in an outdoor room. A cell with no room (a door, a wall) is indoors if it
    // has a roof.
    bool indoors(CellPos c);

    // True if a roof over the cell could be held up: rock (on the ground) or a building that holds
    // roofs (a wall, a column) within the roof support radius, on the cell's own level. An upper
    // floor needs this of the cell under it (docs/design/levels.md).
    bool roofSupported(CellPos c) const;

    // The cells with a thin roof, by level then cell index, and how many times that set has changed.
    const std::vector<CellPos>& thinRoofCells() const { return thinCells_; }
    uint64_t roofChanges() const { return roofChanges_; }

    uint64_t rebuilds() const { return rebuilds_; }

private:
    void rebuild();
    void roof();
    void scanContents();
    bool roofHolderAt(CellPos c) const;

    Map& map_;
    Things& things_;
    RoomNumbers numbers_;
    std::vector<uint8_t> isBed_, isTable_, isChair_, isStove_, holdsRoof_;  // per def id
    std::vector<std::pair<int, int>> supportOffsets_;  // within the support radius, nearest first

    bool built_ = false;
    uint64_t regionChanges_ = 0;
    uint64_t buildingChanges_ = 0;
    uint64_t rebuilds_ = 0;
    uint64_t roofChanges_ = 0;
    std::vector<Room> rooms_;
    std::vector<RoomId> roomOfRegion_;  // by region id
    std::vector<RegionId> roomRegions_;  // every room's regions, room after room
    std::vector<uint32_t> firstRegion_;  // per room, into roomRegions_ (rooms_.size() + 1 entries)
    std::vector<CellPos> cells_;
    std::vector<CellPos> thinCells_;
};

}  // namespace colony
