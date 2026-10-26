#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace colony;

#define FIGHT_REQUIRE(...)     \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// The game's real data plus a test wall and sandbags (C4 has walls, sandbags aren't written yet).
// These tests check docs/design/fighters.md.
std::shared_ptr<const DefDatabase> fightTestDefs() {
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
        sources.push_back(DefSource{"Things", "zz_fight_test.json", R"([
            {"defName": "FightTestWall", "category": "Building", "passability": "Impassable", "fillPercent": 1.0, "stats": {"maxHitPoints": 3000}},
            {"defName": "FightTestSandbags", "category": "Building", "passability": "PassThroughOnly", "fillPercent": 0.55, "stats": {"maxHitPoints": 3000}}
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

std::unique_ptr<Sim> fightTestSim(uint64_t seed = 5, int size = 96) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = size;
    settings.flatMap = true;
    return std::make_unique<Sim>(settings, fightTestDefs());
}

Entity fightTestPawn(Sim& sim, int x, int y, const char* gun = nullptr) {
    const Entity pawn = sim.things().spawn("Human", CellPos{x, y, 0});
    if (gun != nullptr) {
        sim.combat().equip(pawn, sim.things().make(sim.defs().idOf("Things", gun)));
    }
    return pawn;
}

void fightTestBuild(Sim& sim, const char* def, int x0, int y0, int x1, int y1) {
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            sim.things().spawn(def, CellPos{x, y, 0});
        }
    }
}

// Commands applied at the next step, which the test takes.
void fightTestDraft(Sim& sim, Entity pawn) {
    sim.submit(DraftCommand{pawn, true});
}
void fightTestHostile(Sim& sim, Entity pawn, uint32_t faction = 2) {
    sim.submit(SetFactionCommand{pawn, faction});
}

const Fighter& fightTestFighter(Sim& sim, Entity pawn) {
    return sim.things().registry().get<Fighter>(pawn);
}

CellPos fightTestCell(Sim& sim, Entity pawn) {
    return cellOf(sim.things().registry().get<Position>(pawn));
}

bool fightTestFailed(const Sim& sim) {
    return std::any_of(sim.events().begin(), sim.events().end(),
                       [](const Event& e) { return e.type == EventType::CommandFailed; });
}

// Keeps a pawn alive and unhurt, so a test about positions isn't ended by a lucky shot.
void fightTestHeal(Sim& sim, Entity pawn) {
    sim.things().registry().replace<Health>(pawn, Health{});
    sim.health().refresh(pawn);
}

}  // namespace

TEST_CASE("Fighters: drafting takes a colonist off work, undrafting gives it back") {
    auto sim = fightTestSim();
    sim->submit(SpawnThingCommand{"Human", CellPos{30, 30, 0}});
    sim->step();
    Entity pawn = kNoEntity;
    for (const Entity e : sim->things().registry().view<Worker>()) {
        pawn = e;
    }
    FIGHT_REQUIRE(pawn != kNoEntity);
    sim->stepMany(5);
    CHECK(sim->things().registry().get<Worker>(pawn).job.type != JobType::None);  // idling

    fightTestDraft(*sim, pawn);
    sim->step();
    CHECK_FALSE(fightTestFailed(*sim));
    FIGHT_REQUIRE(sim->things().registry().all_of<Fighter>(pawn));
    CHECK(sim->things().registry().get<Worker>(pawn).job.type == JobType::None);
    sim->stepMany(80);
    CHECK(sim->things().registry().get<Worker>(pawn).job.type == JobType::None);  // the job system leaves it alone
    Snapshot snap;
    sim->writeSnapshot(snap);
    CHECK(snap.find(pawn)->drafted);
    CHECK(snap.find(pawn)->faction == kPlayerFaction);

    sim->submit(DraftCommand{pawn, true});  // twice: fails
    sim->step();
    CHECK(fightTestFailed(*sim));
    sim->submit(DraftCommand{pawn, false});
    sim->step();
    CHECK_FALSE(fightTestFailed(*sim));
    CHECK_FALSE(sim->things().registry().all_of<Fighter>(pawn));
    sim->stepMany(3);
    CHECK(sim->things().registry().get<Worker>(pawn).job.type != JobType::None);  // back to idling

    // Another faction's pawn can't be drafted, and fights by itself.
    const Entity raider = fightTestPawn(*sim, 60, 60, "Gun_Revolver");
    fightTestHostile(*sim, raider);
    sim->step();
    CHECK(sim->fighters().factionOf(raider) == 2);
    CHECK(sim->things().registry().all_of<Fighter>(raider));
    CHECK_FALSE(fightTestFighter(*sim, raider).drafted);
    sim->submit(DraftCommand{raider, true});
    sim->step();
    CHECK(fightTestFailed(*sim));
    sim->submit(SetFactionCommand{raider, 0});
    sim->step();
    CHECK(fightTestFailed(*sim));
    sim->submit(MoveOrderCommand{pawn, CellPos{40, 40, 0}});  // not drafted
    sim->step();
    CHECK(fightTestFailed(*sim));
}

