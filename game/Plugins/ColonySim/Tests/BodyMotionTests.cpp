#include "ColonySim/BodyMotion.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Looks.h"
#include "ColonySim/Melee.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include "TestPlants.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace colony;

// docs/design/body-motion.md. sneed's rule, which these tests guard: a clip that moves the body moves it through the

#define MOTION_REQUIRE(...)    \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// A straight path: `metres` forward (and `side` to the right, turning `turn` degrees) eased over `seconds`, then
// `back` metres back by the end of `total` seconds.
std::string motionTestRoot(float metres, float seconds, float total, float back = 0.0f, float side = 0.0f, float turn = 0.0f) {
    std::ostringstream out;
    const int n = static_cast<int>(std::lround(total * 30.0f));
    const int reach = static_cast<int>(std::lround(seconds * 30.0f));
    for (int i = 0; i <= n; ++i) {
        float a = std::min(1.0f, static_cast<float>(i) / static_cast<float>(reach));
        a = a * a * (3.0f - 2.0f * a);
        float f = metres * a;
        if (i > reach && n > reach) {
            f -= back * static_cast<float>(i - reach) / static_cast<float>(n - reach);
        }
        out << (i > 0 ? "," : "") << "[" << f << "," << side * a << "," << turn * a << "]";
    }
    return out.str();
}

// The game's real data, plus a wall and a few clips with known paths.
std::shared_ptr<const DefDatabase> motionTestDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        namespace fs = std::filesystem;
        const fs::path root = fs::path(COLONY_DATA_DIR);
        std::vector<DefSource> sources;
        std::vector<fs::path> files;
        for (const auto& entry : fs::recursive_directory_iterator(root / "defs")) {
            // The measured clips are left out: these tests use clips with known paths, under the same names.
            if (entry.is_regular_file() && entry.path().extension() == ".json" && entry.path().parent_path().filename() != "MotionClips") {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());
        for (const fs::path& file : files) {
            std::ifstream in(file, std::ios::binary);
            std::stringstream text;
            text << in.rdbuf();
            sources.push_back(DefSource{file.parent_path().filename().string(), file.filename().string(), text.str()});
        }
        sources.push_back(DefSource{"Things", "motion-test-things.json", R"([
            {"defName": "MotionTestWall", "category": "Building", "passability": "Impassable", "fillPercent": 1.0, "stats": {"maxHitPoints": 3000}}
        ])"});
        std::string clips = "[";
        clips += R"({"defName": "MotionTest_Dash", "kind": "Test", "root": [)" + motionTestRoot(3.0f, 1.0f, 1.0f) + "]},";
        clips += R"({"defName": "MotionTest_Turn", "kind": "Test", "root": [)" + motionTestRoot(1.0f, 1.0f, 1.0f, 0.0f, 1.0f, 90.0f) + "]},";
        clips += R"({"defName": "Motion_StaggerBack", "kind": "Stagger", "root": [)" + motionTestRoot(-0.6f, 0.4f, 0.6f) + "]},";
        clips += R"({"defName": "Motion_KnockdownBack", "kind": "Knockdown", "keepWhenDown": true, "root": [)" + motionTestRoot(-1.2f, 0.6f, 1.0f) + "]},";
        clips += R"({"defName": "Motion_Club_Downward", "kind": "Attack", "hitSeconds": 0.5, "root": [)" + motionTestRoot(0.3f, 0.5f, 1.0f, 0.2f) + "]},";
        // One-hand and unarmed swings, and downward blows at a body on the ground (focus.md): the knife's own stab, the
        // one-hand style's hack; fists have none, so they punch down with their usual clip.
        clips += R"({"defName": "Motion_OneHand_Slash1", "kind": "Attack", "hitSeconds": 0.5, "root": [)" + motionTestRoot(0.2f, 0.5f, 1.0f, 0.2f) + "]},";
        clips += R"({"defName": "Motion_Knife_Stab_Low", "kind": "Attack", "hitSeconds": 0.5, "root": [)" + motionTestRoot(0.2f, 0.5f, 1.0f, 0.2f) + "]},";
        clips += R"({"defName": "Motion_OneHand_Hack_Low", "kind": "Attack", "hitSeconds": 0.5, "root": [)" + motionTestRoot(0.2f, 0.5f, 1.0f, 0.2f) + "]},";
        clips += R"({"defName": "Motion_Unarmed_Jab", "kind": "Attack", "hitSeconds": 0.4, "root": [)" + motionTestRoot(0.2f, 0.4f, 0.8f, 0.2f) + "]},";
        // The spear's own thrust (MeleeRules.styleAttacks.Melee_Spear); its lunge is not measured here.
        clips += R"({"defName": "Motion_Spear_Stab", "kind": "Attack", "hitSeconds": 0.45, "root": [)" + motionTestRoot(0.3f, 0.45f, 0.9f, 0.2f) + "]},";
        // A six second flourish: never used in a fight.
        clips += R"({"defName": "Motion_Club_Horizontal", "kind": "Attack", "hitSeconds": 3.0, "root": [)" + motionTestRoot(0.5f, 3.0f, 6.0f) + "]}";
        clips += "]";
        sources.push_back(DefSource{"MotionClips", "motion-test-clips.json", clips});
        std::ifstream tuningFile(root / "tuning.json", std::ios::binary);
        std::stringstream tuning;
        tuning << tuningFile.rdbuf();
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::build(sources, tuning.str(), *db, errors);
        for (const std::string& e : errors) {
            MESSAGE(e);
        }
        return db;
    }();
    return defs;
}

