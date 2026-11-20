#include "ColonySim/HairStrands.h"

#include <doctest.h>

#include <cmath>
#include <vector>

using namespace colony;

#define HAIR_REQUIRE(...)      \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

// docs/design/hair-physics.md. sneed (2026-11-19): long hair clipped through the body; hair must obey physics and rest
// on the body. Axes in these tests: z up, the body faces +x, so the back is -x.

namespace {

HairVec hv(float x, float y, float z) { return {x, y, z}; }
float hvDist(HairVec a, HairVec b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }

// A strand of `n` segments of `length` cm from `root` going in direction d (unit).
std::vector<HairVec> strand(HairVec root, HairVec d, int n, float length) {
    std::vector<HairVec> p;
    for (int i = 0; i <= n; ++i) {
        p.push_back(hv(root.x + d.x * length * i, root.y + d.y * length * i, root.z + d.z * length * i));
    }
    return p;
}

bool finite(const std::vector<std::vector<HairVec>>& s) {
    for (const auto& p : s) {
        for (const HairVec& v : p) {
            if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) {
                return false;
            }
        }
    }
    return true;
}

// A simple woman-sized skeleton in its reference pose, and hair lying down her back and over her shoulders.
HairBodyBones testBody() {
    HairBodyBones b;
    b.pelvis = hv(0, 0, 97);
    b.spine = {hv(0, 0, 105), hv(0, 0, 118), hv(0, 0, 132)};
    b.hasNeck = true;
    b.neck = hv(0, 0, 145);
    b.head = hv(0, 0, 155);
    b.hasArms = true;
    b.clavicle[0] = hv(2, 3, 142);
    b.clavicle[1] = hv(2, -3, 142);
    b.upperArm[0] = hv(-1, 18, 138);
    b.upperArm[1] = hv(-1, -18, 138);
    b.lowerArm[0] = hv(-2, 28, 112);
    b.lowerArm[1] = hv(-2, -28, 112);
    return b;
}

std::vector<std::vector<HairVec>> testHair() {
    // Down the back (14 cm behind the spine), the back corners and the sides; four segments each.
    std::vector<std::vector<HairVec>> h;
    h.push_back({hv(-10, 0, 160), hv(-13, 0, 148), hv(-14, 0, 138), hv(-14, 0, 128), hv(-14, 0, 118)});
    h.push_back({hv(-8, 7, 160), hv(-11, 10, 148), hv(-12, 12, 138), hv(-12, 12, 128), hv(-12, 12, 118)});
    h.push_back({hv(-8, -7, 160), hv(-11, -10, 148), hv(-12, -12, 138), hv(-12, -12, 128), hv(-12, -12, 118)});
    return h;
}

std::vector<HairCapsule> capsulesOf(const std::vector<HairShape>& shapes) {
    std::vector<HairCapsule> c;
    for (const HairShape& s : shapes) {
        c.push_back(s.capsule);
    }
    return c;
}

}  // namespace

TEST_CASE("hair: a strand hangs still in its styled shape and keeps its lengths") {
    HairStrands hair;
    HairSettings settings;
    const std::vector<std::vector<HairVec>> goals = {strand(hv(0, 0, 160), hv(0, 0, -1), 4, 10.0f)};
    for (int f = 0; f < 120; ++f) {
        hair.step(1.0f / 60.0f, goals, {}, settings);
    }
    HAIR_REQUIRE(hair.points().size() == 1);
    const auto& p = hair.points()[0];
    for (size_t k = 1; k < p.size(); ++k) {
        CHECK(hvDist(p[k], p[k - 1]) == doctest::Approx(10.0f).epsilon(0.01));
        CHECK(hvDist(p[k], goals[0][k]) < 1.0f);
    }
}

TEST_CASE("hair: gravity pulls hair styled sideways down, and the root stays on the head") {
    HairStrands hair;
    HairSettings settings;
    const std::vector<std::vector<HairVec>> goals = {strand(hv(0, 0, 160), hv(-1, 0, 0), 4, 10.0f)};
    for (int f = 0; f < 180; ++f) {
        hair.step(1.0f / 60.0f, goals, {}, settings);
    }
    const auto& p = hair.points()[0];
    CHECK(hvDist(p[0], goals[0][0]) < 1.0e-3f);
    CHECK(p[4].z < 160.0f - 10.0f);  // the tip has fallen well below the root
    CHECK(p[4].x < -5.0f);           // but the style still holds it out a little
}

