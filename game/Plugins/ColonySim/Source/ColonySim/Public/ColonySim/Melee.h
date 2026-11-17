#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"

#include <cstdint>
#include <string>
#include <vector>
#include <utility>

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
// before the hit (`commitSteps`), with only a small wobble of the hand by skill: no hit-chance dice
// (docs/design/real-time-combat.md). Melee skill makes swings faster and harder. A defender that reads
// the swing in time always does the sensible thing: a shield goes up (Fighters), a melee weapon parries
// (until its guard breaks), empty hands sidestep. Damage goes through armor like a bullet's
// (CombatSystem::hitPawn).
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
    // Guard and breath (melee section 2): parries in a row break the guard; a sidestep or its own swing leaves a gap.
    uint8_t guardCount = 0;      // parries in a row, each within guardWindowSteps of the last
    int64_t guardFrom = 0;       // step of its last parry
    int64_t guardBrokenUntil = 0;  // no parrying before this step
    int64_t noDefendUntil = 0;   // no defending at all before this step (after a sidestep, or its own swing)
    // Phase 2 (melee section 3): when it began holding its swing at a raised guard; 0 when not waiting.
    int64_t guardWaitFrom = 0;
    int64_t nextRollAt = 0;      // an AI fighter rolls at most once every aiRollEverySteps
};

// The numbers (MeleeRules in data/defs/Rules/MeleeRules.json). Ours and sneed's: RimWorld's hit and dodge
// chances are gone (docs/decisions/0010-real-time-combat.md).
struct MeleeRules {
    int defaultSkill = 4;             // pawns without skills
    float skillSpeed[2] = {0.8f, 1.2f};   // at Melee skill 0 and 20: a swing takes its time divided by this (sneed)
    float skillDamage[2] = {0.8f, 1.2f};  // at Melee skill 0 and 20: damage and armour penetration times this (sneed)
    float aimWobble[2] = {0.12f, 0.03f};  // at Melee skill 0 and 20: metres the hand wobbles off the aimed point
    std::vector<std::pair<std::string, float>> reactionTraitSteps = {{"Nimble", -3.0f}};  // traits that read swings quicker
    int guardBlocks = 3;              // this many parries, each within guardWindowSteps of the last, break the guard
    int guardWindowSteps = 150;
    int guardBreakSteps = 75;         // a broken guard: stagger, and no parrying for this long
    int dodgeRecoverySteps = 30;      // after a sidestep, no defending for this long
    int recoveryNoDefendSteps = 12;   // after its own strike, no defending for this long
    int guardWaitSteps = 60;          // an AI fighter holds its swing at a raised guard facing it for up to this long
    float guardFacingCos = 0.5f;      // a guard faces the attacker when the defender looks within 60 degrees of it
    float rollClearance = 2.0f;       // an AI roll needs this many metres of open ground in its direction
    int aiRollEverySteps = 180;       // and comes at most this often: between rolls it sidesteps (fights don't turn into chases)
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
    int blockSteps = 4;               // to raise the weapon
    int blockRecoverySteps = 12;      // the blocker's stagger: no swing of its own meanwhile
    int playerBlockSteps = 20;        // how long the player's block holds
    int dodgeHoldSteps = 3;           // a fighter that sidestepped stays put this long after the hit
    float strongPowerShare = 1.15f;   // a swing is strong when its tool's power is this share of the set's mean or more (AN1, ours)
    float arcHalfDegrees = 30.0f;     // a sweeping blow (part C) can meet a pawn this far either side of its aimed line
    int arcRaysPerSide = 2;           // lines traced on each side of the aimed one
    std::vector<std::string> arcDamageTypes = {"Cut", "Slash", "Blunt", "Crush", "Scratch"};  // the sweeping blows
    // Body motion (docs/design/body-motion.md). A person's reach follows this share of their height (arm and weapon:
    // the weapon is the same in anyone's hands, the arm isn't).
    float reachHeightFollow = 0.5f;
    // Attack clips by fighting style, and each melee weapon's style (fists: unarmedStyle). A swing plays one of its
    // style's clips, timed so the clip's blow lands on the sim's hit step; the clip's step forward really moves the body.
    // Clips longer than maxAttackClipSeconds, or whose blow comes later than maxAttackHitSeconds, are left out: fights
    // are fast (sneed).
    std::vector<std::pair<std::string, std::vector<std::string>>> styleAttacks;
    std::vector<std::pair<std::string, std::string>> weaponStyle;
    std::string unarmedStyle = "Unarmed";
    float maxAttackClipSeconds = 1.6f;
    float maxAttackHitSeconds = 0.8f;
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
    // The swing under way (or the last one) uses a strong tool: one whose power is MeleeRules `strongPowerShare`
    // of its set's mean or more (a longsword's cut against its point). Unreal plays a heavy blow for it (AN1).
    bool strongSwing(Entity pawn) const;
    // Metres from the pawn's centre its strike reaches on the ground: its melee weapon's, else its body's.
    float reach(Entity pawn, bool fists = false) const;
    // How far from the target's centre the pawn can start a swing that lands: its reach (which follows its height),
    // plus the part of a big target's body that sticks out towards it (a man's body is already counted), plus the step
    // forward its attack clips take before the blow.
    float engageDistance(Entity pawn, Entity target, bool fists = false) const;
    // The target's centre is within the pawn's engage distance (plus reachSlack).
    bool inReach(Entity pawn, Entity target, bool fists = false) const;
    // The fighter AI charges with this pawn: no gun, and a melee weapon in hand or a trait that says
    // so (Brawler). Others only strike back at enemies in reach.
    bool charges(Entity pawn) const;
    int meleeSkill(Entity pawn) const;
    // Melee skill's factors (MeleeRules skillSpeedBySkill, skillDamageBySkill): >1 is faster or harder.
    float swingSpeed(Entity pawn) const;
    float swingDamage(Entity pawn) const;
    // Steps the pawn takes to read a swing at it, before the per-swing jitter: skill and traits (Nimble).
    float reactionSteps(Entity pawn) const;
    // Metres its hand wobbles off the point it aims a swing at.
    float aimWobble(Entity pawn) const;
    // The pawn's guard is broken: it can't parry until it recovers.
    bool guardBroken(Entity pawn) const;
    // `target` holds a raised guard (a shield up, or a parry) facing `pawn`, and its guard isn't broken.
    bool guardedAgainst(Entity target, Entity pawn) const;
    // The fighter AI asks before a swing at `target`: true to hold it, because the target's guard is up and facing it
    // and it has waited less than guardWaitSteps (real-time-combat.md phase 2). Keeps the wait in the pawn's Melee.
    bool holdSwing(Entity pawn, Entity target);

