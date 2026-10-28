#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"
#include "ColonySim/Grid.h"
#include "ColonySim/ThreatMap.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace colony {

class ArtillerySystem;
class CombatSystem;
class Construction;
class DefDatabase;
class FuelSystem;
class FighterSystem;
class HealthSystem;
class JobSystem;
class Map;
class Rng;
class Rooms;
class StateHasher;
class Things;
struct GameTime;

// Raids and the colony's defense (task F6). See docs/design/raids.md.
//
// A raid is a squad of another faction's pawns, bought with RimWorld's raid points, that walks in
// at a map edge and goes for the colony (RimWorld's "assault colony" plan). Its fighters use the F4
// and F5 fighter AI; the squad decides as a whole whether to advance, hold, fall back or rout, and
// routed raiders run off the map. The colony answers with the alarm: when a colonist spots an enemy
// (or the player sounds it), every colonist does what its defense role says.
//
// Unreal does not include this header: it reads `alarm`, `squads`, `defenseRole` and the fighter
// fields from the snapshot, and sends StartRaidCommand, SoundAlarmCommand, SetDefenseRoleCommand
// and MarkShelterCommand.

// What a colonist does when the alarm sounds (game design doc, "Defense plans replace drafting").
enum class DefenseRole : uint8_t {
    Defender,     // takes cover on the side the threat comes from, or holds its defense spot
    Shelter,      // goes to the shelter room and stays there
    KeepWorking,  // works on, unless an enemy comes within keepWorkingRadius; then it defends itself
    // F10 (docs/design/field-care.md): FieldCare gives these their duty.
    Medic,        // waits behind the line; tends and rescues the downed it can reach without coming under fire
    Firefighter,  // waits behind the line; puts out fires in the home area once it is safe to go to them
};

// A colonist's defense plan, on the pawn. A colonist without one is a Defender with no spot.
struct DefensePlan {
    DefenseRole role = DefenseRole::Defender;
    bool hasSpot = false;  // a Defender holds this cell (within spotLeash) instead of picking its own line
    CellPos spot;
};

// How a raid comes and what it is after (F12, docs/design/raid-strategies.md).
enum class RaidStrategy : uint8_t {
    Assault,    // walks in at an edge and goes for the colony (F6)
    Sappers,    // walks in; when walls shut the colony off, digs through the nearest wall on the way
    DropPods,   // lands in pods round the colony, inside its walls if need be
    Thieves,    // walks in, grabs the most valuable loose items and runs for the edge with them
    Kidnappers, // walks in, fights, and carries off colonists who go down
    Siege,      // walks in, digs in out of reach, builds mortars and sandbags, shells the colony, then assaults
};
inline constexpr int kRaidStrategyCount = 6;

// How far a siege has got (docs/design/sieges.md).
enum class SiegeStage : uint8_t {
    March,    // walking to the camp spot
    Build,    // its first shells have dropped in; builders put up the mortars and the sandbags
    Shell,    // the mortars are up and manned: shelling the colony while the builders finish the sandbags
    Assault,  // out of shells, too many lost, no mortar left or on too long: an ordinary assault from now on
};

// One thing a siege camp builds: a mortar or a sandbag. Built through Construction when its work is done.
struct SiegeBuild {
    std::string def;           // Things defName
    std::string stuff;         // its stuff's defName, empty for none
    CellPos origin;
    Rot4 rotation = Rot4::North;
    float workLeft = 0.0f;     // game seconds of work at speed 1
    bool done = false;         // built, or given up because it can't stand there any more
    Entity built = kNoEntity;  // the finished building while it stands
};

// A siege squad's camp (raids section 3). Meaningful only for RaidStrategy::Siege.
struct Siege {
    SiegeStage stage = SiegeStage::March;
    CellPos camp;                    // the camp's middle
    int64_t endsAt = 0;              // it assaults at this game second whatever else happens
    int resupplies = 0;              // shell drops after the first
    std::vector<SiegeBuild> builds;  // the mortars first, then the sandbags
};

// On every pawn a raid brought: its squad and pawn kind (PawnKinds defName).
struct Raider {
    uint32_t squad = 0;
    std::string kind;
    // F12 (raids section 2): what it is going for, and what it is carrying off. `loot` is a stolen item
    // (the pawn also has Carrying) or a kidnapped pawn (which has Kidnapped and follows it).
    Entity goalThing = kNoEntity;
    Entity loot = kNoEntity;
    bool sparesDowned = false;  // kidnappers don't shoot the downed: they come to carry them off
};

