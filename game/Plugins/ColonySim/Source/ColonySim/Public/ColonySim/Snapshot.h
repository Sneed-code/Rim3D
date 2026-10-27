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
#include <string>
#include <utility>
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
    // Pawns on stairs or a ladder: how far along the climb, 0 to 1; x, y and z are already that
    // share of the way to the end, `level` is the one it started on (docs/design/levels.md).
    float climb = 0.0f;
    uint32_t firstWaypoint = 0;
    uint32_t waypointCount = 0;
    Rot4 rotation = Rot4::North;  // buildings
    int sizeX = 1;                // buildings: footprint as placed (rotation already applied)
    int sizeY = 1;
    int stackCount = 0;  // items
    float rot = 0.0f;       // items that rot: 0 fresh to 1 rotted away (docs/design/spoilage.md)
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
    // Buildings that burn fuel (docs/design/fuel.md): items of fuel in it of `fuelCapacity` (0 for a
    // building with none), whether colonists refuel it by themselves, and a fill order from the player.
    float fuel = 0.0f;
    float fuelCapacity = 0.0f;
    bool autoRefuel = false;
    bool fillOrdered = false;
    // Pawns with needs (docs/design/needs.md): food and rest, 1 full to 0 empty, else -1; asleep
    // while lying down in a bed or on the ground.
    float food = -1.0f;
    float rest = -1.0f;
    bool asleep = false;
    // Mood (docs/design/mood.md): 0 to 100, else -1; the break band as a MoodBand (0 stable to 3 extreme).
    float mood = -1.0f;
    int moodBand = 0;
    // Social life (docs/design/social.md): a pawn's spouse, else its lover (none if it has neither), and
    // whether it is in a fight with fists.
    Entity partner = kNoEntity;
    bool socialFight = false;
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
    // Several weapons (P1, docs/design/weapons.md): the weapon def carried in each slot (Primary, Sidearm, Melee;
    // invalid for none), in the hands or on the body, and while the pawn switches the slot it switches to and
    // the steps left (255 and 0 when it does not).
    DefId carried[3];
    uint8_t switchSlot = 255;
    int switchStepsLeft = 0;
    // Pawns (docs/design/explosives.md): grenades on the belt, steps until the next throw can start
    // (0: ready), and whether a blast has thrown the pawn to the ground (it lies prone meanwhile).
    int grenades = 0;
    int throwReadyIn = 0;
    bool knockedDown = false;
    // Pawns: their side (1 is the player's), and for fighters (docs/design/fighters.md) whether the
    // player drafted them, whom they shoot at, the cell they hold and whether they are falling back.
    uint32_t faction = 1;
    bool onFire = false;  // burning (docs/design/fire.md)
    bool drafted = false;
    bool fighting = false;
    Entity fightTarget = kNoEntity;
    CellPos fightSpot;
    bool fallingBack = false;
    // F6 (docs/design/raids.md): the colony's colonists' defense role (0 Defender, 1 Shelter, 2 Keep
    // working); whether the alarm called it up; a fighter heading for a shelter or fleeing the map;
    // and for raiders their squad (0 for none).
    uint8_t defenseRole = 0;
    bool defending = false;
    bool sheltering = false;
    bool fleeing = false;
    uint32_t squad = 0;
    // Prisoners (docs/design/prisoners.md): held in a prison bed, what wardens do with it (a PrisonerMode number) and its
    // resistance; a bed marked for prisoners.
    bool prisoner = false;
    uint8_t prisonerMode = 0;
    float resistance = 0.0f;
    // Visitors (docs/design/trade.md): a pawn of a visit (a VisitorRole number: 0 visitor, 1 trader, 2 guard) and the visit's id.
    bool visitor = false;
    uint8_t visitRole = 0;
    uint32_t visit = 0;
    bool prisonBed = false;
    // F10 (docs/design/field-care.md): defenseRole 3 is Medic and 4 Firefighter; a fighter carrying a downed friend
    // to cover has it in `rescuing` (the friend's position follows the fighter's after the LiftPawn action).
    Entity rescuing = kNoEntity;
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
    // Animals in fights (docs/design/animal-fights.md): 0 calm, 1 hunting a pawn, 2 avenging a hurt, 3 manhunter;
    // `animalMode` is the AnimalMode number (running, chasing, fighting, eating a kill are the last five).
    int animalTemper = 0;
    int animalMode = 0;
    // Animals and husbandry (docs/design/husbandry.md): marked to be tamed (wild) or slaughtered (tame), the animal zone it
    // stays in (0 none), and the tricks it has learned and that are wanted, a bit per Trainable (Obedience 1, Release 2,
    // Rescue 4, Haul 8). A tame animal's `faction` is the player's.
    bool markTame = false;
    bool markSlaughter = false;
    bool markHunt = false;  // a wild animal marked to be hunted (N2, docs/design/hunting.md)
    uint32_t animalZone = 0;
    std::string animalName;  // the player's name for a tame animal, or empty
    uint32_t learnedTricks = 0;
    uint32_t wantedTricks = 0;
    // Breeding and products (docs/design/breeding.md): sex (0 male, 1 female; -1 for other things), carrying young, and how
    // full its milk and wool are, 0 to 1 (-1: it gives none now).
    int sex = -1;
    bool pregnant = false;
    float milkFullness = -1.0f;
    float woolFullness = -1.0f;
    // Pawns in melee (docs/design/melee.md): winding up a swing at meleeTarget (the hit comes in
    // meleeStepsLeft steps) or recovering from one, and whether their weapon is raised to block.
    MeleePhase meleePhase = MeleePhase::Ready;
    int meleeStepsLeft = 0;
    Entity meleeTarget = kNoEntity;
    bool blocking = false;
    // Turrets and traps (docs/design/defense.md). A turret: what it is doing (a TurretPhase number) and the pawn it
    // is aiming at. A trap: 0 not a trap, 1 armed, 2 sprung (waiting to be rearmed).
    uint8_t turretPhase = 0;
    Entity turretTarget = kNoEntity;
    uint8_t trapState = 0;
    // Clothes (docs/design/apparel.md). Pawns: the outfit they follow (0 for other things), whether a strip
    // order is on them, and what they wear: `apparelCount` pieces from Snapshot::apparel[firstApparel], in the
    // order put on. Apparel items on the map: their `stuff` (as for buildings), `quality` (a place in
    // QualityRules.levels, -1 for things without one) and whether they are tainted. Crafted weapons have a
    // `stuff` and a `quality` too (C14).
    uint32_t outfit = 0;
    bool markedForStrip = false;
    uint32_t firstApparel = 0;
    uint32_t apparelCount = 0;
    int quality = -1;
    bool tainted = false;
    // Resonance (docs/design/resonance.md). Pawns: steps left of a stun (0: not stunned), whether a numb or burden
    // is on them, and for a pawn winding up a power, the Powers def and the steps until it goes off.
    int stunnedFor = 0;
    bool numbed = false;
    bool burdened = false;
    DefId castPower;
    int castStepsLeft = 0;
    // Pawns in a mental break (docs/design/mental-breaks.md): its MentalBreaks def, invalid when not in one.
    DefId mentalBreak;
};

