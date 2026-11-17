#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace colony {

class Map;
class Plants;
class StateHasher;
class Things;

// Body motion: clips that really move the body. See docs/design/body-motion.md.
//
// sneed (2026-11-16): a roll, a sidestep, a stagger, a knockdown or a lunging swing must carry the body through the
// world the way the animation shows it, against walls, rocks, cliffs and other bodies, and never put it back where
//
// - Each clip's root path (how far forward and to the right the body goes, and how much it turns, sample by sample at
//   30 a second) is data: a MotionClips def, measured from the very animation Unreal plays (the MotionClips
//   commandlet) or, until it has been, written by hand.
// - A pawn playing a clip has a BodyMotion. Each step the system takes the next stretch of the path, scaled to the
//   body's height, and sweeps the body along it in short pieces. Ground it can't stand on stops it or makes it slide
//   along; another standing body is shoved by the share of the push its weight allows, and the rest of the push is
//   lost. Motion lost against something is gone: the body doesn't catch up later.
//
// Unreal reads `motionClip`, `motionTime` and `motionRate` from the snapshot and plays that clip at that moment with
// its root motion dropped; the actor follows the sim's position, which follows the same path, so the feet stay put.

// One clip's root path. Index i is the body's offset from where the clip began at i/30 clip seconds, in the frame the
// body faced then: metres forward and to the right for a 1.80 m man, degrees turned to the right. Right is the pawn's own right as the game draws it: (-y, x) of a facing (x, y).
struct MotionClip {
    DefId def;
    std::string name;   // the def's name
    std::string kind;   // Roll, Sidestep, Stagger, Knockdown, Attack, GetUp, Blast...
    std::string anim;   // the Unreal animation it was measured from (empty: hand-written)
    float from = 0.0f;     // where in the animation the clip begins (a trimmed clip), seconds; the path starts there
    float seconds = 0.0f;  // length at play rate 1
    float hitSeconds = -1.0f;  // attacks: when the blow lands (clip seconds); -1 if none
    bool keepWhenDown = false;  // knockdowns and falls go on when the pawn is down; others stop
    std::vector<float> forward;
    std::vector<float> right;
    std::vector<float> yaw;
    // The path at clip time t (seconds), interpolated; past the end it is the last sample.
    void sample(float t, float& f, float& r, float& turn) const;
};

// Which way a blow throws a body, seen from the body: 0 back, 1 forward, 2 left, 3 right.
COLONY_API int motionSideOf(float faceX, float faceY, float pushX, float pushY);

// A pawn playing a clip (or a plain push, `clip` invalid: a blast throwing it, or a roll with no clip def).
struct BodyMotion {
    DefId clip;
    float time = 0.0f;      // clip seconds played
    float rate = 1.0f;      // clip seconds per real second
    float faceX = 1.0f;     // the way the body faced when it began: the clip's forward
    float faceY = 0.0f;
    float scale = 1.0f;     // the body's size: path metres are times this
    float doneF = 0.0f;     // how far along the path it has been taken (clip frame, unscaled)
    float doneR = 0.0f;
    float doneYaw = 0.0f;
    // A plain push: `pushMetres` along (faceX, faceY), eased in and out between steps pushFrom and pushTo of
    // pushSteps (clip-less motions count steps, `time` is the step reached).
    float pushMetres = 0.0f;
    int pushSteps = 0;
    int pushFrom = 0;
    int pushTo = 0;
    bool turnBody = true;   // the clip's turn turns the pawn (attacks and rolls set their own facing: off)
    float lost = 0.0f;      // metres of the path lost against walls and heavier bodies
};