TEST_CASE("Fighters: a drafted rifleman takes the sandbags nearby, crouches and shoots") {
    auto sim = fightTestSim();
    // Sandbags 5 m north of the rifleman; the enemy 30 m north in the open.
    fightTestBuild(*sim, "FightTestSandbags", 36, 45, 44, 45);
    const Entity rifleman = fightTestPawn(*sim, 40, 40, "Gun_BoltActionRifle");
    const Entity enemy = fightTestPawn(*sim, 41, 72, "Gun_Revolver");
    fightTestDraft(*sim, rifleman);
    fightTestHostile(*sim, enemy);
    int shots = 0;
    int crouchedSteps = 0;
    int stillSteps = 0;
    for (int i = 0; i < 400; ++i) {
        fightTestHeal(*sim, rifleman);
        fightTestHeal(*sim, enemy);
        sim->step();
        for (const Event& e : sim->events()) {
            shots += e.type == EventType::ShotFired && e.thing == rifleman ? 1 : 0;
        }
        const Fighter& f = fightTestFighter(*sim, rifleman);
        if (i > 100 && f.walk == 0 && fightTestCell(*sim, rifleman) == f.spot) {
            ++stillSteps;
            crouchedSteps += sim->things().registry().get<Posture>(rifleman).stance == Stance::Crouching ? 1 : 0;
        }
    }
    const Fighter& f = fightTestFighter(*sim, rifleman);
    CHECK(f.target == enemy);
    CHECK(f.hasSpot);
    CHECK(f.spot.y == 44);  // right behind the sandbags
    CHECK(f.spot.x >= 36);
    CHECK(f.spot.x <= 44);
    CHECK(f.fireStance == Stance::Standing);  // pops up over them to shoot
    CHECK(shots >= 3);
    CHECK(stillSteps > 0);
    CHECK(crouchedSteps * 3 > stillSteps);  // crouched while the rifle cools down, up while it aims
    MESSAGE("rifleman fired " << shots << " shots; crouched " << crouchedSteps << " of " << stillSteps << " steps in place");
}

