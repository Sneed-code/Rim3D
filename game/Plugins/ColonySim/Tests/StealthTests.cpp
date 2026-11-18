#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Lights.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Stealth.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace colony;

// These tests check docs/design/stealth.md.

#define STEALTH_REQUIRE(...)   \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// The game's real data plus a plain test wall.
std::shared_ptr<const DefDatabase> stealthTestDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        namespace fs = std::filesystem;
        const fs::path root = fs::path(COLONY_DATA_DIR);
        std::vector<DefSource> sources;
        std::vector<fs::path> files;
        for (const auto& entry : fs::recursive_directory_iterator(root / "defs")) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
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
        sources.push_back(DefSource{"Things", "zz_stealth_test.json", R"([
            {"defName": "StealthTestWall", "category": "Building", "passability": "Impassable", "fillPercent": 1.0, "stats": {"maxHitPoints": 3000}}
        ])"});
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

// Noon by default: the sun is up, so light plays no part unless a test asks for the night.
std::unique_ptr<Sim> stealthTestSim(int64_t startSecond = 12 * kSecondsPerHour, int size = 128) {
    SimSettings settings;
    settings.seed = 5;
    settings.mapSize = size;
    settings.flatMap = true;
    settings.startSecond = startSecond;
    return std::make_unique<Sim>(settings, stealthTestDefs());
}

// A guard of another side at its post: unaware, unarmed, facing (fx, fy).
Entity stealthTestGuard(Sim& sim, int x, int y, float fx, float fy, const char* gun = nullptr) {
    const Entity pawn = sim.things().spawn("Human", CellPos{x, y, 0});
    if (gun != nullptr) {
        sim.combat().equip(pawn, sim.things().make(sim.defs().idOf("Things", gun)));
    }
    sim.submit(SetFactionCommand{pawn, 2});
    sim.submit(SetAlertCommand{pawn, 0});
    sim.step();
    Posture& posture = sim.things().registry().get<Posture>(pawn);
    posture.facingX = fx;
    posture.facingY = fy;
    return pawn;
}

// The player's own body: it stands where it is put until the test moves it.
Entity stealthTestIntruder(Sim& sim, int x, int y) {
    const Entity pawn = sim.things().spawn("Human", CellPos{x, y, 0});
    sim.things().registry().emplace<Colonist>(pawn);
    sim.jobs().addWorker(pawn);
    sim.submit(ControlPawnCommand{pawn});
    sim.step();
    return pawn;
}

const Fighter& stealthTestFighter(Sim& sim, Entity pawn) {
    return sim.things().registry().get<Fighter>(pawn);
}

bool stealthTestKnows(Sim& sim, Entity pawn, Entity enemy) {
    const Fighter& f = stealthTestFighter(sim, pawn);
    return std::any_of(f.known.begin(), f.known.end(), [&](const KnownEnemy& k) { return k.pawn == enemy; });
}

// Keeps the guard looking the way the test set: an unarmed guard that knows nobody stands still, but be sure.
void stealthTestFace(Sim& sim, Entity pawn, float fx, float fy) {
    Posture& posture = sim.things().registry().get<Posture>(pawn);
    posture.facingX = fx;
    posture.facingY = fy;
}

// Walks the controlled pawn towards (x, y) at `speed` metres a step for `steps` steps.
void stealthTestWalk(Sim& sim, Entity pawn, float x, float y, float speed, int steps) {
    for (int i = 0; i < steps; ++i) {
        const Position& pos = sim.things().registry().get<Position>(pawn);
        const float dx = x - pos.x;
        const float dy = y - pos.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 0.01f) {
            sim.step();
            continue;
        }
        const float go = std::min(speed, len);
        sim.submit(DirectMoveCommand{pawn, pos.x + dx / len * go, pos.y + dy / len * go});
        sim.step();
    }
}

float stealthTestDist(Sim& sim, Entity a, Entity b) {
    const Position& pa = sim.things().registry().get<Position>(a);
    const Position& pb = sim.things().registry().get<Position>(b);
    return std::sqrt((pa.x - pb.x) * (pa.x - pb.x) + (pa.y - pb.y) * (pa.y - pb.y));
}

}  // namespace