// One piece of worn apparel (Unreal draws each layer on the pawn, torn by `hitPointShare`).
struct ApparelView {
    Entity item = kNoEntity;
    DefId def;
    DefId stuff;      // invalid for none
    int quality = 2;
    float hitPointShare = 1.0f;  // 0 to 1
    bool tainted = false;
};

// An outfit policy by the id the commands use.
struct OutfitView {
    uint32_t id = 0;
    std::string name;
};

// A raid squad on the map (docs/design/raids.md). `state` is a SquadState: 0 Advance, 1 Hold,
// 2 FallBack, 3 Rout.
struct SquadView {
    uint32_t id = 0;
    uint32_t faction = 0;  // its pawns' faction
    uint8_t state = 0;
    int members = 0;       // still with it: on the map, downed ones included
    int started = 0;
    CellPos target;        // the cell it goes for
    uint8_t strategy = 0;  // RaidStrategy (F12): 0 Assault, 1 Sappers, 2 DropPods, 3 Thieves, 4 Kidnappers
    bool breaching = false;  // sappers are digging through a wall...
    CellPos breach;          // ...this one
};

// A thing in a trader's stock.
struct StockView {
    std::string thing;  // Things defName
    int count = 0;
};

// Visitors, a trade caravan or an orbital trader on the map (docs/design/trade.md). `kind` is a VisitKind (0 visitors, 1 caravan,
// 2 orbital), `phase` a VisitPhase (0 staying, 1 leaving). The trader's `stock` is what the colony may buy; send a TradeCommand to trade.
struct VisitView {
    uint32_t id = 0;
    uint8_t kind = 0;
    uint8_t phase = 0;
    std::string faction;
    std::string traderKind;
    Entity trader = kNoEntity;  // the pawn to trade with, none for a ship
    int64_t leaveAt = 0;        // game second
    int silver = 0;             // what the trader can pay with
    int members = 0;
    CellPos spot;               // where they gather and where goods are put down
    std::vector<StockView> stock;
};