// On a colonist a kidnapper carries off: its position follows `by` until it is put down or leaves the map.
// Rescue and capture jobs leave a kidnapped pawn alone.
struct Kidnapped {
    Entity by = kNoEntity;
};

// How a squad feels about the fight (architecture.md, "Morale and squads").
enum class SquadState : uint8_t {
    Advance,   // stronger: goes for the colony and closes in
    Hold,      // about even: stays in cover and trades shots
    FallBack,  // weaker: every member falls back
    Rout,      // lost too many, or gave up: runs off the map
};

struct Squad {
    uint32_t id = 0;
    std::string faction;    // Factions defName
    FactionId side = 0;     // its pawns' Faction id
    float points = 0.0f;    // the raid points it was bought with
    int started = 0;        // pawns it arrived with
    int64_t arrivedAt = 0;  // game second
    int64_t giveUpAt = 0;   // it leaves at this game second if it hasn't routed before
    SquadState state = SquadState::Advance;
    RaidStrategy strategy = RaidStrategy::Assault;  // F12 (raids section 2)
    bool hasBreach = false;  // Sappers: the wall they are digging through, and the cell they dig from
    CellPos breach;
    CellPos breachFrom;
    Siege siege;             // a siege's camp (RaidStrategy::Siege, raids section 3)
    bool gaveUp = false;    // routed because time ran out rather than losses
    CellPos entry;          // where it walked in
    bool hasTarget = false;  // the cell it goes for: the colonist nearest to it
    CellPos target;
    int64_t nextThink = 0;
    std::vector<Entity> members;  // in id order; the dead and those that left are dropped
};

// A raid that waits for an hour of the day (a night attack), started with the command's strategy then.
struct PendingRaid {
    std::string faction;
    float points = 0.0f;
    int edge = -1;
    RaidStrategy strategy = RaidStrategy::Assault;
    int64_t at = 0;  // game second
};

// Everything a raid and the alarm keep between steps; saved as a whole (Save.cpp).
struct RaidState {
    std::vector<PendingRaid> pending;   // in the order they were asked for (F12, raids section 2)
    std::vector<std::string> factions;  // Factions defNames; faction id = index + 2 (1 is the player)
    std::vector<Squad> squads;          // in id order
    uint32_t nextSquad = 1;
    bool alarm = false;
    bool manual = false;      // the player sounded it: only the player stops it
    bool suppressed = false;  // the player stopped it while enemies were about: no automatic alarm until they are gone
    bool hasThreat = false;   // where the spotted enemies were, on average
    float threatX = 0.0f;
    float threatY = 0.0f;
    int64_t threatSeenAt = 0;
    bool hasHome = false;     // the colony's middle when the alarm sounded: the defense line is drawn from it
    float homeX = 0.0f;
    float homeY = 0.0f;
    bool hasShelter = false;  // the player's shelter room, by a cell in it
    CellPos shelter;
    int64_t nextAlarmThink = 0;
    // Adaptation (wiki "Raid points"): the days the colony has adapted, and the colonists it keeps
    // an eye on to see who goes down or dies (section `raids` 2).
    struct AdaptPawn {
        Entity pawn = entt::null;
        bool downed = false;
    };
    float adaptDays = 0.0f;
    int64_t nextAdaptGrowth = 0;  // game second of the next 12-hour growth step
    std::vector<AdaptPawn> adaptPawns;  // in id order
};