TEST_CASE("Stealth: the cone, the light and the footsteps as plain numbers") {
    StealthRules r;
    // 120 degrees: 60 to either side of the facing.
    CHECK(stealthInCone(1, 0, 10, 0, 120));
    CHECK(stealthInCone(1, 0, 10, 15, 120));   // 56 degrees off
    CHECK(!stealthInCone(1, 0, 10, 20, 120));  // 63 degrees off
    CHECK(!stealthInCone(1, 0, -10, 0, 120));
    CHECK(stealthInCone(1, 0, -10, 0, 360));
    CHECK(stealthInCone(1, 0, 0, 0, 120));
    // Light and stance, for a watcher that isn't alerted; none of it for one that is.
    CHECK(stealthSightShare(r, 1.0f, Stance::Standing, false) == doctest::Approx(1.0f));
    CHECK(stealthSightShare(r, 0.0f, Stance::Standing, false) == doctest::Approx(r.darkSight));
    CHECK(stealthSightShare(r, r.fullLight / 2, Stance::Standing, false) == doctest::Approx((1.0f + r.darkSight) / 2));
    CHECK(stealthSightShare(r, 1.0f, Stance::Crouching, false) == doctest::Approx(r.crouchSight));
    CHECK(stealthSightShare(r, 0.0f, Stance::Crouching, false) == doctest::Approx(r.darkSight * r.crouchSight));
    CHECK(stealthSightShare(r, 0.0f, Stance::Crouching, true) == doctest::Approx(1.0f));
    // Footsteps: still and crouched are silent, a walk carries 3 m, a run 7.
    CHECK(stealthStepNoise(r, 0.0f, Stance::Standing) == 0.0f);
    CHECK(stealthStepNoise(r, 0.1f, Stance::Standing) == doctest::Approx(r.walkNoise));
    CHECK(stealthStepNoise(r, 0.25f, Stance::Standing) == doctest::Approx(r.runNoise));
    CHECK(stealthStepNoise(r, 0.1f, Stance::Crouching) == doctest::Approx(r.crouchNoise));
}

TEST_CASE("Stealth: the rules def is read, with sneed's 20 m call and 8 m melee noise") {
    auto sim = stealthTestSim();
    const StealthRules& r = sim->fighters().stealth();
    CHECK(sim->defs().find("Rules", "StealthRules") != nullptr);
    CHECK(r.calloutRadius == doctest::Approx(20.0f));
    CHECK(r.meleeNoise == doctest::Approx(8.0f));
    CHECK(!r.calledCanCall);
    CHECK(r.coneDegrees == doctest::Approx(120.0f));
}

TEST_CASE("Stealth: an unaware guard doesn't see what is behind it; in front and near it is alerted at once") {
    auto sim = stealthTestSim();
    const Entity guard = stealthTestGuard(*sim, 60, 60, 1, 0);
    STEALTH_REQUIRE(stealthTestFighter(*sim, guard).alert == Alert::Unaware);
    const Entity behind = stealthTestIntruder(*sim, 50, 60);  // 10 m behind its back, in the open, at noon
    for (int i = 0; i < 90; ++i) {
        stealthTestFace(*sim, guard, 1, 0);
        sim->step();
    }
    CHECK(stealthTestFighter(*sim, guard).alert == Alert::Unaware);
    CHECK(!stealthTestKnows(*sim, guard, behind));
    CHECK(stealthTestFighter(*sim, guard).target == kNoEntity);

    // The same guard turned round sees it 10 m off... far enough to be only suspicious at first, then sure.
    stealthTestFace(*sim, guard, -1, 0);
    bool suspicious = false;
    bool alerted = false;
    int alertEvents = 0;
    for (int i = 0; i < 90 && !alerted; ++i) {
        sim->step();
        for (const Event& e : sim->events()) {
            if (e.type == EventType::PawnAction && e.action == PawnAction::Alert && e.thing == guard) {
                ++alertEvents;
            }
        }
        const Alert a = stealthTestFighter(*sim, guard).alert;
        suspicious = suspicious || a == Alert::Suspicious;
        alerted = a == Alert::Alerted;
        if (a == Alert::Suspicious) {
            CHECK(!stealthTestKnows(*sim, guard, behind));  // not sure yet: it doesn't act
        }
    }
    CHECK(suspicious);
    CHECK(alerted);
    CHECK(alertEvents == 2);
    CHECK(stealthTestKnows(*sim, guard, behind));
}

