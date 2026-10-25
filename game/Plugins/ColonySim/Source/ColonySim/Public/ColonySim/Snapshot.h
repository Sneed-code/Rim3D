#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Cover.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Designations.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Grid.h"
#include "ColonySim/JobType.h"
#include "ColonySim/Rooms.h"
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
    // Buildings: Built, or a Blueprint or Frame still to be built (docs/design/construction.md). A
    // site's `def` is the building it will become, `stuff` what it is made of (invalid for none),
    // `buildProgress` its work done, 0 to 1, and `materialsHave` of `materialsNeeded` delivered.
    // `markedForDeconstruct` is a built building with the deconstruct designation. Finished
    // buildings made of something also have `stuff`.
    ConstructionStage stage = ConstructionStage::Built;
    DefId stuff;
    float buildProgress = 0.0f;
    int materialsHave = 0;
    int materialsNeeded = 0;
    bool markedForDeconstruct = false;
    // Doors: 0 closed to 1 open (it opens over about 0.75 real seconds while a pawn waits).
    float doorOpen = 0.0f;
    // Pawns with needs (docs/design/needs.md): food and rest, 1 full to 0 empty, else -1; asleep
    // while lying down in a bed or on the ground.
    float food = -1.0f;
    float rest = -1.0f;
    bool asleep = false;
    // Mood (docs/design/mood.md): 0 to 100, else -1; the break band as a MoodBand (0 stable to 3 extreme).
    float mood = -1.0f;
    int moodBand = 0;
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

// An enclosed room (docs/design/rooms.md); the outdoors is not listed. Ids are good until the next
// change to the walls.
struct RoomView {
    RoomId id = kNoRoom;
    RoomRole role = RoomRole::None;
    int cellCount = 0;
    CellRect bounds;
    bool outdoors = false;  // enclosed but with cells the roof does not reach
    int openRoofCount = 0;
    int beds = 0;
    int tables = 0;
    int chairs = 0;
    int stoves = 0;
};

// One mark on a cell: mine here, cut this plant, harvest it (docs/design/mining.md).
struct DesignationView {
    CellPos cell;
    DesignationKind kind = DesignationKind::None;
};

struct Snapshot {
    int64_t gameSeconds = 0;  // Sim::time().seconds when it was written
    int mapWidth = 0;
    int mapHeight = 0;
    std::vector<ThingView> things;  // sorted by id
    std::vector<PathPointView> waypoints;  // the pawns' routes, see ThingView
    std::vector<ZoneView> zones;           // by id
    std::vector<CellPos> zoneCells;        // the zones' cells, see ZoneView
    std::vector<DesignationView> designations;  // every mark, in cell order
    std::vector<RoomView> rooms;           // enclosed rooms, by id
    // The cells with a thin (built) roof, in cell order; `roofChanges` goes up when the set
    // changes, so Unreal rebuilds the roof only then. The mountain's thick roof is the rock itself
    // (`Map::roof` says which).
    std::vector<CellPos> roofCells;
    uint64_t roofChanges = 0;
    std::vector<ProjectileView> projectiles;  // sorted by id

    // The thing with this id, or null if it is not on the map. Binary search.
    COLONY_API const ThingView* find(Entity id) const;
};

}  // namespace colony
