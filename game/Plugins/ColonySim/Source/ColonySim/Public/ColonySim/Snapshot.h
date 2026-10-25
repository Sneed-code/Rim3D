#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Cover.h"
#include "ColonySim/BillMode.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Designations.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Grid.h"
#include "ColonySim/JobType.h"
#include "ColonySim/Looks.h"
#include "ColonySim/Melee.h"
#include "ColonySim/Power.h"
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
    float jobProgress = 0.0f;  // 0 to 1: a swing of the pick, a plant being cut, a frame being built (hands-on work, U10)
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
    // Work buildings with bills (docs/design/bills.md): `billCount` bills from Snapshot::bills[firstBill].
    uint32_t firstBill = 0;
    uint32_t billCount = 0;
    // Pawns with needs (docs/design/needs.md): food and rest, 1 full to 0 empty, else -1; asleep
    // while lying down in a bed or on the ground.
    float food = -1.0f;
    float rest = -1.0f;
    bool asleep = false;
    // Mood (docs/design/mood.md): 0 to 100, else -1; the break band as a MoodBand (0 stable to 3 extreme).
    float mood = -1.0f;
    int moodBand = 0;
    // Recreation, comfort and outdoors needs (docs/design/recreation.md): 0 empty to 1 full, else -1.
    float joy = -1.0f;
    float comfort = -1.0f;
    float outdoors = -1.0f;
    // Pawns: how they hold their body and which way they face (a unit vector; +x east), the gun
    // in their hands (invalid if none) and steps until it can start a new shot (0: ready).
    // Downed pawns lie prone (docs/design/combat.md).
    Stance stance = Stance::Standing;
    float facingX = 1.0f;
    float facingY = 0.0f;
    DefId weapon;
    int shotReadyIn = 0;
    // Pawns with a gun that takes a magazine (docs/design/aiming.md): rounds left of magazineSize
    // (both 0 without one), and whether it is reloading (shotReadyIn then counts the reload down).
    int rounds = 0;
    int magazineSize = 0;
    bool reloading = false;
    // The player's gun kick (P2, docs/design/shooter-feel.md): where the shots are going above and to
    // the right of where the player aims, in degrees, from the last shots; it wears off by itself. The
    // camera shows the same offset. Zero for everyone else.
    float kickUp = 0.0f;
    float kickSide = 0.0f;
    // A dodge roll in progress (P2): 0 none, 1 start-up, 2 rolling (hitProof says whether bullets pass
    // through it), 3 recovery; the steps left in that phase and its direction (unit vector).
    uint8_t rollPhase = 0;
    int rollStepsLeft = 0;
    float rollDirX = 0.0f;
    float rollDirY = 0.0f;
    bool hitProof = false;
    // Pawns: their side (1 is the player's), and for fighters (docs/design/fighters.md) whether the
    // player drafted them, whom they shoot at, the cell they hold and whether they are falling back.
    uint32_t faction = 1;
    bool drafted = false;
    bool fighting = false;
    Entity fightTarget = kNoEntity;
    CellPos fightSpot;
    bool fallingBack = false;
    // Humanlike pawns: their looks, Snapshot::looks[looks]; -1 for other things.
    int32_t looks = -1;
    // Buildings with a power part (docs/design/power.md): whether they have power, the net they are
    // in (Snapshot::powerNets[powerNet - 1]; 0 for none), watts now (+ generating, - drawing) and,
    // for batteries, how full (0 to 1; -1 for other buildings).
    PowerState power = PowerState::None;
    uint32_t powerNet = 0;
    float powerWatts = 0.0f;
    float batteryFill = -1.0f;
    // Pawns with needs: the beauty need (docs/design/room-stats.md), 0 to 1 with 35% to 65% neutral; else -1.
    float beautyNeed = -1.0f;
    // Animals (docs/design/animal-life.md): life stage (0 baby, 1 juvenile, 2 adult) and body size in
    // RimWorld's units (a human is 1); -1 and 0 for other things. `food` (a share of their capacity),
    // `asleep` and `faction` (100 for wild) are filled for them too.
    int lifeStage = -1;
    float bodySize = 0.0f;
    // Pawns in melee (docs/design/melee.md): winding up a swing at meleeTarget (the hit comes in
    // meleeStepsLeft steps) or recovering from one, and whether their weapon is raised to block.
    MeleePhase meleePhase = MeleePhase::Ready;
    int meleeStepsLeft = 0;
    Entity meleeTarget = kNoEntity;
    bool blocking = false;
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