TEST_CASE("Fighters: behind a wall a fighter steps out past its end to fire, then back") {
    auto sim = fightTestSim();
    // A wall 5 m long across the line to the enemy. The only cover is behind it, and the only
    // shot from there is from the cell past either end.
    fightTestBuild(*sim, "FightTestWall", 38, 45, 42, 45);
    const Entity fighter = fightTestPawn(*sim, 45, 41, "Gun_AssaultRifle");  // sees the enemy past the wall's end
    const Entity enemy = fightTestPawn(*sim, 40, 66, "Gun_Revolver");
    fightTestDraft(*sim, fighter);
    fightTestHostile(*sim, enemy);
    int shotsOut = 0;
    int shotsElsewhere = 0;
    int backInCover = 0;
    for (int i = 0; i < 600; ++i) {
        fightTestHeal(*sim, fighter);
        fightTestHeal(*sim, enemy);
        sim->step();
        const Fighter& f = fightTestFighter(*sim, fighter);
        const CellPos at = fightTestCell(*sim, fighter);
        for (const Event& e : sim->events()) {
            if (e.type == EventType::ShotFired && e.thing == fighter) {
                (at.y == 44 && (at.x == 37 || at.x == 43) ? shotsOut : shotsElsewhere) += 1;
            }
        }
        if (i > 200 && f.walk == 0 && at == f.spot && f.spot != f.fireCell) {
            ++backInCover;
        }
    }
    const Fighter& f = fightTestFighter(*sim, fighter);
    CHECK(f.spot.y == 44);
    CHECK((f.spot.x == 38 || f.spot.x == 42));
    CHECK(f.fireCell.y == 44);
    CHECK((f.fireCell.x == 37 || f.fireCell.x == 43));
    CHECK(shotsOut >= 3);
    CHECK(shotsElsewhere == 0);
    CHECK(backInCover > 0);
    MESSAGE(shotsOut << " shots from the corner, " << backInCover << " steps back in cover");
}

TEST_CASE("Fighters: the closest able enemy is the target, the downed only when none is left") {
    auto sim = fightTestSim();
    const Entity fighter = fightTestPawn(*sim, 40, 40, "Gun_AssaultRifle");
    const Entity nearDowned = fightTestPawn(*sim, 40, 50, "Gun_Revolver");
    const Entity farAble = fightTestPawn(*sim, 48, 60, "Gun_Revolver");
    const Entity farther = fightTestPawn(*sim, 30, 66, "Gun_Revolver");
    fightTestDraft(*sim, fighter);
    for (const Entity e : {nearDowned, farAble, farther}) {
        fightTestHostile(*sim, e);
    }
    // Beat it until it goes down (and stays down: nobody tends it).
    sim->step();
    for (int i = 0; i < 200 && !sim->health().incapacitated(nearDowned); ++i) {
        sim->submit(DamagePawnCommand{nearDowned, "Blunt", 4.0f, -1});
        sim->step();
    }
    FIGHT_REQUIRE(!sim->things().registry().get<Health>(nearDowned).dead);
    FIGHT_REQUIRE(sim->health().incapacitated(nearDowned));
    FIGHT_REQUIRE(!sim->health().incapacitated(farAble));
    // Keep the fighter where it is, so the test is about choosing, not walking.
    sim->submit(MoveOrderCommand{fighter, CellPos{40, 40, 0}});
    sim->stepMany(20);
    CHECK(fightTestFighter(*sim, fighter).target == farAble);

    sim->submit(DestroyThingCommand{farAble});
    sim->stepMany(20);
    CHECK(fightTestFighter(*sim, fighter).target == farther);
    sim->submit(DestroyThingCommand{farther});
    sim->stepMany(20);
    CHECK(fightTestFighter(*sim, fighter).target == nearDowned);

    // An attack order wins over the choice.
    const Entity other = fightTestPawn(*sim, 60, 40, "Gun_Revolver");
    fightTestHostile(*sim, other);
    sim->submit(AttackOrderCommand{fighter, nearDowned});
    sim->stepMany(20);
    CHECK(fightTestFighter(*sim, fighter).target == nearDowned);
    sim->submit(AttackOrderCommand{fighter, kNoEntity});
    sim->stepMany(20);
    CHECK(fightTestFighter(*sim, fighter).target == other);
}