std::unique_ptr<Sim> motionTestSim(uint64_t seed = 5) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = 64;
    settings.flatMap = true;
    settings.checkpointEvery = 50;
    return withoutPlants(std::make_unique<Sim>(settings, motionTestDefs()));
}

Entity motionTestPawn(Sim& sim, float x, float y, const char* weapon = nullptr, const char* def = "Human") {
    const Entity pawn = sim.things().spawn(def, CellPos{static_cast<int>(x), static_cast<int>(y), 0});
    Position& pos = sim.things().registry().get<Position>(pawn);
    pos.x = x;
    pos.y = y;
    if (weapon != nullptr) {
        sim.combat().equip(pawn, sim.things().make(sim.defs().idOf("Things", weapon)));
    }
    return pawn;
}

// Makes it the player's own pawn, so it moves only when the test moves it.
void motionTestControl(Sim& sim, Entity pawn) {
    (void)sim.things().registry().get_or_emplace<Colonist>(pawn);
    sim.jobs().addWorker(pawn);
    sim.submit(ControlPawnCommand{pawn});
    sim.step();
}

void motionTestFace(Sim& sim, Entity pawn, float x, float y) {
    Posture& p = sim.things().registry().get<Posture>(pawn);
    p.facingX = x;
    p.facingY = y;
}

const Position& motionTestPos(const Sim& sim, Entity pawn) {
    return sim.things().registry().get<Position>(pawn);
}

float motionTestDist(const Sim& sim, Entity a, Entity b) {
    const Position& p = motionTestPos(sim, a);
    const Position& q = motionTestPos(sim, b);
    return std::sqrt((p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y));
}

void motionTestWall(Sim& sim, int x0, int y0, int x1, int y1) {
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            sim.things().spawn("MotionTestWall", CellPos{x, y, 0});
        }
    }
}

// Steps until the pawn's motion is over (at most `most` steps); the steps taken.
int motionTestFinish(Sim& sim, Entity pawn, int most = 400) {
    int steps = 0;
    while (sim.combat().motion().moving(pawn) && steps < most) {
        sim.step();
        ++steps;
    }
    return steps;
}

}  // namespace

TEST_CASE("Motion: a clip carries the body along its path and the body stays there when it ends") {
    auto sim = motionTestSim();
    BodyMotionSystem& motion = sim->combat().motion();
    const Entity pawn = motionTestPawn(*sim, 20.5f, 30.5f);
    sim->stepMany(3);
    motionTestFace(*sim, pawn, 1.0f, 0.0f);
    const float startX = motionTestPos(*sim, pawn).x;
    const float startY = motionTestPos(*sim, pawn).y;
    const float scale = motion.sizeScale(pawn);
    MOTION_REQUIRE(motion.play(pawn, motion.clipId("MotionTest_Dash")));
    float last = startX;
    int steps = 0;
    while (motion.moving(pawn) && steps < 100) {
        sim->step();
        ++steps;
        // Never backwards on a path that only goes forward.
        CHECK(motionTestPos(*sim, pawn).x >= last - 1e-4f);
        last = motionTestPos(*sim, pawn).x;
    }
    CHECK(steps == 30);  // one second
    CHECK(motionTestPos(*sim, pawn).x == doctest::Approx(startX + 3.0f * scale).epsilon(0.001));
    CHECK(motionTestPos(*sim, pawn).y == doctest::Approx(startY).epsilon(0.001));
    // The clip is over: for the next ten seconds nothing brings the body back.
    const float endX = motionTestPos(*sim, pawn).x;
    for (int i = 0; i < 300; ++i) {
        sim->step();
        CHECK(motionTestPos(*sim, pawn).x == doctest::Approx(endX));
    }
    CHECK(std::fabs(motionTestPos(*sim, pawn).x - startX) > 2.5f);
}

TEST_CASE("Motion: a clip follows the way the body faced, goes to its right, turns it, and can be stretched") {
    auto sim = motionTestSim();
    BodyMotionSystem& motion = sim->combat().motion();
    const Entity pawn = motionTestPawn(*sim, 30.5f, 30.5f);
    sim->stepMany(3);
    motionTestFace(*sim, pawn, 0.0f, 1.0f);  // facing +y: its right is -x (as Unreal draws the map)
    const Position start = motionTestPos(*sim, pawn);
    const float scale = motion.sizeScale(pawn);
    MOTION_REQUIRE(motion.play(pawn, motion.clipId("MotionTest_Turn"), 0.0f, 0.0f, 60));  // at half speed
    CHECK(motionTestFinish(*sim, pawn) == 60);
    CHECK(motionTestPos(*sim, pawn).y == doctest::Approx(start.y + 1.0f * scale).epsilon(0.002));
    CHECK(motionTestPos(*sim, pawn).x == doctest::Approx(start.x - 1.0f * scale).epsilon(0.002));
    // Turned a quarter to the right: now facing -x.
    const Posture& posture = sim->things().registry().get<Posture>(pawn);
    CHECK(posture.facingX == doctest::Approx(-1.0f).epsilon(0.001));
    CHECK(std::fabs(posture.facingY) < 0.001f);
}