// How a pawn looks (docs/design/looks.md). The lineage is invalid if the defs have no such
// Lineages def (the test defs).
struct PawnLooksView {
    DefId lineage;
    bool female = false;
    BodyType body = BodyType::Average;
    float weight = 0.0f;  // -1 thin to 1 heavy
    float height = 1.0f;  // times 1.80 m
    LooksColor skin;
    LooksColor hair;
};

// One bill on a work building, in the building's order (first is tried first).
struct BillView {
    uint32_t id = 0;  // the id the bill commands use, per building
    DefId recipe;     // a Recipes def
    BillMode mode = BillMode::Count;
    int count = 0;    // Count: times left; UntilHave: the stock to reach; Forever: 0
    bool suspended = false;
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
    // Room stats (docs/design/room-stats.md); all 0 for a room without stats (open to the sky).
    // The names of their stages come from beautyLabel(), spaceLabel() and the like (RoomStats.h).
    bool hasStats = false;
    float beauty = 0.0f;
    float space = 0.0f;
    float wealth = 0.0f;
    float cleanliness = 0.0f;
    float impressiveness = 0.0f;
    float temperature = 0.0f;  // degrees Celsius (docs/design/temperature.md)
};

// A floor laid on a cell, and a pile of filth on one (docs/design/room-stats.md).
struct FloorView {
    CellPos cell;
    DefId def;  // a Terrain def
};

struct FilthView {
    CellPos cell;
    DefId def;  // a Filth def
    int thickness = 1;
};

// One mark on a cell: mine here, cut this plant, harvest it (docs/design/mining.md).
struct DesignationView {
    CellPos cell;
    DesignationKind kind = DesignationKind::None;
};

// One research project and where it stands (docs/design/research.md).
struct ResearchView {
    DefId project;          // a Research def
    double progress = 0.0;  // points so far (the cost when done)
    double cost = 0.0;
    bool done = false;
    bool available = false;  // not done, every prerequisite done: can be picked
    bool current = false;
};

// A talent rank the protagonist has bought (docs/design/talents.md).
struct TalentRankView {
    DefId talent;  // a Talents def
    int rank = 0;
};

// The protagonist's talent progress. One entry per pawn that has talents, in id order.
struct TalentView {
    Entity pawn = kNoEntity;
    int64_t xp = 0;
    int64_t xpForNextPoint = 0;  // total XP at which the next point comes (0 at the top)
    int pointsFree = 0;
    std::vector<TalentRankView> ranks;  // in Talents def order
};

struct Snapshot {
    int64_t gameSeconds = 0;  // Sim::time().seconds when it was written
    int mapWidth = 0;
    int mapHeight = 0;
    std::vector<ThingView> things;  // sorted by id
    std::vector<PathPointView> waypoints;  // the pawns' routes, see ThingView
    std::vector<BillView> bills;           // the work buildings' bills, see ThingView
    std::vector<ZoneView> zones;           // by id
    std::vector<CellPos> zoneCells;        // the zones' cells, see ZoneView
    std::vector<DesignationView> designations;  // every mark, in cell order
    std::vector<RoomView> rooms;           // enclosed rooms, by id
    float outdoorTemperature = 0.0f;       // degrees Celsius
    // The cells with a thin (built) roof, in cell order; `roofChanges` goes up when the set
    // changes, so Unreal rebuilds the roof only then. The mountain's thick roof is the rock itself
    // (`Map::roof` says which).
    std::vector<CellPos> roofCells;
    uint64_t roofChanges = 0;
    std::vector<ProjectileView> projectiles;  // sorted by id
    std::vector<PawnLooksView> looks;      // see ThingView::looks
    std::vector<PowerNetView> powerNets;   // by id - 1, see ThingView::powerNet
    std::vector<ResearchView> research;    // every project, in def order
    std::vector<TalentView> talents;       // see TalentView
    // Floors and filth, in cell order. Rewritten only when `floorChanges` / `filthChanges` goes up.
    std::vector<FloorView> floors;
    uint64_t floorChanges = ~0ULL;
    std::vector<FilthView> filth;
    uint64_t filthChanges = ~0ULL;

    // The thing with this id, or null if it is not on the map. Binary search.
    COLONY_API const ThingView* find(Entity id) const;
};

}  // namespace colony
