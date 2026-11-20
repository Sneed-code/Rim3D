#pragma once

#include "ColonySim/Api.h"

namespace colony::arm {

// Arms on weapons without broken joints (docs/design/weapon-hold.md, "Arms that can't bend wrong"; sneed 2026-11-19:
// "the arms cannot be messed up, like unnatural bends in hands, forearms or otherwise").
//
// Plain geometry, no Unreal: Unreal's hand IK (FMakeHumanArmsNode) solves every arm through these, and the smoke test
// measures every drawn arm with `measureArm`, so the same limits hold in both places and the solver is tested here.
// Any space works (Unreal passes component space, cm). Only directions given by the caller (up, forward, the body's
// right) say which way is which, so the coordinate system's handedness doesn't matter.

struct V3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

COLONY_API V3 add(const V3& a, const V3& b);
COLONY_API V3 sub(const V3& a, const V3& b);
COLONY_API V3 mul(const V3& a, double s);
COLONY_API double dot(const V3& a, const V3& b);
COLONY_API V3 cross(const V3& a, const V3& b);
COLONY_API double length(const V3& a);
// a / |a|, or zero for a (near) zero vector.
COLONY_API V3 normal(const V3& a);
// a turned about the unit axis by degrees (right-hand rule in the algebra's own sense).
COLONY_API V3 rotate(const V3& a, const V3& axis, double degrees);

// Human joint ranges, degrees (a healthy adult's, a little inside the extremes so a pose never looks strained).
struct Limits {
    double elbowFlexMax = 145.0;   // from straight; the elbow is a hinge and never bends backwards
    double wristFlex = 70.0;       // palm-ward and back-ward bend
    double wristDeviation = 28.0;  // to the thumb or little-finger side
    double forearmTwist = 85.0;    // either way from "thumb up" (the palm facing the elbow's inner side)
    double reachShare = 0.995;     // the farthest the wrist goes, as a share of upper + lower arm: no stretching
};

// The result of a two-bone solve.
struct ArmSolve {
    V3 elbow;              // where the elbow goes
    V3 wrist;              // where the wrist goes (the target, or as near as the arm reaches)
    double elbowFlex = 0;  // degrees from straight, 0 .. Limits::elbowFlexMax
    double shortBy = 0;    // cm the target was out of reach (or too close to the shoulder), 0 when reached
};

// Puts the wrist on `target` from `shoulder` with an upper arm and forearm of these lengths: the elbow is a hinge on the
// side `pole` points to (from the shoulder-wrist line), never straighter than straight nor more bent than the limit, and
// the arm never stretches: a target out of reach leaves the wrist short of it, on the line to it.
COLONY_API ArmSolve solveArm(const V3& shoulder, double upperLength, double lowerLength, const V3& target, const V3& pole,
                             const Limits& limits = Limits{});

// The way an elbow should point for a hand at `wrist` (from `shoulder`): down and out to the arm's own side and a
// little back, as people hold a weapon. `right` is the body's right; bRight says which arm.
COLONY_API V3 naturalPole(const V3& shoulder, const V3& wrist, const V3& up, const V3& forward, const V3& right, bool bRight);

// The elbow's hinge axis for an arm, unit, pointing to the arm's inner (medial) side: from the upper arm (shoulder to
// elbow) and forearm (elbow to wrist) directions; zero when the arm is too straight to tell.
COLONY_API V3 medialHinge(const V3& upper, const V3& forearm, const V3& up, const V3& forward, const V3& right, bool bRight);

// A hand's pose in the arm, as people's joints measure it. Degrees.
struct ArmAngles {
    double elbowFlex = 0;       // 0 straight .. 145 fully bent; negative = bent backwards (never allowed)
    double forearmTwist = 0;    // 0 thumb up (palm towards the elbow's inner side), + or - either way
    double wristFlex = 0;       // + towards the palm
    double wristDeviation = 0;  // sideways
    bool hingeKnown = false;    // false when the elbow was too straight to tell its hinge (twist then reads 0)
};

// Measures an arm from its joints: shoulder, elbow, wrist positions, the hand's finger direction (wrist to the middle
// knuckle) and palm normal (out of the palm), the elbow's medial hinge (`medialHinge`, or a remembered one when the
// arm is straight; zero for none). `neutralFlex`/`neutralDeviation` are what the skeleton reads in its rest pose (its
// knuckles are rarely exactly in line with the forearm), taken off.
COLONY_API ArmAngles measureArm(const V3& shoulder, const V3& elbow, const V3& wrist, const V3& fingers, const V3& palm,
                                const V3& hinge, double neutralFlex = 0.0, double neutralDeviation = 0.0);

// Whether every angle is inside the limits, with `slack` degrees to spare (the smoke test's tolerance).
COLONY_API bool withinLimits(const ArmAngles& a, const Limits& limits = Limits{}, double slack = 0.0);

// A hand frame: finger direction and palm normal (unit, at right angles).
struct HandFrame {
    V3 fingers;
    V3 palm;
};

// The nearest hand frame to `want` that a wrist on this forearm can take: the forearm's twist clamped about its axis,
// then the wrist's bend and sideways bend clamped. `forearm` points elbow to wrist, `hinge` is the medial hinge (zero:
// no twist limit). Returns `want` unchanged when it is already inside the limits.
COLONY_API HandFrame clampHand(const V3& forearm, const V3& hinge, const HandFrame& want, const Limits& limits = Limits{},
                               double neutralFlex = 0.0, double neutralDeviation = 0.0);

// The elbow's swivel for a hand that must take a given turn (a grip on a weapon): the elbow is swung round the
// shoulder-wrist line from `basePole` (the natural way, down and out), at most `maxSwivel` degrees either way, to where
// the forearm lets the wrist take `hand` with the least bend past the limits (a rifle's grip hand lifts its elbow out
// to the side, as shooters do, instead of bending the wrist 70 degrees sideways). A little swing costs a little, an
// elbow raised above the shoulder or swung in against the chest costs more, so the natural pole wins whenever the hand
// fits there, and a swing away from `previousSwivel` (last frame's) costs a little too, so the elbow doesn't hop between
// poses that are nearly as good. Searched in 5 degree steps, then to the degree. Returns the pole to give `solveArm`,
// and how far (degrees) the best pose still asked past the limits.
struct SwivelChoice {
    V3 pole;
    double swivel = 0.0;  // degrees from basePole
    double excess = 0.0;  // degrees past the limits left at the chosen swivel (summed over twist, bend, sideways bend)
};
COLONY_API SwivelChoice chooseSwivel(const V3& shoulder, double upperLength, double lowerLength, const V3& target,
                                     const V3& basePole, const HandFrame& hand, const V3& up, const V3& forward,
                                     const V3& right, bool bRight, const Limits& limits = Limits{},
                                     double neutralFlex = 0.0, double neutralDeviation = 0.0, double maxSwivel = 100.0,
                                     double previousSwivel = 0.0);

}  // namespace colony::arm
