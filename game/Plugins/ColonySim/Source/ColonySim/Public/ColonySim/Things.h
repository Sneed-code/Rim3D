#pragma once

#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"
#include "ColonySim/Grid.h"

#include <entt/entity/registry.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace colony {

class Map;

// Things are pawns, items and buildings: entities in an EnTT registry, each made from a def of
// kind "Things" plus components. See docs/design/things.md.
//
// Unreal does not include this header: it reads things through the sim's snapshot and events
// (docs/design/bridge.md).

// Every thing has one.
struct Thing {
    DefId def;
    ThingCategory category = ThingCategory::Item;
};

// Where a spawned thing is, in metres on a level: the centre of its cell (items), of its
// footprint (buildings), or anywhere (pawns, which move continuously). Height comes from the
// map. A thing without a Position is not on the map, e.g. an item being carried.
struct Position {
    float x = 0.0f;
    float y = 0.0f;
    int level = 0;
};

inline CellPos cellOf(const Position& p) {
    return CellPos{static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)), p.level};
}

// Buildings. Size and blocking come from the def (`size`, `passability`). A door is a def with
// `thingClass` Building_Door, as in RimWorld, and never blocks movement. `fillPercent` makes it
// block sight and give cover. Origin and rotation are set while the building is spawned.
struct Footprint {
    int sizeX = 1;  // as placed facing North
    int sizeY = 1;
    bool blocksMovement = false;
    bool isDoor = false;
    uint8_t obstacleDm = 0;  // height for sight and cover, from the def's `fillPercent` (Cover.h)
    CellPos origin;
    Rot4 rotation = Rot4::North;

    CellRect rect() const { return footprintRect(origin, sizeX, sizeY, rotation); }
};

// Items. Never above the def's `stackLimit` (1 if it has none).
struct StackCount {
    int count = 1;
};

// Items and buildings whose def has `stats.maxHitPoints`. Pawns have per-part health instead
// (Health.h).
struct HitPoints {
    int current = 0;
    int max = 0;
};

// Pawns. `metresPerStep` is the def's `stats.moveSpeed` (metres per game second, converted by the
// importer): the speed on flat ground with no path cost. `path` is the route being walked, the
// waypoints left after `smoothPath`; `next` the index of the waypoint being walked to, and
// (`fromX`, `fromY`) where the line to it started. The sim's movement system moves the pawn along
// it (docs/design/movement.md); a pawn whose `next` has reached the end of `path` has arrived, or
// has given up (`gaveUp`) after being held up for too long.
struct Mover {
    float metresPerStep = 0.0f;
    std::vector<CellPos> path;
    size_t next = 0;
    float fromX = 0.0f;
    float fromY = 0.0f;
    // Metres per step right now. Unreal reads it to turn and animate the pawn.
    float vx = 0.0f;
    float vy = 0.0f;
    // Steps in a row spent wanting to walk but held back (by pawns or walls), and steps left of
    // walking through other pawns after that went on too long (the fix for two pawns meeting in a
    // one-cell corridor). `heldSteps` keeps counting through that, until the pawn gives up.
    uint16_t blockedSteps = 0;
    uint16_t ghostSteps = 0;
    uint16_t heldSteps = 0;
    bool gaveUp = false;
    // Set by the job system every step for a pawn at a job step it does standing still (building,
    // mining, cooking, eating, sleeping): the movement system keeps it where it is, so a pawn
    // walking past can't shove it out of its working cell. Others steer round it. Not saved: the
    // next step sets it again before anyone moves.
    bool hold = false;

    bool arrived() const { return next >= path.size(); }
    // Sets off along `route` (cells after the one the pawn is in) from where the pawn stands.
    void walkTo(float x, float y, std::vector<CellPos> route) {
        path = std::move(route);
        next = 0;
        fromX = x;
        fromY = y;
        blockedSteps = 0;
        heldSteps = 0;
        gaveUp = false;
    }
};

