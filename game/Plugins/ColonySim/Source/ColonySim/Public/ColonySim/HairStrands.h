#pragma once

#include "ColonySim/Api.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace colony {

// Hair strands (docs/design/hair-physics.md): long hair that hangs, swings and rests on the body instead of passing
// through it. Cosmetic only, never sim state: it lives in the sim plugin so it builds and is tested without Unreal.
// Unreal's MakeHuman hair node feeds it the animated hair bones and the body's collision shapes each frame and turns
// the points it returns back into bone rotations.
//
// Each strand is a chain of points (root first) along a chain of hair bones. The root is pinned to the head; the others
// fall under gravity, keep their lengths, are pulled softly back towards the hair style's own shape, lag behind the
// head's starts, stops and turns, and are pushed out of capsules round the head, neck, upper back, chest, shoulders and
// upper arms. Neighbouring strands of one sheet of hair are kept from drifting apart or through each other.
// Units: cm, seconds; world space (z up).

struct HairVec {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// A capsule: the points within `radius` of the segment a-b (a sphere when a == b).
struct HairCapsule {
    HairVec a;
    HairVec b;
    float radius = 0.0f;
};

// The tuning (the defaults are the game's).
struct HairSettings {
    float gravity = 980.0f;        // cm/s^2, straight down
    float damping = 7.0f;          // 1/s: how fast a strand's swing (relative to the head) dies away
    float rootStiffness = 400.0f;  // 1/s^2: the spring pulling the first segment back to the style's shape (it sags
                                   // gravity / stiffness, about 2.5 cm, from its styled place)
    float tipStiffness = 25.0f;    // 1/s^2: the same at the last segment, so the ends hang loose (geometric in between)
    float maxAngleDeg = 80.0f;     // the farthest a segment turns from the style's shape
    float maxSpeed = 700.0f;       // cm/s: the fastest a point moves relative to the head
    float substep = 1.0f / 120.0f; // longest simulation step, s
    int iterations = 3;            // constraint passes per step
    float followDamping = 0.9f;    // share of the move given to the point below taken off a point's speed (0..1)
    float friction = 20.0f;        // 1/s: how fast a touching point's sliding dies away
    float grip = 4.0f;             // cm/s: a touching point sliding slower than this stops (hair rests on the body)
    float sheetStretch = 1.35f;    // neighbouring strands stay within these times their styled distance apart
    float sheetSquash = 0.55f;
    float teleport = 60.0f;        // cm: a root jumping further in one frame starts the hair again at rest
};

class COLONY_API HairStrands {
public:
    // Strands that form one sheet (neighbours round the head), by index into the goal lists.
    void setSheetNeighbours(std::vector<std::pair<int, int>> pairs) { neighbours_ = std::move(pairs); }

    // One frame of `dt` seconds. `goals`: per strand, where the animation puts its points this frame (root first, at
    // least two each). `capsules`: the body this frame. The result is points().
    void step(float dt, const std::vector<std::vector<HairVec>>& goals, const std::vector<HairCapsule>& capsules,
              const HairSettings& settings);

    // No dynamics: the animated points pushed out of the body (far from the camera). The next step() goes on from here.
    void pose(const std::vector<std::vector<HairVec>>& goals, const std::vector<HairCapsule>& capsules,
              const HairSettings& settings);

    const std::vector<std::vector<HairVec>>& points() const { return points_; }
    void reset() { started_ = false; }
    bool started() const { return started_; }

private:
    void start(const std::vector<std::vector<HairVec>>& goals, const std::vector<HairCapsule>& capsules,
               const HairSettings& settings);
    // Lengths, the angle limit and the body, root to tip, on one strand (goal: this moment's animated points).
    bool solveStrand(std::vector<HairVec>& points, const std::vector<HairVec>& goal,
                     const std::vector<HairCapsule>& capsules, float cosMax, float sinMax, std::vector<HairVec>* contacts,
                     std::vector<HairVec>* follow) const;
    void solveSheet(const std::vector<std::vector<HairVec>>& goal, const HairSettings& settings);

    std::vector<std::vector<HairVec>> points_;
    std::vector<std::vector<HairVec>> previous_;   // last step's points (velocity)
    std::vector<std::vector<HairVec>> lastGoals_;  // last frame's goals (steps blend from them)
    std::vector<HairCapsule> lastCapsules_;
    std::vector<std::pair<int, int>> neighbours_;
    // Scratch, kept to avoid allocating every frame.
    std::vector<std::vector<HairVec>> goalNow_;
    std::vector<HairCapsule> capsulesNow_;
    std::vector<std::vector<HairVec>> contacts_;  // per point, this step: the body's push-out directions summed
    std::vector<std::vector<HairVec>> follow_;    // per point, this step: the length constraint's own corrections
    std::vector<HairVec> rootMoves_;
    bool started_ = false;
};

// The deepest any strand (its points and the lines between them, the first quarter of the first segment left out
// since the root sits in the scalp) is inside any capsule, cm; 0 if none is.
COLONY_API float hairDeepestPenetration(const std::vector<std::vector<HairVec>>& strands,
                                        const std::vector<HairCapsule>& capsules);

// Which strands of one sheet are neighbours: their roots sorted round the head (about `up`, through `head`), each
// joined to the next unless they are more than 100 degrees apart (so never across the face).
COLONY_API std::vector<std::pair<int, int>> hairSheetNeighbours(const std::vector<HairVec>& roots, HairVec head, HairVec up);

// Long hair swept off the stock while a long gun is aimed (docs/design/hair-physics.md, "Aiming"): as a shooter with
// long hair puts it over the other shoulder, every strand's styled shape is turned towards `away` (unit: the side away
// from the stock and a little back) by up to `angleDeg` times `amount` (0 at the ready .. 1 aimed). Strands rooted on the
// stock's side (`stockSide`, unit, from `head`) turn fully, those down the middle of the back by 0.7, the far side's by
// 0.4; the first segment, on the scalp, by half. Roots and every segment's length are kept. Only the goals change: the
// physics carries the hair there with its usual lag, its collisions and its rest on the body.
COLONY_API void sweepHairGoals(std::vector<std::vector<HairVec>>& goals, HairVec head, HairVec stockSide, HairVec away, float amount,
                               float angleDeg = 35.0f);

// The body's collision shapes for hair, fitted to one skeleton in its reference pose (component space, z up).
enum class HairBone : uint8_t { Head, Neck, SpineTop, SpineMid, ClavicleL, ClavicleR, UpperArmL, UpperArmR };

struct HairShape {
    HairBone bone = HairBone::Head;  // the bone it rides on
    HairCapsule capsule;             // in the reference pose
};

struct HairBodyBones {
    HairVec pelvis;
    HairVec head;
    bool hasNeck = false;
    HairVec neck;
    std::vector<HairVec> spine;      // spine_01 upwards (any number, may be empty)
    bool hasArms = false;
    HairVec clavicle[2];             // [0] left, [1] right
    HairVec upperArm[2];
    HairVec lowerArm[2];
    int spineMid = -1;               // set by buildHairShapes: which spine bone SpineMid rides on (-1: none)
};

// The shapes for a body: sizes from the skeleton's proportions, then each shrunk so the hair's rest shape (`restStrands`,
// points root first) is never inside it, and the ones hair rests on (neck, back, chest, shoulders) grown to touch it
// where it lies a little off them. `bones.spineMid` is set to the spine bone the middle of the back rides on.
COLONY_API std::vector<HairShape> buildHairShapes(HairBodyBones& bones, const std::vector<std::vector<HairVec>>& restStrands);

}  // namespace colony