// The numbers (RaidRules in data/defs/Rules/RaidRules.json).
struct RaidRules {
    struct Point {
        float x, y;
    };
    // RimWorld's raid points (wiki "Raid points"): wealth points by storyteller wealth, points per
    // colonist by wealth, the difficulty's threat scale, the days-passed factor, the adaptation
    // factor, and the limits.
    std::vector<Point> wealthPoints{{0, 0}, {14000, 0}, {400000, 2400}, {700000, 3600}, {1000000, 4200}};
    std::vector<Point> pointsPerColonist{{0, 15}, {10000, 15}, {400000, 140}, {1000000, 200}};
    std::vector<Point> daysPassedFactor{{10, 0.7f}, {40, 1.0f}};
    float buildingWealthFactor = 0.5f;
    float threatScale = 1.0f;
    // Adaptation: AdaptDays go up every `adaptGrowthHours` by the step list's value for where they
    // are (halved: the wiki's table is per day; never above 0 before `adaptGraceDays`), and down by
    // the curves when a colonist dies or goes down (by colonists); the factor is the curve of
    // AdaptDays, blended with 1 by the difficulty's adaptationImpact.
    std::vector<Point> adaptGrowth{{-60, 3}, {0, 1}, {60, 0.5f}, {120, 0.25f}};  // from AdaptDays: added per day
    std::vector<Point> adaptFactor{{-30, 0.4f}, {0, 0.8f}, {30, 1.0f}, {60, 1.2f}, {120, 1.6f}, {180, 2.0f}};
    std::vector<Point> adaptDeath{{0, 30}, {11, 30}, {20, 20}};
    std::vector<Point> adaptDowned{{1, 8}, {2, 6}, {3, 6}, {11, 5}, {20, 3.5f}};
    float adaptMin = -60.0f;
    float adaptMax = 100.0f;
    float adaptGraceDays = 30.0f;
    float adaptGrowthHours = 12.0f;
    float minPoints = 35.0f;
    float maxPoints = 10000.0f;
    // When a squad routs or gives up.
    float fleeLostFraction = 0.5f;  // share of its pawns downed, dead or gone
    float giveUpHoursMin = 10.4f;   // RimWorld's 26,000 to 38,000 ticks
    float giveUpHoursMax = 15.2f;
    // Ours.
    int squadThinkEvery = 30;      // game seconds between a squad's morale checks (one real second)
    float advanceRatio = 1.0f;     // own strength / known enemy strength at least this: advance
    float holdRatio = 0.5f;        // at least this: hold; below: fall back
    float hysteresis = 0.25f;      // share the ratios move by in favour of the state the squad is in
    float retargetDistance = 8.0f; // the squad's target moves only when the nearest colonist is this far from it
    int edgeDepth = 2;             // cells in from the map edge a raid walks in at
    int alarmThinkEvery = 15;      // game seconds between alarm checks
    float sightRange = 60.0f;      // a colonist spots an enemy this close with a line of sight
    float keepWorkingRadius = 20.0f;
    float defenseLine = 15.0f;     // a Defender with no spot holds this far from home toward the threat
    float defenderLeash = 10.0f;   // and picks cover this close to that point
    float spotLeash = 3.0f;        // a Defender with a spot, this close to it
    float lineMoves = 6.0f;        // the line moves when the threat has moved it this far
    int shelterSearch = 4000;      // cells searched for an indoor cell when no shelter is marked
    float shelterFlee = 25.0f;     // no indoor cell: this far from the threat instead
    // F12 (all ours): which strategy a raid picks when the caller doesn't say, and how each one works.
    float weightAssault = 60.0f;
    float weightSappers = 12.0f;   // times blockedBoost when walls shut the colony off from every edge
    float weightDropPods = 14.0f;
    float weightThieves = 8.0f;
    float weightKidnappers = 6.0f;
    float blockedBoost = 8.0f;
    float dropPodsMinPoints = 120.0f;  // smaller raids walk
    float dropMinDistance = 8.0f;      // pods land this far from the target...
    float dropMaxDistance = 24.0f;     // ...to this far
    float sapDamage = 4.0f;            // hit points a digging raider takes off its wall each second
    float sapReach = 2.2f;             // metres from the wall's cell middle a raider digs from
    int sapSearch = 60000;             // cells searched for the wall to dig through
    float grabReach = 1.6f;            // metres from the loot or the downed colonist a raider can lift it
    float stealMass = 40.0f;           // kilograms of items a thief carries off
    float kidnapRange = 90.0f;         // a kidnapper goes for downed colonists this close
    float seizeRange = 12.0f;          // and stops fighting to lift one this close
    // Sieges (docs/design/sieges.md). The unverified ones are recalled RimWorld numbers, the rest ours.
    float weightSiege = 8.0f;
    float siegeMinPoints = 300.0f;     // smaller raids don't dig in
    float campDistanceMax = 70.0f;     // the camp is this far from the colony at most...
    float campDistanceMin = 35.0f;     // ...and at least (outside the mortar's 29.9 m minimum range)
    float campArrive = 8.0f;           // metres from the camp's middle at which the squad has got there
    float campLeash = 10.0f;           // the guards stay this close to it
    float campDefend = 30.0f;          // an enemy this close to the camp calls the builders to fight
    float campSupplies = 12.0f;        // shells lying this close to the camp's middle are its supplies
    float builderShare = 0.4f;         // of the able members build (unverified: RimWorld 0.25 to 0.4)
    float buildReach = 2.0f;           // metres from what it builds a builder works from
    float pointsPerMortar = 60.0f;     // mortars: points / this, rounded, 1 to mortarsMax (unverified)
    int mortarsMax = 2;
    int coverLinesMin = 2;             // lines of sandbags (unverified)
    int coverLinesMax = 4;
    int coverLengthMin = 2;            // sandbags in a line (unverified)
    int coverLengthMax = 7;
    int shellsPerMortar = 5;           // shells in the first drop, per mortar (unverified)
    int resupplyAt = 4;                // another drop when the camp has fewer shells than this (unverified)
    int resupplyShells = 10;           // shells in it (unverified)
    int resupplyMax = 3;               // drops after the first: then the supplies run out
    float siegeAssaultLost = 0.4f;     // share of its pawns lost that sends it in (unverified)
    float siegeDays = 2.0f;            // it assaults after this long whatever else happens ("a few days")
    std::string siegeMortar = "Turret_Mortar";
    std::string siegeCover = "Sandbags";
    std::string siegeCoverStuff = "Cloth";
    std::string siegeShell = "Shell_HighExplosive";
};