TEST_CASE("Motion: a wall stops the body, it never ends up inside or beyond it, and the lost motion is gone") {
    auto sim = motionTestSim();
    BodyMotionSystem& motion = sim->combat().motion();
    motionTestWall(*sim, 23, 25, 23, 35);
    const Entity pawn = motionTestPawn(*sim, 21.5f, 30.5f);
    sim->stepMany(3);
    motionTestFace(*sim, pawn, 1.0f, 0.0f);
    MOTION_REQUIRE(motion.play(pawn, motion.clipId("MotionTest_Dash")));
    float lost = 0.0f;
    while (motion.moving(pawn)) {
        lost = sim->things().registry().get<BodyMotion>(pawn).lost;
        sim->step();
        CHECK(motionTestPos(*sim, pawn).x <= 23.0f - kPawnRadius + 1e-3f);
        CHECK(pawnFitsAt(sim->map(), motionTestPos(*sim, pawn).x, motionTestPos(*sim, pawn).y, kPawnRadius, 0));
    }
    CHECK(lost > 1.0f);
    CHECK(motionTestPos(*sim, pawn).x > 22.5f);  // right up against it
    const float endX = motionTestPos(*sim, pawn).x;
    sim->stepMany(120);
    CHECK(motionTestPos(*sim, pawn).x == doctest::Approx(endX));  // not back to the start, not through the wall
}

TEST_CASE("Motion: a body slides along a wall it meets at an angle") {
    auto sim = motionTestSim();
    BodyMotionSystem& motion = sim->combat().motion();
    motionTestWall(*sim, 23, 20, 23, 40);
    const Entity pawn = motionTestPawn(*sim, 22.0f, 30.5f);
    sim->stepMany(3);
    const float startY = motionTestPos(*sim, pawn).y;
    MOTION_REQUIRE(motion.play(pawn, motion.clipId("MotionTest_Dash"), 0.7071f, 0.7071f));
    motionTestFinish(*sim, pawn);
    CHECK(motionTestPos(*sim, pawn).x <= 23.0f - kPawnRadius + 1e-3f);
    CHECK(motionTestPos(*sim, pawn).y > startY + 1.5f);
}

TEST_CASE("Motion: bodies don't pass through each other: an equal body is shoved, a heavy one stops you") {
    auto sim = motionTestSim();
    BodyMotionSystem& motion = sim->combat().motion();
    const Entity mover = motionTestPawn(*sim, 20.5f, 30.5f);
    const Entity man = motionTestPawn(*sim, 22.0f, 30.5f);
    sim->stepMany(3);
    motionTestFace(*sim, mover, 1.0f, 0.0f);
    const float manStart = motionTestPos(*sim, man).x;
    const float moverStart = motionTestPos(*sim, mover).x;
    MOTION_REQUIRE(motion.play(mover, motion.clipId("MotionTest_Dash")));
    const float apart = motion.bodyRadius(mover) + motion.bodyRadius(man);
    while (motion.moving(mover)) {
        sim->step();
        CHECK(motionTestDist(*sim, mover, man) >= apart - 0.03f);
        CHECK(motionTestPos(*sim, mover).x < motionTestPos(*sim, man).x);  // never past or through him
    }
    CHECK(motionTestPos(*sim, man).x > manStart + 0.5f);  // shoved along
    CHECK(motionTestPos(*sim, mover).x < moverStart + 3.0f * motion.sizeScale(mover) - 0.5f);  // and it cost the mover ground
    CHECK(motion.shoves() > 0);

    // The same dash into a thrumbo: it barely moves, and the man is stopped at its flank.
    auto sim2 = motionTestSim();
    BodyMotionSystem& motion2 = sim2->combat().motion();
    const Entity runner = motionTestPawn(*sim2, 20.5f, 30.5f);
    const Entity beast = motionTestPawn(*sim2, 23.0f, 30.5f, nullptr, "Thrumbo");
    sim2->stepMany(3);
    motionTestFace(*sim2, runner, 1.0f, 0.0f);
    const Position beastStart = motionTestPos(*sim2, beast);
    CHECK(motion2.bodyRadius(beast) > 1.5f * motion2.bodyRadius(runner));
    CHECK(motion2.mass(beast) > 3.0f * motion2.mass(runner));
    MOTION_REQUIRE(motion2.play(runner, motion2.clipId("MotionTest_Dash")));
    const Position runnerStart = motionTestPos(*sim2, runner);
    float beastMoved = 0.0f;
    while (motion2.moving(runner)) {
        const Position before = motionTestPos(*sim2, beast);
        sim2->step();
        // What the dash did to it this step (it may also walk about by itself).
        CHECK(motionTestDist(*sim2, runner, beast) >= motion2.bodyRadius(runner) + motion2.bodyRadius(beast) - 0.2f);
        beastMoved += std::fabs(motionTestPos(*sim2, beast).x - before.x);
    }
    (void)beastStart;
    CHECK(motionTestPos(*sim2, runner).x - runnerStart.x < 3.0f * motion2.sizeScale(runner) - 0.5f);
    CHECK(motionTestPos(*sim2, runner).x < motionTestPos(*sim2, beast).x);
}

