#include "doctest.h"

#include "ColonySim/ArmPose.h"
#include "ColonySim/Random.h"

#include <cmath>
#include <initializer_list>

using namespace colony::arm;

namespace {

// Unreal's axes (x ahead, y right, z up), cm, a 1.80 m body's right arm.
const V3 kArmUp{0.0, 0.0, 1.0};
const V3 kArmForward{1.0, 0.0, 0.0};
const V3 kArmRight{0.0, 1.0, 0.0};
const V3 kArmShoulderR{0.0, 18.0, 145.0};
const V3 kArmShoulderL{0.0, -18.0, 145.0};
constexpr double kArmUpper = 30.0;
constexpr double kArmLower = 27.0;

bool armNear(const V3& a, const V3& b, double tolerance = 1e-6) {
    return length(sub(a, b)) <= tolerance;
}

// A hand frame from any two directions (fingers, then the palm made square to them).
HandFrame armFrame(const V3& fingers, const V3& palmish) {
    HandFrame f;
    f.fingers = normal(fingers);
    f.palm = normal(sub(palmish, mul(f.fingers, dot(palmish, f.fingers))));
    return f;
}

double armUniform(colony::Rng& rng, double lo, double hi) {
    return static_cast<double>(rng.range(static_cast<float>(lo), static_cast<float>(hi)));
}

}  // namespace

TEST_CASE("Arm pose: a reachable grip is reached exactly, bones keep their length, the elbow goes the pole's way") {
    const V3 target{40.0, 5.0, 130.0};
    const V3 pole = naturalPole(kArmShoulderR, target, kArmUp, kArmForward, kArmRight, true);
    const ArmSolve s = solveArm(kArmShoulderR, kArmUpper, kArmLower, target, pole);
    CHECK(armNear(s.wrist, target));
    CHECK(s.shortBy == doctest::Approx(0.0));
    CHECK(length(sub(s.elbow, kArmShoulderR)) == doctest::Approx(kArmUpper));
    CHECK(length(sub(s.wrist, s.elbow)) == doctest::Approx(kArmLower));
    // Down and out: below the shoulder-wrist line and to the right of it.
    const V3 dir = normal(sub(target, kArmShoulderR));
    const V3 off = sub(sub(s.elbow, kArmShoulderR), mul(dir, dot(sub(s.elbow, kArmShoulderR), dir)));
    CHECK(dot(off, kArmUp) < 0.0);
    CHECK(dot(off, kArmRight) > 0.0);
    CHECK(s.elbowFlex > 0.0);
    CHECK(s.elbowFlex <= 145.0);
}

TEST_CASE("Arm pose: the arm never stretches; out of reach the wrist stops short on the line to the grip") {
    const V3 target{120.0, 18.0, 145.0};
    const ArmSolve s = solveArm(kArmShoulderR, kArmUpper, kArmLower, target, V3{0.0, 0.0, -1.0});
    CHECK(length(sub(s.elbow, kArmShoulderR)) == doctest::Approx(kArmUpper));
    CHECK(length(sub(s.wrist, s.elbow)) == doctest::Approx(kArmLower));
    CHECK(length(sub(s.wrist, kArmShoulderR)) <= (kArmUpper + kArmLower) * 0.995 + 1e-6);
    CHECK(s.shortBy > 60.0);
    CHECK(s.wrist.y == doctest::Approx(18.0));
    CHECK(s.wrist.z == doctest::Approx(145.0));
}

TEST_CASE("Arm pose: a grip against the shoulder doesn't fold the elbow past its limit") {
    const ArmSolve s = solveArm(kArmShoulderR, kArmUpper, kArmLower, add(kArmShoulderR, V3{2.0, 0.0, 0.0}), V3{0.0, 0.0, -1.0});
    CHECK(s.elbowFlex <= 145.0 + 1e-6);
    CHECK(s.shortBy > 0.0);
}

