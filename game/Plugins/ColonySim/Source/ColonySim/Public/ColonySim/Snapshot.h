#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Cover.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Grid.h"
#include "ColonySim/JobType.h"
#include "ColonySim/Zones.h"

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
    // Pawns with a body: can't act (pain shock, unconscious, can't move), and dead. A dead pawn
    // stays where it fell. A full health report is `Sim::healthReport`.
    bool downed = false;
    bool dead = false;
    // Pawns: the job they are on (docs/design/jobs.md) and what it is about, and what they carry.
    JobType job = JobType::None;
    Entity jobTarget = kNoEntity;
    DefId carriedDef;
    int carriedCount = 0;
    // Pawns: how they hold their body and which way they face (a unit vector; +x east), the gun
    // in their hands (invalid if none) and steps until it can start a new shot (0: ready).
    // Downed pawns lie prone (docs/design/combat.md).
    Stance stance = Stance::Standing;
    float facingX = 1.0f;
    float facingY = 0.0f;
    DefId weapon;
    int shotReadyIn = 0;
};

// A bullet in flight (docs/design/combat.md). Unreal draws a tracer along its velocity; it
// appears with a ShotFired event and disappears with a ProjectileHit event.
struct ProjectileView {
    Entity id = kNoEntity;
    DefId def;  // Projectiles def
    float x = 0.0f;  // metres; z above sea level
    float y = 0.0f;
    float z = 0.0f;
    float vx = 0.0f;  // metres per step
    float vy = 0.0f;
    float vz = 0.0f;
    int level = 0;
};

// One stockpile zone: `cellCount` cells from Snapshot::zoneCells[firstCell].
struct ZoneView {
    ZoneId id = kNoZone;
    StoragePriority priority = StoragePriority::Normal;
    uint32_t firstCell = 0;
    uint32_t cellCount = 0;
};

struct Snapshot {
    int64_t gameSeconds = 0;  // Sim::time().seconds when it was written
    int mapWidth = 0;
    int mapHeight = 0;
    std::vector<ThingView> things;  // sorted by id
    std::vector<PathPointView> waypoints;  // the pawns' routes, see ThingView
    std::vector<ZoneView> zones;           // by id
    std::vector<CellPos> zoneCells;        // the zones' cells, see ZoneView
    std::vector<ProjectileView> projectiles;  // sorted by id

    // The thing with this id, or null if it is not on the map. Binary search.
    COLONY_API const ThingView* find(Entity id) const;
};

}  // namespace colony