TEST_CASE("Stealth: an enemy within 8 m in front alerts at once, and the guard is caught off guard") {
    auto sim = stealthTestSim();
    const Entity guard = stealthTestGuard(*sim, 60, 60, 1, 0);
    const Entity intruder = stealthTestIntruder(*sim, 66, 60);
    bool wasSuspicious = false;
    for (int i = 0; i < 20; ++i) {
        sim->step();
        wasSuspicious = wasSuspicious || stealthTestFighter(*sim, guard).alert == Alert::Suspicious;
    }
    const Fighter& f = stealthTestFighter(*sim, guard);
    CHECK(!wasSuspicious);
    CHECK(f.alert == Alert::Alerted);
    CHECK(stealthTestKnows(*sim, guard, intruder));
    CHECK(!f.called);
    bool inSnapshot = false;
    Snapshot snapshot;
    sim->writeSnapshot(snapshot);
    for (const ThingView& v : snapshot.things) {
        if (v.id == guard) {
            inSnapshot = v.alert == 2;
        }
    }
    CHECK(inSnapshot);
}

TEST_CASE("Stealth: a crouched intruder is seen from less far by a guard that isn't alerted") {
    // 45 m in front: standing it is seen (60 m), crouched not (60 x 0.6 = 36 m).
    for (const bool crouched : {true, false}) {
        auto sim = stealthTestSim();
        const Entity guard = stealthTestGuard(*sim, 30, 60, 1, 0);
        const Entity intruder = stealthTestIntruder(*sim, 75, 60);
        if (crouched) {
            sim->submit(SetStanceCommand{intruder, Stance::Crouching});
        }
        for (int i = 0; i < 120; ++i) {
            stealthTestFace(*sim, guard, 1, 0);
            sim->step();
        }
        CAPTURE(crouched);
        CHECK((stealthTestFighter(*sim, guard).alert == Alert::Unaware) == crouched);
        CHECK(stealthTestKnows(*sim, guard, intruder) == !crouched);
    }
}

TEST_CASE("Stealth: at night a guard that isn't alerted sees less far; an alerted one as far as by day") {
    // Midnight, in the open, no lamps. 30 m in front: by day seen, by night not (60 m x the dark share).
    auto night = stealthTestSim(0);
    const float light = night->lights().lightAt(CellPos{60, 60, 0});
    const StealthRules& r = night->fighters().stealth();
    const float reach = night->fighters().rules().sightRange * stealthSightShare(r, light, Stance::Standing, false);
    CAPTURE(light);
    CAPTURE(reach);
    STEALTH_REQUIRE(reach < 28.0f);
    const Entity guard = stealthTestGuard(*night, 30, 60, 1, 0);
    const Entity intruder = stealthTestIntruder(*night, 60, 60);
    for (int i = 0; i < 120; ++i) {
        stealthTestFace(*night, guard, 1, 0);
        night->step();
    }
    CHECK(stealthTestFighter(*night, guard).alert == Alert::Unaware);
    // Alerted (told to be, here), it looks as far as ever.
    night->submit(SetAlertCommand{guard, 2});
    night->stepMany(20);
    CHECK(stealthTestKnows(*night, guard, intruder));
}

TEST_CASE("Stealth: footsteps behind a guard make it turn; a crouched walk gets close unheard") {
    for (const bool crouched : {false, true}) {
        auto sim = stealthTestSim();
        const Entity guard = stealthTestGuard(*sim, 60, 60, 1, 0);
        const Entity intruder = stealthTestIntruder(*sim, 50, 60);
        if (crouched) {
            sim->submit(SetStanceCommand{intruder, Stance::Crouching});
        }
        // Walk up behind it at 0.1 m a step (3 m a real second) to 1.6 m from it (it looks and listens twice a second).
        float heardAt = -1.0f;
        for (int i = 0; i < 400 && heardAt < 0.0f && stealthTestDist(*sim, guard, intruder) > 1.6f; ++i) {
            stealthTestWalk(*sim, intruder, 60.5f, 60.5f, 0.1f, 1);
            if (stealthTestFighter(*sim, guard).alert != Alert::Unaware) {
                heardAt = stealthTestDist(*sim, guard, intruder);
            }
        }
        CAPTURE(crouched);
        CAPTURE(heardAt);
        const float endDist = stealthTestDist(*sim, guard, intruder);
        CAPTURE(endDist);
        CHECK(endDist < 3.2f);  // it did walk up
        if (crouched) {
            CHECK(heardAt < 0.0f);
            CHECK(stealthTestFighter(*sim, guard).alert == Alert::Unaware);
        } else {
            CHECK(heardAt > 1.5f);
            CHECK(heardAt <= sim->fighters().stealth().walkNoise + 0.2f);
            // It turned to look, and with the intruder now in front of it and near, the next look alerts it.
            sim->stepMany(20);
            CHECK(stealthTestFighter(*sim, guard).alert == Alert::Alerted);
            CHECK(stealthTestKnows(*sim, guard, intruder));
        }
    }
}