class COLONY_API RaidSystem {
public:
    RaidSystem(Things& things, Map& map, Rooms& rooms, CombatSystem& combat, HealthSystem& health, JobSystem& jobs,
               FighterSystem& fighters, const DefDatabase& defs, const GameTime& time, Rng& rng);
    ~RaidSystem();
    RaidSystem(const RaidSystem&) = delete;
    RaidSystem& operator=(const RaidSystem&) = delete;

    const RaidRules& rules() const { return rules_; }
    void setEventLog(std::vector<Event>* events) { events_ = events; }
    // What a siege needs to build its camp, man its mortars and load them (null: sieges can't start).
    void setSiegeParts(Construction* construction, ArtillerySystem* artillery, FuelSystem* fuel) {
        construction_ = construction;
        artillery_ = artillery;
        fuel_ = fuel;
    }
    // Gives a new raider its looks (Sim::giveLooks): pawn, seed, female.
    void setLooksMaker(std::function<void(Entity, uint64_t, bool)> maker) { looks_ = std::move(maker); }

    // RimWorld's storyteller wealth (wiki "Raid points"): items on the map and carried by the
    // player's pawns, and the player's own pawns (colonists, tame animals), at their market value,
    // plus buildings at half theirs. Defs without a marketValue count 0.
    float colonyWealth() const { return colonyWealth(rules_.buildingWealthFactor); }
    // The same with buildings at the given share of their value: 1 is RimWorld's map wealth, which sets the colonists'
    // expectations (docs/design/mood.md).
    float colonyWealth(float buildingFactor) const;
    // The adaptation factor now (1 with the impact at 0), and what the difficulty says about it
    // (its adaptationGrowth and adaptationImpact; the narrator sets them with the difficulty).
    float adaptationFactor() const;
    void setAdaptation(float growth, float impact);
    // The raid points a raid would get now (wiki "Raid points"), between minPoints and maxPoints.
    float threatPoints() const { return threatPoints(1.0f); }
    // The same with another factor before the limits: the narrator's difficulty and points factor (T1).
    float threatPoints(float scale) const;
    // The player's living colonists on the map.
    int colonistCount() const;

    // Commands. False, with a reason in plain words, when they can't be done.
    // A raid of `faction` (a Factions defName) with `points` (0: threatPoints()) walks in at `edge`
    // (0 north, 1 east, 2 south, 3 west, -1 any).
    bool startRaid(const std::string& faction, float points, int edge, std::string& error);
    // F12: the same with a strategy. `hour` 0 to 23.99 waits for that hour of the day (a raid at night; the
    // next one, today or tomorrow), -1 comes now. Fails when the strategy has nothing to work with (thieves
    // with no loose items, kidnappers with no colonist, sappers with no wall in the way are fine).
    bool startRaid(const std::string& faction, float points, int edge, RaidStrategy strategy, float hour, std::string& error);
    // The strategy a raid of these points would pick now, by the rules' weights: sappers favoured when no edge
    // reaches the colony, drop pods only from dropPodsMinPoints, thieves only when loose items exist, kidnappers
    // only when colonists do, sieges only from siegeMinPoints and for a faction that can build (not a neolithic one;
    // an empty `faction` is any). Draws from the sim's Rng, so call it from the sim.
    RaidStrategy pickStrategy(float points, const std::string& faction = std::string());
    // Where outsiders walk in: `target` is the player's colonist nearest the middle (else the middle) and `entry` a cell
    // near a map edge (`edge` 0 to 3, or -1 for any) that can reach it. False, with a reason, if there is no way in.
    bool findEntry(int edge, CellPos& target, CellPos& entry, std::string& error);
    bool soundAlarm(bool on, std::string& error);
    bool setRole(Entity pawn, DefenseRole role, bool hasSpot, CellPos spot, std::string& error);
    // The room at `cell` is where Shelter colonists go; `clear` takes the mark off.
    bool markShelter(CellPos cell, bool clear, std::string& error);