TEST_CASE("Motion: every dodge roll ends where it rolled to, in every direction, and stays there") {
    for (int dir = 0; dir < 8; ++dir) {
        auto sim = motionTestSim();
        const Entity pawn = motionTestPawn(*sim, 30.5f, 30.5f);
        motionTestControl(*sim, pawn);
        const float dx[8] = {1, 1, 0, -1, -1, -1, 0, 1};
        const float dy[8] = {0, 1, 1, 1, 0, -1, -1, -1};
        const Position start = motionTestPos(*sim, pawn);
        sim->submit(DodgeRollCommand{pawn, dx[dir], dy[dir]});
        sim->stepMany(27);
        CHECK(sim->combat().rollPhase(pawn) == RollPhase::Idle);
        CHECK_FALSE(sim->combat().motion().moving(pawn));
        const Position end = motionTestPos(*sim, pawn);
        const float len = std::sqrt(dx[dir] * dx[dir] + dy[dir] * dy[dir]);
        const float want = 4.0f * sim->combat().motion().sizeScale(pawn);
        CHECK(end.x - start.x == doctest::Approx(dx[dir] / len * want).epsilon(0.01));
        CHECK(end.y - start.y == doctest::Approx(dy[dir] / len * want).epsilon(0.01));
        sim->stepMany(200);
        CHECK(motionTestPos(*sim, pawn).x == doctest::Approx(end.x));
        CHECK(motionTestPos(*sim, pawn).y == doctest::Approx(end.y));
    }
}

TEST_CASE("Motion: a roll into a wall stops at the wall, and a roll into a man doesn't go through him") {
    auto sim = motionTestSim();
    motionTestWall(*sim, 33, 25, 33, 35);
    const Entity pawn = motionTestPawn(*sim, 31.5f, 30.5f);
    motionTestControl(*sim, pawn);
    sim->submit(DodgeRollCommand{pawn, 1.0f, 0.0f});
    sim->stepMany(60);
    CHECK(motionTestPos(*sim, pawn).x <= 33.0f - kPawnRadius + 1e-3f);
    CHECK(motionTestPos(*sim, pawn).x > 32.5f);

    auto sim2 = motionTestSim();
    const Entity roller = motionTestPawn(*sim2, 30.5f, 30.5f);
    const Entity man = motionTestPawn(*sim2, 32.0f, 30.5f);
    motionTestControl(*sim2, roller);
    sim2->submit(DodgeRollCommand{roller, 1.0f, 0.0f});
    for (int i = 0; i < 40; ++i) {
        sim2->step();
        CHECK(motionTestPos(*sim2, roller).x < motionTestPos(*sim2, man).x);
    }
}

TEST_CASE("Motion: a staggering blow throws the body away from it, through the world, and it stays thrown") {
    auto sim = motionTestSim();
    BodyMotionSystem& motion = sim->combat().motion();
    const Entity pawn = motionTestPawn(*sim, 30.5f, 30.5f);
    const Entity deer = motionTestPawn(*sim, 40.5f, 40.5f, nullptr, "Deer");
    sim->stepMany(3);
    motionTestFace(*sim, pawn, -1.0f, 0.0f);  // facing the blow, which comes from the west
    const Position start = motionTestPos(*sim, pawn);
    sim->combat().stagger(pawn, HitFrom{true, start.x - 1.0f, start.y, true});  // a blow by hand
    MOTION_REQUIRE(motion.moving(pawn));
    // A person plays the stagger clip for a blow from the front: the one that goes back.
    CHECK(sim->things().registry().get<BodyMotion>(pawn).clip == motion.clipId("Motion_StaggerBack"));
    sim->stepMany(4);
    // Thrown back, it still faces the blow: it doesn't turn round to face its own push.
    CHECK(sim->things().registry().get<Posture>(pawn).facingX == doctest::Approx(-1.0f));
    motionTestFinish(*sim, pawn);
    CHECK(sim->things().registry().get<Posture>(pawn).facingX == doctest::Approx(-1.0f));
    CHECK(motionTestPos(*sim, pawn).x == doctest::Approx(start.x + 0.6f * motion.sizeScale(pawn)).epsilon(0.01));
    const float endX = motionTestPos(*sim, pawn).x;
    sim->stepMany(150);
    CHECK(motionTestPos(*sim, pawn).x == doctest::Approx(endX));  // no snapping back when the stagger ends

    // An animal has no clips: a plain push away from the blow.
    const Position deerStart = motionTestPos(*sim, deer);
    sim->combat().stagger(deer, HitFrom{true, deerStart.x, deerStart.y - 1.0f, true});  // a blow by hand
    MOTION_REQUIRE(motion.moving(deer));
    CHECK_FALSE(sim->things().registry().get<BodyMotion>(deer).clip.valid());
    sim->step();
    CHECK(motionTestPos(*sim, deer).y > deerStart.y);

    // Against a wall the stagger can't push the body into it.
    auto sim2 = motionTestSim();
    motionTestWall(*sim2, 31, 25, 31, 35);
    const Entity cornered = motionTestPawn(*sim2, 30.6f, 30.5f);
    sim2->stepMany(3);
    motionTestFace(*sim2, cornered, -1.0f, 0.0f);
    sim2->combat().stagger(cornered, HitFrom{true, 29.0f, 30.5f, true});
    motionTestFinish(*sim2, cornered);
    CHECK(motionTestPos(*sim2, cornered).x <= 31.0f - kPawnRadius + 1e-3f);

    // A bullet's stagger is a flinch: with no flinch clip measured it leaves the body where it stands.
    const Entity shot = motionTestPawn(*sim2, 40.5f, 30.5f);
    sim2->stepMany(3);
    const Position shotStart = motionTestPos(*sim2, shot);
    sim2->combat().stagger(shot, HitFrom{true, 20.0f, 30.5f});
    CHECK_FALSE(sim2->combat().motion().moving(shot));
    sim2->stepMany(30);
    CHECK(motionTestPos(*sim2, shot).x == doctest::Approx(shotStart.x));
}

