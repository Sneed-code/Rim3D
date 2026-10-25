#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Grid.h"
#include "ColonySim/JobType.h"

#include <cstdint>
#include <string>

namespace colony {

// Sim to Unreal: what happened during one step, for sounds, effects, animations and UI messages.
// Positions and states are in the snapshot; events say what changed. See docs/design/bridge.md.
enum class EventType : uint8_t {
    ThingSpawned,    // a thing appeared on the map (made, or put down)
    ThingDespawned,  // a thing left the map but still exists (e.g. picked up to carry)
    ThingDestroyed,  // a thing stopped existing; forget its id (a ThingDespawned comes first if it was on the map)
    CommandFailed,   // a command could not be applied
    PawnInjured,     // a pawn got an injury (docs/design/health.md)
    PawnPartLost,    // a pawn lost a body part (a PawnInjured does not come with it)
    PawnDowned,      // a pawn can no longer act (pain shock, unconscious, or can't move)
    PawnGotUp,       // a downed pawn recovered enough to act again
    PawnDied,
    PawnAction,      // a pawn did something with its hands that Unreal animates (picked up, put down)
    JobStarted,      // a pawn took a job (docs/design/jobs.md)
    JobEnded,        // a pawn's job ended: `message` says "done", "failed: why" or "interrupted: why"
    PawnAte,         // a pawn finished eating (docs/design/needs.md)
    PawnWokeUp,      // a pawn got up after sleeping
    FrameStarted,    // a blueprint got its first materials and is a frame now (docs/design/construction.md)
    PawnMoodBand,    // a pawn's mood crossed a mental break threshold (docs/design/mood.md)
    CellMined,       // a rock or ore cell was dug out (docs/design/mining.md)
    PlantRemoved,    // a plant was felled or cut down
    PlantHarvested,  // a plant was picked and stays, grown back to a lower stage
    DesignationsChanged,  // marks were added, removed or finished: read the snapshot's designations
    ShotFired,       // a pawn fired a projectile (docs/design/combat.md)
    ProjectileHit,   // a projectile stopped: it hit something, or flew off the map or too long
    RecipeDone,      // a pawn finished one run of a bill (docs/design/bills.md)
    PowerChanged,    // a building gained or lost power (docs/design/power.md)
    ShortCircuit,    // a short circuit emptied a net's batteries (docs/design/power.md)
};

// What stopped a projectile (ProjectileHit).
enum class ProjectileHitKind : uint8_t {
    None,      // (trace only) nothing in the way
    Ground,    // the ground
    Rock,      // a rock cell (mountain)
    Building,  // a building (`target`); it lost `amount` hit points
    Pawn,      // a pawn (`target`) in the body part `part`; health events follow
    Armor,     // a pawn's armor stopped it (`target`, `part`)
    Gone,      // flew off the map or for too long, hitting nothing
};

// What a PawnAction event's pawn did. Later jobs add theirs (chop, mine, build, eat); Unreal plays
// an animation for each, and a short default one for any it doesn't know.
enum class PawnAction : uint8_t {
    PickUp,   // took `target` off the map to carry it
    PutDown,  // put the carried `target` down (it gets a ThingSpawned too), or the stack it was added to
    Eat,      // started eating the carried `target`
    LieDown,  // lay down to sleep in the bed `target`, or on the ground (no target)
    GetUp,    // got up from sleeping
    Build,    // started work on `target`, a frame (once per job; the work itself takes game seconds)
    Deconstruct,  // started taking down `target`, a building
    Mine,     // swung the pick at the rock in `cell`: once for every hit, for sounds and dust
    Cut,      // started cutting down or felling the plant in `cell`; the work goes on until its JobEnded
    Harvest,  // started picking from the plant in `cell`
    Reload,   // started reloading the gun `target` (docs/design/aiming.md); it is ready again when shotReadyIn is 0
    Dodge,    // sidestepped into `cell` out of the way of the projectile `target`
    Cook,         // started work on a bill at `target`, a work building (once per run of the bill)
    Clean,        // started cleaning the filth in `cell`; a layer comes off every few seconds until its JobEnded
};

// One flat struct for every event type; each type uses only the fields named for it.
struct Event {
    EventType type = EventType::ThingSpawned;
    Entity thing = kNoEntity;  // ThingSpawned, ThingDespawned, ThingDestroyed; PawnAction, Job*: the pawn
    DefId def;                 // ThingSpawned, ThingDespawned, ThingDestroyed; Job*: the pawn's def
    CellPos cell;              // ThingSpawned: its cell (for a building, the footprint origin); Job*: the job's cell
    JobType job = JobType::None;  // JobStarted, JobEnded
    uint64_t command = 0;      // CommandFailed: the id Sim::submit returned
    std::string message;       // CommandFailed: why, in plain words; PawnDied: the cause ("blood loss"); JobEnded: how it ended
    PawnAction action = PawnAction::PickUp;  // PawnAction
    Entity target = kNoEntity;               // PawnAction: the thing it was done to; Job*: what the job is about, if any
    // PawnInjured: `def` is the injury's Hediffs def; PawnPartLost: the Hediffs def is MissingBodyPart.
    // `thing` is the pawn, `cell` where it stands, `part` the index of the body part in its Bodies
    // def (depth-first, torso 0), `amount` the hit points of damage dealt to it.
    int part = -1;
    // PawnAte: `thing` the pawn, `def` the food's Things def, `amount` the nutrition gained, `cell`
    // where it stood and `message` "table" (in a chair at a table) or "ground". PawnWokeUp: `thing`
    // the pawn, `target` the bed it slept in or none, `message` "bed" or "ground", `amount` its rest.
    // PawnMoodBand: `thing` the pawn, `message` the new band ("stable", "minor", "major", "extreme"),
    // `amount` its mood.
    // CellMined: `cell` the cell, `def` the rock or ore's Things def, `amount` its hit points. The
    // chunk or ore it left comes as a ThingSpawned. PlantRemoved, PlantHarvested: `cell`, `def` the
    // plant's Things def, `amount` how many items it gave (they come as a ThingSpawned); `message`
    // "felled" (a tree) or "cut" for removed, "picked" for harvested. PawnAction Mine, Cut, Harvest:
    // `thing` the pawn, `cell` the cell worked on, `def` the pawn's def.
    // RecipeDone: `thing` the pawn, `def` the Recipes def, `target` the work building, `cell` where the
    // pawn stood, `amount` how many products were made, `message` "bill finished" if that was the
    // bill's last run (a "Do X times" bill that reached 0, which is removed), else empty.
    // PowerChanged: `thing` the building (a generator, consumer or battery; not conduits), `def` its
    // def, `cell` its origin, `message` "on" (in a net that works and switched on) or "off".
    // ShortCircuit: `thing` the building it happened at, `cell` its origin, `amount` the energy
    // released from the net's batteries, Wd.
    float amount = 0.0f;
    // ShotFired: `thing` the shooter, `def` its gun's Things def, `target` the projectile, (x, y, z)
    // the muzzle. ProjectileHit: `thing` the projectile (forget its id), `def` its Projectiles def,
    // `hit` what stopped it, `target` the pawn or building hit, `part` the body part hit (index as
    // above), `amount` the damage dealt after armor, (x, y, z) where. Metres, z above sea level.
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    ProjectileHitKind hit = ProjectileHitKind::None;
};

COLONY_API const char* eventTypeName(EventType type);
COLONY_API const char* pawnActionName(PawnAction action);
COLONY_API const char* projectileHitKindName(ProjectileHitKind kind);

}  // namespace colony
