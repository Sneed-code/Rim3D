#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Designations.h"
#include "ColonySim/Cover.h"
#include "ColonySim/BillMode.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Grid.h"
#include "ColonySim/Needs.h"
#include "ColonySim/WeaponSlot.h"
#include "ColonySim/Zones.h"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

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

// The player's colonist takes the stairs or ladder it stands at (U17, docs/design/levels.md): from
// the cell at either end of a level link, the link whose other end lies most nearly along (dx, dy)
// (any length; zero or not a number takes the first). The sim climbs it the way it climbs any pawn,
// at the link's share of its walking speed, and DirectMove is ignored until it stands at the other
// end. Fails if the pawn isn't the colonist under direct control, is downed, rolling or climbing
// already, or no link starts at its cell within 60 degrees of that direction.
struct ClimbCommand {
    Entity pawn = kNoEntity;
    float dx = 0.0f;
    float dy = 0.0f;
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

// Throws a grenade (task F9, docs/design/explosives.md): the pawn winds up, then throws one grenade of
// `item` (a Things defName; empty for the first kind on its belt) at the ground under `target` if that
// is a pawn on the map, else at the point (x, y) in metres. `player` is the protagonist's own throw: no
// wind-up. Fails if the pawn has no such grenade, is downed, knocked down or mid-shot, or is still
// cooling down from its last throw.
struct ThrowCommand {
    Entity pawn = kNoEntity;
    std::string item;
    Entity target = kNoEntity;
    float x = 0.0f;
    float y = 0.0f;
    bool player = false;
};

// Dev command: a blast now at (x, y) on the ground, as if a grenade went off there. `damageType` is a
// DamageTypes defName ("Bomb"). Fails for an unknown type, a point off the map or a radius or damage
// of 0 or less.
struct ExplodeCommand {
    float x = 0.0f;
    float y = 0.0f;
    float radius = 1.9f;
    float damage = 50.0f;
    std::string damageType = "Bomb";
    float penetration = 0.1f;
};

// Raids (F6, docs/design/raids.md). Dev command until the narrator sends raids: a raid of
// `faction` (a Factions defName) worth `points` raid points (0: what the colony is worth now, by
// RimWorld's rule) walks in at `edge` (0 north, 1 east, 2 south, 3 west, -1 any) and attacks.
// Fails for an unknown faction or edge, or when there is no way in from that edge.
struct StartRaidCommand {
    std::string faction = "Pirate";
    float points = 0.0f;
    int edge = -1;
};

// The colony alarm: on calls every colonist to its defense role, off sends them back to work. It
// also sounds by itself when a colonist spots an enemy, and stops when the enemies are gone or
// fleeing; one the player sounded stays on until the player stops it. Fails if it already is.
struct SoundAlarmCommand {
    bool on = true;
};

// What a colonist does when the alarm sounds: 0 Defender, 1 Shelter, 2 Keep working (DefenseRole
// in Raids.h). A Defender can have a defense spot it holds. Fails for a pawn that isn't the
// colony's, a spot for another role, or a spot nobody can stand on.
struct SetDefenseRoleCommand {
    Entity pawn = kNoEntity;
    uint8_t role = 0;
    bool hasSpot = false;
    CellPos spot;
};

// Marks the room at `cell` as the shelter for Shelter colonists, or with `clear` takes the mark off.
// Fails if the cell is not in a room under a roof.
struct MarkShelterCommand {
    CellPos cell;
    bool clear = false;
};

// Taming, training and pens (task N3, docs/design/husbandry.md). `mark` is "Tame" (a wild animal, to be tamed by a colonist
// with the Handling work), "Hunt" (a wild one, to be shot by a colonist with the Hunting work and a gun; N2,
// docs/design/hunting.md), "Slaughter" (a tame one, to be killed) or "Release" (a tame one goes wild at once; `on` is
// ignored). `on` false takes a mark off. Fails for a thing that is no animal, a dead one, or one of the wrong kind.
struct MarkAnimalCommand {
    Entity animal = kNoEntity;
    std::string mark;
    bool on = true;
};

// Marks every animal in `area` that can take the mark (a wild one for "Tame" or "Hunt", a tame one for "Slaughter"); the
// others are skipped. Fails if there is none.
struct MarkAnimalsInAreaCommand {
    CellRect area;
    std::string mark;
    bool on = true;
};

// Names a tame animal (up to 24 characters; an empty name takes the name off). Fails for a wild or dead animal.
struct SetAnimalNameCommand {
    Entity animal = kNoEntity;
    std::string name;
};

// A tame animal is (or is no longer) to be taught a trick: `trainable` is "Obedience", "Release", "Rescue" or "Haul".
// Fails if the animal is wild or can't learn it.
struct SetTrainingCommand {
    Entity animal = kNoEntity;
    std::string trainable;
    bool wanted = true;
};

// Paints an animal zone over the walkable cells of `area` that are in no animal zone. Fails if there are none.
struct CreateAnimalZoneCommand {
    CellRect area;
};

// Takes the cells of `area` out of their animal zones; an emptied zone is deleted, and animals that had it roam freely.
struct RemoveAnimalZoneCellsCommand {
    CellRect area;
};

// A tame animal stays in this animal zone; zone 0 lets it roam near home again.
struct AssignAnimalZoneCommand {
    Entity animal = kNoEntity;
    uint32_t zone = 0;
};

// Switches breeding on or off: for one tame animal, or (with no animal) for every animal in a zone.
struct SetBreedingCommand {
    Entity animal = kNoEntity;
    uint32_t zone = 0;
    bool allow = true;
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

// Clothes (task C15, docs/design/apparel.md). A pawn follows an outfit (an id from the snapshot's outfits):
// it takes off what the outfit doesn't allow and puts on the best clothes it may wear. Fails if the thing
// isn't a pawn or there is no such outfit.
struct SetOutfitCommand {
    Entity pawn = kNoEntity;
    uint32_t outfit = 1;
};

// Changes an outfit: the apparel defs it doesn't allow (a Things defName each), the share of its hit
// points apparel must have (0 to 1) and the quality range (0 awful to 6 legendary). Fails if there is no
// such outfit or a value is wrong.
struct EditOutfitCommand {
    uint32_t outfit = 1;
    std::vector<std::string> disallowed;
    float hitPointsMin = 0.0f;
    float hitPointsMax = 1.0f;
    int qualityMin = 0;
    int qualityMax = 6;
};

// A new outfit with the given name that allows everything. Its id is the next one (the snapshot lists it).
struct AddOutfitCommand {
    std::string name;
};

// Removes an outfit; pawns that followed it go back to the default. Fails for the first outfit.
struct RemoveOutfitCommand {
    uint32_t outfit = 0;
};

// Marks a downed or dead pawn to be stripped of its clothes and weapon by a colonist with Hauling work
// (`strip` true), or takes the mark off. Fails if it isn't a pawn on the map, isn't downed or dead, or
// has nothing on it.
struct StripCommand {
    Entity thing = kNoEntity;
    bool strip = true;
};

// Dev command (C20, docs/design/social.md): `initiator` does the Interactions def `interaction` to `recipient`
// now, whatever the dice and the opinion limits would have said. `outcome` is 1 to make a roll that could go
// either way (a romantic gesture, a proposal) work, 0 to make it fail, -1 to leave it to the dice. Fails if
// the interaction is unknown, either pawn can't talk now (dead, downed, asleep, fighting, drafted, under the
// player's hand), they are out of earshot, or the interaction needs what is not there (a lover).
struct InteractCommand {
    Entity initiator = kNoEntity;
    Entity recipient = kNoEntity;
    std::string interaction;
    int outcome = -1;
};

// The protagonist's dodge roll (P2, docs/design/shooter-feel.md): a start-up, a roll of a few metres
// in the direction (dx, dy) (any length; not a number, or zero, is a backstep away from where the
// pawn faces) in which bullets pass through the pawn for a short window, then a recovery in which it
// can be hit again but can't move or shoot, then a short wait before the next roll. The sim moves
// the pawn, so Unreal plays the animation on top and never moves the body itself. Fails if the pawn
// isn't a colonist under direct control, is downed, lying prone, already rolling, or waiting out the
// last roll.
struct DodgeRollCommand {
    Entity pawn = kNoEntity;
    float dx = 0.0f;
    float dy = 0.0f;
};

// Medicine (task H1, docs/design/medicine.md): how far a colonist's medicine may go. `care` is a name from
// medicalCareName: NoCare, NoMedicine, Herbal, Normal or Best. Fails for a pawn without a body or a name that
// isn't one.
struct SetMedicalCareCommand {
    Entity pawn = kNoEntity;
    std::string care;
};

// Prisoners (task C22, docs/design/prisoners.md): what wardens do with a prisoner. `mode` is NoInteraction, ReduceResistance,
// AttemptRecruit or Release (prisonerModeName). Fails for a pawn that is no prisoner, or a name that isn't one; Release also fails
// for a prisoner who can't walk.
struct SetPrisonerModeCommand {
    Entity pawn = kNoEntity;
    std::string mode;
};

// Marks a bed as a prison bed (only prisoners sleep in it, and downed enemies are carried to it) or takes the mark off. Fails for a
// building that is no bed, or a bed someone not fit for it already owns.
struct SetPrisonBedCommand {
    Entity bed = kNoEntity;
    bool on = true;
};

// Dev command (task H2, docs/design/diseases.md): the pawn catches a disease (a Hediffs def of class Disease, by
// defName) at `severity` (below 0: the def's initial severity); an infection needs the wound's body part in `part`.
// Fails for a pawn without a body, a name that is not a disease, or when the pawn already has it or is immune.
struct ContractDiseaseCommand {
    Entity pawn = kNoEntity;
    std::string disease;
    float severity = -1.0f;
    int part = -1;
};

// Dev command: the weather becomes this Weathers def now (by defName, e.g. "Rain") and lasts `hours`
// game hours (0: its usual length), then the next is chosen as usual (docs/design/weather.md).
// Fails if no Weathers def has the name.
struct SetWeatherCommand {
    std::string weather;
    float hours = 0.0f;
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

// Traps (task F11, docs/design/defense.md): sets a sprung spike trap again. Fails for a thing that is not a trap,
// a trap that is armed already, or one that can't be used twice (an IED). No colonist job rearms traps yet.
struct RearmTrapCommand {
    Entity trap = kNoEntity;
};

// Several weapons (task P1, docs/design/weapons.md): adds a weapon to what the pawn carries, in the hands
// if they are empty, else on the body; the weapon carried in its slot is dropped. The item may lie on the
// map. Fails if the pawn has no body or is dead, or the item is no weapon or is someone else's.
struct CarryWeaponCommand {
    Entity pawn = kNoEntity;
    Entity item = kNoEntity;
};

// Switches to the weapon carried in `slot`: it takes a while (CombatRules `weaponSwitchSteps`) in which the
// pawn can't shoot, reload or swing, then the weapon is in the hands and the old one on the body. Fails if
// nothing is carried in that slot, it is in the hands already, or the pawn is downed, rolling, mid-shot,
// mid-swing or switching already.
struct SwitchWeaponCommand {
    Entity pawn = kNoEntity;
    WeaponSlot slot = WeaponSlot::Primary;
};

// Growing zones (task C13, docs/design/farming.md). CreateGrowingZone paints a zone with `crop` (a Things def of a
// sowable plant, like "Plant_Rice") over the cells of `area` that are soil, free and in no zone; fails if there are none.
struct CreateGrowingZoneCommand {
    CellRect area;
    std::string crop;
};

// Takes the cells of `area` out of their growing zones; an emptied zone is deleted. Fails if none was in a zone.
struct RemoveGrowingZoneCellsCommand {
    CellRect area;
};

// Changes the crop of a growing zone; plants of the old crop are cut and replaced as colonists get to them.
struct SetGrowingZoneCropCommand {
    uint32_t zone = 0;
    std::string crop;
};

// Picks the narrator (a Narrators defName: "Orrin", "Maren", "Jinx"; "" turns the narrator off) and
// the difficulty (a Difficulties defName; "" keeps it). The narrator is off until this is sent: the
// game sends it at the start (docs/design/narrator.md). Fails for an unknown name.
struct SetNarratorCommand {
    std::string narrator = "Orrin";
    std::string difficulty;
};

// A dev command: makes an incident (an Incidents defName, like "RaidEnemy") happen now, whatever its
// timing, with `points` threat points (0: the colony's). Fails if it is unknown or can't happen now.
struct FireIncidentCommand {
    std::string incident;
    float points = 0.0f;
};

// Fire (task E4, docs/design/fire.md). Dev command: starts a fire of `size` (the start size if 0) in a cell, or sets `pawn`
// alight if it names one. Fails if nothing could burn: no fuel, wet ground, water or rock, a fire already there, a pawn
// that is dead or fire-resistant. `force` lights even a cell with nothing to burn (it dies away in a moment).
struct IgniteCommand {
    CellPos cell;
    float size = 0.0f;
    Entity pawn = kNoEntity;
    bool force = false;
};

// Paints the home area, where colonists with the Firefighter work type put fires out: adds the rectangle (or takes it
// off). The first use turns the automatic growth round new buildings off. Fails if no cell changed.
struct SetHomeAreaCommand {
    CellRect area;
    bool add = true;
};

// Multi-storey (task M1, docs/design/levels.md): lays an upper floor on every cell of `area` (on
// level 1 or above) that can hold one, or takes them up with `remove`. A floor needs a wall or column
// within the roof support radius on the level below; one is taken up only when nothing stands on it.
// Instant for now: the work to build one comes with stairs (M2). Fails, with the last reason, if no
// cell changed.
struct PlaceUpperFloorCommand {
    CellRect area;
    bool remove = false;
};

// Fuel (task C11, docs/design/fuel.md): whether colonists refuel the building by themselves when it
// is low (`autoRefuel`), and `fillNow`, an order to fill it to the top regardless. Fails if the thing
// is not a finished building that burns fuel.
struct SetRefuelCommand {
    Entity building = kNoEntity;
    bool autoRefuel = true;
    bool fillNow = false;
};

// Changes the details of a bill (task C14, docs/design/crafting.md): the least and most skill a worker may
// have in the recipe's skill, how far from the building ingredients are taken (metres, 0 anywhere), a stockpile
// the products are taken to (0: none), the stock an "until you have" bill waits to fall under before it starts
// again (0: as soon as it is under its count) and the item defNames the bill may not use. Replaces all of them.
// Fails if there is no such bill, a number is out of range, the stockpile does not exist or a name is not an item.
struct SetBillOptionsCommand {
    Entity building = kNoEntity;
    uint32_t bill = 0;
    int skillMin = 0;
    int skillMax = 20;
    float radius = 0.0f;
    uint32_t storeZone = 0;
    int unpauseAt = 0;
    std::vector<std::string> disallowed;
};

// Moves a bill to a place in the building's list (0 first, tried first). Fails if there is no such bill.
struct MoveBillCommand {
    Entity building = kNoEntity;
    uint32_t bill = 0;
    int index = 0;
};

// Dev command: the colonist has a mental break now (a MentalBreaks defName, docs/design/mental-breaks.md),
// whatever its mood. Fails for a pawn that isn't a colonist on the map, is controlled by the player,
// downed, dead or in a break already; for a break that needs something the game doesn't have yet
// (fire); or when the break has nothing to do (a binge with no food).
struct StartMentalBreakCommand {
    Entity pawn = kNoEntity;
    std::string breakDef;
};

// Dev command: ends the colonist's mental break at once, with no catharsis. Fails if it isn't in one.
struct EndMentalBreakCommand {
    Entity pawn = kNoEntity;
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
                             SetResearchCommand, UnlockResearchCommand, StartRaidCommand, SoundAlarmCommand,
                             SetDefenseRoleCommand, MarkShelterCommand, ThrowCommand, ExplodeCommand,
                             MeleeCommand, DodgeRollCommand,
                             SetOutfitCommand, EditOutfitCommand, AddOutfitCommand, RemoveOutfitCommand,
                             StripCommand, SetWeatherCommand, MarkAnimalCommand, SetTrainingCommand,
                             MarkAnimalsInAreaCommand, SetAnimalNameCommand,
                             CreateAnimalZoneCommand, RemoveAnimalZoneCellsCommand, AssignAnimalZoneCommand, SetBreedingCommand,
                             SpendTalentPointCommand, AddTalentXpCommand, RearmTrapCommand,
                             CarryWeaponCommand, SwitchWeaponCommand, CreateGrowingZoneCommand,
                             RemoveGrowingZoneCellsCommand, SetGrowingZoneCropCommand, IgniteCommand,
                             SetHomeAreaCommand, SetMedicalCareCommand, InteractCommand, SetPrisonerModeCommand,
                             SetPrisonBedCommand, PlaceUpperFloorCommand, SetRefuelCommand, SetBillOptionsCommand,
                             MoveBillCommand, StartMentalBreakCommand, EndMentalBreakCommand,
                             SetNarratorCommand, FireIncidentCommand, ContractDiseaseCommand, ClimbCommand>;

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
