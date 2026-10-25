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

// Direct control (task U9, docs/design/protagonist.md): the player takes over one colonist. The
// sim's own routines leave that colonist alone until control is let go (C1's job system will
// cancel its job when it is taken over). Only one colonist is controlled at a time: taking a
// second lets the first go. `pawn` = kNoEntity lets go of whoever is controlled. Fails if the
// thing is not a spawned colonist, or is one of the hauling test's.
struct ControlPawnCommand {
    Entity pawn = kNoEntity;
};

// Where the player's body wants to be (metres, on its level), sent every step while a colonist is
// controlled. The sim moves the pawn there, at most kDirectMaxStep metres a step, and keeps it
// out of walls and off ground it can't walk: a blocked move slides along the wall or stops. The
// pawn's velocity becomes the distance it really moved, which Unreal animates from. Fails if the
// pawn is not the controlled one or the position is not a number.
struct DirectMoveCommand {
    Entity pawn = kNoEntity;
    float x = 0.0f;
    float y = 0.0f;
};

// The farthest a controlled pawn moves in one step: 0.5 m, which is 15 m/s at 1x, over three times
// the fastest pawn. Longer requests are cut to this.
inline constexpr float kDirectMaxStep = 0.5f;

using Command = std::variant<SpawnThingCommand, DestroyThingCommand, ControlPawnCommand, DirectMoveCommand>;

// A command as applied: `step` is the game second (Sim::time().seconds) of the step it was
// applied in. Failed commands are logged too.
struct LoggedCommand {
    int64_t step = 0;
    Command command;
};

// Name used in replay files and logs, e.g. "SpawnThing".
COLONY_API const char* commandName(const Command& command);

}  // namespace colony
