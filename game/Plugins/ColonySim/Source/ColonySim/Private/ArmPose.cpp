#include "ColonySim/ArmPose.h"

#include <algorithm>
#include <cmath>

namespace colony::arm {

namespace {

constexpr double kArmPi = 3.14159265358979323846;
constexpr double kArmDeg = 180.0 / kArmPi;

double armClamp(double v, double lo, double hi) {
    return std::min(std::max(v, lo), hi);
}

// Any unit vector at right angles to the unit vector a.
V3 armAnyPerpendicular(const V3& a) {
    const V3 other = std::fabs(a.z) < 0.9 ? V3{0.0, 0.0, 1.0} : V3{1.0, 0.0, 0.0};
    return normal(cross(a, other));
}

// a with its part along the unit vector axis taken out.
V3 armFlatten(const V3& a, const V3& axis) {
    return sub(a, mul(axis, dot(a, axis)));
}

// The three readings of a hand on a forearm: twist about the forearm (from the hinge to the palm), the wrist's bend
// towards the palm and its sideways bend, before taking off the skeleton's neutral.
struct ArmWristRaw {
    double twist = 0.0;
    double flex = 0.0;
    double deviation = 0.0;
    bool hingeKnown = false;
};

ArmWristRaw armWristRaw(const V3& forearm, const V3& fingers, const V3& palm, const V3& hinge) {
    ArmWristRaw out;
    const V3 l = normal(forearm);
    const V3 palmFlat = normal(armFlatten(palm, l));
    const V3 hingeFlat = normal(armFlatten(hinge, l));
    if (length(hingeFlat) > 0.5 && length(palmFlat) > 0.5) {
        out.twist = std::atan2(dot(cross(hingeFlat, palmFlat), l), dot(hingeFlat, palmFlat)) * kArmDeg;
        out.hingeKnown = true;
    }
    if (length(palmFlat) > 0.5) {
        const V3 side = cross(l, palmFlat);
        const double ahead = dot(fingers, l);
        out.flex = std::atan2(dot(fingers, palmFlat), ahead) * kArmDeg;
        out.deviation = std::atan2(dot(fingers, side), ahead) * kArmDeg;
    }
    return out;
}

}  // namespace

V3 add(const V3& a, const V3& b) { return V3{a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 sub(const V3& a, const V3& b) { return V3{a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 mul(const V3& a, double s) { return V3{a.x * s, a.y * s, a.z * s}; }
double dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(const V3& a, const V3& b) { return V3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double length(const V3& a) { return std::sqrt(dot(a, a)); }

V3 normal(const V3& a) {
    const double n = length(a);
    return n > 1e-9 ? mul(a, 1.0 / n) : V3{};
}

V3 rotate(const V3& a, const V3& axis, double degrees) {
    const V3 k = normal(axis);
    const double r = degrees / kArmDeg;
    const double c = std::cos(r);
    const double s = std::sin(r);
    return add(add(mul(a, c), mul(cross(k, a), s)), mul(k, dot(k, a) * (1.0 - c)));
}

ArmSolve solveArm(const V3& shoulder, double upperLength, double lowerLength, const V3& target, const V3& pole,
                  const Limits& limits) {
    ArmSolve out;
    const double u = std::max(upperLength, 0.01);
    const double l = std::max(lowerLength, 0.01);
    const V3 toTarget = sub(target, shoulder);
    const double want = length(toTarget);
    V3 dir = normal(toTarget);
    if (length(dir) < 0.5) {
        dir = length(normal(pole)) > 0.5 ? armAnyPerpendicular(normal(pole)) : V3{1.0, 0.0, 0.0};
    }
    // Reach: never stretched, never folded past the elbow's limit.
    const double maxReach = (u + l) * armClamp(limits.reachShare, 0.5, 1.0);
    const double inner = (180.0 - armClamp(limits.elbowFlexMax, 0.0, 179.0)) / kArmDeg;
    const double minReach = std::sqrt(std::max(0.0, u * u + l * l - 2.0 * u * l * std::cos(inner)));
    const double reach = armClamp(want, std::min(minReach, maxReach), maxReach);
    out.shortBy = std::fabs(want - reach);
    out.wrist = add(shoulder, mul(dir, reach));
    // The elbow on the circle round the shoulder-wrist line, towards the pole.
    const double cosShoulder = armClamp((u * u + reach * reach - l * l) / (2.0 * u * reach), -1.0, 1.0);
    const double sinShoulder = std::sqrt(std::max(0.0, 1.0 - cosShoulder * cosShoulder));
    V3 side = normal(armFlatten(pole, dir));
    if (length(side) < 0.5) {
        side = armAnyPerpendicular(dir);
    }
    out.elbow = add(shoulder, add(mul(dir, u * cosShoulder), mul(side, u * sinShoulder)));
    const double cosElbow = armClamp((u * u + l * l - reach * reach) / (2.0 * u * l), -1.0, 1.0);
    out.elbowFlex = 180.0 - std::acos(cosElbow) * kArmDeg;
    return out;
}

V3 naturalPole(const V3& shoulder, const V3& wrist, const V3& up, const V3& forward, const V3& right, bool bRight) {
    (void)shoulder;
    (void)wrist;
    const V3 out = bRight ? normal(right) : mul(normal(right), -1.0);
    return normal(add(add(mul(normal(up), -1.0), mul(out, 0.75)), mul(normal(forward), -0.25)));
}

V3 medialHinge(const V3& upper, const V3& forearm, const V3& up, const V3& forward, const V3& right, bool bRight) {
    const V3 u = normal(upper);
    const V3 l = normal(forearm);
    const V3 c = cross(u, l);
    // Too straight to tell (under about 3 degrees of bend).
    if (length(c) < 0.05) {
        return V3{};
    }
    // Which way the algebra's cross product turns in this space: an arm hanging down with its forearm raised forward
    // has its inner side towards the body's middle.
    const V3 reference = cross(mul(normal(up), -1.0), normal(forward));
    const V3 inner = bRight ? mul(normal(right), -1.0) : normal(right);
    const double sign = dot(reference, inner) >= 0.0 ? 1.0 : -1.0;
    return mul(normal(c), sign);
}

ArmAngles measureArm(const V3& shoulder, const V3& elbow, const V3& wrist, const V3& fingers, const V3& palm,
                     const V3& hinge, double neutralFlex, double neutralDeviation) {
    ArmAngles out;
    const V3 u = normal(sub(elbow, shoulder));
    const V3 l = normal(sub(wrist, elbow));
    out.elbowFlex = std::acos(armClamp(dot(u, l), -1.0, 1.0)) * kArmDeg;
    const ArmWristRaw raw = armWristRaw(l, normal(fingers), normal(palm), hinge);
    out.forearmTwist = raw.twist;
    out.hingeKnown = raw.hingeKnown;
    out.wristFlex = raw.flex - neutralFlex;
    out.wristDeviation = raw.deviation - neutralDeviation;
    return out;
}

bool withinLimits(const ArmAngles& a, const Limits& limits, double slack) {
    return a.elbowFlex >= -slack && a.elbowFlex <= limits.elbowFlexMax + slack &&
           std::fabs(a.forearmTwist) <= limits.forearmTwist + slack && std::fabs(a.wristFlex) <= limits.wristFlex + slack &&
           std::fabs(a.wristDeviation) <= limits.wristDeviation + slack;
}

HandFrame clampHand(const V3& forearm, const V3& hinge, const HandFrame& want, const Limits& limits, double neutralFlex,
                    double neutralDeviation) {
    const V3 l = normal(forearm);
    if (length(l) < 0.5) {
        return want;
    }
    const ArmWristRaw raw = armWristRaw(l, normal(want.fingers), normal(want.palm), hinge);
    const double twist = raw.hingeKnown ? armClamp(raw.twist, -limits.forearmTwist, limits.forearmTwist) : raw.twist;
    const double flex = neutralFlex + armClamp(raw.flex - neutralFlex, -limits.wristFlex, limits.wristFlex);
    const double deviation =
        neutralDeviation + armClamp(raw.deviation - neutralDeviation, -limits.wristDeviation, limits.wristDeviation);
    // Fingers bent back past the forearm's side (more than 90 degrees from it) read as a smaller bend: rebuild those too.
    const bool behind = dot(normal(want.fingers), l) <= 0.0;
    if (!behind && std::fabs(twist - raw.twist) < 1e-6 && std::fabs(flex - raw.flex) < 1e-6 &&
        std::fabs(deviation - raw.deviation) < 1e-6) {
        return want;
    }
    // The palm side at that twist, then the fingers bent from the forearm's line by the clamped angles; the palm at
    // right angles to them in the plane of the forearm and the palm side, so measuring it again reads the same angles.
    V3 palmSide;
    const V3 hingeFlat = normal(armFlatten(hinge, l));
    if (raw.hingeKnown && length(hingeFlat) > 0.5) {
        palmSide = normal(rotate(hingeFlat, l, twist));
    } else {
        palmSide = normal(armFlatten(want.palm, l));
        if (length(palmSide) < 0.5) {
            palmSide = armAnyPerpendicular(l);
        }
    }
    const V3 side = cross(l, palmSide);
    const double tFlex = std::tan(armClamp(flex, -85.0, 85.0) / kArmDeg);
    const double tDev = std::tan(armClamp(deviation, -85.0, 85.0) / kArmDeg);
    HandFrame out;
    out.fingers = normal(add(add(l, mul(palmSide, tFlex)), mul(side, tDev)));
    out.palm = normal(sub(palmSide, mul(l, tFlex)));
    return out;
}

SwivelChoice chooseSwivel(const V3& shoulder, double upperLength, double lowerLength, const V3& target,
                          const V3& basePole, const HandFrame& hand, const V3& up, const V3& forward, const V3& right,
                          bool bRight, const Limits& limits, double neutralFlex, double neutralDeviation,
                          double maxSwivel, double previousSwivel) {
    SwivelChoice best;
    best.pole = basePole;
    const V3 axis = normal(sub(target, shoulder));
    if (length(axis) < 0.5 || length(normal(hand.fingers)) < 0.5 || length(normal(hand.palm)) < 0.5) {
        return best;
    }
    const V3 upN = normal(up);
    const V3 outN = bRight ? normal(right) : mul(normal(right), -1.0);
    const double u = std::max(upperLength, 0.01);
    const double span = armClamp(maxSwivel, 0.0, 180.0);
    // One swing's cost: what the wrist is still asked past its limits, how far from natural and from last frame, and an
    // elbow raised over the shoulder or swung in across the chest.
    const auto costAt = [&](double swivel, SwivelChoice& out) {
        const V3 pole = rotate(basePole, axis, swivel);
        const ArmSolve solve = solveArm(shoulder, upperLength, lowerLength, target, pole, limits);
        const V3 upper = normal(sub(solve.elbow, shoulder));
        const V3 forearm = normal(sub(solve.wrist, solve.elbow));
        const V3 hinge = medialHinge(upper, forearm, up, forward, right, bRight);
        const ArmAngles a = measureArm(shoulder, solve.elbow, solve.wrist, hand.fingers, hand.palm, hinge, neutralFlex,
                                       neutralDeviation);
        double excess = 0.0;
        if (a.hingeKnown) {
            excess += std::max(0.0, std::fabs(a.forearmTwist) - limits.forearmTwist);
        }
        excess += std::max(0.0, std::fabs(a.wristFlex) - limits.wristFlex);
        excess += std::max(0.0, std::fabs(a.wristDeviation) - limits.wristDeviation);
        const V3 elbowFrom = mul(sub(solve.elbow, shoulder), 1.0 / u);
        const double raised = std::max(0.0, dot(elbowFrom, upN) - 0.15);
        const double inward = std::max(0.0, -dot(elbowFrom, outN) - 0.1);
        out.pole = pole;
        out.swivel = swivel;
        out.excess = excess;
        return excess * 4.0 + std::fabs(swivel) * 0.1 + std::fabs(swivel - previousSwivel) * 0.06 + raised * 120.0 +
               inward * 120.0;
    };
    double bestCost = 1e18;
    const int coarse = static_cast<int>(std::floor(span / 5.0));
    double coarseBest = 0.0;
    for (int i = -coarse; i <= coarse; ++i) {
        SwivelChoice c;
        const double cost = costAt(5.0 * i, c);
        if (cost < bestCost - 1e-9) {
            bestCost = cost;
            best = c;
            coarseBest = 5.0 * i;
        }
    }
    for (int i = -4; i <= 4; ++i) {
        const double swivel = coarseBest + i;
        if (i == 0 || std::fabs(swivel) > span) {
            continue;
        }
        SwivelChoice c;
        const double cost = costAt(swivel, c);
        if (cost < bestCost - 1e-9) {
            bestCost = cost;
            best = c;
        }
    }
    return best;
}

}  // namespace colony::arm