TEST_CASE("Stealth: the one who spots calls friends within 20 m; those it called call nobody") {
    auto sim = stealthTestSim();
    // A faces the intruder 6 m away. B is 15 m from A, C 30 m from A (15 m from B); B and C look away, and walls
    // stand between them and the intruder, so neither can see it for itself.
    const Entity a = stealthTestGuard(*sim, 60, 60, 1, 0);
    const Entity b = stealthTestGuard(*sim, 60, 75, 0, 1);
    const Entity c = stealthTestGuard(*sim, 60, 90, 0, 1);
    for (int x = 55; x <= 75; ++x) {
        sim->things().spawn("StealthTestWall", CellPos{x, 67, 0});
    }
    const Entity intruder = stealthTestIntruder(*sim, 66, 60);
    for (int i = 0; i < 60; ++i) {
        sim->step();
    }
    CHECK(stealthTestFighter(*sim, a).alert == Alert::Alerted);
    CHECK(stealthTestFighter(*sim, b).alert == Alert::Alerted);
    CHECK(stealthTestFighter(*sim, b).called);
    CHECK(stealthTestKnows(*sim, b, intruder));
    CHECK(stealthTestFighter(*sim, c).alert == Alert::Unaware);
    CHECK(!stealthTestKnows(*sim, c, intruder));
    const Fighter& fb = stealthTestFighter(*sim, b);
    for (const KnownEnemy& k : fb.known) {
        CHECK(!k.firsthand);
    }
}

TEST_CASE("Stealth: a gunshot is heard far off and gives the shooter away; a bow only close by; a blow 8 m") {
    auto sim = stealthTestSim();
    // Guards with their backs to the noise, at 6, 12, 50 and 100 m from it.
    const Entity g6 = stealthTestGuard(*sim, 26, 60, 1, 0);
    const Entity g12 = stealthTestGuard(*sim, 32, 60, 1, 0);
    const Entity g50 = stealthTestGuard(*sim, 70, 60, 1, 0);
    const Entity g100 = stealthTestGuard(*sim, 120, 60, 1, 0);
    const Entity intruder = stealthTestIntruder(*sim, 20, 60);
    const StealthRules& r = sim->fighters().stealth();
    const auto reset = [&] {
        for (const Entity g : {g6, g12, g50, g100}) {
            sim->submit(SetAlertCommand{g, 0});
        }
        sim->step();
        for (const Entity g : {g6, g12, g50, g100}) {
            stealthTestFace(*sim, g, 1, 0);
        }
    };
    const auto alerted = [&](Entity g) { return stealthTestFighter(*sim, g).alert == Alert::Alerted; };

    // A melee blow: 8 m.
    sim->fighters().noise(20.5f, 60.5f, 0, r.meleeNoise, intruder, true, "heard a blow");
    CHECK(alerted(g6));
    CHECK(stealthTestKnows(*sim, g6, intruder));
    CHECK(!alerted(g12));
    CHECK(!alerted(g50));

    // A real shot with an unsilenced pistol, fired away from them: heard at 50 m, not at 100.
    reset();
    STEALTH_REQUIRE(!alerted(g6));
    sim->combat().equip(intruder, sim->things().make(sim->defs().idOf("Things", "Gun_Autopistol")));
    ShootCommand shot;
    shot.pawn = intruder;
    shot.x = 5.5f;
    shot.y = 60.5f;
    shot.z = 1.0f;
    sim->submit(shot);
    bool fired = false;
    for (int i = 0; i < 60 && !fired; ++i) {
        sim->step();
        for (const Event& e : sim->events()) {
            fired = fired || e.type == EventType::ShotFired;
        }
    }
    STEALTH_REQUIRE(fired);
    CHECK(alerted(g6));
    CHECK(alerted(g12));
    CHECK(alerted(g50));
    CHECK(stealthTestKnows(*sim, g50, intruder));
    CHECK(!alerted(g100));

    // A bow's def is quiet: 15 m.
    const DefId bow = sim->defs().idOf("Things", "Gun_RecurveBow");
    CHECK(sim->defs().get(bow).number("noiseRadius", 0.0) == doctest::Approx(15.0));
}

