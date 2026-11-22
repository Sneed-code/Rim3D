#include "ColonySim/HairStrands.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace colony {

namespace {

// Prefixed: Unreal's unity builds put sim files together.
HairVec hsAdd(HairVec a, HairVec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
HairVec hsSub(HairVec a, HairVec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
HairVec hsMul(HairVec a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float hsDot(HairVec a, HairVec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float hsLen(HairVec a) { return std::sqrt(hsDot(a, a)); }
HairVec hsLerp(HairVec a, HairVec b, float t) { return hsAdd(a, hsMul(hsSub(b, a), t)); }
HairVec hsCross(HairVec a, HairVec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
HairVec hsNormal(HairVec a, HairVec fallback) {
    const float l = hsLen(a);
    return l > 1.0e-6f ? hsMul(a, 1.0f / l) : fallback;
}
float hsClamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// The closest points of segments p1-q1 and p2-q2 (Ericson, Real-Time Collision Detection 5.1.9): s along the first,
// t along the second.
void hsClosestSegments(HairVec p1, HairVec q1, HairVec p2, HairVec q2, float& s, float& t, HairVec& c1, HairVec& c2) {
    const HairVec d1 = hsSub(q1, p1);
    const HairVec d2 = hsSub(q2, p2);
    const HairVec r = hsSub(p1, p2);
    const float a = hsDot(d1, d1);
    const float e = hsDot(d2, d2);
    const float f = hsDot(d2, r);
    const float eps = 1.0e-8f;
    if (a <= eps && e <= eps) {
        s = t = 0.0f;
    } else if (a <= eps) {
        s = 0.0f;
        t = hsClamp01(f / e);
    } else {
        const float c = hsDot(d1, r);
        if (e <= eps) {
            t = 0.0f;
            s = hsClamp01(-c / a);
        } else {
            const float b = hsDot(d1, d2);
            const float denom = a * e - b * b;
            s = denom > eps ? hsClamp01((b * f - c * e) / denom) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = hsClamp01(-c / a);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = hsClamp01((b - c) / a);
            }
        }
    }
    c1 = hsAdd(p1, hsMul(d1, s));
    c2 = hsAdd(p2, hsMul(d2, t));
}

float hsPointSegmentDistance(HairVec p, HairVec a, HairVec b) {
    const HairVec ab = hsSub(b, a);
    const float l2 = hsDot(ab, ab);
    const float t = l2 > 1.0e-8f ? hsClamp01(hsDot(hsSub(p, a), ab) / l2) : 0.0f;
    return hsLen(hsSub(p, hsAdd(a, hsMul(ab, t))));
}

bool hsSameShape(const std::vector<std::vector<HairVec>>& a, const std::vector<std::vector<HairVec>>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].size() != b[i].size()) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool HairStrands::solveStrand(std::vector<HairVec>& p, const std::vector<HairVec>& goal, const std::vector<HairCapsule>& capsules,
                              float cosMax, float sinMax, std::vector<HairVec>* contacts, std::vector<HairVec>* follow) const {
    bool any = false;
    const size_t n = p.size();
    p[0] = goal[0];
    for (size_t k = 1; k < n; ++k) {
        const HairVec styled = hsSub(goal[k], goal[k - 1]);
        const float length = hsLen(styled);
        if (length < 1.0e-4f) {
            p[k] = p[k - 1];
            continue;
        }
        const HairVec styledDir = hsMul(styled, 1.0f / length);
        HairVec dir = hsNormal(hsSub(p[k], p[k - 1]), styledDir);
        // No further from the style's shape than the limit.
        const float c = hsDot(dir, styledDir);
        if (c < cosMax) {
            const HairVec across = hsSub(dir, hsMul(styledDir, c));
            const float acrossLen = hsLen(across);
            dir = acrossLen > 1.0e-6f ? hsAdd(hsMul(styledDir, cosMax), hsMul(across, sinMax / acrossLen)) : styledDir;
        }
        const HairVec kept = hsAdd(p[k - 1], hsMul(dir, length));
        if (follow) {
            (*follow)[k] = hsAdd((*follow)[k], hsSub(kept, p[k]));  // the length's own correction (follow the leader)
        }
        p[k] = kept;
        // Out of the body: the segment's deepest point pushed to the surface by turning it about its parent point (the
        // first segment's top quarter is left alone: the root sits in the scalp).
        for (int round = 0; round < 3; ++round) {
            bool hit = false;
            HairVec normals;
            for (const HairCapsule& cap : capsules) {
                const float from = k == 1 ? 0.25f : 0.0f;
                const HairVec start = hsLerp(p[k - 1], p[k], from);
                float s = 0.0f;
                float t = 0.0f;
                HairVec onHair;
                HairVec onAxis;
                hsClosestSegments(start, p[k], cap.a, cap.b, s, t, onHair, onAxis);
                const HairVec out = hsSub(onHair, onAxis);
                const float distance = hsLen(out);
                if (distance >= cap.radius) {
                    continue;
                }
                const HairVec normal = distance > 1.0e-5f ? hsMul(out, 1.0f / distance) : hsNormal(hsSub(p[k], onAxis), HairVec{0.0f, 0.0f, 1.0f});
                const float depth = cap.radius - distance;
                // Turned about the point above, the end moves further than the deepest point (by the lever), but never
                // past the surface by much: an over-push becomes an outward speed, which threw hair off the body
                // (sneed 2026-11-20: "touches the body and freaks out to pulse off it").
                const float lever = std::max(from + (1.0f - from) * s, 0.5f);
                p[k] = hsAdd(p[k], hsMul(normal, depth / lever));
                normals = hsAdd(normals, normal);
                hit = true;
            }
            if (!hit) {
                break;
            }
            p[k] = hsAdd(p[k - 1], hsMul(hsNormal(hsSub(p[k], p[k - 1]), styledDir), length));
            any = true;
            if (contacts) {
                (*contacts)[k] = hsAdd((*contacts)[k], normals);
            }
        }
    }
    return any;
}

void HairStrands::solveSheet(const std::vector<std::vector<HairVec>>& goal, const HairSettings& settings) {
    for (const auto& [a, b] : neighbours_) {
        if (a < 0 || b < 0 || a >= static_cast<int>(points_.size()) || b >= static_cast<int>(points_.size())) {
            continue;
        }
        std::vector<HairVec>& pa = points_[static_cast<size_t>(a)];
        std::vector<HairVec>& pb = points_[static_cast<size_t>(b)];
        const size_t n = std::min(pa.size(), pb.size());
        for (size_t k = 1; k < n; ++k) {
            const float styled = hsLen(hsSub(goal[static_cast<size_t>(b)][k], goal[static_cast<size_t>(a)][k]));
            const HairVec d = hsSub(pb[k], pa[k]);
            const float len = hsLen(d);
            float want = len;
            if (len > styled * settings.sheetStretch) {
                want = styled * settings.sheetStretch;
            } else if (len < styled * settings.sheetSquash) {
                want = styled * settings.sheetSquash;
            }
            if (want == len || len < 1.0e-5f) {
                continue;
            }
            const HairVec move = hsMul(d, 0.5f * (len - want) / len);
            pa[k] = hsAdd(pa[k], move);
            pb[k] = hsSub(pb[k], move);
        }
    }
}

void HairStrands::start(const std::vector<std::vector<HairVec>>& goals, const std::vector<HairCapsule>& capsules,
                        const HairSettings& settings) {
    points_ = goals;
    contacts_.assign(goals.size(), {});
    follow_.assign(goals.size(), {});
    for (size_t i = 0; i < goals.size(); ++i) {
        contacts_[i].assign(goals[i].size(), HairVec{});
        follow_[i].assign(goals[i].size(), HairVec{});
        if (goals[i].size() >= 2) {
            for (int it = 0; it < 2; ++it) {
                solveStrand(points_[i], goals[i], capsules, -1.0f, 0.0f, nullptr, nullptr);
            }
        }
    }
    (void)settings;
    previous_ = points_;
    lastGoals_ = goals;
    lastCapsules_ = capsules;
    started_ = true;
}

void HairStrands::pose(const std::vector<std::vector<HairVec>>& goals, const std::vector<HairCapsule>& capsules,
                       const HairSettings& settings) {
    start(goals, capsules, settings);
}

void HairStrands::step(float dt, const std::vector<std::vector<HairVec>>& goals, const std::vector<HairCapsule>& capsules,
                       const HairSettings& settings) {
    bool restart = !started_ || !hsSameShape(goals, points_) || !hsSameShape(goals, lastGoals_);
    for (size_t i = 0; !restart && i < goals.size(); ++i) {
        restart = goals[i].empty() || hsLen(hsSub(goals[i][0], lastGoals_[i][0])) > settings.teleport;
    }
    if (restart) {
        start(goals, capsules, settings);
        return;
    }
    dt = std::min(dt, 0.1f);
    if (!(dt > 1.0e-6f)) {
        return;  // paused: the hair stays where it is
    }
    const int steps = std::clamp(static_cast<int>(std::ceil(dt / std::max(settings.substep, 1.0e-3f))), 1, 16);
    const float h = dt / static_cast<float>(steps);
    const float keep = std::exp(-settings.damping * h);
    const float maxMove = settings.maxSpeed * h;
    const float angle = std::clamp(settings.maxAngleDeg, 0.0f, 180.0f) * 3.14159265f / 180.0f;
    const float cosMax = std::cos(angle);
    const float sinMax = std::sin(angle);
    const HairVec fall{0.0f, 0.0f, -settings.gravity * h * h};
    const bool blendCapsules = lastCapsules_.size() == capsules.size();
    goalNow_ = goals;
    capsulesNow_ = capsules;
    rootMoves_.assign(goals.size(), HairVec{});
    for (int s = 0; s < steps; ++s) {
        const float a = static_cast<float>(s + 1) / static_cast<float>(steps);
        for (size_t i = 0; i < goals.size(); ++i) {
            for (size_t k = 0; k < goals[i].size(); ++k) {
                goalNow_[i][k] = hsLerp(lastGoals_[i][k], goals[i][k], a);
            }
        }
        if (blendCapsules) {
            for (size_t c = 0; c < capsules.size(); ++c) {
                capsulesNow_[c].a = hsLerp(lastCapsules_[c].a, capsules[c].a, a);
                capsulesNow_[c].b = hsLerp(lastCapsules_[c].b, capsules[c].b, a);
                capsulesNow_[c].radius = capsules[c].radius;
            }
        }
        // Move: the head carries the strands; their own swing (relative to it) keeps going, slowly dying away; they
        // fall; and a spring pulls each segment back towards the style's shape, stiffer near the root.
        for (size_t i = 0; i < goals.size(); ++i) {
            std::vector<HairVec>& p = points_[i];
            std::vector<HairVec>& prev = previous_[i];
            const std::vector<HairVec>& g = goalNow_[i];
            const size_t n = p.size();
            if (n < 2) {
                continue;
            }
            const HairVec rootMove = hsSub(g[0], p[0]);
            rootMoves_[i] = rootMove;
            prev[0] = p[0];
            p[0] = g[0];
            for (size_t k = 1; k < n; ++k) {
                HairVec swing = hsMul(hsSub(hsSub(p[k], prev[k]), rootMove), keep);
                const float speed = hsLen(swing);
                if (speed > maxMove) {
                    swing = hsMul(swing, maxMove / speed);
                }
                // The spring towards the style's shape: from where the point above it is now, the styled offset.
                const float along = n > 2 ? static_cast<float>(k - 1) / static_cast<float>(n - 2) : 0.0f;
                const float stiffness = settings.rootStiffness * std::pow(std::max(settings.tipStiffness, 0.0f) /
                                                                              std::max(settings.rootStiffness, 1.0e-3f), along);
                const HairVec target = hsAdd(p[k - 1], hsSub(g[k], g[k - 1]));
                const HairVec pull = hsMul(hsSub(target, p[k]), std::min(stiffness * h * h, 0.5f));
                prev[k] = p[k];
                p[k] = hsAdd(p[k], hsAdd(hsAdd(hsAdd(rootMove, swing), fall), pull));
            }
        }
        for (size_t i = 0; i < goals.size(); ++i) {
            std::fill(contacts_[i].begin(), contacts_[i].end(), HairVec{});
            std::fill(follow_[i].begin(), follow_[i].end(), HairVec{});
        }
        const int iterations = std::max(settings.iterations, 1);
        for (int it = 0; it < iterations; ++it) {
            if (!neighbours_.empty()) {
                solveSheet(goalNow_, settings);
            }
            for (size_t i = 0; i < goals.size(); ++i) {
                if (points_[i].size() >= 2) {
                    solveStrand(points_[i], goalNow_[i], capsulesNow_, cosMax, sinMax, &contacts_[i], &follow_[i]);
                }
            }
        }
        // Each point is only ever moved after the one above it (follow the leader), so the length's correction given to
        // the point below is taken back off this one's speed, or the chain gains energy and swings on (Mueller et al.
        // 2012, "Fast simulation of inextensible hair and fur"). Only the length's own correction: a push out of the
        // body fed back up the strand like this kicked the point above into the body every step.
        for (size_t i = 0; i < goals.size(); ++i) {
            for (size_t k = 1; k + 1 < points_[i].size(); ++k) {
                previous_[i][k] = hsAdd(previous_[i][k], hsMul(follow_[i][k + 1], settings.followDamping));
            }
        }
        // Touching the body: hair lands and stays (no bounce: the push out is not kept as speed away from the body),
        // and grips it: slow sliding stops dead, faster sliding loses a share each step.
        const float gripMove = settings.grip * h;
        const float slideKeep = std::exp(-settings.friction * h);
        for (size_t i = 0; i < goals.size(); ++i) {
            for (size_t k = 1; k < points_[i].size(); ++k) {
                const HairVec sum = contacts_[i][k];
                if (hsDot(sum, sum) < 1.0e-8f) {
                    continue;
                }
                const HairVec normal = hsNormal(sum, HairVec{0.0f, 0.0f, 1.0f});
                HairVec v = hsSub(hsSub(points_[i][k], previous_[i][k]), rootMoves_[i]);
                const float away = hsDot(v, normal);
                const HairVec normalPart = hsMul(normal, std::min(away, 0.0f));
                HairVec slide = hsSub(v, hsMul(normal, away));
                const float slideLen = hsLen(slide);
                slide = slideLen <= gripMove ? HairVec{} : hsMul(slide, slideKeep);
                v = hsAdd(normalPart, slide);
                previous_[i][k] = hsSub(hsSub(points_[i][k], rootMoves_[i]), v);
            }
        }
    }
    lastGoals_ = goals;
    lastCapsules_ = capsules;
}

float hairDeepestPenetration(const std::vector<std::vector<HairVec>>& strands, const std::vector<HairCapsule>& capsules) {
    float deepest = 0.0f;
    for (const std::vector<HairVec>& p : strands) {
        for (size_t k = 1; k < p.size(); ++k) {
            for (int j = 0; j <= 8; ++j) {
                const float t = static_cast<float>(j) / 8.0f;
                if (k == 1 && t < 0.25f) {
                    continue;
                }
                const HairVec at = hsLerp(p[k - 1], p[k], t);
                for (const HairCapsule& cap : capsules) {
                    deepest = std::max(deepest, cap.radius - hsPointSegmentDistance(at, cap.a, cap.b));
                }
            }
        }
    }
    return deepest;
}

std::vector<std::pair<int, int>> hairSheetNeighbours(const std::vector<HairVec>& roots, HairVec head, HairVec up) {
    std::vector<std::pair<int, int>> pairs;
    const int n = static_cast<int>(roots.size());
    if (n < 2) {
        return pairs;
    }
    up = hsNormal(up, HairVec{0.0f, 0.0f, 1.0f});
    HairVec e1 = hsCross(up, HairVec{1.0f, 0.0f, 0.0f});
    if (hsLen(e1) < 0.1f) {
        e1 = hsCross(up, HairVec{0.0f, 1.0f, 0.0f});
    }
    e1 = hsNormal(e1, HairVec{0.0f, 1.0f, 0.0f});
    const HairVec e2 = hsCross(up, e1);
    std::vector<float> angle(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const HairVec d = hsSub(roots[static_cast<size_t>(i)], head);
        angle[static_cast<size_t>(i)] = std::atan2(hsDot(d, e2), hsDot(d, e1));
    }
    std::vector<int> order(static_cast<size_t>(n));
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return angle[static_cast<size_t>(a)] < angle[static_cast<size_t>(b)]; });
    const float limit = 100.0f * 3.14159265f / 180.0f;
    const int links = n == 2 ? 1 : n;
    for (int j = 0; j < links; ++j) {
        const int a = order[static_cast<size_t>(j)];
        const int b = order[static_cast<size_t>((j + 1) % n)];
        float gap = angle[static_cast<size_t>(b)] - angle[static_cast<size_t>(a)];
        if (gap < 0.0f) {
            gap += 2.0f * 3.14159265f;
        }
        if (gap < limit) {
            pairs.emplace_back(std::min(a, b), std::max(a, b));
        }
    }
    return pairs;
}

std::vector<HairShape> buildHairShapes(HairBodyBones& bones, const std::vector<std::vector<HairVec>>& restStrands) {
    const HairVec up{0.0f, 0.0f, 1.0f};
    const auto horizontal = [&](HairVec v) { return hsSub(v, hsMul(up, hsDot(v, up))); };
    const float scale = std::clamp(hsLen(hsSub(bones.head, bones.pelvis)) / 60.0f, 0.6f, 1.5f);
    // Where the hair lies at rest (the points the shapes must stay clear of): along every segment, not the scalp end.
    std::vector<HairVec> samples;
    HairVec rootSum;
    int roots = 0;
    for (const std::vector<HairVec>& p : restStrands) {
        if (p.empty()) {
            continue;
        }
        rootSum = hsAdd(rootSum, p[0]);
        ++roots;
        for (size_t k = 1; k < p.size(); ++k) {
            for (int j = 1; j <= 8; ++j) {
                const float t = static_cast<float>(j) / 8.0f;
                if (k == 1 && t < 0.5f) {
                    continue;
                }
                samples.push_back(hsLerp(p[k - 1], p[k], t));
            }
        }
    }
    const HairVec back = roots > 0 ? hsNormal(horizontal(hsSub(hsMul(rootSum, 1.0f / static_cast<float>(roots)), bones.head)), HairVec{}) : HairVec{};
    HairVec side = bones.hasArms ? hsNormal(horizontal(hsSub(bones.upperArm[0], bones.upperArm[1])), HairVec{}) : HairVec{};
    if (hsLen(side) < 0.5f) {
        side = hsNormal(hsCross(up, back), HairVec{0.0f, 1.0f, 0.0f});
    }
    const float halfWidth = bones.hasArms ? 0.5f * hsLen(horizontal(hsSub(bones.upperArm[0], bones.upperArm[1]))) : 18.0f * scale;
    const float margin = 0.3f;
    const float reach = 4.0f * scale;

    std::vector<HairShape> shapes;
    // A shape `nominal` cm round a-b: shrunk to clear the hair at rest, or (`grow`) grown to touch it if it lies close.
    const auto add = [&](HairBone bone, HairVec a, HairVec b, float nominal, float lo, float hi, bool grow) {
        float nearest = 1.0e9f;
        for (const HairVec& p : samples) {
            nearest = std::min(nearest, hsPointSegmentDistance(p, a, b));
        }
        float r = nominal * scale;
        if (nearest < r) {
            r = std::max(nearest - margin, lo * scale);
        } else if (grow && nearest < r + reach) {
            r = std::min(nearest - margin, hi * scale);
        }
        HairShape shape;
        shape.bone = bone;
        shape.capsule = {a, b, r};
        shapes.push_back(shape);
    };

    // The skull: a ball above the head bone (which sits at the top of the neck), a little forward of it.
    const HairVec skull = hsSub(hsAdd(bones.head, hsMul(up, 8.0f * scale)), hsMul(back, 1.5f * scale));
    add(HairBone::Head, skull, skull, 9.5f, 6.5f, 10.5f, false);
    if (bones.hasNeck) {
        add(HairBone::Neck, bones.neck, bones.head, 5.5f, 4.0f, 7.5f, true);
    }
    bones.spineMid = -1;
    if (!bones.spine.empty()) {
        const HairVec top = bones.spine.back();
        // Chest and upper back: across the body at the top spine bone, and higher, towards the base of the neck.
        add(HairBone::SpineTop, hsAdd(top, hsMul(side, 0.5f * halfWidth)), hsSub(top, hsMul(side, 0.5f * halfWidth)), 10.0f, 6.0f, 14.0f, true);
        const float chest = shapes.back().capsule.radius;
        if (bones.hasNeck) {
            const HairVec yoke = hsLerp(top, bones.neck, 0.6f);
            add(HairBone::SpineTop, hsAdd(yoke, hsMul(side, 0.55f * halfWidth)), hsSub(yoke, hsMul(side, 0.55f * halfWidth)), 8.0f, 5.0f, 12.0f, true);
        }
        // The middle of the back: the spine bone nearest 55% of the way up from the pelvis to the top.
        if (bones.spine.size() >= 2) {
            const float want = bones.pelvis.z + 0.55f * (top.z - bones.pelvis.z);
            int best = 0;
            for (int i = 1; i + 1 < static_cast<int>(bones.spine.size()); ++i) {
                if (std::fabs(bones.spine[static_cast<size_t>(i)].z - want) < std::fabs(bones.spine[static_cast<size_t>(best)].z - want)) {
                    best = i;
                }
            }
            bones.spineMid = best;
            const HairVec mid = bones.spine[static_cast<size_t>(best)];
            add(HairBone::SpineMid, hsAdd(mid, hsMul(side, 0.4f * halfWidth)), hsSub(mid, hsMul(side, 0.4f * halfWidth)), 10.5f, 7.0f, 14.0f, true);
            // The small of the back, at the lowest spine bone, where waist-length hair ends.
            if (best > 0) {
                const HairVec low = bones.spine.front();
                add(HairBone::SpineLow, hsAdd(low, hsMul(side, 0.4f * halfWidth)), hsSub(low, hsMul(side, 0.4f * halfWidth)), 11.0f, 7.0f, 15.0f, true);
            }
        }
        // The front of the chest (the bust): a bar across the chest ahead of the spine and a little below the top spine
        // bone, sized from the chest shape just fitted to the back hair, so hair falling forward over the shoulders lies
        // on the chest instead of sinking into it. Only ever shrunk to clear the hair at rest.
        const HairVec bust = hsSub(hsSub(top, hsMul(back, 0.45f * chest)), hsMul(up, 4.0f * scale));
        add(HairBone::SpineTop, hsAdd(bust, hsMul(side, 0.4f * halfWidth)), hsSub(bust, hsMul(side, 0.4f * halfWidth)), 0.8f * chest / scale,
            4.0f, 0.8f * chest / scale, false);
    }
    // The hips: across the pelvis (below the hair's reach for most styles; the longest lie on them).
    add(HairBone::Pelvis, hsAdd(bones.pelvis, hsMul(side, 0.45f * halfWidth)), hsSub(bones.pelvis, hsMul(side, 0.45f * halfWidth)), 12.0f, 7.0f,
        12.0f, false);
    if (bones.hasArms) {
        // Shoulders: collarbone to the top of the arm; then the upper arms (which only clear the hair, never grow).
        add(HairBone::ClavicleL, bones.clavicle[0], bones.upperArm[0], 6.0f, 4.0f, 8.5f, true);
        add(HairBone::ClavicleR, bones.clavicle[1], bones.upperArm[1], 6.0f, 4.0f, 8.5f, true);
        add(HairBone::UpperArmL, bones.upperArm[0], bones.lowerArm[0], 5.0f, 3.5f, 6.5f, false);
        add(HairBone::UpperArmR, bones.upperArm[1], bones.lowerArm[1], 5.0f, 3.5f, 6.5f, false);
    }
    return shapes;
}

}  // namespace colony