TEST_CASE("hair: the head's acceleration swings the hair the other way, then it settles back") {
    HairStrands hair;
    HairSettings settings;
    float x = 0.0f;
    const auto goalsAt = [&](float rootX) { return std::vector<std::vector<HairVec>>{strand(hv(rootX, 0, 160), hv(0, 0, -1), 4, 10.0f)}; };
    for (int f = 0; f < 60; ++f) {
        hair.step(1.0f / 60.0f, goalsAt(x), {}, settings);
    }
    // A quick start forwards (+x) at 4 m/s: the tip lags behind the root.
    float lag = 0.0f;
    for (int f = 0; f < 15; ++f) {
        x += 400.0f / 60.0f;
        hair.step(1.0f / 60.0f, goalsAt(x), {}, settings);
        lag = std::max(lag, x - hair.points()[0][4].x);
    }
    CHECK(lag > 3.0f);
    // Stopped for two seconds: back to hanging under the head.
    for (int f = 0; f < 120; ++f) {
        hair.step(1.0f / 60.0f, goalsAt(x), {}, settings);
    }
    CHECK(std::fabs(hair.points()[0][4].x - x) < 1.5f);
}

TEST_CASE("hair: a strand styled through the body is pushed out, with and without dynamics") {
    const std::vector<HairCapsule> body = {{hv(0, -15, 130), hv(0, 15, 130), 10.0f}};
    // Straight down from behind the head, through the capsule.
    const std::vector<std::vector<HairVec>> goals = {strand(hv(-3, 0, 160), hv(0, 0, -1), 4, 12.0f)};
    HAIR_REQUIRE(hairDeepestPenetration(goals, body) > 5.0f);
    HairStrands far;
    far.pose(goals, body, HairSettings{});
    CHECK(hairDeepestPenetration(far.points(), body) < 0.6f);
    HairStrands near;
    float worst = 0.0f;
    for (int f = 0; f < 120; ++f) {
        near.step(1.0f / 60.0f, goals, body, HairSettings{});
        worst = std::max(worst, hairDeepestPenetration(near.points(), body));
    }
    CHECK(worst < 0.6f);
    // It rests on the back, not floating off it.
    float nearest = 1.0e9f;
    for (const HairVec& v : near.points()[0]) {
        nearest = std::min(nearest, std::sqrt(v.x * v.x + (v.z - 130.0f) * (v.z - 130.0f)) - 10.0f);
    }
    CHECK(nearest < 1.5f);
}

TEST_CASE("hair: a body turning into the hair never passes through it") {
    // Hair hanging beside the shoulder; the shoulder capsule swings round into it over half a second and back.
    const std::vector<std::vector<HairVec>> goals = {strand(hv(0, 12, 160), hv(0, 0, -1), 4, 10.0f)};
    HairStrands hair;
    float worst = 0.0f;
    for (int f = 0; f < 120; ++f) {
        const float a = 1.2f * std::sin(static_cast<float>(f) / 60.0f * 3.14159265f * 2.0f);
        const HairVec shoulder = hv(20.0f * std::cos(a + 1.57f), 20.0f * std::sin(a + 1.57f), 140.0f);
        const std::vector<HairCapsule> body = {{hv(0, 0, 140), shoulder, 6.0f}};
        hair.step(1.0f / 60.0f, goals, body, HairSettings{});
        worst = std::max(worst, hairDeepestPenetration(hair.points(), body));
    }
    CHECK(worst < 1.0f);
}