// What a faction thinks of the colony: goodwill from -100 to 100 and its relation (0 hostile, 1 neutral, 2 ally).
struct GoodwillView {
    std::string faction;
    float goodwill = 0.0f;
    uint8_t relation = 1;
};
// A quest, offered or finished (docs/design/quests.md): `state` is a QuestState (0 Offered, 1 Active, 2 Completed, 3 Failed, 4 Declined,
// 5 Expired), `expiresAt` the game second an unanswered offer lapses, `deadline` the game second an active raid quest fails
// (0 for none), `people` how many come, `points` the raid's points (0 for none), `rewards` the defName and count of each reward item.
struct QuestView {
    uint32_t id = 0;
    std::string kind;
    std::string label;
    uint8_t state = 0;
    int64_t expiresAt = 0;
    int64_t deadline = 0;
    int people = 0;
    float points = 0.0f;
    std::vector<std::pair<std::string, int>> rewards;
};

// A bullet in flight (docs/design/combat.md). Unreal draws a tracer along its velocity; it
// appears with a ShotFired event and disappears with a ProjectileHit event. A thrown grenade is here
// too (docs/design/explosives.md): its def is the grenade's Projectiles def, it appears with a
// GrenadeThrown event and goes with an Explosion; `fuse` is -1 while it flies and the steps left
// once it lies on the ground (its velocity is 0 then).
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
    int fuse = -1;
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
    // V1 (docs/design/looks.md): the face, hair style, beard, body hair and skin marks.
    float faceShape = 0.5f;
    float brow = 0.5f;
    float nose = 0.5f;
    float jaw = 0.5f;
    LooksColor eyes;
    std::string hairStyle;  // empty for a pawn from an older save
    std::string beard;
    float bodyHair = 0.0f;
    std::vector<LooksMark> tattoos;
    std::vector<LooksMark> scars;
};

// One bill on a work building, in the building's order (first is tried first).
struct BillView {
    uint32_t id = 0;  // the id the bill commands use, per building
    DefId recipe;     // a Recipes def
    BillMode mode = BillMode::Count;
    int count = 0;    // Count: times left; UntilHave: the stock to reach; Forever: 0
    bool suspended = false;
    // The bill's details (C14, docs/design/crafting.md); the commands SetBillOptions and MoveBill change them.
    int skillMin = 0;
    int skillMax = 20;
    float radius = 0.0f;     // metres from the building ingredients are taken from; 0 anywhere
    uint32_t storeZone = 0;  // the stockpile the products go to; 0 none
    uint32_t countZone = 0;  // the stockpile an "until you have" bill counts in; 0 the whole map
    int unpauseAt = 0;
    bool satisfied = false;  // an "until you have" bill waiting for the stock to fall to `unpauseAt`
    std::vector<DefId> disallowed;  // ingredient item defs it may not use
};

// One stockpile zone: `cellCount` cells from Snapshot::zoneCells[firstCell].
struct ZoneView {
    ZoneId id = kNoZone;
    StoragePriority priority = StoragePriority::Normal;
    uint32_t firstCell = 0;
    uint32_t cellCount = 0;
};

// One animal zone: `cellCount` cells from Snapshot::animalZoneCells[firstCell]. A pen is walled in all round.
struct AnimalZoneView {
    uint32_t id = 0;
    uint32_t firstCell = 0;
    uint32_t cellCount = 0;
    bool pen = false;
};

