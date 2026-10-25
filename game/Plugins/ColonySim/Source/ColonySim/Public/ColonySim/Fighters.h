#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Cover.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"
#include "ColonySim/Explosives.h"
#include "ColonySim/Grid.h"
#include "ColonySim/Pathfinder.h"
#include "ColonySim/ThreatMap.h"

#include <cstdint>
#include <string>
#include <vector>

namespace colony {

class CombatSystem;
class DefDatabase;
class ExplosiveSystem;
class HealthSystem;
class JobSystem;
class Map;
class MeleeSystem;
class StateHasher;
class Things;
struct GameTime;

// Fighter AI: where to stand, whom to shoot, when to fall back. See docs/design/fighters.md.
//
// A fighter re-plans about twice a second: it scores the cells around it for cover from the
// enemies it knows about, sight to its target, weapon range, distance and crowding, walks to the
// best one, crouches there, and steps or stands up to fire. It waits its reaction time before
// shooting a new target, reloads in cover and sidesteps slow projectiles (F5, docs/design/aiming.md);
// squads, morale and raids are F6.
//
// Unreal does not include this header: it reads `drafted`, `faction`, `fightTarget` and
// `fallingBack` from the snapshot and sends DraftCommand, MoveOrderCommand, AttackOrderCommand.

// Which side a pawn is on. Pawns without a Faction component are the player's.
inline constexpr FactionId kPlayerFaction = 1;

struct Faction {
    FactionId id = kPlayerFaction;
};

// An enemy a fighter knows about: where it was last seen (metres) and when (game second).
struct KnownEnemy {
    Entity pawn = kNoEntity;
    float x = 0.0f;
    float y = 0.0f;
    int level = 0;
    int64_t seenAt = 0;
};

// On a pawn that fights instead of working: a drafted colonist, or every pawn of another faction.
// The job system leaves it alone. Every field is saved.
struct Fighter {
    bool drafted = false;    // a colonist the player drafted; otherwise an AI fighter of its faction
    bool advances = false;   // moves toward its target when it can't shoot from where it is
    CellPos anchor;          // where it was drafted or ordered to
    float leash = 0.0f;      // how far from the anchor it may stand, metres; 0 for no limit
    bool moveOrdered = false;
    Entity forcedTarget = kNoEntity;  // the player's attack order
    Entity target = kNoEntity;        // whom it shoots at
    bool hasSpot = false;
    CellPos spot;            // where it stands between shots
    CellPos fireCell;        // where it shoots from: the spot, or a cell beside it (stepping out)
    Stance fireStance = Stance::Crouching;
    bool canFire = false;    // the plan found a way to shoot the target
    bool fallingBack = false;
    int64_t nextPlan = 0;    // game second of the next re-plan
    uint8_t walk = 0;        // 0 standing, 1 waiting for a path to the spot, 2 walking there
    uint8_t repaths = 0;
    PathTicket path;         // while `walk` is 1
    std::vector<KnownEnemy> known;  // by pawn id
    int64_t reactUntil = 0;  // game second from which it may shoot its target (its reaction time, F5)
    int64_t dodgeUntil = 0;  // sidestepping a projectile until this game second: no plans, no shots (F5)
};

// The numbers (FighterRules in data/defs/Rules/FighterRules.json, all ours: RimWorld has no
// cover-seeking AI to copy).
struct FighterRules {
    int planEvery = 15;             // game seconds between re-plans (half a real second at 1x)
    float searchRadius = 10.0f;     // metres around the pawn it looks for a spot
    float draftLeash = 10.0f;       // a drafted colonist with no move order stays this close to where it was drafted
    float holdRadius = 2.0f;        // and this close to a move order's cell
    float sightRange = 60.0f;       // metres it can spot an enemy at
    int memorySeconds = 300;        // how long an enemy stays known after it was last seen
    int threats = 4;                // nearest known enemies a spot is scored against
    int refine = 10;                // best spots by the quick score that get the line of sight checks
    float coverWeight = 8.0f;       // the score's terms, in metres of walking they are worth
    float exposedWeight = 6.0f;
    float noShotWeight = 6.0f;
    float travelWeight = 0.4f;
    float rangeWeight = 1.0f;
    float tooClose = 4.0f;
    float crowdRadius = 1.5f;
    float crowdWeight = 3.0f;
    float stayBonus = 1.5f;
    float advanceWeight = 0.5f;
    float fallBackWeight = 0.8f;
    float standPenalty = 0.5f;      // firing standing (popping up) rather than crouched
    float stepOutPenalty = 1.0f;    // firing from the cell beside the spot
    float outnumberedRatio = 2.0f;  // known able enemies at least this many times the friends near: fall back
    float countRadius = 35.0f;      // metres within which enemies and friends are counted
    float hurtPain = 0.35f;         // badly hurt at this pain, blood loss or less Moving: fall back
    float hurtBloodLoss = 0.3f;
    float hurtMoving = 0.6f;
    int stepBackBelow = 20;         // steps of cooldown left under which it stays out instead of stepping back
    // F5 (docs/design/aiming.md).
    float reloadBelow = 0.5f;       // with enemies about but no shot, reloads in cover below this share of a magazine
    float dodgeRadius = 0.6f;       // a projectile passing closer than this to the pawn's centre would hit it
    float dodgeClearance = 0.9f;    // how far from the projectile's line a sidestep has to take it
    int dodgeHoldSteps = 3;         // steps it stays out of the way after the projectile has passed
    // F9 (docs/design/explosives.md): diving away from grenades and throwing them.
    float diveMargin = 0.6f;        // it runs when closer than the blast radius plus this
    float diveSafe = 0.4f;          // and wants to end this far beyond the radius (or behind cover)
    int throwEvery = 15;            // game seconds between looks for a place to throw (staggered by pawn id)
    int throwMinEnemies = 2;        // throws at a group of at least this many known enemies...
    int throwMemory = 150;          // ...seen this recently (steps), or at one it has no shot at
    float throwSelfMargin = 1.5f;   // never throws closer to itself than the radius plus this
    float throwFriendMargin = 0.5f; // nor with a friend within the radius plus this
};

class COLONY_API FighterSystem {
public:
    FighterSystem(Things& things, Map& map, CombatSystem& combat, HealthSystem& health, JobSystem& jobs,
                  Pathfinder& pathfinder, const DefDatabase& defs, const GameTime& time);
    ~FighterSystem();
    FighterSystem(const FighterSystem&) = delete;
    FighterSystem& operator=(const FighterSystem&) = delete;