TEST_CASE("Fighters: a fighter falls back when outnumbered, and holds when it isn't") {
    const auto run = [](int enemies) {
        auto sim = fightTestSim(9);
        const Entity raider = fightTestPawn(*sim, 48, 30, "Gun_AssaultRifle");
        fightTestHostile(*sim, raider);
        std::vector<Entity> colonists;
        for (int i = 0; i < enemies; ++i) {
            colonists.push_back(fightTestPawn(*sim, 44 + 3 * i, 55));  // unarmed: they hide and don't shoot
            fightTestDraft(*sim, colonists.back());
        }
        sim->step();
        const Position start = sim->things().registry().get<Position>(raider);
        bool fellBack = false;
        for (int i = 0; i < 300; ++i) {
            for (const Entity c : colonists) {
                fightTestHeal(*sim, c);
            }
            sim->step();
            fellBack = fellBack || fightTestFighter(*sim, raider).fallingBack;
        }
        const Position end = sim->things().registry().get<Position>(raider);
        return std::make_pair(fellBack, end.y - start.y);  // the colonists are north: negative is away
    };
    const auto [alone, movedAlone] = run(1);
    const auto [outnumbered, movedOutnumbered] = run(3);
    CHECK_FALSE(alone);
    CHECK(outnumbered);
    CHECK(movedOutnumbered < -3.0f);
    CHECK(movedOutnumbered < movedAlone);
    MESSAGE("1 against 1 moved " << movedAlone << " m north, 1 against 3 moved " << movedOutnumbered << " m");
}

TEST_CASE("Fighters: a move order sends a drafted pawn there and it holds close by") {
    auto sim = fightTestSim();
    fightTestBuild(*sim, "FightTestSandbags", 70, 60, 72, 60);
    const Entity pawn = fightTestPawn(*sim, 20, 20, "Gun_Revolver");
    const Entity enemy = fightTestPawn(*sim, 71, 85);
    fightTestDraft(*sim, pawn);
    fightTestHostile(*sim, enemy);
    sim->step();
    sim->submit(MoveOrderCommand{pawn, CellPos{71, 58, 0}});
    sim->stepMany(900);
    const CellPos at = fightTestCell(*sim, pawn);
    CHECK(std::abs(at.x - 71) <= 2);
    CHECK(std::abs(at.y - 58) <= 2);
    sim->submit(MoveOrderCommand{pawn, CellPos{71, 60, 0}});  // on the sandbags: they can be walked over
    sim->submit(MoveOrderCommand{pawn, CellPos{-5, 3, 0}});   // off the map
    sim->step();
    CHECK(fightTestFailed(*sim));
}

