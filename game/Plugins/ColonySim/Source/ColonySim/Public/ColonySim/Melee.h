#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"
#include "ColonySim/FightResponse.h"

#include <cstdint>
#include <functional>
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

enum class MeleeDefense : uint8_t { None, Dodge, Block, Backstep };

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
    // Focus and footwork (melee section 4, docs/design/focus.md).
    Entity focus = kNoEntity;    // the pawn it is locked on to: it faces it, and its swings go at its centre
    bool focusPlayer = false;    // the player set it (FocusCommand); else the fighter AI did and keeps it up to date
    bool low = false;            // the swing under way goes down at a body on the ground (Unreal plays a downward blow)
    int8_t circleDir = 1;        // footwork: 1 circles to its left round the target, -1 to its right
    int64_t nextFootworkAt = 0;  // the next footwork choice comes no sooner than this step
    int64_t footworkUntil = 0;   // stepping round or back until this step (the snapshot's `footwork`)
    int64_t engagedAt = 0;       // the step it last swung at an enemy (a fighter fresh from a swing follows one that runs)
    FightResponse response = FightResponse::Attack;  // what it does about enemies (SetFightResponseCommand)
    // Lock-on (melee section 5, docs/design/lock-on.md): the last step the player's pawn saw its focus.
    int64_t focusSeenAt = 0;
    // Tab's round (melee section 6, sneed 2026-11-19): the targets Tab has already given this round, and the step of the
    // last Tab. Tab gives the nearest one not yet given, so it walks out through every enemy instead of swapping two.
    std::vector<Entity> focusTried;
    int64_t focusTriedAt = 0;
    // A finisher (melee section 7, sneed 2026-11-20): a blow at a body on the ground with its own slow move (the spear
    // rammed down, the club smashed, the sword driven into the chest, the knife slammed down), 1 + its index in
    // MeleeRules finishers (0: none); the step it began and its whole length in steps (wind-up and recovery).
    uint8_t finisher = 0;
    int64_t finisherFrom = 0;
    int finisherSteps = 0;
};