TEST_CASE("Motion: a swing plays a short attack clip timed to the hit, steps the body in, and never a long flourish") {
    auto sim = motionTestSim();
    BodyMotionSystem& motion = sim->combat().motion();
    const Entity attacker = motionTestPawn(*sim, 30.5f, 30.5f, "Melee_Club");
    const Entity target = motionTestPawn(*sim, 31.7f, 30.5f);
    sim->stepMany(3);
    const DefId quick = motion.clipId("Motion_Club_Downward");
    const DefId flourish = motion.clipId("Motion_Club_Horizontal");
    MOTION_REQUIRE(quick.valid());
    MOTION_REQUIRE(flourish.valid());
    int swings = 0;
    for (int i = 0; i < 12; ++i) {
        // Put them back so every swing starts the same.
        Position& a = sim->things().registry().get<Position>(attacker);
        a.x = 30.5f;
        a.y = 30.5f;
        Position& t = sim->things().registry().get<Position>(target);
        t.x = 31.7f;
        t.y = 30.5f;
        sim->things().registry().replace<Health>(target, Health{});
        sim->health().refresh(target);
        motion.stop(target);
        sim->submit(MeleeCommand{attacker, target, MeleeMode::Attack});
        sim->step();
        if (!sim->melee().winding(attacker)) {
            continue;
        }
        ++swings;
        MOTION_REQUIRE(motion.moving(attacker));
        const BodyMotion& m = sim->things().registry().get<BodyMotion>(attacker);
        CHECK(m.clip == quick);  // the six second one is never picked
        CHECK(m.rate > 0.5f);
        const float startX = 30.5f;
        // Up to the hit: the clip's blow (0.5 s in) lands on the sim's hit step, and the body has stepped in by then.
        while (sim->melee().winding(attacker)) {
            sim->step();
        }
        if (motion.moving(attacker)) {
            CHECK(sim->things().registry().get<BodyMotion>(attacker).time == doctest::Approx(0.5f).epsilon(0.08));
        }
        CHECK(motionTestPos(*sim, attacker).x > startX + 0.15f);
        for (int s = 0; s < 200 && !sim->melee().ready(attacker); ++s) {
            sim->step();
        }
        CHECK_FALSE(motion.moving(attacker));  // the clip is over well inside the swing's recovery
    }
    CHECK(swings >= 6);
}

TEST_CASE("Motion: how close a fighter must be follows its height, its weapon and the target's size") {
    auto sim = motionTestSim();
    const Entity knife = motionTestPawn(*sim, 20.5f, 20.5f, "Melee_Knife");
    const Entity spear = motionTestPawn(*sim, 22.5f, 20.5f, "Melee_Spear");
    const Entity man = motionTestPawn(*sim, 30.5f, 20.5f);
    const Entity beast = motionTestPawn(*sim, 40.5f, 20.5f, nullptr, "Thrumbo");
    sim->stepMany(3);
    entt::registry& reg = sim->things().registry();
    reg.get_or_emplace<PawnLooks>(knife).height = 1.0f;
    reg.get_or_emplace<PawnLooks>(spear).height = 1.0f;
    const MeleeSystem& melee = sim->melee();
    // The weapon.
    CHECK(melee.engageDistance(spear, man) > melee.engageDistance(knife, man) + 0.5f);
    // The target's size: a thrumbo's flank is nearer than its centre.
    CHECK(melee.engageDistance(knife, beast) > melee.engageDistance(knife, man) + 0.2f);
    // The attacker's height: a tall fighter reaches further, a short one less.
    const float average = melee.reach(knife);
    reg.get<PawnLooks>(knife).height = 1.09f;
    const float tall = melee.reach(knife);
    reg.get<PawnLooks>(knife).height = 0.88f;
    const float small = melee.reach(knife);
    CHECK(tall > average);
    CHECK(small < average);
    CHECK(tall - small == doctest::Approx(average * 0.21f * melee.rules().reachHeightFollow).epsilon(0.01));
    // And inReach agrees: at the thrumbo's flank the knife reaches, at the same distance from a man's centre it doesn't.
    reg.get<PawnLooks>(knife).height = 1.0f;
    Position& p = reg.get<Position>(knife);
    const float d = melee.engageDistance(knife, beast);
    p.x = reg.get<Position>(beast).x - d;
    p.y = reg.get<Position>(beast).y;
    CHECK(melee.inReach(knife, beast));
    p.x = reg.get<Position>(man).x - d;
    p.y = reg.get<Position>(man).y;
    CHECK_FALSE(melee.inReach(knife, man));
}