TEST_CASE("Arm pose: the elbow's inner side, and a hand's twist, wrist bend and sideways bend read as a person's") {
    // Right arm hanging, forearm raised straight ahead.
    const V3 elbow{0.0, 18.0, 115.0};
    const V3 wrist{27.0, 18.0, 115.0};
    const V3 hinge = medialHinge(sub(elbow, kArmShoulderR), sub(wrist, elbow), kArmUp, kArmForward, kArmRight, true);
    CHECK(dot(hinge, kArmRight) < -0.99);  // towards the body's middle (left, for the right arm)
    const V3 hingeL = medialHinge(V3{0.0, 0.0, -30.0}, V3{27.0, 0.0, 0.0}, kArmUp, kArmForward, kArmRight, false);
    CHECK(dot(hingeL, kArmRight) > 0.99);
    // Thumb up: the palm faces the body's middle.
    ArmAngles a = measureArm(kArmShoulderR, elbow, wrist, kArmForward, V3{0.0, -1.0, 0.0}, hinge);
    CHECK(a.elbowFlex == doctest::Approx(90.0));
    CHECK(a.forearmTwist == doctest::Approx(0.0));
    CHECK(a.wristFlex == doctest::Approx(0.0));
    CHECK(withinLimits(a));
    // Palm down: a quarter turn.
    a = measureArm(kArmShoulderR, elbow, wrist, kArmForward, V3{0.0, 0.0, -1.0}, hinge);
    CHECK(std::fabs(a.forearmTwist) == doctest::Approx(90.0));
    CHECK_FALSE(withinLimits(a));
    // Palm down, wrist bent 30 degrees down (towards the palm).
    a = measureArm(kArmShoulderR, elbow, wrist, rotate(kArmForward, kArmRight, 30.0), rotate(V3{0.0, 0.0, -1.0}, kArmRight, 30.0), hinge);
    CHECK(std::fabs(a.wristFlex) == doctest::Approx(30.0));
    CHECK(std::fabs(a.wristDeviation) == doctest::Approx(0.0).epsilon(1e-6));
    // An elbow bent backwards reads as the forearm turned round: never within the limits.
    const V3 backWrist{-27.0, 18.0, 115.0};
    const V3 backHinge = medialHinge(sub(elbow, kArmShoulderR), sub(backWrist, elbow), kArmUp, kArmForward, kArmRight, true);
    a = measureArm(kArmShoulderR, elbow, backWrist, mul(kArmForward, -1.0), V3{0.0, -1.0, 0.0}, backHinge);
    CHECK(std::fabs(a.forearmTwist) == doctest::Approx(180.0));
    CHECK_FALSE(withinLimits(a));
}

TEST_CASE("Arm pose: a hand turned or bent too far is brought back to the nearest pose a wrist can take") {
    const V3 forearm{1.0, 0.0, 0.0};
    const V3 hinge{0.0, -1.0, 0.0};
    // Palm up and the wrist bent 100 degrees back: clamped to the twist and bend limits.
    const HandFrame want = armFrame(rotate(forearm, V3{0.0, 1.0, 0.0}, 100.0), V3{0.0, 0.0, 1.0});
    const HandFrame got = clampHand(forearm, hinge, want);
    const ArmAngles a = measureArm(V3{-27.0, 0.0, 30.0}, V3{-27.0, 0.0, 0.0}, V3{}, got.fingers, got.palm, hinge);
    CHECK(withinLimits(a, Limits{}, 1e-6));
    CHECK(std::fabs(a.forearmTwist) == doctest::Approx(85.0));
    CHECK(dot(got.fingers, got.palm) == doctest::Approx(0.0).epsilon(1e-9));
    // A hand already inside the limits is left exactly as it was.
    const HandFrame fine = armFrame(rotate(forearm, V3{0.0, 0.0, 1.0}, 10.0), V3{0.0, -1.0, 0.2});
    const HandFrame same = clampHand(forearm, hinge, fine);
    CHECK(armNear(same.fingers, fine.fingers));
    CHECK(armNear(same.palm, fine.palm));
}