TEST_CASE("hair: rough motion at any frame rate stays steady, keeps lengths and stays near the style") {
    HairStrands hair;
    HairSettings settings;
    const std::vector<HairCapsule> body = {{hv(0, -15, 130), hv(0, 15, 130), 9.0f}, {hv(5, 0, 165), hv(5, 0, 165), 9.0f}};
    uint32_t seed = 12345u;
    const auto rnd = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / 16777216.0f;
    };
    float x = 0.0f;
    float y = 0.0f;
    for (int f = 0; f < 2000; ++f) {
        const float dt = 1.0f / 240.0f + rnd() * 0.1f;
        x += (rnd() - 0.5f) * 30.0f;
        y += (rnd() - 0.5f) * 30.0f;
        std::vector<std::vector<HairVec>> goals;
        for (int c = 0; c < 3; ++c) {
            goals.push_back(strand(hv(x - 10.0f, y + (c - 1) * 6.0f, 160.0f), hv(-0.3f, 0.0f, -0.95f), 4, 10.0f));
        }
        std::vector<HairCapsule> moved = body;
        for (HairCapsule& c : moved) {
            c.a.x += x;
            c.b.x += x;
            c.a.y += y;
            c.b.y += y;
        }
        hair.step(dt, goals, moved, settings);
        HAIR_REQUIRE(finite(hair.points()));
        for (const auto& p : hair.points()) {
            for (size_t k = 1; k < p.size(); ++k) {
                HAIR_REQUIRE(hvDist(p[k], p[k - 1]) == doctest::Approx(10.0f).epsilon(0.02));
            }
        }
        CHECK(hairDeepestPenetration(hair.points(), moved) < 1.5f);
    }
}

TEST_CASE("hair: a teleport starts the hair again at rest") {
    HairStrands hair;
    const auto goalsAt = [](float x) { return std::vector<std::vector<HairVec>>{strand(hv(x, 0, 160), hv(0, 0, -1), 3, 10.0f)}; };
    hair.step(1.0f / 60.0f, goalsAt(0.0f), {}, HairSettings{});
    hair.step(1.0f / 60.0f, goalsAt(500.0f), {}, HairSettings{});
    CHECK(hvDist(hair.points()[0][3], goalsAt(500.0f)[0][3]) < 1.0e-3f);
}

TEST_CASE("hair: paused, the hair stays exactly where it was") {
    HairStrands hair;
    const auto goalsAt = [](float x) { return std::vector<std::vector<HairVec>>{strand(hv(x, 0, 160), hv(-1, 0, 0), 3, 10.0f)}; };
    for (int f = 0; f < 10; ++f) {
        hair.step(1.0f / 60.0f, goalsAt(static_cast<float>(f)), {}, HairSettings{});
    }
    const auto before = hair.points();
    hair.step(0.0f, goalsAt(9.0f), {}, HairSettings{});
    CHECK(hvDist(hair.points()[0][3], before[0][3]) < 1.0e-6f);
}

TEST_CASE("hair: neighbouring strands of a sheet don't drift apart") {
    HairSettings settings;
    // Two strands 6 cm apart; a small ball pushes only the second one back (-x), away from the first.
    const std::vector<std::vector<HairVec>> goals = {strand(hv(0, 0, 160), hv(0, 0, -1), 4, 10.0f), strand(hv(0, 6, 160), hv(0, 0, -1), 4, 10.0f)};
    const std::vector<HairCapsule> body = {{hv(2, 6, 135), hv(2, 6, 125), 6.0f}};
    const auto widest = [&](bool sheet) {
        HairStrands hair;
        if (sheet) {
            hair.setSheetNeighbours({{0, 1}});
        }
        for (int f = 0; f < 120; ++f) {
            hair.step(1.0f / 60.0f, goals, body, settings);
        }
        float w = 0.0f;
        for (size_t k = 1; k < 5; ++k) {
            w = std::max(w, hvDist(hair.points()[0][k], hair.points()[1][k]));
        }
        return w;
    };
    const float apart = widest(false);
    const float together = widest(true);
    CHECK(apart > 6.0f * settings.sheetStretch + 0.5f);
    CHECK(together < apart);
    CHECK(together < 6.0f * settings.sheetStretch + 1.0f);
}