TEST_CASE("Motion: a body in the middle of a clip survives a save and load, and the game goes on the same") {
    auto sim = motionTestSim();
    BodyMotionSystem& motion = sim->combat().motion();
    const Entity a = motionTestPawn(*sim, 20.5f, 30.5f);
    const Entity b = motionTestPawn(*sim, 20.5f, 34.5f);
    sim->stepMany(3);
    motionTestFace(*sim, a, 1.0f, 0.0f);
    MOTION_REQUIRE(motion.play(a, motion.clipId("MotionTest_Dash")));
    MOTION_REQUIRE(motion.push(b, 0.0f, 1.0f, 2.0f, 30));
    const uint64_t before = sim->stateHash();
    sim->stepMany(10);
    CHECK(sim->stateHash() != before);
    MOTION_REQUIRE(motion.moving(a));
    MOTION_REQUIRE(motion.moving(b));

    CHECK(saveToJson(*sim).find("\"motion\"") != std::string::npos);
    SaveLoadResult loaded = loadSim(saveSim(*sim), motionTestDefs());
    MOTION_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->stateHash() == sim->stateHash());
    CHECK(loaded.sim->combat().motion().moving(a));
    sim->stepMany(100);
    loaded.sim->stepMany(100);
    CHECK(loaded.sim->stateHash() == sim->stateHash());
    CHECK(motionTestPos(*loaded.sim, a).x == doctest::Approx(motionTestPos(*sim, a).x));
    CHECK(motionTestPos(*sim, b).y > 36.0f);
}

TEST_CASE("Motion: the snapshot names the clip and its time, and a pawn in a clip doesn't walk") {
    auto sim = motionTestSim();
    BodyMotionSystem& motion = sim->combat().motion();
    const Entity pawn = motionTestPawn(*sim, 20.5f, 30.5f);
    motionTestControl(*sim, pawn);
    motionTestFace(*sim, pawn, 1.0f, 0.0f);
    MOTION_REQUIRE(motion.play(pawn, motion.clipId("MotionTest_Dash")));
    sim->stepMany(6);
    Snapshot snap;
    sim->writeSnapshot(snap);
    bool found = false;
    for (const ThingView& v : snap.things) {
        if (v.id == pawn) {
            found = true;
            CHECK(v.motionClip == motion.clipId("MotionTest_Dash"));
            CHECK(v.motionTime == doctest::Approx(0.2f).epsilon(0.01));
            CHECK(v.motionRate == doctest::Approx(1.0f));
        }
    }
    CHECK(found);
    // The player's walking is ignored while the clip has the body.
    const float y = motionTestPos(*sim, pawn).y;
    sim->submit(DirectMoveCommand{pawn, motionTestPos(*sim, pawn).x, y + 0.1f});
    sim->step();
    CHECK(motionTestPos(*sim, pawn).y == doctest::Approx(y));
}