// On the one pawn the player controls directly (task U9, docs/design/protagonist.md). The
// movement system leaves it alone and DirectMoveCommand moves it.
struct PlayerControl {};

// Owns the registry of every thing and keeps the map's building layer in step with it.
// The DefDatabase must outlive this object (the sim holds it by shared pointer).
class Things {
public:
    Things(Map& map, std::shared_ptr<const DefDatabase> defs);

    entt::registry& registry() { return registry_; }
    const entt::registry& registry() const { return registry_; }
    const DefDatabase& defs() const { return *defs_; }

    // Makes a thing from a Things def and puts it on the map (see make and spawnAt). Returns
    // kNoEntity, and changes nothing, if either step fails.
    Entity spawn(DefId def, CellPos cell, int stackCount = 1, Rot4 rot = Rot4::North);
    Entity spawn(const std::string& defName, CellPos cell, int stackCount = 1, Rot4 rot = Rot4::North);

    // Makes a thing that is not on the map yet, with the components its def asks for. Returns
    // kNoEntity if the def is not a Things def or its category is not Item, Building or Pawn.
    Entity make(DefId def, int stackCount = 1);

    // Puts a thing that is not on the map at a cell. Items and pawns go to the cell's centre and
    // need walkable ground. A building's footprint starts at `cell` (lowest x and y corner),
    // faces `rot`, and every cell under it must be on the map, walkable ground and free of other
    // buildings. Returns false, and changes nothing, if the thing can't go there.
    bool spawnAt(Entity thing, CellPos cell, Rot4 rot = Rot4::North);

    // For loading a save: a thing that already has its Position goes back into the map's layers
    // (a building's footprint cells get the building and its passage; an item goes into the
    // items-per-cell index). Returns false, and changes nothing, if a building's cells are off the
    // map, not walkable ground or already taken.
    bool relink(Entity thing);

    // Items lying in a cell, in entity id order. Usually none or one (one stack per cell is the
    // storage rule), but spawning can put more in one cell.
    const std::vector<Entity>& itemsAt(CellPos cell) const;
    // Goes up whenever an item is put on the map or taken off it, so a system that reads what lies
    // in cells can tell its picture is stale (RoomStats).
    uint64_t itemChanges() const { return itemChanges_; }

    // Splits `count` off an item stack: a new item of the same def, off the map, with `count`,
    // and the stack keeps the rest. Returns kNoEntity if `count` isn't less than the stack.
    Entity splitOff(Entity item, int count);

    // Takes a thing off the map (removes its Position, frees its building cells). It still
    // exists, e.g. to be carried. Does nothing if it is not spawned.
    void despawn(Entity thing);

    // Despawns if needed, then destroys the entity.
    void destroy(Entity thing);

    // Where spawn, despawn and destroy report ThingSpawned, ThingDespawned and ThingDestroyed
    // events; null (the default) reports nothing. The sim points it at its event list.
    void setEventLog(std::vector<Event>* events) { events_ = events; }

    bool exists(Entity thing) const { return registry_.valid(thing); }
    bool spawned(Entity thing) const { return registry_.valid(thing) && registry_.all_of<Position>(thing); }
    // Number of things that exist, spawned or not.
    size_t count() const;

private:
    Map& map_;
    std::shared_ptr<const DefDatabase> defs_;
    entt::registry registry_;
    std::vector<Event>* events_ = nullptr;
    // Items per cell, by packed cell key. Looked up only, never iterated (its order is not fixed).
    std::unordered_map<uint64_t, std::vector<Entity>> itemsAt_;
    uint64_t itemChanges_ = 0;

    void indexItem(Entity thing, CellPos cell, bool add);
    void placeFootprint(Entity thing, const CellRect& rect, const Footprint& footprint);
    void report(EventType type, Entity thing, CellPos cell = CellPos{});
};

}  // namespace colony