TEST_CASE("Stealth: a mission site that finds an intruder stays alerted for good; elsewhere guards calm down") {
    for (const bool mission : {true, false}) {
        auto sim = stealthTestSim();
        sim->fighters().setStaysAlerted(mission);
        const Entity a = stealthTestGuard(*sim, 60, 60, 1, 0);
        const Entity far = stealthTestGuard(*sim, 60, 124, 0, 1);  // 64 m away: no call reaches it, and too far to see
        const Entity intruder = stealthTestIntruder(*sim, 66, 60);
        sim->stepMany(20);
        CAPTURE(mission);
        STEALTH_REQUIRE(stealthTestFighter(*sim, a).alert == Alert::Alerted);
        CHECK((stealthTestFighter(*sim, far).alert == Alert::Alerted) == mission);
        CHECK(stealthTestFighter(*sim, far).onGuard == mission);
        CHECK(!stealthTestKnows(*sim, far, intruder));  // it knows there is a threat, not where
        // The intruder is gone (hidden for good): half a game hour later...
        sim->things().destroy(intruder);
        const StealthRules& r = sim->fighters().stealth();
        bool wasSuspicious = false;
        for (int i = 0; i < r.searchSeconds + r.suspiciousSeconds + 700; ++i) {
            sim->step();
            wasSuspicious = wasSuspicious || stealthTestFighter(*sim, a).alert == Alert::Suspicious;
        }
        if (mission) {
            CHECK(stealthTestFighter(*sim, a).alert == Alert::Alerted);
            CHECK(stealthTestFighter(*sim, far).alert == Alert::Alerted);
        } else {
            CHECK(wasSuspicious);
            CHECK(stealthTestFighter(*sim, a).alert == Alert::Unaware);
        }
    }
}

TEST_CASE("Stealth: fighters sent to fight (drafted) stay alerted, and a hit alerts and names the attacker") {
    auto sim = stealthTestSim();
    const Entity colonist = sim->things().spawn("Human", CellPos{40, 40, 0});
    sim->submit(DraftCommand{colonist, true});
    sim->stepMany(sim->fighters().stealth().searchSeconds + 200);
    CHECK(stealthTestFighter(*sim, colonist).alert == Alert::Alerted);

    const Entity guard = stealthTestGuard(*sim, 80, 80, 1, 0);
    const Entity sniper = stealthTestIntruder(*sim, 40, 80);  // 40 m behind it
    STEALTH_REQUIRE(stealthTestFighter(*sim, guard).alert == Alert::Unaware);
    sim->fighters().struck(guard, sniper);
    CHECK(stealthTestFighter(*sim, guard).alert == Alert::Alerted);
    CHECK(stealthTestKnows(*sim, guard, sniper));
}

TEST_CASE("Stealth: SetAlert fails for a pawn that isn't a fighter and for a bad state") {
    auto sim = stealthTestSim();
    const Entity pawn = sim->things().spawn("Human", CellPos{40, 40, 0});
    sim->submit(SetAlertCommand{pawn, 0});
    sim->step();
    CHECK(std::any_of(sim->events().begin(), sim->events().end(), [](const Event& e) { return e.type == EventType::CommandFailed; }));
    const Entity guard = stealthTestGuard(*sim, 60, 60, 1, 0);
    sim->submit(SetAlertCommand{guard, 3});
    sim->step();
    CHECK(std::any_of(sim->events().begin(), sim->events().end(), [](const Event& e) { return e.type == EventType::CommandFailed; }));
    CHECK(stealthTestFighter(*sim, guard).alert == Alert::Unaware);
}

