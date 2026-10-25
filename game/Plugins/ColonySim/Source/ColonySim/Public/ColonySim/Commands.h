#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Grid.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Zones.h"

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

// Dev command: destroys a thing. Fails if it no longer exists. Jobs using it end or fail.
struct DestroyThingCommand {
    Entity thing = kNoEntity;
};

// Dev command: hurts a pawn, like RimWorld's dev-mode damage tool. `damageType` is a DamageTypes
// def ("Bullet", "Cut", "Blunt"...), `amount` hit points of damage, `part` an index into the
// pawn's body (Bodies def, depth-first, torso 0) or -1 to pick at random as a hit would. Fails if
// the pawn has no body, is dead, the type is unknown or the part is missing. Real damage comes
// from combat (F2), not from commands.
struct DamagePawnCommand {
    Entity pawn = kNoEntity;
    std::string damageType;
    float amount = 0.0f;
    int part = -1;
};

// Dev command: tends all of a pawn's untended injuries at this quality (0 to 1 and up), as a
// doctor will once medicine is in. Fails if the pawn has no body, is dead, or has nothing to tend.
struct TendPawnCommand {
    Entity pawn = kNoEntity;
    float quality = 1.0f;
};


// Work tab: sets a pawn's priority for a work type (a WorkTypes defName): 0 off, 1 first to 4 last.
// Fails for a pawn that doesn't work, an unknown work type, or work the pawn can never do.
struct SetWorkPriorityCommand {
    Entity pawn = kNoEntity;
    std::string workType;
    int priority = 3;
};

// Makes a stockpile zone of the cells in `area` that can hold one (walkable, in no zone yet).
// Its id is the next zone id: 1, 2, 3... in the order zones are made. Fails if no cell qualifies.
struct CreateStockpileCommand {
    CellRect area;
    StoragePriority priority = StoragePriority::Normal;
};

// Takes the cells in `area` out of every zone; zones left empty are deleted. Fails if no cell was
// in a zone.
struct RemoveZoneCellsCommand {
    CellRect area;
};

// Fails for an unknown zone or the Unstored priority.
struct SetStockpilePriorityCommand {
    ZoneId zone = kNoZone;
    StoragePriority priority = StoragePriority::Normal;
};

// Lets a stockpile take an item def (a Things defName), or not.
struct SetStockpileAllowedCommand {
    ZoneId zone = kNoZone;
    std::string def;
    bool allowed = true;
};

// A player order, RimWorld's "prioritize hauling": the pawn drops what it is doing and hauls this
// item to the best stockpile. Fails if it can't (unreachable, no room, another pawn has it).
struct OrderHaulCommand {
    Entity pawn = kNoEntity;
    Entity thing = kNoEntity;
};

// Schedule tab: what a pawn does in one hour of the day (0 to 23). Fails for a pawn with no
// schedule (it needs food and rest, so the defs must have a NeedRules def) or a bad hour.
struct SetScheduleCommand {
    Entity pawn = kNoEntity;
    int hour = 0;
    ScheduleKind kind = ScheduleKind::Anything;
};

// Bed ownership: `pawn` owns `bed` (and gives up any other bed). `pawn` kNoEntity takes every
// owner off the bed. Fails if the bed is full or either is not what it should be.
struct AssignBedCommand {
    Entity bed = kNoEntity;
    Entity pawn = kNoEntity;
};

using Command = std::variant<SpawnThingCommand, DestroyThingCommand, DamagePawnCommand, TendPawnCommand,
                             SetWorkPriorityCommand, CreateStockpileCommand, RemoveZoneCellsCommand,
                             SetStockpilePriorityCommand, SetStockpileAllowedCommand, OrderHaulCommand,
                             SetScheduleCommand, AssignBedCommand>;

// A command as applied: `step` is the game second (Sim::time().seconds) of the step it was
// applied in. Failed commands are logged too.
struct LoggedCommand {
    int64_t step = 0;
    Command command;
};

// Name used in replay files and logs, e.g. "SpawnThing".
COLONY_API const char* commandName(const Command& command);

}  // namespace colony