    const FighterRules& rules() const { return rules_; }
    void setEventLog(std::vector<Event>* events) { events_ = events; }
    // Grenades (F9): fighters throw the ones on their belts and dive away from ones about to go off.
    void setExplosives(ExplosiveSystem* explosives) { explosives_ = explosives; }
    // Melee (F8, docs/design/melee.md): fighters armed for it charge, the others strike back at
    // enemies in reach. Without it nobody fights in melee.
    void setMelee(MeleeSystem* melee) { melee_ = melee; }

    // A pawn's side (kPlayerFaction without a Faction component). Different factions are enemies.
    FactionId factionOf(Entity pawn) const;

    // Commands. False, with a reason in plain words, when they can't be done.
    // Drafts a colonist of the player (its job ends, it fights) or lets it go back to work.
    bool draft(Entity pawn, bool drafted, std::string& error);
    // A fighter goes to `cell` and holds there, taking the best cover within holdRadius.
    bool orderMove(Entity pawn, CellPos cell, std::string& error);
    // A fighter shoots `target` whenever it can; kNoEntity lets it choose again.
    bool orderAttack(Entity pawn, Entity target, std::string& error);
    // Dev command until raids (F6): puts a pawn on a side. A pawn of another faction than the
    // player's fights at once and advances on its enemies; back on the player's side it works again.
    bool setFaction(Entity pawn, FactionId faction, std::string& error);

