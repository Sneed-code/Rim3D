#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"

#include <cstdint>
#include <string>
#include <vector>

namespace colony {

class CombatSystem;
class HealthSystem;
class Map;
class StateHasher;
class Things;
struct GameTime;

// Melee: fists, teeth and melee weapons. See docs/design/melee.md.
//
// A swing has a wind-up, a hit moment and a recovery, timed in the sim. At the hit moment a strike
// line from the attacker's shoulder is traced against hitboxes (CombatSystem::trace), so reach is
// physical: a target that stepped back or aside is missed. Where the strike goes is fixed a little
// before the hit (`commitSteps`), with a spread sized so that a standing man is hit as often as
// RimWorld's melee hit chance says. A defender that sees the swing coming in time sidesteps (as
// often as RimWorld's dodge chance says) or raises its weapon to block. Damage goes through armor
// like a bullet's (CombatSystem::hitPawn).
//
// Unreal reads `meleePhase`, `meleeTarget`, `meleeStepsLeft` and `blocking` from the snapshot (which
// is why the snapshot includes this header), MeleeSwing and MeleeHit events, and sends MeleeCommand.

enum class MeleePhase : uint8_t {
    Ready,     // nothing to do
    Windup,    // drawing back: the hit comes when stepsLeft reaches 0
    Recovery,  // after the hit (or a blocked strike's stagger): no new swing until it is over
};

enum class MeleeDefense : uint8_t { None, Dodge, Block };

// On a pawn once it has swung or been swung at. Every field is saved.
struct Melee {
    MeleePhase phase = MeleePhase::Ready;
    int stepsLeft = 0;           // of the phase
    Entity target = kNoEntity;   // whom the swing is at; none: straight ahead (the player's own swing)
    uint8_t tool = 0;            // index into the tools it swings with (the weapon's or the body's)
    bool weaponTool = false;     // the weapon's tool, not the body's
    bool player = false;         // the player's own swing: straight at the target or ahead, no spread
    bool committed = false;      // where the strike goes is fixed (aim below)
    float aimX = 0.0f;           // world metres, z above sea level
    float aimY = 0.0f;
    float aimZ = 0.0f;
    uint64_t seed = 0;           // the swing's random choices (tool, spread, the defender's reading of it)
    // Defending against one swing.
    Entity threat = kNoEntity;   // the attacker it reacts to
    MeleeDefense defense = MeleeDefense::None;
    int64_t reactAt = 0;         // game second it acts on the swing (sidesteps, or raises its weapon)
    int64_t blockFrom = 0;       // blocking from this game second up to (not including) blockUntil
    int64_t blockUntil = 0;
    bool dodged = false;         // it sidestepped this swing
};

// The numbers (MeleeRules in data/defs/Rules/MeleeRules.json). The hit and dodge curves and the
// capacity weights are RimWorld's (wiki); reach and timing are ours.
struct MeleeRules {
    std::vector<float> hitCurveX = {0.0f, 10.0f, 20.0f, 40.0f};  // base score (Melee skill and offsets)...
    std::vector<float> hitCurveY = {0.5f, 0.8f, 0.9f, 0.96f};     // ...to the chance a standing man is hit
    std::vector<float> dodgeCurveX = {5.0f, 20.0f, 60.0f};
    std::vector<float> dodgeCurveY = {0.0f, 0.3f, 0.5f};
    int defaultSkill = 4;             // pawns without skills
    float hitCapacityWeight = 12.0f;  // base score per 100% of Manipulation and of Sight above 100%...
    float hitCapacityCap = 1.5f;      // ...counted up to this
    float dodgeMovingWeight = 18.0f;
    float dodgeSightWeight = 8.0f;
    float dodgeSightCap = 1.4f;
    float penetrationPerDamage = 0.015f;  // a tool's armor penetration when it gives none
    std::vector<std::string> chargeTraits = {"Brawler"};
    float bodyReach = 0.85f;          // metres from the pawn's centre that fists, teeth and claws reach
    float animalReachBase = 0.65f;    // an animal's teeth and claws reach this plus animalReachPerRoot times the root of its body size (N5)
    float animalReachPerRoot = 0.4f;
    float defaultWeaponReach = 1.2f;  // a melee weapon not in weaponReach
    float shoulderBelowEye = 0.25f;   // the strike starts this far below the eyes
    float strikeDepth = 0.35f;        // and goes this far past the point it aims at
    float reachSlack = 0.1f;          // a swing starts at a target whose centre is this far beyond the reach (its front is in reach)
    float standGround = 1.5f;         // a fighter falling back stands and fights instead when an enemy is this far beyond its reach
    float closeIn = 0.2f;             // a charging fighter walks on until the target is this far inside its reach (0.7 m at least)
    float windupShare = 0.3f;         // of a tool's cooldown spent winding up; the rest is recovery
    int minWindup = 9;
    int commitSteps = 6;              // steps before the hit at which the strike's line is fixed
    float reactionSteps[2] = {14.0f, 5.0f};  // at Melee skill 0 and 20: steps to read a swing
    float reactionJitter = 4.0f;      // plus or minus this, picked per swing
    float blockChance[2] = {0.1f, 0.5f};     // at Melee skill 0 and 20, holding a melee weapon
    int blockSteps = 4;               // to raise the weapon
    int blockRecoverySteps = 12;      // the blocker's stagger: no swing of its own meanwhile
    int playerBlockSteps = 20;        // how long the player's block holds
    int dodgeHoldSteps = 3;           // a fighter that sidestepped stays put this long after the hit
};

class COLONY_API MeleeSystem {
public:
    MeleeSystem(Things& things, Map& map, CombatSystem& combat, HealthSystem& health, const DefDatabase& defs,
                const GameTime& time, uint64_t seed);
    ~MeleeSystem();
    MeleeSystem(const MeleeSystem&) = delete;
    MeleeSystem& operator=(const MeleeSystem&) = delete;

