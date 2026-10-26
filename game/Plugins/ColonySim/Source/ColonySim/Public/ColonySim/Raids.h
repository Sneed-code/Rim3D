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

class CombatSystem;
class DefDatabase;
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

// On every pawn a raid brought: its squad and pawn kind (PawnKinds defName).
struct Raider {
    uint32_t squad = 0;
    std::string kind;
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
    bool gaveUp = false;    // routed because time ran out rather than losses
    CellPos entry;          // where it walked in
    bool hasTarget = false;  // the cell it goes for: the colonist nearest to it
    CellPos target;
    int64_t nextThink = 0;
    std::vector<Entity> members;  // in id order; the dead and those that left are dropped
};

// Everything a raid and the alarm keep between steps; saved as a whole (Save.cpp).
struct RaidState {
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
};

// The numbers (RaidRules in data/defs/Rules/RaidRules.json).
struct RaidRules {
    struct Point {
        float x, y;
    };
    // RimWorld's raid points (wiki "Raid points"): wealth points by storyteller wealth, points per
    // colonist by wealth, the difficulty's threat scale, the days-passed factor, the adaptation
    // factor (fixed at its start value until adaptation is tracked), and the limits.
    std::vector<Point> wealthPoints{{0, 0}, {14000, 0}, {400000, 2400}, {700000, 3600}, {1000000, 4200}};
    std::vector<Point> pointsPerColonist{{0, 15}, {10000, 15}, {400000, 140}, {1000000, 200}};
    std::vector<Point> daysPassedFactor{{10, 0.7f}, {40, 1.0f}};
    float buildingWealthFactor = 0.5f;
    float threatScale = 1.0f;
    float adaptation = 0.8f;
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
    // Gives a new raider its looks (Sim::giveLooks): pawn, seed, female.
    void setLooksMaker(std::function<void(Entity, uint64_t, bool)> maker) { looks_ = std::move(maker); }

    // RimWorld's storyteller wealth: items on the map and carried by the player's pawns at their
    // market value, plus buildings at half theirs. Defs without a marketValue count 0.
    float colonyWealth() const;
    // The raid points a raid would get now (wiki "Raid points"), between minPoints and maxPoints.
    float threatPoints() const;

    // Commands. False, with a reason in plain words, when they can't be done.
    // A raid of `faction` (a Factions defName) with `points` (0: threatPoints()) walks in at `edge`
    // (0 north, 1 east, 2 south, 3 west, -1 any).
    bool startRaid(const std::string& faction, float points, int edge, std::string& error);
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
    void leaveAtEdge(Squad& squad);
    void thinkAlarm();
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
    std::vector<float> marketValue_;  // per def id
    std::vector<float> armor_;        // per def id: armorRatingSharp of apparel
    std::vector<Event>* events_ = nullptr;
    std::function<void(Entity, uint64_t, bool)> looks_;
};

COLONY_API const char* defenseRoleName(DefenseRole role);
COLONY_API const char* squadStateName(SquadState state);

}  // namespace colony