TEST_CASE("Arm pose: any grip anywhere, any hand turn: the solved arm is always inside human limits (20,000 tries)") {
    colony::Rng rng(20261119);
    const Limits limits;
    int checked = 0;
    for (int i = 0; i < 20000; ++i) {
        const bool right = (i % 2) == 0;
        const V3 shoulder = right ? kArmShoulderR : kArmShoulderL;
        // Grips from right at the shoulder to well out of reach, all round the body.
        const V3 target = add(shoulder, V3{armUniform(rng, -60.0, 110.0), armUniform(rng, -90.0, 90.0), armUniform(rng, -90.0, 90.0)});
        const double upper = armUniform(rng, 24.0, 34.0);
        const double lower = armUniform(rng, 21.0, 30.0);
        const V3 pole = naturalPole(shoulder, target, kArmUp, kArmForward, kArmRight, right);
        const ArmSolve s = solveArm(shoulder, upper, lower, target, pole, limits);
        CHECK(length(sub(s.elbow, shoulder)) == doctest::Approx(upper));
        CHECK(length(sub(s.wrist, s.elbow)) == doctest::Approx(lower));
        CHECK(s.elbowFlex >= 0.0);
        CHECK(s.elbowFlex <= limits.elbowFlexMax + 1e-6);
        // Never fully straight, so the elbow's hinge is always known.
        const V3 hinge = medialHinge(sub(s.elbow, shoulder), sub(s.wrist, s.elbow), kArmUp, kArmForward, kArmRight, right);
        CHECK(length(hinge) > 0.5);
        // Any hand turn the weapon asks for.
        const V3 fingers{armUniform(rng, -1.0, 1.0), armUniform(rng, -1.0, 1.0), armUniform(rng, -1.0, 1.0)};
        const V3 palmish{armUniform(rng, -1.0, 1.0), armUniform(rng, -1.0, 1.0), armUniform(rng, -1.0, 1.0)};
        if (length(fingers) < 0.1 || length(cross(fingers, palmish)) < 0.1) {
            continue;
        }
        const double neutralFlex = armUniform(rng, -8.0, 8.0);
        const double neutralDev = armUniform(rng, -8.0, 8.0);
        const HandFrame got = clampHand(sub(s.wrist, s.elbow), hinge, armFrame(fingers, palmish), limits, neutralFlex, neutralDev);
        const ArmAngles a = measureArm(shoulder, s.elbow, s.wrist, got.fingers, got.palm, hinge, neutralFlex, neutralDev);
        CHECK(a.hingeKnown);
        CHECK(withinLimits(a, limits, 1e-6));
        CHECK(dot(got.fingers, got.palm) == doctest::Approx(0.0).epsilon(1e-9));
        ++checked;
    }
    CHECK(checked > 19000);
}

TEST_CASE("Arm pose: the elbow swings round so a grip's hand turn fits the wrist, and stays natural when it already fits") {
    const V3 grip{30.0, 4.0, 140.0};  // a long gun's grip before the right shoulder
    const V3 base = naturalPole(kArmShoulderR, grip, kArmUp, kArmForward, kArmRight, true);

    // A hand that fits the natural elbow (straight wrist, thumb up): no swing.
    {
        const ArmSolve s = solveArm(kArmShoulderR, kArmUpper, kArmLower, grip, base);
        const V3 forearm = normal(sub(s.wrist, s.elbow));
        const V3 hinge = medialHinge(normal(sub(s.elbow, kArmShoulderR)), forearm, kArmUp, kArmForward, kArmRight, true);
        const SwivelChoice c = chooseSwivel(kArmShoulderR, kArmUpper, kArmLower, grip, base, armFrame(forearm, hinge), kArmUp,
                                            kArmForward, kArmRight, true);
        CHECK(c.swivel == doctest::Approx(0.0));
        CHECK(c.excess == doctest::Approx(0.0));
    }
    // A hand only an elbow swung out 60 degrees can take: found, and nothing is left past the limits there.
    for (const double swing : {-60.0, 60.0}) {
        const V3 axis = normal(sub(grip, kArmShoulderR));
        const ArmSolve s = solveArm(kArmShoulderR, kArmUpper, kArmLower, grip, rotate(base, axis, swing));
        const V3 forearm = normal(sub(s.wrist, s.elbow));
        const V3 hinge = medialHinge(normal(sub(s.elbow, kArmShoulderR)), forearm, kArmUp, kArmForward, kArmRight, true);
        const HandFrame hand = armFrame(rotate(forearm, cross(forearm, hinge), 0.0), rotate(hinge, forearm, 70.0));
        const ArmSolve natural = solveArm(kArmShoulderR, kArmUpper, kArmLower, grip, base);
        const V3 natForearm = normal(sub(natural.wrist, natural.elbow));
        const ArmAngles atNatural = measureArm(kArmShoulderR, natural.elbow, natural.wrist, hand.fingers, hand.palm,
                                               medialHinge(normal(sub(natural.elbow, kArmShoulderR)), natForearm, kArmUp,
                                                           kArmForward, kArmRight, true));
        const SwivelChoice c = chooseSwivel(kArmShoulderR, kArmUpper, kArmLower, grip, base, hand, kArmUp, kArmForward,
                                            kArmRight, true);
        CHECK_FALSE(withinLimits(atNatural));
        CHECK(c.excess == doctest::Approx(0.0));
        const ArmSolve chosen = solveArm(kArmShoulderR, kArmUpper, kArmLower, grip, c.pole);
        const V3 chForearm = normal(sub(chosen.wrist, chosen.elbow));
        const ArmAngles at = measureArm(kArmShoulderR, chosen.elbow, chosen.wrist, hand.fingers, hand.palm,
                                        medialHinge(normal(sub(chosen.elbow, kArmShoulderR)), chForearm, kArmUp, kArmForward,
                                                    kArmRight, true));
        CHECK(withinLimits(at, Limits{}, 0.5));
    }
}
