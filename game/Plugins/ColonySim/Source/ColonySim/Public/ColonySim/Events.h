#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Grid.h"

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
};

// What a PawnAction event's pawn did. Later jobs add theirs (chop, mine, build, eat); Unreal plays
// an animation for each, and a short default one for any it doesn't know.
enum class PawnAction : uint8_t {
    PickUp,   // took `target` off the map to carry it
    PutDown,  // put the carried `target` down (it gets a ThingSpawned too)
};

// One flat struct for every event type; each type uses only the fields named for it.
struct Event {
    EventType type = EventType::ThingSpawned;
    Entity thing = kNoEntity;  // ThingSpawned, ThingDespawned, ThingDestroyed; PawnAction: the pawn
    DefId def;                 // ThingSpawned, ThingDespawned, ThingDestroyed
    CellPos cell;              // ThingSpawned: its cell (for a building, the footprint origin)
    uint64_t command = 0;      // CommandFailed: the id Sim::submit returned
    std::string message;       // CommandFailed: why, in plain words; PawnDied: the cause ("blood loss")
    PawnAction action = PawnAction::PickUp;  // PawnAction
    Entity target = kNoEntity;               // PawnAction: the thing it was done to, if any
    // PawnInjured: `def` is the injury's Hediffs def; PawnPartLost: the Hediffs def is MissingBodyPart.
    // `thing` is the pawn, `cell` where it stands, `part` the index of the body part in its Bodies
    // def (depth-first, torso 0), `amount` the hit points of damage dealt to it.
    int part = -1;
    float amount = 0.0f;
};

COLONY_API const char* eventTypeName(EventType type);
COLONY_API const char* pawnActionName(PawnAction action);

}  // namespace colony
