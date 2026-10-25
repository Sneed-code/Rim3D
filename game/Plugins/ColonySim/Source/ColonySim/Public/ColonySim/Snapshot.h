#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Grid.h"

#include <cstdint>
#include <vector>

namespace colony {

// Unreal reads sim state through this copy, never through the sim's own containers, so the sim
// can later step on its own thread without changing the bridge. Sim::writeSnapshot fills it.
// Later tasks add what they draw or show (pawn info, work tab, room stats). See docs/design/bridge.md.

// A point on a pawn's route, in metres on its level (the middle of the cell the route goes through).
struct PathPointView {
    float x = 0.0f;
    float y = 0.0f;
};

// One thing on the map. Things off the map (e.g. a carried item) are not in the snapshot.
struct ThingView {
    Entity id = kNoEntity;
    DefId def;
    ThingCategory category = ThingCategory::Item;
    float x = 0.0f;  // metres on the level: cell centre, footprint centre, or a pawn's position
    float y = 0.0f;
    float z = 0.0f;  // ground height under it, metres (Map::heightAt)
    int level = 0;
    // Pawns: how fast they are moving, metres per game second (a step), and the route still ahead:
    // `waypointCount` points from Snapshot::waypoints[firstWaypoint], nearest first. The route is
    // already straightened, so the points are where the pawn turns (docs/design/movement.md).
    float vx = 0.0f;
    float vy = 0.0f;
    uint32_t firstWaypoint = 0;
    uint32_t waypointCount = 0;
    Rot4 rotation = Rot4::North;  // buildings
    int sizeX = 1;                // buildings: footprint as placed (rotation already applied)
    int sizeY = 1;
    int stackCount = 0;  // items
    int hitPoints = 0;   // 0 and 0 when the thing has no hit points
    int maxHitPoints = 0;
};

struct Snapshot {
    int64_t gameSeconds = 0;  // Sim::time().seconds when it was written
    int mapWidth = 0;
    int mapHeight = 0;
    std::vector<ThingView> things;  // sorted by id
    std::vector<PathPointView> waypoints;  // the pawns' routes, see ThingView

    // The thing with this id, or null if it is not on the map. Binary search.
    COLONY_API const ThingView* find(Entity id) const;
};

}  // namespace colony