    // One step: defenders act on swings they read, swings wind up, strike and recover. Runs after
    // movement (so strikes see where pawns are now) and before combat.
    void step();

    void hashThing(Entity e, StateHasher& h) const;

    // Counters for tests and simrun.
    uint64_t swings() const { return swings_; }
    uint64_t hits() const { return hits_; }
    uint64_t dodges() const { return dodges_; }
    uint64_t blocks() const { return blocks_; }
    uint64_t guardBreaks() const { return guardBreaks_; }
    uint64_t rolls() const { return rolls_; }

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
        std::vector<DefId> attackClips;  // MotionClips of its style that are short enough
        float lunge = 0.0f;              // the least any of them steps forward by its blow (a man's metres)
    };

    const ToolSet* toolsOf(Entity pawn, bool& weapon, bool fists = false) const;
    const Tool* toolOf(Entity pawn, const Melee& m) const;
    void prepareDefense(Entity defender, Entity attacker, int stepsToHit, uint64_t seed);
    void defend(Entity defender, Melee& d);
    void sidestep(Entity defender, Melee& d);
    bool roll(Entity defender, Melee& d);
    void commit(Entity pawn, Melee& m);
    void strike(Entity pawn, Melee& m);
    bool arcSwing(DefId damageType) const;
    void face(Entity pawn, float x, float y);
    bool shoulder(Entity pawn, float& x, float& y, float& z) const;
    float swingTimeFactor(Entity pawn) const;
    int64_t now() const;

    Things& things_;
    Map& map_;
    CombatSystem& combat_;
    HealthSystem& health_;
    const DefDatabase& defs_;
    const GameTime& time_;
    uint64_t seed_;
    MeleeRules rules_;
    std::vector<uint8_t> arcTypes_;  // by DamageTypes DefId value: 1 for a sweeping blow
    std::vector<ToolSet> tools_;  // by Things DefId value: a melee weapon's tools, or a pawn def's body tools
    std::vector<Entity> order_;
    std::vector<Event>* events_ = nullptr;
    uint64_t swings_ = 0;
    uint64_t hits_ = 0;
    uint64_t dodges_ = 0;
    uint64_t blocks_ = 0;
    uint64_t guardBreaks_ = 0;
    uint64_t rolls_ = 0;
};

}  // namespace colony