// The numbers (Rules/MotionRules). Ours: RimWorld has no body motion.
struct MotionRules {
    float stepMetres = 0.05f;    // the sweep moves a body this far at a time
    float bodyRadius = 0.25f;    // a man's body, for bodies meeting (pathing still uses kPawnRadius between walls)
    float minBodyRadius = 0.12f;
    float maxBodyRadius = 1.2f;
    float heightFollow = 1.0f;   // a person's paths scale with this share of their height (1: fully)
    float animalScalePerRoot = 1.0f;  // an animal's plain pushes scale with the root of its body size times this
    // Which clip plays for what (MotionClips defNames; a missing one falls back to a plain push below).
    std::string rollClip = "Motion_Roll";
    std::string sidestepClip[2] = {"Motion_SidestepLeft", "Motion_SidestepRight"};
    // By the way the blow throws the body: back, forward, left, right.
    std::string staggerClip[4] = {"Motion_StaggerBack", "Motion_StaggerForward", "Motion_StaggerLeft", "Motion_StaggerRight"};
    std::string knockdownClip[4] = {"Motion_KnockdownBack", "Motion_KnockdownForward", "Motion_KnockdownLeft", "Motion_KnockdownRight"};
    std::string getUpClip = "Motion_GetUp";
    // Plain pushes, for bodies with no clips (animals) or clips not there yet.
    float staggerMetres = 0.5f;
    int staggerSteps = 12;
    float knockdownMetres = 1.0f;
    int knockdownSteps = 20;
    float sidestepMetres = 0.7f;
    int sidestepSteps = 9;
    int blastSteps = 12;         // a blast throws a body its knockback metres over this many steps
    // Against tree trunks a body is never wider than this (an elephant or a T-rex still gets through a jungle the
    // pathfinder thinks it can walk). docs/design/plant-bodies.md.
    float trunkBodyRadiusMax = 0.4f;
    // Loose props (items whose def says `minor`): a body walking or thrown into one shoves it by weight.
    float propRadius = 0.15f;        // a prop's size on the ground
    float bodyKgPerSize = 60.0f;     // a body's weight per body size (RimWorld: a human's mass is 60)
    float propMaxMassShare = 0.5f;   // a prop heavier than this share of the body isn't moved (stepped round or over)
    float propCellMargin = 0.1f;     // a prop stays this far inside its own cell (storage and hauling go by cell)
};

class COLONY_API BodyMotionSystem {
public:
    BodyMotionSystem(Things& things, Map& map);
    ~BodyMotionSystem();
    BodyMotionSystem(const BodyMotionSystem&) = delete;
    BodyMotionSystem& operator=(const BodyMotionSystem&) = delete;

    const MotionRules& rules() const { return rules_; }
    // A pawn's weight for shoving (body size: a man 1, an elephant 4); unset: 1.
    void setMass(std::function<float(Entity)> mass) { mass_ = std::move(mass); }
    // A pawn's height, times 1.80 m (PawnLooks); unset or 0: 1.
    void setHeight(std::function<float(Entity)> height) { height_ = std::move(height); }
    // Whether a pawn stands in the way of others (false for the downed and the prone); unset: all spawned pawns do.
    void setSolid(std::function<bool(Entity)> solid) { solid_ = std::move(solid); }
    // Whether a pawn can still be moved by its clip (false for the dead and, unless the clip says, the downed).
    void setAble(std::function<bool(Entity)> able) { able_ = std::move(able); }

    const MotionClip* clip(DefId def) const;
    const MotionClip* clip(const std::string& name) const;
    DefId clipId(const std::string& name) const;

