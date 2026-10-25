#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Grid.h"

#include <cstdint>
#include <string>
#include <variant>

namespace colony {

// Unreal to sim: the only way anything outside the sim changes game state. Sim::submit queues a
// command; it is applied at the start of the next step and logged, so a replay (seed plus the
// command log) gives the same game. See docs/design/bridge.md, which says how to add one.

// Dev command: makes a thing from a Things def and puts it on the map, like RimWorld's dev-mode
// spawn. `cell` is where Things::spawn puts it (a building's footprint origin). Fails if the def
// is unknown or the thing can't go there.
struct SpawnThingCommand {
    std::string def;
    CellPos cell;
    int count = 1;
    Rot4 rot = Rot4::North;
};

// Dev command: destroys a thing. Fails if it no longer exists or a job is using it.
struct DestroyThingCommand {
    Entity thing = kNoEntity;
};

using Command = std::variant<SpawnThingCommand, DestroyThingCommand>;

// A command as applied: `step` is the game second (Sim::time().seconds) of the step it was
// applied in. Failed commands are logged too.
struct LoggedCommand {
    int64_t step = 0;
    Command command;
};

// Name used in replay files and logs, e.g. "SpawnThing".
COLONY_API const char* commandName(const Command& command);

}  // namespace colony