TEST_CASE("Motion: a blow at a body on the ground plays the weapon's downward clip, and only then") {
    auto sim = motionTestSim(11);
    BodyMotionSystem& motion = sim->combat().motion();
    const DefId slash = motion.clipId("Motion_OneHand_Slash1");
    const DefId stab = motion.clipId("Motion_Knife_Stab_Low");
    const DefId hack = motion.clipId("Motion_OneHand_Hack_Low");
    const DefId jab = motion.clipId("Motion_Unarmed_Jab");
    const DefId smash = motion.clipId("Motion_Club_Downward");
    MOTION_REQUIRE(slash.valid());
    MOTION_REQUIRE(stab.valid());
    MOTION_REQUIRE(hack.valid());
    MOTION_REQUIRE(jab.valid());
    const DefDatabase& defs = sim->defs();
    // What each weapon swings with: the knife's own stab (a weapon's list wins over its style's), the gladius its
    // one-hand style's hack, the club the overhead smash; fists have no downward clip measured here.
    CHECK(sim->melee().lowAttackClipsOf(defs.idOf("Things", "Melee_Knife")) == std::vector<DefId>{stab});
    CHECK(sim->melee().lowAttackClipsOf(defs.idOf("Things", "Melee_Gladius")) == std::vector<DefId>{hack});
    CHECK(sim->melee().lowAttackClipsOf(defs.idOf("Things", "Melee_Club")) == std::vector<DefId>{smash});
    CHECK(sim->melee().lowAttackClipsOf(defs.idOf("Things", "Human")).empty());
    // The downward clips are never among the usual swings.
    for (const DefId clip : sim->melee().attackClipsOf(defs.idOf("Things", "Melee_Knife"))) {
        CHECK(clip != stab);
        CHECK(clip != hack);
    }

    struct Case {
        const char* weapon;
        bool down;
        DefId wanted;
    };
    const Case cases[] = {{"Melee_Knife", false, slash}, {"Melee_Knife", true, stab}, {"Melee_Gladius", false, slash},
                          {"Melee_Gladius", true, hack},  {nullptr, false, jab},          {nullptr, true, jab}};
    float x = 10.5f;
    for (const Case& c : cases) {
        x += 6.0f;
        const Entity attacker = motionTestPawn(*sim, x, 30.5f, c.weapon);
        const Entity target = motionTestPawn(*sim, x + 1.0f, 30.5f);
        sim->stepMany(3);
        if (c.down) {
            sim->things().registry().get<Health>(target).downed = true;
            sim->things().registry().get<Posture>(target).stance = Stance::Prone;
        }
        sim->submit(MeleeCommand{attacker, target, MeleeMode::Attack});
        sim->step();
        CAPTURE(c.weapon == nullptr ? "fists" : c.weapon);
        CAPTURE(c.down);
        MOTION_REQUIRE(sim->melee().winding(attacker));
        MOTION_REQUIRE(motion.moving(attacker));
        CHECK(sim->things().registry().get<BodyMotion>(attacker).clip == c.wanted);
        Snapshot snap;
        sim->writeSnapshot(snap);
        const ThingView* view = snap.find(attacker);
        MOTION_REQUIRE(view != nullptr);
        CHECK(view->meleeLow == c.down);
        CHECK(view->motionClip == c.wanted);
        // Out of the way of the next pair.
        motion.stop(attacker);
        sim->things().destroy(attacker);
        sim->things().destroy(target);
    }
}

TEST_CASE("Motion: a spear thrusts with its own clip; a weapon without one swings its style's") {
    auto sim = motionTestSim(12);
    const BodyMotionSystem& motion = sim->combat().motion();
    const DefId thrust = motion.clipId("Motion_Spear_Stab");
    MOTION_REQUIRE(thrust.valid());
    const DefDatabase& defs = sim->defs();
    // The weapon's own list wins (sneed 2026-11-18: the spear must thrust, not slash); its unmeasured lunge is skipped.
    CHECK(sim->melee().attackClipsOf(defs.idOf("Things", "Melee_Spear")) == std::vector<DefId>{thrust});
    // The gladius has no list of its own: its one-hand style's slash.
    CHECK(sim->melee().attackClipsOf(defs.idOf("Things", "Melee_Gladius")) == std::vector<DefId>{motion.clipId("Motion_OneHand_Slash1")});
    // The thrust stays the spear's: the knife (another style) never gets it.
    for (const DefId clip : sim->melee().attackClipsOf(defs.idOf("Things", "Melee_Knife"))) {
        CHECK(clip != thrust);
    }
}

TEST_CASE("Motion: the player's own swing leaves the feet free: walking goes on, the clip doesn't carry the body") {
    // sneed (2026-11-18): "it resets my position if I try to move while meleeing ... it's critical I can move while
    // meleeing". The swing is the arms' and the chest's; the walking keys keep the body.
    auto sim = motionTestSim();
    BodyMotionSystem& motion = sim->combat().motion();
    const Entity player = motionTestPawn(*sim, 20.5f, 30.5f, "Melee_Club");
    const Entity target = motionTestPawn(*sim, 21.7f, 30.5f);
    motionTestControl(*sim, player);
    motionTestFace(*sim, player, 1.0f, 0.0f);
    sim->stepMany(2);
    sim->submit(MeleeCommand{player, target, MeleeMode::Attack});
    sim->step();
    MOTION_REQUIRE(sim->melee().winding(player));
    MOTION_REQUIRE(motion.moving(player));
    const BodyMotion& m = sim->things().registry().get<BodyMotion>(player);
    CHECK(m.upper);
    CHECK(m.freeFeet);
    CHECK_FALSE(motion.holdsBody(player));
    Snapshot snap;
    sim->writeSnapshot(snap);
    const ThingView* view = snap.find(player);
    MOTION_REQUIRE(view != nullptr);
    CHECK(view->motionUpper);
    CHECK(view->motionFreeFeet);
    // Walking sideways through the swing: every step's move is taken, and the clip adds nothing of its own.
    const Position start = motionTestPos(*sim, player);
    float y = start.y;
    for (int i = 0; i < 10 && motion.moving(player); ++i) {
        y -= 0.04f;
        sim->submit(DirectMoveCommand{player, start.x, y});
        sim->step();
        CHECK(motionTestPos(*sim, player).y == doctest::Approx(y));
        CHECK(motionTestPos(*sim, player).x == doctest::Approx(start.x));
    }
    // The swing still lands: its reach makes up for the step the clip no longer takes.
    for (int s = 0; s < 120 && sim->melee().winding(player); ++s) {
        sim->submit(DirectMoveCommand{player, start.x, y});
        sim->step();
    }
    CHECK(sim->melee().hits() >= 1);

    // A blow at someone on the ground (a stomp, an overhead smash) is still the whole body's.
    auto sim2 = motionTestSim();
    const Entity player2 = motionTestPawn(*sim2, 20.5f, 30.5f, "Melee_Club");
    const Entity lying = motionTestPawn(*sim2, 21.5f, 30.5f);
    motionTestControl(*sim2, player2);
    sim2->things().registry().get<Health>(lying).downed = true;
    sim2->things().registry().get<Posture>(lying).stance = Stance::Prone;
    sim2->submit(MeleeCommand{player2, lying, MeleeMode::Attack});
    sim2->step();
    MOTION_REQUIRE(sim2->combat().motion().moving(player2));
    CHECK_FALSE(sim2->things().registry().get<BodyMotion>(player2).freeFeet);
    CHECK(sim2->combat().motion().holdsBody(player2));
}

