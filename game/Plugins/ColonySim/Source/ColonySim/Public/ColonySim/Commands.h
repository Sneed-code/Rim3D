#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Cover.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Grid.h"
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

// Puts a gun in a pawn's hands or apparel on it (docs/design/combat.md); what it replaces is
// dropped where the pawn stands. The item may lie on the map. Fails if the pawn has no body, is
// dead, or the item is neither a gun nor apparel or is someone else's. Later the job system
// fetches gear itself; this is for the start, tests and dev mode.
struct EquipCommand {
    Entity pawn = kNoEntity;
    Entity item = kNoEntity;
};

// Shoots. With `target` a pawn on the map, at its chest at each shot; otherwise at the point
// (x, y, z) in metres (z above sea level, like Snapshot heights). `player` is the protagonist's
// own trigger: fires at once with the gun's own spread (the player aims). Otherwise the pawn aims
// for the gun's warm-up and its Shooting skill sets the spread. Fails if the pawn has no gun, is
// downed, or is mid-burst or cooling down.
struct ShootCommand {
    Entity pawn = kNoEntity;
    Entity target = kNoEntity;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    bool player = false;
};

// Stands, crouches or lies down: changes the pawn's hitboxes, muzzle height and what hides it
// (docs/design/cover.md). Fails if the thing is not a pawn.
struct SetStanceCommand {
    Entity pawn = kNoEntity;
    Stance stance = Stance::Standing;
};

using Command = std::variant<SpawnThingCommand, DestroyThingCommand, DamagePawnCommand, TendPawnCommand,
                             SetWorkPriorityCommand, CreateStockpileCommand, RemoveZoneCellsCommand,
                             SetStockpilePriorityCommand, SetStockpileAllowedCommand, OrderHaulCommand, EquipCommand,
                             ShootCommand, SetStanceCommand>;

// A command as applied: `step` is the game second (Sim::time().seconds) of the step it was
// applied in. Failed commands are logged too.
struct LoggedCommand {
    int64_t step = 0;
    Command command;
};

// Name used in replay files and logs, e.g. "SpawnThing".
COLONY_API const char* commandName(const Command& command);

}  // namespace colony