    // Plays `clip` on the pawn from the start, facing (faceX, faceY) (zero: the way it faces now), stretched to last
    // `steps` steps (0: its own length at rate 1). A motion already playing ends where the body is.
    // False (nothing changes) if the clip or the pawn is unknown.
    bool play(Entity pawn, DefId clip, float faceX = 0.0f, float faceY = 0.0f, int steps = 0, bool turnBody = true);
    // Plays the clip so that clip time `atSeconds` comes after `steps` steps (an attack's hit lands on the sim's hit
    // step); the rest of the clip plays at the same rate.
    bool playTimed(Entity pawn, DefId clip, float atSeconds, int steps, float faceX, float faceY, bool turnBody);
    // A plain push: `metres` along (dx, dy) over `steps` steps, moving only from step `from` to step `to`.
    bool push(Entity pawn, float dx, float dy, float metres, int steps, int from = 0, int to = -1);
    // A blow throws the body along (pushX, pushY): a person plays the stagger (or knockdown) clip for the way it goes,
    // seen from the body; anyone else, or a person whose clip isn't there, gets a plain push of the rules' metres
    // times `strength` (scaled by the body's size). False if nothing moved it (a zero direction).
    // `plainPush` false: only a clip moves it (a bullet's stagger doesn't shove a body that has no flinch clip).
    bool throwBody(Entity pawn, float pushX, float pushY, bool knockdown, float strength = 1.0f, bool plainPush = true);
    // A quick step along (dx, dy): to the body's left or right it is the sidestep clip, otherwise (or with no clip)
    // a plain push of sidestepMetres.
    bool sidestep(Entity pawn, float dx, float dy);
    // Tree trunks are solid posts the sweep stops at (docs/design/plant-bodies.md); null: no trunks.
    void setPlants(const Plants* plants) { plants_ = plants; }
    const Plants* plants() const { return plants_; }
    // Whether the pawn has clips (a person: a humanlike race); animals get plain pushes.
    void setHasClips(std::function<bool(Entity)> hasClips) { hasClips_ = std::move(hasClips); }
    bool hasClips(Entity pawn) const { return !hasClips_ || hasClips_(pawn); }
    void stop(Entity pawn);
    bool moving(Entity pawn) const;
    // Metres forward the clip carries the body by clip time t (for a man), 0 for an unknown clip.
    float forwardAt(DefId clip, float t) const;

    // Moves the pawn by (dx, dy) as a body: in pieces of stepMetres, stopping at ground it can't stand on (or
    // sliding along it), shoving the standing bodies it meets by the share their weight allows. Returns the metres
    // it really moved. Used by the clips and by anything else that throws a body about.
    float sweep(Entity pawn, float dx, float dy);
    // A body's radius for bodies meeting.
    float bodyRadius(Entity pawn) const;
    float mass(Entity pawn) const;
    float sizeScale(Entity pawn) const;

    // A body's radius against tree trunks: its body radius, at most trunkBodyRadiusMax.
    float trunkRadius(Entity pawn) const;
    // Every standing body shoves the loose props it overlaps out of its way, by the share of the push their weight
    // allows, keeping each in its own cell. Called once a step after walking and motion.
    void shoveProps();
    // A prop's weight in kg if its def is `minor` (the whole stack), or a negative number if it is not a loose prop.
    float propMass(Entity item) const;
    uint64_t propShoves() const { return propShoves_; }

    // One step of every motion, in entity order.
    void step();
    void hashThing(Entity e, StateHasher& h) const;

    uint64_t sweeps() const { return sweeps_; }
    uint64_t shoves() const { return shoves_; }
    uint64_t stopped() const { return stopped_; }

private:
    bool fits(Entity pawn, float fromX, float fromY, float x, float y, int level) const;
    void gatherBodies(Entity pawn, float x, float y, int level, float range);

    Things& things_;
    Map& map_;
    const Plants* plants_ = nullptr;
    MotionRules rules_;
    std::vector<MotionClip> clips_;
    std::unordered_map<std::string, size_t> byName_;
    std::vector<int32_t> byDef_;  // by DefId value: index into clips_, -1 none
    std::function<float(Entity)> mass_;
    std::function<float(Entity)> height_;
    std::function<bool(Entity)> solid_;
    std::function<bool(Entity)> able_;
    std::function<bool(Entity)> hasClips_;
    std::vector<Entity> order_;
    std::vector<Entity> near_;
    uint64_t sweeps_ = 0;
    uint64_t shoves_ = 0;
    uint64_t stopped_ = 0;
    uint64_t propShoves_ = 0;
    std::vector<float> propKg_;  // by DefId value: a minor item's mass per unit, negative for anything else
};

}  // namespace colony