// One finisher (MeleeRules `finishers`, ours: RimWorld has none). Keyed by weapon def name or attack style; `move` names
// the drawn motion (Spear, Club, Sword, Knife), `seconds` its whole length and `hitSeconds` when the blow lands, `standOff`
// the metres from the body's centre the attacker steps to, `kneel` whether it goes down on its knees, `bend` the degrees
// it bends over at the waist. `clip` (sneed 2026-11-22: "the whole body as an animation", not arms posed by hand): a
// MotionClips def the whole body plays, its blow landing at `hitSeconds`; with it Unreal draws no hand-made pose, kneel
// or bend, and the weapon simply stays in the hand. A clip finisher starts only within `standOff` + `clipSlack` metres of
// the body (it does not glide the attacker in); further off the weapon's plain low blow plays instead.
struct MeleeFinisher {
    std::string key;
    std::string move;
    std::string clip;
    float clipSlack = 0.45f;
    // When in the clip (its seconds from its start) the blade comes down on the body, if not the clip's own measured
    // hitSeconds: those are mid-swing, the weapon still a metre up (PC round 3, 2026-11-22). 0: the clip's own.
    float clipHitSeconds = 0.0f;
    float seconds = 1.6f;
    float hitSeconds = 1.0f;
    float standOff = 0.6f;
    bool kneel = false;
    float bend = 25.0f;
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
    // sneed 2026-11-23: every melee attack 33% faster (windup, recovery and the clip with them), and a person's bare
    // fists four times faster and weak (quick jabs). Times are multiplied, power too.
    float attackTimeFactor = 1.0f;
    float unarmedTimeFactor = 1.0f;   // a person's (humanlike) own tools: fists and head
    float unarmedPowerFactor = 1.0f;
    int unarmedMinWindup = 9;
    // sneed 2026-11-23: the player's swing from standing plays on the whole body, so the feet step into a thrust instead
    // of the chest leaning over planted legs; walking (faster than stepInMaxSpeed, m/s) it stays the arms' over walking
    // legs. With stepInFreesFeet, starting to walk during a standing swing hands it to the upper body there and then.
    // Both off: every player swing is the upper body's, as before (the revert).
    bool playerStepIn = false;
    bool stepInFreesFeet = false;
    float stepInMaxSpeed = 0.6f;
    int commitSteps = 6;              // steps before the hit at which the strike's line is fixed
    float reactionSteps[2] = {9.0f, 3.0f};  // at Melee skill 0 and 20: steps to read a swing
    float reactionJitter = 4.0f;      // plus or minus this, picked per swing
    int blockSteps = 4;               // to raise the weapon
    int blockRecoverySteps = 12;      // the blocker's stagger: no swing of its own meanwhile
    int playerBlockSteps = 20;        // how long the player's block holds
    int playerBlockHoldSteps = 6;     // held: each press while it is up keeps it up this much longer
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
    // Blows at a body on the ground (Melee::low; docs/design/focus.md "Blows at the downed"): clips by style or by weapon
    // def name (a weapon's own list wins): a stab down for knives and spears, an overhead smash for clubs and hammers,
    // a hack for blades and axes. A style with none of them measured swings its usual clips.
    std::vector<std::pair<std::string, std::vector<std::string>>> lowAttacks;
    // Finishers (sneed 2026-11-20): a person's blow at a body on the ground with a weapon listed here (by def name, else
    // its attack style) is a finisher instead of a low clip.
    std::vector<MeleeFinisher> finishers;
    float finisherStepMetres = 0.06f;  // the attacker closes to its finisher's standOff at most this far a step
    std::string unarmedStyle = "Unarmed";
    float maxAttackClipSeconds = 1.6f;
    float maxAttackHitSeconds = 0.8f;
    float maxAttackTravel = 0.8f;  // metres (for a 1.80 m man): a clip that carries the body further is left out too
    // Focus (sneed, 2026-11-17; docs/design/focus.md). Ours: Dark Souls' lock-on has no numbers to copy.
    float focusRange = 20.0f;         // Tab picks targets this close
    float focusBreakRange = 25.0f;    // a focus further off than this lets go by itself
    float focusFrontCos = 0.5f;       // targets within 60 degrees of where it faces come first...
    float focusBehindFactor = 2.0f;   // ...ones behind count this many times further off
    float aiFocusRange = 8.0f;        // a melee fighter locks on to its target this close (and faces it)
    // Lock-on, Elden Ring style (docs/design/lock-on.md, ours).
    float lockBigBodySize = 2.0f;     // a target this big (a bear and up) is struck at its body part nearest the attacker
    int lockSightLostSteps = 90;      // the player's lock lets go after this long without seeing the target
    float orbitRadialShare = 0.35f;   // a locked-on step with melee mostly sideways (radial part under this share) circles the target
    // Tab and flicks (sneed 2026-11-19, ours): Tab's round starts again after this long without a Tab; a flick takes
    // the nearest enemy on that side within this many degrees of the current one.
    int focusRoundSteps = 150;
    float flickMaxDegrees = 150.0f;
    // Landing blows (sneed 2026-11-19, ours). The player's swing with no lock takes the nearest enemy within this cosine
    // of straight ahead and in reach; a locked target this far past its reach still gets the swing (the player walks on
    // under it). A blow at someone who didn't dodge it follows them to the blow if they moved no more than
    // strikeTrackDegrees off its line (a step aside that wasn't a dodge doesn't make it whiff).
    float assistCos = 0.6f;
    float playerReachSlack = 0.6f;
    float corpseReachExtra = 0.6f;  // a lying corpse is long: the player's swing reaches its centre this much further
    float strikeTrackDegrees = 35.0f;
    // Defending (sneed 2026-11-19, ours): a defender that reads a swing in time defends with this chance by Melee skill
    // (0 and 20); with a weapon it parries parryShare of those times, else (and the rest) it gets out of the way: a roll
    // from a heavy blow, else a step back (backstepShare) or aside. The step back goes backstepMetres in backstepSteps.
    float defendChance[2] = {0.35f, 0.85f};
    float parryShare = 0.6f;
    float backstepShare = 0.4f;
    float backstepMetres = 1.3f;
    int backstepSteps = 8;
    // Footwork (sneed: "move around a bit without compromising their ability to attack"; ours). Between its own swings,
    // in reach of its target, a melee fighter steps round it or back out of a blow it can't stop.
    int footworkEverySteps = 24;      // a choice at most this often
    float footworkChance[2] = {0.35f, 0.8f};  // at Melee skill 0 and 20: the chance it moves at a choice
    float footworkTraitBonus = 0.2f;  // and this much more with a footwork trait (Nimble)
    std::vector<std::string> footworkTraits = {"Nimble"};
    float recklessFootwork = 0.5f;    // a reckless side (raiders) moves this share as often: it presses
    float circleMetres = 0.6f;        // a step round the target...
    int circleSteps = 18;             // ...over this many steps (2 m/s)
    float circleFlip = 0.3f;          // the chance it changes the way it circles at a choice
    float backShare = 0.5f;           // of the footwork chance: a step back from a blow it can't stop
    float circleShare = 0.6f;         // of the footwork chance: a step round the enemy
    float backMetres = 0.6f;          // a step back out of reach of a blow it can't stop
    int backSteps = 12;
    float crowdedShare = 0.6f;        // closer than this share of its engage distance it steps back to it
    float lineOfFireRange = 30.0f;    // footwork keeps out of the line of a friend's gun at the target within this range...
    float lineOfFireClearance = 0.8f; // ...by this many metres
    float chaseSeconds = 6.0f;        // a fighter that swung within this long follows a target that runs (whatever its side)
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
    // The player's block: raises the weapon (or a person's fists) for playerBlockSteps (after blockSteps); sent again
    // while it is up (the button held) it stays up playerBlockHoldSteps longer. Not with a gun in hand.
    bool block(Entity pawn, std::string* why = nullptr);