    // One step for every fighter, in id order: re-plan when due, walk, crouch, step out and shoot.
    // Runs after the job system and before movement.
    void step();

    // Call before a thing is destroyed: drops its path request.
    void forget(Entity thing);
    // After loading a save: checks the fighters' path tickets.
    bool afterLoad(std::string& error);
    // Hashes the fighter components of one thing (Sim::stateHash calls it for every thing).
    void hashThing(Entity e, StateHasher& h) const;

    // Counters for tests and simrun.
    uint64_t plans() const { return plans_; }
    uint64_t sightChecks() const { return sightChecks_; }

private:
    struct PawnInfo {
        Entity pawn = kNoEntity;
        FactionId faction = kPlayerFaction;
        float x = 0.0f;
        float y = 0.0f;
        int level = 0;
        bool able = false;    // not downed
        bool fighter = false;
        bool animal = false;  // wild animals are nobody's enemy unless ordered or provoked (animal-life.md, N5)
        Stance stance = Stance::Standing;  // as it stands now (prone when downed)
    };
    struct Candidate {
        CellPos cell;
        float score = 0.0f;
        CellPos fireCell;
        Stance fireStance = Stance::Crouching;
        bool canFire = false;
    };

    struct Incoming {
        Entity shot = kNoEntity;
        Entity launcher = kNoEntity;
        float x, y, z, vx, vy, vz;
        int level;
        int age;
    };

    void plan(Entity pawn, Fighter& f);
    bool dodge(Entity pawn, Fighter& f);
    bool dive(Entity pawn, Fighter& f);
    bool considerThrow(Entity pawn, const Fighter& f);
    void tacticalReload(Entity pawn, const Fighter& f, bool canShoot);
    void act(Entity pawn, Fighter& f);
    void see(Entity pawn, Fighter& f, const PawnInfo& self);
    Entity chooseTarget(Entity pawn, const Fighter& f, const PawnInfo& self) const;
    bool fireOption(CellPos cell, const PawnInfo& target, float range, float tx, float ty, Candidate& c);
    bool seesFrom(CellPos cell, Stance stance, const PawnInfo& target);
    void walkToSpot(Entity pawn, Fighter& f);
    void stepTo(Entity pawn, CellPos cell);
    void stop(Entity pawn, Fighter& f);
    const PawnInfo* info(Entity pawn) const;
    float weaponRange(Entity pawn) const;
    bool badlyHurt(Entity pawn) const;
    int64_t now() const;
    void planCharge(Entity pawn, Fighter& f, CellPos goal);
    bool meleeAct(Entity pawn, Fighter& f);

    Things& things_;
    Map& map_;
    CombatSystem& combat_;
    HealthSystem& health_;
    JobSystem& jobs_;
    Pathfinder& pathfinder_;
    const DefDatabase& defs_;
    const GameTime& time_;
    FighterRules rules_;
    std::vector<float> range_;  // per def id: the gun's range in metres, 0 if not a gun
    std::vector<PawnInfo> pawns_;  // scratch: every pawn on the map this step, by id
    std::vector<Candidate> candidates_;
    std::vector<CellPos> friendSpots_;   // scratch for one plan
    std::vector<KnownEnemy> merged_;     // scratch for sharing what friends know
    std::vector<Entity> order_;
    std::vector<Incoming> incoming_;     // scratch: projectiles in flight this step, by id
    ExplosiveSystem* explosives_ = nullptr;
    std::vector<GrenadeDanger> blasts_;  // scratch: grenades about to go off this step, by id
    std::vector<Event>* events_ = nullptr;
    MeleeSystem* melee_ = nullptr;
    uint64_t plans_ = 0;
    uint64_t sightChecks_ = 0;
};

}  // namespace colony
