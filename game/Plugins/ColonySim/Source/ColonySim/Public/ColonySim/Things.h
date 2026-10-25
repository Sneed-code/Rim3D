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
// `thingClass` Building_Door, as in RimWorld, and never blocks movement. Origin and rotation are
// set while the building is spawned.
struct Footprint {
    int sizeX = 1;  // as placed facing North
    int sizeY = 1;
    bool blocksMovement = false;
    bool isDoor = false;
    CellPos origin;
    Rot4 rotation = Rot4::North;

    CellRect rect() const { return footprintRect(origin, sizeX, sizeY, rotation); }
};

// Items. Never above the def's `stackLimit` (1 if it has none).
struct StackCount {
    int count = 1;
};

// Items and buildings whose def has `stats.maxHitPoints`. Pawns get per-part health later (F1).
struct HitPoints {
    int current = 0;
    int max = 0;
};

// Pawns. Speed from the def's `stats.moveSpeed` (metres per game second, converted by the
// importer). `path` is the cell route being walked; `next` the index of the next cell in it.
struct Mover {
    float metresPerStep = 0.0f;
    std::vector<CellPos> path;
    size_t next = 0;
};

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

    void report(EventType type, Entity thing, CellPos cell = CellPos{});
};

}  // namespace colony