TEST_CASE("Stealth: a suspicious guard is the same after a save and load") {
    auto sim = stealthTestSim();
    const Entity guard = stealthTestGuard(*sim, 60, 60, 1, 0);
    const Entity b = stealthTestGuard(*sim, 60, 70, 1, 0);
    stealthTestIntruder(*sim, 85, 60);  // 25 m in front: suspicious first
    bool suspicious = false;
    for (int i = 0; i < 40 && !suspicious; ++i) {
        sim->step();
        suspicious = stealthTestFighter(*sim, guard).alert == Alert::Suspicious;
    }
    STEALTH_REQUIRE(suspicious);
    SaveLoadResult loaded = loadSim(saveSim(*sim), stealthTestDefs());
    STEALTH_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->stateHash() == sim->stateHash());
    const Fighter& f = loaded.sim->things().registry().get<Fighter>(guard);
    CHECK(f.alert == Alert::Suspicious);
    CHECK(f.suspect == stealthTestFighter(*sim, guard).suspect);
    for (int i = 0; i < 200; ++i) {
        sim->step();
        loaded.sim->step();
    }
    CHECK(loaded.sim->stateHash() == sim->stateHash());
    CHECK(stealthTestFighter(*sim, guard).alert == Alert::Alerted);
    CHECK(stealthTestFighter(*sim, b).alert == Alert::Alerted);
}

// The combat range's guards (SmokeTestGunRange.cpp, U): a target on the player's side is drafted and walked 3 m away from
// the player, then put on another side and set unaware, with the same commands the range sends.
TEST_CASE("Stealth: a range guard walked off and set unaware keeps its back turned until it sees or hears") {
    auto sim = stealthTestSim();
    const Entity player = stealthTestIntruder(*sim, 50, 64);
    const Entity guard = sim->things().spawn("Human", CellPos{62, 64, 0});
    sim->submit(SetFactionCommand{guard, 1});
    sim->step();
    sim->submit(DraftCommand{guard, true});
    sim->submit(MoveOrderCommand{guard, CellPos{65, 64, 0}});
    for (int i = 0; i < 180; ++i) {
        sim->step();
    }
    const Position at = sim->things().registry().get<Position>(guard);
    CHECK(at.x > 64.5f);
    sim->submit(SetFactionCommand{guard, 2});
    sim->submit(SetAlertCommand{guard, 0});
    sim->submit(SetFightResponseCommand{guard, FightResponse::Fists});
    sim->step();
    STEALTH_REQUIRE(sim->things().registry().all_of<Fighter>(guard));
    CHECK(stealthTestFighter(*sim, guard).alert == Alert::Unaware);
    CHECK(sim->things().registry().get<Posture>(guard).facingX > 0.9f);
    // The player stands 15 m behind it for 20 real seconds: nothing.
    for (int i = 0; i < 600; ++i) {
        sim->step();
    }
    CHECK(stealthTestFighter(*sim, guard).alert == Alert::Unaware);
    CHECK(sim->things().registry().get<Posture>(guard).facingX > 0.9f);
    CHECK(stealthTestDist(*sim, guard, player) > 14.0f);
    const Position still = sim->things().registry().get<Position>(guard);
    CHECK(std::abs(still.x - at.x) < 0.5f);
    // Walking up behind it is heard: it turns, sees and is alerted.
    for (int i = 0; i < 400 && stealthTestFighter(*sim, guard).alert != Alert::Alerted; ++i) {
        stealthTestWalk(*sim, player, still.x, still.y, 0.1f, 1);
    }
    CHECK(stealthTestFighter(*sim, guard).alert == Alert::Alerted);
    CHECK(stealthTestKnows(*sim, guard, player));
    // Unarmed, with the Fists response the range gives her, she comes at the player and swings.
    for (int i = 0; i < 200; ++i) {
        stealthTestWalk(*sim, player, 50.5f, 64.5f, 0.2f, 1);  // the player backs off 15 m again
    }
    const float apart = stealthTestDist(*sim, guard, player);
    int swings = 0;
    for (int i = 0; i < 900; ++i) {
        sim->step();
        for (const Event& e : sim->events()) {
            swings += e.type == EventType::MeleeSwing && e.thing == guard ? 1 : 0;
        }
    }
    CHECK(stealthTestDist(*sim, guard, player) < 3.0f);
    // (No "closer than before": since fighters sprint in a fight she keeps up with the backing player, and is at
    // arm's length already when the count starts.)
    MESSAGE("apart before " << apart << " m, after " << stealthTestDist(*sim, guard, player) << " m");
    CHECK(swings > 0);
}