    // Squads think, routed raiders leave at the edge, the alarm looks for enemies and calls the
    // colonists up or sends them back. Runs after the job system, before the fighters.
    void step();

    // A pawn walks off the map for good (its gear goes with it): a PawnLeftMap event, then it is destroyed.
    void leaveMap(Entity pawn);
    // Call before a thing is destroyed: it leaves its squad.
    void forget(Entity thing);
    // A Faction id for a Factions def: the same def always gets the same id.
    FactionId factionId(const std::string& faction);
    DefenseRole roleOf(Entity pawn) const;

    const RaidState& state() const { return state_; }
    // After loading a save. False if it doesn't fit the defs.
    bool restore(RaidState state, std::string& error);
    void hashState(StateHasher& h) const;
    void hashThing(Entity e, StateHasher& h) const;

private:
    struct PawnKind {
        std::string defName;
        std::string race;
        float combatPower = 0.0f;
        std::vector<std::string> weapons;
        struct Wear {
            std::string thing;
            float chance = 1.0f;
        };
        std::vector<Wear> apparel;
    };

    void thinkSquad(Squad& squad);
    // F12.
    bool pickEntry(int edge, CellPos target, bool needReach, CellPos& entry);
    void stepStrategies();
    void thinkBreach(Squad& squad);
    void thinkLoot(Squad& squad);
    void thinkKidnap(Squad& squad);
    // Sieges (docs/design/sieges.md).
    bool planSiege(Siege& siege, float points, CellPos entry, CellPos target, std::string& error);
    void thinkSiege(Squad& squad, float lost);
    void siegeOrders(Squad& squad);
    void stepSiege(Squad& squad);
    void finishBuild(Squad& squad, SiegeBuild& build);
    void dropShells(Squad& squad, int count);
    int siegeShells(const Squad& squad, std::vector<Entity>* stacks) const;
    void siegeAim(const Squad& squad, Entity mortar);
    void startAssault(Squad& squad, const char* why);
    void siegeEvent(const Squad& squad, std::string message);
    CellRect buildRect(const SiegeBuild& build) const;
    void dropLoot(Entity pawn);
    void flee(Entity pawn, Squad& squad);
    bool findBreach(const Squad& squad, CellPos from, CellPos& wall, CellPos& approach) const;
    bool blockedFromColony(CellPos from, CellPos target) const;
    bool colonyWalledOff();
    void removeForGood(Entity pawn);
    void leaveAtEdge(Squad& squad);
    void thinkAlarm();
    void thinkAdaptation();
    void setAlarm(bool on, const char* why);
    void callUp(Entity pawn, const std::vector<Entity>& spotted);
    CellPos defensePoint(Entity pawn, const DefensePlan* plan);
    CellPos shelterCell(Entity pawn);
    CellPos exitCell(Entity pawn, const Squad& squad);
    bool nearestFree(float x, float y, int level, CellPos from, int radius, CellPos& out);
    float strength(Entity pawn) const;
    bool canSee(Entity watcher, Entity pawn) const;
    void squadEvent(const Squad& squad, EventType type, std::string message);
    int64_t now() const;

    Things& things_;
    Map& map_;
    Rooms& rooms_;
    CombatSystem& combat_;
    HealthSystem& health_;
    JobSystem& jobs_;
    FighterSystem& fighters_;
    const DefDatabase& defs_;
    const GameTime& time_;
    Rng& rng_;
    RaidRules rules_;
    RaidState state_;
    float adaptGrowthRate_ = 1.0f;  // the difficulty's, not saved
    float adaptImpact_ = 1.0f;
    std::vector<float> marketValue_;  // per def id
    std::vector<float> armor_;        // per def id: armorRatingSharp of apparel
    std::vector<Event>* events_ = nullptr;
    std::function<void(Entity, uint64_t, bool)> looks_;
    Construction* construction_ = nullptr;
    ArtillerySystem* artillery_ = nullptr;
    FuelSystem* fuel_ = nullptr;
};

COLONY_API const char* defenseRoleName(DefenseRole role);
COLONY_API const char* squadStateName(SquadState state);
COLONY_API const char* raidStrategyName(RaidStrategy strategy);
COLONY_API const char* siegeStageName(SiegeStage stage);
// The strategy with this name ("Assault", "Sappers", "DropPods", "Thieves", "Kidnappers", "Siege"); false if there is none.
COLONY_API bool raidStrategyFromName(const std::string& name, RaidStrategy& out);

}  // namespace colony