    const MeleeRules& rules() const { return rules_; }
    void setEventLog(std::vector<Event>* events) { events_ = events; }

    // Starts a swing at `target` (a living pawn in reach), or straight ahead if it is kNoEntity and
    // `player` is set. False, with a reason in plain words, if the pawn can't swing now: downed, mid-shot,
    // still swinging or recovering, nothing to hit with, or the target out of reach.
    // `fists` swings the body's own tools even with a weapon in the hands (a social fight, C20).
    bool attack(Entity pawn, Entity target, bool player, std::string* why = nullptr, bool fists = false);
    // The player's block: raises the weapon for playerBlockSteps (after blockSteps). Needs a melee weapon.
    bool block(Entity pawn, std::string* why = nullptr);

    // Whether the pawn could start a swing now (not winding up or recovering, able).
    bool ready(Entity pawn) const;
    bool winding(Entity pawn) const;
    bool blocking(Entity pawn) const;
    // Metres from the pawn's centre its strike reaches on the ground: its melee weapon's, else its body's.
    float reach(Entity pawn, bool fists = false) const;
    // The target's centre is within the pawn's reach (plus reachSlack: its front is).
    bool inReach(Entity pawn, Entity target, bool fists = false) const;
    // The fighter AI charges with this pawn: no gun, and a melee weapon in hand or a trait that says
    // so (Brawler). Others only strike back at enemies in reach.
    bool charges(Entity pawn) const;
    // RimWorld's numbers for the pawn as it is now.
    int meleeSkill(Entity pawn) const;
    float hitChance(Entity pawn) const;
    float dodgeChance(Entity pawn) const;
    float blockChance(Entity pawn) const;

    // One step: defenders act on swings they read, swings wind up, strike and recover. Runs after
    // movement (so strikes see where pawns are now) and before combat.
    void step();

    void hashThing(Entity e, StateHasher& h) const;

    // Counters for tests and simrun.
    uint64_t swings() const { return swings_; }
    uint64_t hits() const { return hits_; }
    uint64_t dodges() const { return dodges_; }
    uint64_t blocks() const { return blocks_; }

private:
    struct Tool {
        std::string label;
        DefId damageType;
        float power = 0.0f;
        int cooldown = 60;  // steps for the whole swing
        float penetration = 0.0f;
        float weight = 1.0f;
    };
    struct ToolSet {
        std::vector<Tool> tools;
        float reach = 0.0f;
    };

    const ToolSet* toolsOf(Entity pawn, bool& weapon, bool fists = false) const;
    const Tool* toolOf(Entity pawn, const Melee& m) const;
    void prepareDefense(Entity defender, Entity attacker, int stepsToHit, uint64_t seed);
    void defend(Entity defender, Melee& d);
    void sidestep(Entity defender, Melee& d);
    void commit(Entity pawn, Melee& m);
    void strike(Entity pawn, Melee& m);
    void face(Entity pawn, float x, float y);
    bool shoulder(Entity pawn, float& x, float& y, float& z) const;
    float capacity(Entity pawn, const char* name) const;
    float traitOffset(Entity pawn, const char* stat) const;
    int64_t now() const;

    Things& things_;
    Map& map_;
    CombatSystem& combat_;
    HealthSystem& health_;
    const DefDatabase& defs_;
    const GameTime& time_;
    uint64_t seed_;
    MeleeRules rules_;
    std::vector<ToolSet> tools_;  // by Things DefId value: a melee weapon's tools, or a pawn def's body tools
    std::vector<Entity> order_;
    std::vector<Event>* events_ = nullptr;
    uint64_t swings_ = 0;
    uint64_t hits_ = 0;
    uint64_t dodges_ = 0;
    uint64_t blocks_ = 0;
};

}  // namespace colony