    // Whether the pawn could start a swing now (not winding up or recovering, able).
    bool ready(Entity pawn) const;
    bool winding(Entity pawn) const;
    // The player's standing swing (MeleeRules playerStepIn) carries the whole body; when the player walks during it
    // (`move` metres asked this step) and stepInFreesFeet is on, the swing goes on in the upper body and the feet are the
    // player's again. True if the feet are free (now or already).
    bool freeFeetToWalk(Entity pawn, float move);
    bool blocking(Entity pawn) const;
    // A finisher under way (sneed 2026-11-20): its MeleeRules entry, else null; how far through it is (0 to 1).
    const MeleeFinisher* finisherOf(Entity pawn) const;
    float finisherProgress(Entity pawn) const;
    // The finisher a melee weapon def would use at a body on the ground (1 + its index; 0 none).
    int finisherFor(DefId weapon) const;
    // Whether finisher `finisher` (1 + its MeleeRules index) plays its whole-body clip (sneed 2026-11-22): the clip is
    // named and measured. Unreal then draws no hand-made pose, kneel or bend for it.
    bool finisherPlaysClip(int finisher) const;
    // When in its clip finisher `finisher`'s blow lands (clip seconds from the clip's start): its clipHitSeconds, else the
    // clip's own hitSeconds. 0 if it plays no clip.
    float finisherClipHit(int finisher) const;
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
    // The attack clips a melee weapon def (or a body def's own tools) swings with: its hold style's, the short ones
    // (docs/design/weapon-hold.md, body-motion.md). Empty for a def with no melee tools.
    std::vector<DefId> attackClipsOf(DefId def) const;
    // The clips it swings with at a body on the ground (focus.md); empty: it uses attackClipsOf.
    std::vector<DefId> lowAttackClipsOf(DefId def) const;
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

    // Focus (docs/design/focus.md). The player's FocusCommand: lock on to `target`, cycle to the next nearest target
    // (Tab), or let go (F1). False, with a reason, if it can't (no pawn, nothing to lock on to).
    // Next and the flicks (Left, Right) take where the camera looks (viewX, viewY; zero: where the body faces) and
    // `anySide` (also the pawn's own side: the practice range's targets).
    bool focusCommand(Entity pawn, Entity target, FocusMode mode, std::string* why = nullptr, float viewX = 0.0f, float viewY = 0.0f,
                      bool anySide = false);
    // The fighter AI: lock on to `target` (kNoEntity lets go). Never overrides a focus the player set.
    void setAiFocus(Entity pawn, Entity target);
    // The pawn it is locked on to, or kNoEntity.
    Entity focusOf(Entity pawn) const;
    // Which pawns count as enemies of a pawn, for Tab (Sim sets it: factions, mad animals). Unset: any other pawn.
    void setHostile(std::function<bool(Entity, Entity)> hostile) { hostile_ = std::move(hostile); }
    // Turns every pawn locked on to a target within reach of sight to face it (after movement turned it to its walk), so
    // it steps sideways and back facing its enemy. Runs from the combat step.
    void faceFocus();
    // Where a swing at `target` goes (lock-on.md): a big body's part nearest the attacker, a person's head for fists,
    // else its chest (CombatSystem::aimPoint). False when the target can't be aimed at.
    bool focusPoint(Entity attacker, Entity target, bool fists, float& x, float& y, float& z) const;
    // The player's step (dx, dy) while locked on with no gun in hand: a mostly sideways step walks round the focus at
    // the same distance (Elden Ring circling). Leaves it alone otherwise.
    void orbitMove(Entity pawn, float& dx, float& dy) const;
    // The player's swing with no target: its focus if in reach, else a body on the ground before it in reach (the blow
    // goes down at it), else kNoEntity (straight ahead).
    Entity playerTarget(Entity pawn) const;
    // The target is in reach of the player's swing: its reach plus playerReachSlack (the player walks on under it).
    bool inPlayerReach(Entity pawn, Entity target) const;
    // A corpse that can still be cut up lying before the player within reach of its swing (within 60 degrees; a lying
    // body reaches half its length further), nearest first; kNoEntity for none. A swing at nobody with no living one
    // to take it goes down at this one (sneed, 2026-11-22: "melee weapons need to hit corpses if I swing at them").
    Entity corpseAhead(Entity pawn) const;
    // Who the player's swing may be steered at without a lock (sneed 2026-11-20: "a non-friendly, even neutral"): Sim sets
    // it (another side, a visitor, a wild animal; not its own side, tame animals or prisoners). Unset: hostile ones.
    void setNotFriendly(std::function<bool(Entity, Entity)> notFriendly) { notFriendly_ = std::move(notFriendly); }
    // Footwork (the fighter AI between swings, in reach of `target`): maybe a step round it, or back out of a blow it
    // can't stop. True if it moved. `reckless`: its side presses (moves less).
    bool footwork(Entity pawn, Entity target, bool reckless);
    // A fighter swung at an enemy within MeleeRules chaseSeconds: it follows one that runs.
    bool engagedRecently(Entity pawn) const;
    // The fight response (SetFightResponseCommand).
    bool setResponse(Entity pawn, FightResponse response, std::string* why = nullptr);
    FightResponse responseOf(Entity pawn) const;