// One growing zone (C13): `cellCount` cells from Snapshot::growingZoneCells[firstCell], growing `crop`.
struct GrowingZoneView {
    uint32_t id = 0;
    DefId crop;
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

// A fire in a cell, its size 0 to 1.75 (docs/design/fire.md).
struct FireView {
    CellPos cell;
    float size = 0.0f;
};

// One mark on a cell: mine here, cut this plant, harvest it (docs/design/mining.md).
struct DesignationView {
    CellPos cell;
    DesignationKind kind = DesignationKind::None;
};

// One cell of deep resources a scanner found (docs/design/deep-mining.md).
struct DeepCellView {
    CellPos cell;
    DefId def;       // the item under it
    int units = 0;
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

// The spaceship ending (docs/design/ship.md). `phase` is a ShipPhase number.
struct ShipView {
    uint8_t phase = 0;
    int64_t activationLeft = 0;  // game seconds until the reactor is active (0 unless activating)
    int waves = 0;               // siege waves sent
    int beams = 0, caskets = 0, reactors = 0, engines = 0, cores = 0, sensors = 0;
    int riders = 0;              // living riders in caskets that touch a beam
    bool canLaunch = false;
    std::string problem;         // why not, in plain words
    bool won = false;
    int escaped = 0;
};

// A caravan (docs/design/caravans.md). `state` is a CaravanState number: 0 forming on the map, 1 travelling,
// 2 resting for the night, 3 waiting on a tile. Tiles are World tile indices (-1 none).
struct CaravanView {
    int id = 0;
    uint8_t state = 0;
    int tile = -1;
    int nextTile = -1;
    int destination = -1;
    float progress = 0.0f;     // 0 to 1 of the way to nextTile
    int pawns = 0;             // members (downed ones too)
    int items = 0;             // stacks it carries
    float massKg = 0.0f;
    float capacityKg = 0.0f;
    float tilesPerDay = 0.0f;  // at the next tile's cost and its load, counting the night's rest; 0 when it can't move
};

// The date, the weather and the sky's mood (docs/design/weather.md). Snow per cell is not here:
// read `Sim::weather().snowDepth(cell)` when `snowChanges` goes up.
struct WeatherView {
    int year = 0;       // 0 for the first
    int dayOfYear = 0;  // 0 to 59
    int season = 0;     // 0 spring to 3 winter
    float temperature = 15.0f;  // outdoors, degrees Celsius
    DefId weather;      // the Weathers def now, and the one it is changing from
    DefId previous;
    float transition = 1.0f;  // 0 just changed to 1 fully changed: blend the looks by it
    float skyGlow = 1.0f;     // the effects now, blended by the transition
    float rainRate = 0.0f;
    float snowRate = 0.0f;
    float wetness = 0.0f;
    uint64_t snowChanges = 0;
    float snowCover = 0.0f;   // share of the map's cells with snow
};

// The narrator (docs/design/narrator.md): which one and the difficulty (invalid DefIds while it is
// off), the colony's wealth and threat points at its last check, and incidents so far. Letters come
// as Letter events.
struct NarratorView {
    DefId narrator;
    DefId difficulty;
    float wealth = 0.0f;
    float threatPoints = 0.0f;
    uint32_t incidents = 0;
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

// An attuned pawn (docs/design/resonance.md). One entry per attuned pawn, in id order.
struct AttunedView {
    Entity pawn = kNoEntity;
    int level = 0;
    float strain = 0.0f;       // RimWorld's neural heat
    float strainLimit = 0.0f;
    float focus = 0.0f;        // RimWorld's psyfocus, 0 to 1
    std::vector<DefId> powers;  // the Powers defs it knows
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
    std::vector<AnimalZoneView> animalZones;  // animal zones and pens (N3)
    std::vector<CellPos> animalZoneCells;     // their cells, see AnimalZoneView
    std::vector<GrowingZoneView> growingZones;  // growing zones (C13)
    std::vector<CellPos> growingZoneCells;      // their cells, see GrowingZoneView
    std::vector<DesignationView> designations;  // every mark, in cell order
    std::vector<RoomView> rooms;           // enclosed rooms, by id
    float outdoorTemperature = 0.0f;       // degrees Celsius
    // The cells with a thin (built) roof, in cell order; `roofChanges` goes up when the set
    // changes, so Unreal rebuilds the roof only then. The mountain's thick roof is the rock itself
    // (`Map::roof` says which).
    std::vector<CellPos> roofCells;
    uint64_t roofChanges = 0;
    // How many roofs and floors have fallen (M3): when it goes up, redraw the mountain's roof and the upper floors
    // (`Map::roof`, `Map::upperFloorCells`).
    uint64_t roofCollapses = 0;
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
    // Fire (docs/design/fire.md): the burning cells, the burnt ground and the home area, in cell order. Each is rewritten only
    // when its counter goes up (the fires' counter goes up every pass while any burn, as their sizes change).
    std::vector<FireView> fires;
    uint64_t fireChanges = ~0ULL;
    std::vector<CellPos> burnt;
    uint64_t burntChanges = ~0ULL;
    // Deep resources found by scanners, in cell order; rewritten when `deepChanges` moves (drills take units out).
    std::vector<DeepCellView> deepResources;
    uint64_t deepChanges = ~0ULL;
    std::vector<CellPos> homeArea;
    uint64_t homeChanges = ~0ULL;
    // The colony alarm (docs/design/raids.md): on, and whether the player sounded it (only the
    // player stops that one); the raid squads on the map.
    bool alarm = false;
    bool alarmManual = false;
    std::vector<SquadView> squads;
    std::vector<VisitView> visits;         // visitors and traders here now (docs/design/trade.md)
    std::vector<GoodwillView> goodwill;    // every faction's goodwill with the colony
    std::vector<QuestView> quests;         // offered, active and recently finished quests, by id
    std::vector<ApparelView> apparel;      // worn apparel, see ThingView
    std::vector<OutfitView> outfits;       // the outfit policies, by id
    WeatherView weather;
    std::vector<AttunedView> attuned;      // see AttunedView
    ShipView ship;
    NarratorView narrator;
    std::vector<CaravanView> caravans;  // by id

    // The thing with this id, or null if it is not on the map. Binary search.
    COLONY_API const ThingView* find(Entity id) const;
};

}  // namespace colony
