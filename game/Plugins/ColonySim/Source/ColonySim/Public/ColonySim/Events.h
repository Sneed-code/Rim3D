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
};

// One flat struct for every event type; each type uses only the fields named for it.
struct Event {
    EventType type = EventType::ThingSpawned;
    Entity thing = kNoEntity;  // ThingSpawned, ThingDespawned, ThingDestroyed
    DefId def;                 // ThingSpawned, ThingDespawned, ThingDestroyed
    CellPos cell;              // ThingSpawned: its cell (for a building, the footprint origin)
    uint64_t command = 0;      // CommandFailed: the id Sim::submit returned
    std::string message;       // CommandFailed: why, in plain words
};

COLONY_API const char* eventTypeName(EventType type);

}  // namespace colony