    // One step: defenders act on swings they read, swings wind up, strike and recover. Runs after
    // movement (so strikes see where pawns are now) and before combat.
    void step();

    void hashThing(Entity e, StateHasher& h) const;

    // Counters for tests and simrun.
    uint64_t swings() const { return swings_; }
    uint64_t hits() const { return hits_; }
    // Blows that hurt a building or a tree (docs/design/structure-damage.md).
    uint64_t structureHits() const { return structureHits_; }
    uint64_t dodges() const { return dodges_; }
    uint64_t blocks() const { return blocks_; }
    uint64_t armBlocks() const { return armBlocks_; }  // weapon blows taken on a bare forearm held up
    uint64_t guardBreaks() const { return guardBreaks_; }
    uint64_t rolls() const { return rolls_; }
    uint64_t footworks() const { return footworks_; }

private:
    struct Tool {
        std::string label;
        DefId damageType;
        float power = 0.0f;
        int cooldown = 60;  // steps for the whole swing
        int minWindup = 9;  // MeleeRules minWindup (or unarmedMinWindup), times attackTimeFactor
        float penetration = 0.0f;
        float weight = 1.0f;
    };
    struct ToolSet {
        std::vector<Tool> tools;
        float reach = 0.0f;
        std::vector<DefId> attackClips;  // MotionClips of its style that are short enough
        std::vector<DefId> lowAttackClips;  // the same for blows at a body on the ground (MeleeRules lowAttacks)
        int finisher = 0;                // 1 + its MeleeRules finishers index (0: none): its blow at a body on the ground
        float lunge = 0.0f;              // the least any of them steps forward by its blow (a man's metres)
    };

    const ToolSet* toolsOf(Entity pawn, bool& weapon, bool fists = false) const;
    const Tool* toolOf(Entity pawn, const Melee& m) const;
    void prepareDefense(Entity defender, Entity attacker, int stepsToHit, uint64_t seed);
    void defend(Entity defender, Melee& d);
    void sidestep(Entity defender, Melee& d);
    bool backstep(Entity defender, Melee& d);
    bool focusNext(Entity pawn, FocusMode mode, float viewX, float viewY, bool anySide, std::string* why);
    bool roll(Entity defender, Melee& d);
    void commit(Entity pawn, Melee& m);
    void followToBlow(Entity pawn, Melee& m);
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
    std::function<bool(Entity, Entity)> hostile_;
    std::function<bool(Entity, Entity)> notFriendly_;
    uint64_t swings_ = 0;
    uint64_t hits_ = 0;
    uint64_t structureHits_ = 0;  // blows that hurt a building or a tree
    uint64_t dodges_ = 0;
    uint64_t blocks_ = 0;
    uint64_t armBlocks_ = 0;
    uint64_t guardBreaks_ = 0;
    uint64_t rolls_ = 0;
    uint64_t footworks_ = 0;
};

}  // namespace colony