TEST_CASE("Motion: a stagger is drawn on the upper body and still carries the body; a knockdown is the whole body's") {
    // sneed (2026-11-18, third time): staggered people slid in the stagger clip. The clip now plays on the chest and
    // arms and the legs walk the stumble the sim carries the body through.
    auto sim = motionTestSim();
    BodyMotionSystem& motion = sim->combat().motion();
    const Entity pawn = motionTestPawn(*sim, 30.5f, 30.5f);
    sim->stepMany(3);
    motionTestFace(*sim, pawn, -1.0f, 0.0f);
    const Position start = motionTestPos(*sim, pawn);
    sim->combat().stagger(pawn, HitFrom{true, start.x - 1.0f, start.y, true});
    MOTION_REQUIRE(motion.moving(pawn));
    CHECK(sim->things().registry().get<BodyMotion>(pawn).upper);
    CHECK_FALSE(sim->things().registry().get<BodyMotion>(pawn).freeFeet);
    CHECK(motion.holdsBody(pawn));
    sim->stepMany(4);
    // The snapshot's speed is the stumble's, so the legs have something to walk.
    Snapshot snap;
    sim->writeSnapshot(snap);
    const ThingView* view = snap.find(pawn);
    MOTION_REQUIRE(view != nullptr);
    CHECK(view->motionUpper);
    CHECK(view->vx > 0.0f);
    motionTestFinish(*sim, pawn);
    CHECK(motionTestPos(*sim, pawn).x > start.x + 0.3f);

    const Entity other = motionTestPawn(*sim, 40.5f, 30.5f);
    sim->stepMany(3);
    motionTestFace(*sim, other, -1.0f, 0.0f);
    MOTION_REQUIRE(motion.throwBody(other, 1.0f, 0.0f, true));
    CHECK_FALSE(sim->things().registry().get<BodyMotion>(other).upper);
}

TEST_CASE("Motion: the player walks into people and stops, slides round them, and can always walk away") {
    // sneed (2026-11-18): "enemies should have physical collision so I can't run through them".
    auto sim = motionTestSim();
    BodyMotionSystem& motion = sim->combat().motion();
    const Entity player = motionTestPawn(*sim, 20.5f, 30.5f);
    const Entity man = motionTestPawn(*sim, 22.0f, 30.5f);
    motionTestControl(*sim, player);
    sim->things().registry().emplace<Stunned>(man).stepsLeft = 100000;  // stands where it is
    const float touching = motion.bodyRadius(player) + motion.bodyRadius(man);
    for (int i = 0; i < 40; ++i) {
        const Position& p = motionTestPos(*sim, player);
        sim->submit(DirectMoveCommand{player, p.x + 0.1f, p.y});
        sim->step();
        CHECK(motionTestDist(*sim, player, man) >= touching - 1e-4f);
    }
    CHECK(motionTestPos(*sim, player).x > 21.0f);  // it did walk up to him
    CHECK(motionTestPos(*sim, man).x == doctest::Approx(22.0f));  // and didn't shove him along
    // Walking diagonally into him it slides along (the move across him is kept).
    const float y0 = motionTestPos(*sim, player).y;
    for (int i = 0; i < 5; ++i) {
        const Position& p = motionTestPos(*sim, player);
        sim->submit(DirectMoveCommand{player, p.x + 0.1f, p.y + 0.1f});
        sim->step();
    }
    CHECK(motionTestPos(*sim, player).y > y0 + 0.2f);
    // Stuck inside someone (a body shoved into it), walking out is always allowed.
    Position& p = sim->things().registry().get<Position>(player);
    p.x = 21.8f;
    p.y = 30.5f;
    sim->submit(DirectMoveCommand{player, 21.6f, 30.5f});
    sim->step();
    CHECK(motionTestPos(*sim, player).x == doctest::Approx(21.6f));
    // Bodies on the ground are stepped over.
    sim->things().registry().get<Health>(man).downed = true;
    sim->health().refresh(man);
    sim->things().registry().get<Posture>(man).stance = Stance::Prone;
    CHECK(motion.bodiesClear(player, 21.6f, 30.5f, 22.0f, 30.5f, 0));
}