TEST_CASE("hair: sheet neighbours go round the back of the head, never across the face") {
    // Roots at 0 (behind), +-55 and +-115 degrees, as mh_common.add_hair_bones makes them; behind is -x.
    std::vector<HairVec> roots;
    const float az[] = {0.0f, 55.0f, -55.0f, 115.0f, -115.0f};
    for (float a : az) {
        const float r = a * 3.14159265f / 180.0f;
        roots.push_back(hv(-10.0f * std::cos(r), 10.0f * std::sin(r), 160.0f));
    }
    const auto pairs = hairSheetNeighbours(roots, hv(0, 0, 160), hv(0, 0, 1));
    HAIR_REQUIRE(pairs.size() == 4);
    for (const auto& [a, b] : pairs) {
        CHECK_FALSE(((a == 3 && b == 4) || (a == 4 && b == 3)));
    }
    CHECK(hairSheetNeighbours({hv(-10, 0, 160)}, hv(0, 0, 160), hv(0, 0, 1)).empty());
    CHECK(hairSheetNeighbours({hv(-10, 0, 160), hv(-8, 5, 160)}, hv(0, 0, 160), hv(0, 0, 1)).size() == 1);
}

TEST_CASE("hair: the body's shapes clear the hair at rest and the back is grown to meet it") {
    HairBodyBones bones = testBody();
    const auto hair = testHair();
    const auto shapes = buildHairShapes(bones, hair);
    // Skull, neck, chest, yoke, mid back, two shoulders, two upper arms.
    HAIR_REQUIRE(shapes.size() == 9);
    CHECK(bones.spineMid == 1);
    CHECK(hairDeepestPenetration(hair, capsulesOf(shapes)) <= 0.31f);
    // The chest/upper back capsule at the top spine bone: the hair lies 12 to 14 cm from it, so it grows to meet it.
    bool foundChest = false;
    for (const HairShape& s : shapes) {
        if (s.bone == HairBone::SpineTop && std::fabs(s.capsule.a.z - 132.0f) < 0.01f) {
            foundChest = true;
            CHECK(s.capsule.radius > 11.0f);  // nominal 10 cm x 0.97 for her size
        }
    }
    CHECK(foundChest);
    // Hair already at rest stays at rest: no pop when the simulation starts.
    HairStrands strands;
    strands.pose(hair, capsulesOf(shapes), HairSettings{});
    for (size_t i = 0; i < hair.size(); ++i) {
        for (size_t k = 0; k < hair[i].size(); ++k) {
            CHECK(hvDist(strands.points()[i][k], hair[i][k]) < 1.0e-3f);
        }
    }
}

TEST_CASE("hair: shapes for a skeleton without arms, neck or spine still make a skull") {
    HairBodyBones bones;
    bones.pelvis = hv(0, 0, 97);
    bones.head = hv(0, 0, 155);
    const auto shapes = buildHairShapes(bones, testHair());
    HAIR_REQUIRE(shapes.size() == 1);
    CHECK(shapes[0].bone == HairBone::Head);
    CHECK(bones.spineMid == -1);
}

