#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Designations.h"
#include "ColonySim/Cover.h"
#include "ColonySim/BillMode.h"
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

// Direct control (task U9, docs/design/protagonist.md): the player takes over one colonist. Its
// job ends and the job system leaves it alone until control is let go. Only one colonist is
// controlled at a time: taking a second lets the first go. `pawn` = kNoEntity lets go of whoever is
// controlled. Fails if the thing is not a living colonist on the map.
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

// Plans a building (RimWorld's blueprint): `def` is a Things building, `stuff` the Things item it is
// made of ("" for a building that takes no stuff, like the stove), `cell` where its footprint starts
// (lowest x and y), `rot` which way it faces. Pawns with Construction work bring the materials and
// build it (docs/design/construction.md). Fails for an unknown def or stuff, a stuff that doesn't
// fit, or a footprint that is off the map, on water or rock, on a building or on another plan.
struct PlaceBlueprintCommand {
    std::string def;
    std::string stuff;
    CellPos cell;
    Rot4 rot = Rot4::North;
};

// Takes back a plan: a blueprint or a frame goes away and what was delivered to it is put on the
// ground; a building marked for deconstruction is unmarked. Fails if `thing` is none of those.
struct CancelBuildCommand {
    Entity thing = kNoEntity;
};

// Marks a finished building to be taken down; a pawn with Construction work does it and the
// materials come back (half of them). Fails if `thing` isn't a building, or is marked already.
struct DeconstructCommand {
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

// Marks the cells in `area` for work (task C3, docs/design/mining.md): Mine marks rock and ore,
// CutPlant any plant, HarvestPlant plants that give something. Cells that don't suit the kind are
// left out; a cell already marked changes to this kind. Fails if no cell qualified. The designation
// is the player's order: any colonist with the work type switched on takes it, nearest first.
struct DesignateCommand {
    CellRect area;
    DesignationKind kind = DesignationKind::Mine;
};

// Takes the marks off the cells in `area`; with a kind, only marks of that kind. Fails if there
// was none. A colonist working on a cancelled mark stops.
struct CancelDesignationsCommand {
    CellRect area;
    DesignationKind kind = DesignationKind::None;  // None: every kind
};

// Fighter AI (F4, docs/design/fighters.md). Drafts one of the player's pawns (its job ends and it
// fights: takes cover, shoots the nearest threat, falls back when outnumbered), or with `drafted`
// false sends it back to work. Fails if it isn't the player's pawn on the map, is downed, or is
// drafted (undrafted) already.
struct DraftCommand {
    Entity pawn = kNoEntity;
    bool drafted = true;
};

// A drafted pawn goes to `cell` and holds there, taking the best cover within a couple of metres.
// Fails if the pawn isn't drafted or can't get there.
struct MoveOrderCommand {
    Entity pawn = kNoEntity;
    CellPos cell;
};

// A drafted pawn shoots `target` whenever it can; kNoEntity lets it pick its own targets again.
// Fails if the pawn isn't drafted or the target isn't a living pawn on the map.
struct AttackOrderCommand {
    Entity pawn = kNoEntity;
    Entity target = kNoEntity;
};

// Dev command until raids (F6) bring real factions: puts a pawn on a side. Every faction other
// than the player's (1) is hostile to all others; its pawns fight at once and advance on their
// enemies. Back on side 1 the pawn works again. Fails for faction 0 or a thing that isn't a pawn.
struct SetFactionCommand {
    Entity pawn = kNoEntity;
    uint32_t faction = 1;
};

// Dev command: makes a humanlike pawn one of another lineage (a Lineages defName), as a lineage
// graft will once there are grafts. Only its looks change (docs/design/looks.md). Fails for a
// pawn without looks or an unknown lineage.
struct SetLineageCommand {
    Entity pawn = kNoEntity;
    std::string lineage;
};

// Reloads the pawn's gun now rather than when it runs dry (F5, docs/design/aiming.md): the
// protagonist's reload key, or a tactical reload. Fails if the pawn has no gun with a magazine, the
// magazine is full, or the pawn is downed, mid-shot or reloading already.
struct ReloadCommand {
    Entity pawn = kNoEntity;
};

// Puts a bill on a work building (the stove): make `recipe` (a Recipes defName) `count` times
// (`Count`), until `count` of the product lie on the map (`UntilHave`), or forever. `count` is
// ignored for Forever. Pawns with the recipe's work type do bills in the order listed
// (docs/design/bills.md). Fails if the building is not a finished building that can make the
// recipe, the recipe or count is wrong, or the building already has the most bills it can hold.
struct AddBillCommand {
    Entity building = kNoEntity;
    std::string recipe;
    BillMode mode = BillMode::Count;
    int count = 1;
};

// Takes a bill off a building by the id the snapshot shows. Fails if there is no such bill.
struct RemoveBillCommand {
    Entity building = kNoEntity;
    uint32_t bill = 0;
};

// Changes a bill's mode and count and whether it is suspended (paused). Fails like AddBill.
struct SetBillCommand {
    Entity building = kNoEntity;
    uint32_t bill = 0;
    BillMode mode = BillMode::Count;
    int count = 1;
    bool suspended = false;
};

// Hands-on work (task U10, docs/design/handson.md): what the controlled colonist does with its
// hands. Work: start the sim's own job on what the player looks at, in place (no walking): `thing` is
// a blueprint, a frame or an item, else `cell` is a rock to mine or a plant to chop or pick. The job
// runs while the player stays within kHandsOnReach and ends when it is done, or on Stop. Picking up
// is instant. PutDown: set down what the colonist carries on `cell` (else near its feet). Fails if
// the pawn is not the controlled one, the target is out of reach or taken, or there is nothing to do.
enum class HandsOnMode : uint8_t { Work, PutDown, Stop };

struct HandsOnCommand {
    Entity pawn = kNoEntity;
    HandsOnMode mode = HandsOnMode::Work;
    Entity thing = kNoEntity;
    CellPos cell;
};

// Lays a floor (a Terrain def whose designationCategory is Floors, by defName) on every cell of `area`
// that can take one (walkable ground), replacing what is there; an empty `floor` takes floors off
// instead. Instant for now: the architect's floor tool and the work to lay one come later
// (docs/design/room-stats.md). Fails if no cell changed or the floor is unknown.
struct PlaceFloorCommand {
    CellRect area;
    std::string floor;
};

// Dev command: adds `layers` of filth (a Filth defName: Dirt, Blood, Ash...) to a cell. Fires, fights
// and spills make filth through the sim; this is for tests and the console. Fails for an unknown kind
// or a cell that can't hold filth (rock, water, a wall) or is already as thick as it gets.
struct AddFilthCommand {
    CellPos cell;
    std::string kind;
    int layers = 1;
};

// How far from the edge of a rock, plant, frame or item a controlled colonist can work on it, in metres.
inline constexpr float kHandsOnReach = 2.0f;

// Power (task E5, docs/design/power.md): opens or closes a switch, or flicks a consumer or generator on
// or off. Fails if the building has nothing to switch.
struct FlickCommand {
    Entity thing = kNoEntity;
    bool on = true;
};

// Dev command, and what the events task T3 will send: a short circuit at a building on a net. Empties
// the net's batteries and destroys the building if it is a conduit or a switch.
struct ShortCircuitCommand {
    Entity thing = kNoEntity;
};

// Research (task C21, docs/design/research.md): picks the project the colony works on; an empty name puts
// the current one down (its progress is kept). Fails for a project that doesn't exist, is done or has
// prerequisites missing.
struct SetResearchCommand {
    std::string project;
};

// Dev command: a project is done at once, whatever it needs. Fails for a project that doesn't exist.
struct UnlockResearchCommand {
    std::string project;
};

// Melee (task F8, docs/design/melee.md). Attack: `pawn` swings at `target`, a pawn within reach;
// for the colonist the player controls, `target` may be none for a swing straight ahead, and its
// swings have no spread (the player aims). Block: `pawn` raises its melee weapon for a moment.
// Fails if the pawn can't swing or block now (downed, mid-shot, still swinging, out of reach, no
// melee weapon to block with).
enum class MeleeMode : uint8_t { Attack, Block };

struct MeleeCommand {
    Entity pawn = kNoEntity;
    Entity target = kNoEntity;
    MeleeMode mode = MeleeMode::Attack;
};

// Talents (task P3, docs/design/talents.md): the protagonist spends talent points on one more rank of a
// talent. Fails for a pawn that has no talents, a talent that doesn't exist or is at its top rank, a
// requirement missing, or too few points.
struct SpendTalentPointCommand {
    Entity pawn = kNoEntity;
    std::string talent;
};

// Dev command: adds talent XP to the protagonist.
struct AddTalentXpCommand {
    Entity pawn = kNoEntity;
    int64_t amount = 0;
};

// New commands go at the end: a save names a command by its place in this list.
using Command = std::variant<SpawnThingCommand, DestroyThingCommand, DamagePawnCommand, TendPawnCommand,
                             SetWorkPriorityCommand, CreateStockpileCommand, RemoveZoneCellsCommand,
                             SetStockpilePriorityCommand, SetStockpileAllowedCommand, OrderHaulCommand,
                             ControlPawnCommand, DirectMoveCommand, SetScheduleCommand, AssignBedCommand,
                             PlaceBlueprintCommand, CancelBuildCommand, DeconstructCommand, EquipCommand,
                             ShootCommand, SetStanceCommand, DesignateCommand, CancelDesignationsCommand,
                             DraftCommand, MoveOrderCommand, AttackOrderCommand, SetFactionCommand, SetLineageCommand,
                             ReloadCommand, AddBillCommand,
                             RemoveBillCommand, SetBillCommand, HandsOnCommand, FlickCommand, ShortCircuitCommand,
                             PlaceFloorCommand, AddFilthCommand,
                             SetResearchCommand, UnlockResearchCommand,
                             MeleeCommand,
                             SpendTalentPointCommand, AddTalentXpCommand>;

// A command as applied: `step` is the game second (Sim::time().seconds) of the step it was
// applied in. Failed commands are logged too.
struct LoggedCommand {
    int64_t step = 0;
    Command command;
};

// Name used in replay files and logs, e.g. "SpawnThing".
COLONY_API const char* commandName(const Command& command);
// The other way: sets `out` to a command of the type with that name, its fields at their defaults.
// False if no command type has that name. Saves name commands this way (docs/design/saves.md), so
// the order of the types in `Command` doesn't matter to any file.
COLONY_API bool commandFromName(const std::string& name, Command& out);

}  // namespace colony