TEST_CASE("Fighters: a firefight is the same after a save and load, and stays in cover") {
    // Three against three across a field with sandbags and walls on both sides.
    const auto setUp = [](Sim& sim) {
        fightTestBuild(sim, "FightTestSandbags", 30, 38, 36, 38);
        fightTestBuild(sim, "FightTestSandbags", 44, 39, 50, 39);
        fightTestBuild(sim, "FightTestWall", 40, 37, 41, 37);
        fightTestBuild(sim, "FightTestSandbags", 32, 62, 38, 62);
        fightTestBuild(sim, "FightTestWall", 46, 63, 48, 63);
        fightTestBuild(sim, "FightTestSandbags", 52, 61, 56, 61);
        const char* guns[] = {"Gun_AssaultRifle", "Gun_BoltActionRifle", "Gun_Revolver"};
        std::vector<Entity> pawns;
        for (int i = 0; i < 3; ++i) {
            pawns.push_back(fightTestPawn(sim, 34 + 7 * i, 33, guns[i]));
            sim.submit(DraftCommand{pawns.back(), true});
        }
        for (int i = 0; i < 3; ++i) {
            pawns.push_back(fightTestPawn(sim, 35 + 7 * i, 68, guns[2 - i]));
            sim.submit(SetFactionCommand{pawns.back(), 2});
        }
        return pawns;
    };
    auto straight = fightTestSim(13);
    const std::vector<Entity> pawns = setUp(*straight);
    auto saved = fightTestSim(13);
    setUp(*saved);

    int inPlace = 0;
    int inOpen = 0;
    int downed = 0;
    int shots = 0;
    int pawnHits = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 1800; ++i) {
        straight->step();
        saved->step();
        if (i == 400) {
            SaveLoadResult loaded = loadSim(saveSim(*saved), fightTestDefs());
            FIGHT_REQUIRE(loaded.sim != nullptr);
            CHECK(loaded.sim->stateHash() == saved->stateHash());
            CHECK(saveToJson(*saved).find("\"fighter\"") != std::string::npos);
            saved = std::move(loaded.sim);
        }
        // Waiting in place (not walking, not shooting) with an enemy known: is it hidden from the
        // nearest one when crouched?
        Sim& s = *straight;
        for (const Event& e : s.events()) {
            shots += e.type == EventType::ShotFired ? 1 : 0;
            pawnHits += e.type == EventType::ProjectileHit && e.hit == ProjectileHitKind::Pawn ? 1 : 0;
        }
        for (const Entity p : pawns) {
            if (s.health().incapacitated(p)) {
                continue;
            }
            const Fighter& f = s.things().registry().get<Fighter>(p);
            const Shooter* sh = s.things().registry().try_get<Shooter>(p);
            const bool shooting = sh != nullptr && (sh->phase == ShotPhase::Aiming || sh->phase == ShotPhase::Firing);
            const CellPos at = fightTestCell(s, p);
            if (f.walk != 0 || shooting || f.known.empty() || at != f.spot) {
                continue;
            }
            const Position& pos = s.things().registry().get<Position>(p);
            float best = 1.0e9f;
            float ex = 0.0f;
            float ey = 0.0f;
            for (const KnownEnemy& k : f.known) {
                const float d = (k.x - pos.x) * (k.x - pos.x) + (k.y - pos.y) * (k.y - pos.y);
                if (d < best && !s.health().incapacitated(k.pawn)) {
                    best = d;
                    ex = k.x;
                    ey = k.y;
                }
            }
            if (best < 1.0e9f) {
                ++inPlace;
                inOpen += s.cover().coverAgainst(at, Stance::Crouching, ex, ey) < 0.5f ? 1 : 0;
            }
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    for (const Entity p : pawns) {
        downed += straight->health().incapacitated(p) ? 1 : 0;
    }
    CHECK(saved->stateHash() == straight->stateHash());
    CHECK(inPlace > 0);
    CHECK(inOpen * 10 < inPlace);  // under 10% of the waiting in the open, with cover within 10 m
    CHECK(shots >= 50);  // they fight: nobody waits for the other to show himself
    CHECK(pawnHits >= 1);
    MESSAGE("waiting in place " << inPlace << " pawn-steps, " << inOpen << " of them in the open; " << shots
                                << " shots, " << pawnHits << " hit a pawn; " << downed << " of 6 down after 1800 steps; " << straight->fighters().plans() << " plans, "
                                << straight->fighters().sightChecks() << " sight checks; " << ms / 3600.0
                                << " ms per step (both sims)");
}
TEST_CASE("Fighters: 30 colonists against 40 raiders spread their plans over the steps") {
    // The Stage 1 target fight (docs/plans/stage-1.md): 30 drafted colonists with assault rifles
    // against 40 raiders with rifles, among scattered walls and sandbags.
    auto sim = fightTestSim(3, 160);
    Sim& s = *sim;
    Rng rng(7);
    for (int i = 0; i < 120; ++i) {
        const int x = 20 + static_cast<int>(rng.below(120));
        const int y = 50 + static_cast<int>(rng.below(60));
        fightTestBuild(s, rng.below(3) == 0 ? "FightTestWall" : "FightTestSandbags", x, y, x + static_cast<int>(rng.below(4)), y);
    }
    std::vector<Entity> pawns;
    for (int i = 0; i < 30; ++i) {
        pawns.push_back(fightTestPawn(s, 40 + (i % 15) * 5, 45 + (i / 15) * 3, "Gun_AssaultRifle"));
        s.submit(DraftCommand{pawns.back(), true});
    }
    for (int i = 0; i < 40; ++i) {
        pawns.push_back(fightTestPawn(s, 30 + (i % 20) * 5, 92 + (i / 20) * 3, "Gun_BoltActionRifle"));
        s.submit(SetFactionCommand{pawns.back(), 2});
    }
    s.step();
    constexpr int kSteps = 900;
    uint64_t mostPlans = 0;
    double totalMs = 0.0;
    double worstMs = 0.0;
    int shots = 0;
    for (int i = 0; i < kSteps; ++i) {
        const uint64_t before = s.fighters().plans();
        const auto t0 = std::chrono::steady_clock::now();
        s.step();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        totalMs += ms;
        worstMs = std::max(worstMs, ms);
        mostPlans = std::max(mostPlans, s.fighters().plans() - before);
        for (const Event& e : s.events()) {
            shots += e.type == EventType::ShotFired ? 1 : 0;
        }
    }
    int down = 0;
    for (const Entity p : pawns) {
        down += s.health().incapacitated(p) ? 1 : 0;
    }
    // Each of the 70 plans every 15 steps, spread out: about 5 plans a step, never all at once.
    CHECK(s.fighters().plans() >= 70u * kSteps / 15u * 9u / 10u);
    CHECK(mostPlans <= 12u);
    CHECK(shots > 100);
    MESSAGE("whole sim " << totalMs / kSteps << " ms a step on average, worst " << worstMs << " ms; at most "
                         << mostPlans << " plans in a step; " << s.fighters().sightChecks() / std::max<uint64_t>(1, s.fighters().plans())
                         << " sight checks a plan; " << shots << " shots, " << down << " of 70 down after " << kSteps << " steps");
}

TEST_CASE("Fighters: the orders go through a replay file and play back the same fight") {
    SimSettings settings;
    settings.seed = 17;
    settings.mapSize = 96;
    settings.flatMap = true;
    settings.checkpointEvery = 100;
    Sim original(settings, fightTestDefs());
    original.submit(SpawnThingCommand{"Human", CellPos{40, 30, 0}});
    original.submit(SpawnThingCommand{"Human", CellPos{44, 62, 0}});
    original.submit(SpawnThingCommand{"Gun_AssaultRifle", CellPos{40, 30, 0}});
    original.submit(SpawnThingCommand{"Gun_Revolver", CellPos{44, 62, 0}});
    original.submit(SpawnThingCommand{"FightTestSandbags", CellPos{40, 36, 0}});
    original.step();
    std::vector<Entity> spawned;
    for (const Event& e : original.events()) {
        if (e.type == EventType::ThingSpawned) {
            spawned.push_back(e.thing);
        }
    }
    FIGHT_REQUIRE(spawned.size() == 5);
    const Entity colonist = spawned[0];
    const Entity raider = spawned[1];
    original.submit(EquipCommand{colonist, spawned[2]});
    original.submit(EquipCommand{raider, spawned[3]});
    original.submit(DraftCommand{colonist, true});
    original.submit(SetFactionCommand{raider, 3});
    original.step();
    original.submit(MoveOrderCommand{colonist, CellPos{40, 35, 0}});
    original.submit(AttackOrderCommand{colonist, raider});
    original.stepMany(600);
    CHECK(original.things().registry().get<Fighter>(colonist).target == raider);
    original.submit(ReloadCommand{colonist});  // F5: may fail (full, mid-shot), but goes into the replay
    original.stepMany(100);
    const Replay recorded = recordReplay(original);
    Replay loaded;
    std::string error;
    FIGHT_REQUIRE(readReplay(writeReplay(recorded), loaded, error));
    CHECK(writeReplay(loaded) == writeReplay(recorded));
    CHECK(writeReplay(loaded).find("\"AttackOrder\"") != std::string::npos);
    CHECK(writeReplay(loaded).find("\"Reload\"") != std::string::npos);
    Sim replayed(loaded.settings, fightTestDefs());
    playReplay(loaded, replayed, loaded.endStep);
    CHECK(replayed.stateHash() == original.stateHash());
    CHECK(checkReplay(loaded, replayed).matched());
}

TEST_CASE("Fighters: a gunner's shooting spot is inside its gun's range, not at the very edge") {
    auto sim = fightTestSim(21, 128);
    GunStats gun;
    FIGHT_REQUIRE(sim->combat().gunStats(sim->defs().idOf("Things", "Gun_Revolver"), gun));
    const float margin = sim->fighters().rules().rangeMargin;
    CHECK(margin > 0.0f);
    // The enemy stands as far off as the revolver reaches, in the open, and doesn't move.
    const Entity gunner = fightTestPawn(*sim, 60, 40, "Gun_Revolver");
    const Entity enemy = fightTestPawn(*sim, 60, 40 + static_cast<int>(gun.range), "Gun_Revolver");
    fightTestDraft(*sim, gunner);
    fightTestHostile(*sim, enemy);
    sim->step();
    sim->things().registry().get<Fighter>(enemy).advances = false;
    int shots = 0;
    int checked = 0;
    for (int i = 0; i < 600; ++i) {
        fightTestHeal(*sim, gunner);
        fightTestHeal(*sim, enemy);
        sim->step();
        for (const Event& e : sim->events()) {
            shots += e.type == EventType::ShotFired && e.thing == gunner ? 1 : 0;
        }
        const Fighter& f = fightTestFighter(*sim, gunner);
        if (f.canFire && f.target == enemy) {
            const Position& t = sim->things().registry().get<Position>(enemy);
            const float d = std::hypot(static_cast<float>(f.fireCell.x) + 0.5f - t.x, static_cast<float>(f.fireCell.y) + 0.5f - t.y);
            CHECK(d <= gun.range - margin + 0.6f);  // the enemy may have drifted a little since the plan
            ++checked;
        }
    }
    CHECK(checked > 0);
    CHECK(shots >= 3);
}

TEST_CASE("Fighters: an advancing raider with no shot goes round a wall by its goal") {
    auto sim = fightTestSim(23, 128);
    // A long wall across the way with its ends far off: only the goal's route gets round it.
    fightTestBuild(*sim, "FightTestWall", 64, 20, 64, 108);
    const Entity raider = fightTestPawn(*sim, 40, 64, "Gun_Revolver");
    const Entity colonist = fightTestPawn(*sim, 90, 64);
    fightTestHostile(*sim, raider);
    sim->step();
    {
        Fighter& f = sim->things().registry().get<Fighter>(raider);
        f.hasGoal = true;
        f.goal = CellPos{90, 64, 0};
        f.known.push_back(KnownEnemy{colonist, 90.5f, 64.5f, 0, sim->time().seconds});
    }
    float furthestEast = 0.0f;
    for (int i = 0; i < 2400; ++i) {
        fightTestHeal(*sim, raider);
        fightTestHeal(*sim, colonist);
        sim->step();
        furthestEast = std::max(furthestEast, sim->things().registry().get<Position>(raider).x);
        Fighter& f = sim->things().registry().get<Fighter>(raider);
        if (f.known.empty() || f.known.front().pawn != colonist) {
            f.known.clear();  // it can't see the colonist through the wall: keep the knowledge it was given
            f.known.push_back(KnownEnemy{colonist, 90.5f, 64.5f, 0, sim->time().seconds});
        }
    }
    CHECK(furthestEast > 66.0f);  // past the wall
    MESSAGE("the raider got as far east as x = " << furthestEast);
}