namespace {

// Long hair lying down her back and over her shoulders, as styled (touching the fitted body), five strands of a sheet.
std::vector<std::vector<HairVec>> restingHair() {
    std::vector<std::vector<HairVec>> h;
    h.push_back({hv(-10, 0, 160), hv(-13, 0, 148), hv(-14, 0, 138), hv(-14, 0, 128), hv(-14, 0, 118), hv(-14, 0, 108)});
    h.push_back({hv(-8, 7, 160), hv(-11, 10, 148), hv(-12, 12, 138), hv(-12, 12, 128), hv(-12, 12, 118), hv(-12, 12, 108)});
    h.push_back({hv(-8, -7, 160), hv(-11, -10, 148), hv(-12, -12, 138), hv(-12, -12, 128), hv(-12, -12, 118), hv(-12, -12, 108)});
    h.push_back({hv(-2, 10, 160), hv(-3, 15, 150), hv(-2, 18, 142), hv(-2, 19, 132), hv(-2, 19, 122), hv(-2, 19, 112)});
    h.push_back({hv(-2, -10, 160), hv(-3, -15, 150), hv(-2, -18, 142), hv(-2, -19, 132), hv(-2, -19, 122), hv(-2, -19, 112)});
    return h;
}

// The fastest any point moves relative to where the animation puts it, over the last `measure` of `frames` frames at
// 60 fps, with the whole body leaning forward and back by `sway` cm at the head every 4 s (an idle's breathing).
float restlessness(float sway, int frames, int measure) {
    HairBodyBones bones = testBody();
    const auto rest = restingHair();
    std::vector<HairCapsule> body;
    for (const HairShape& s : buildHairShapes(bones, rest)) {
        body.push_back(s.capsule);
    }
    HairStrands hair;
    hair.setSheetNeighbours({{0, 1}, {0, 2}, {1, 3}, {2, 4}});
    float fastest = 0.0f;
    std::vector<std::vector<HairVec>> lastOffset;
    for (int f = 0; f < frames; ++f) {
        const float lean = sway * std::sin(static_cast<float>(f) / 60.0f * 2.0f * 3.14159265f * 0.25f) / 58.0f;
        auto goals = rest;
        for (auto& p : goals) {
            for (HairVec& v : p) {
                v.x += lean * (v.z - 97.0f);
            }
        }
        auto moved = body;
        for (HairCapsule& c : moved) {
            c.a.x += lean * (c.a.z - 97.0f);
            c.b.x += lean * (c.b.z - 97.0f);
        }
        hair.step(1.0f / 60.0f, goals, moved, HairSettings{});
        std::vector<std::vector<HairVec>> offset = hair.points();
        for (size_t i = 0; i < offset.size(); ++i) {
            for (size_t k = 0; k < offset[i].size(); ++k) {
                offset[i][k] = hv(offset[i][k].x - goals[i][k].x, offset[i][k].y - goals[i][k].y, offset[i][k].z - goals[i][k].z);
            }
        }
        if (f >= frames - measure && !lastOffset.empty()) {
            for (size_t i = 0; i < offset.size(); ++i) {
                for (size_t k = 1; k < offset[i].size(); ++k) {
                    fastest = std::max(fastest, hvDist(offset[i][k], lastOffset[i][k]) * 60.0f);
                }
            }
        }
        lastOffset = offset;
    }
    return fastest;
}

}  // namespace

TEST_CASE("hair: long hair lying on the back comes to rest and stays still (no pulsing off the body)") {
    // sneed 2026-11-20: "the hair touches the body and freaks out to pulse off it". A body standing dead still: after
    // settling, nothing moves at all.
    CHECK(restlessness(0.0f, 600, 240) < 0.05f);
    // An idle's gentle lean (1 cm at the head): the hair goes with the body, never faster than the lean itself drives it.
    CHECK(restlessness(1.0f, 600, 240) < 3.0f);
}

TEST_CASE("hair: hair falling onto a shoulder lands and stays, without bouncing off") {
    // A strand styled out sideways, falling under gravity onto a capsule under it.
    const std::vector<HairCapsule> body = {{hv(-15, 10, 140), hv(15, 10, 140), 6.0f}};
    const std::vector<std::vector<HairVec>> goals = {strand(hv(0, 0, 160), hv(0, 0.6f, 0.8f), 4, 8.0f)};
    HairSettings settings;
    settings.rootStiffness = 0.0f;  // nothing holds it up: it drops onto the shoulder
    settings.tipStiffness = 0.0f;
    HairStrands hair;
    float lowestAfterLanding = 1.0e9f;
    float highestAfterLanding = -1.0e9f;
    for (int f = 0; f < 360; ++f) {
        hair.step(1.0f / 60.0f, goals, body, settings);
        if (f > 120) {
            lowestAfterLanding = std::min(lowestAfterLanding, hair.points()[0][2].z);
            highestAfterLanding = std::max(highestAfterLanding, hair.points()[0][2].z);
        }
        CHECK(hairDeepestPenetration(hair.points(), body) < 1.0f);
    }
    // Once landed it lies still: the middle of the strand moves less than half a centimetre up or down.
    CHECK(highestAfterLanding - lowestAfterLanding < 0.5f);
}
