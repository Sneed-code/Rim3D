#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace colony;

#define FEEL_REQUIRE(...)      \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// The game's real data. These tests check docs/design/shooter-feel.md.
std::shared_ptr<const DefDatabase> feelDefs() {
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

std::unique_ptr<Sim> feelSim(uint64_t seed = 7) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = 96;
    settings.flatMap = true;
    settings.checkpointEvery = 50;
    return std::make_unique<Sim>(settings, feelDefs());
}

// A colonist the player controls, so it moves only when the test moves it.
Entity feelPlayer(Sim& sim, int x, int y, const char* gun = nullptr, int shootingSkill = 0) {
    const Entity pawn = sim.things().spawn("Human", CellPos{x, y, 0});
    if (gun != nullptr) {
        sim.combat().equip(pawn, sim.things().make(sim.defs().idOf("Things", gun)));
    }
    Colonist& c = sim.things().registry().get_or_emplace<Colonist>(pawn);
    c.skills.clear();
    c.skills.push_back(SkillRecord{"Shooting", shootingSkill});
    sim.jobs().addWorker(pawn);
    sim.submit(ControlPawnCommand{pawn});
    sim.step();
    return pawn;
}

bool feelFailed(const Sim& sim, uint64_t id) {
    return std::any_of(sim.events().begin(), sim.events().end(),
                       [&](const Event& e) { return e.type == EventType::CommandFailed && e.command == id; });
}

const Position& feelPos(const Sim& sim, Entity pawn) {
    return sim.things().registry().get<Position>(pawn);
}

// What a horizontal line through the pawn's chest, 3 m either side, meets first.
ProjectileHitKind feelProbe(Sim& sim, Entity pawn) {
    float ax = 0.0f;
    float ay = 0.0f;
    float az = 0.0f;
    sim.combat().aimPoint(pawn, ax, ay, az);
    return sim.combat().trace(ax - 3.0f, ay, az, ax + 3.0f, ay, az, 0, kNoEntity, CellPos{-1, -1, 0}).kind;
}

}  // namespace


TEST_CASE("Feel: a burst lifts the player's aim shot by shot from the weapon's kick list") {
    auto sim = feelSim();
    const Entity pawn = feelPlayer(*sim, 20, 40, "Gun_AssaultRifle");  // skill 0: the kicks as written
    float up = 1.0f;
    float side = 1.0f;
    sim->combat().kickOf(pawn, up, side);
    CHECK(up == 0.0f);
    CHECK(side == 0.0f);

    // The player's trigger: three shots of a burst, five steps apart.
    sim->submit(ShootCommand{pawn, kNoEntity, 60.5f, 40.5f, 1.0f, true});
    std::vector<std::pair<float, float>> after;  // the kick just after each shot
    for (int i = 0; i < 20 && after.size() < 3; ++i) {
        sim->step();
        const bool shot = std::any_of(sim->events().begin(), sim->events().end(),
                                      [&](const Event& e) { return e.type == EventType::ShotFired && e.thing == pawn; });
        if (shot) {
            sim->combat().kickOf(pawn, up, side);
            after.emplace_back(up, side);
        }
    }
    FEEL_REQUIRE(after.size() == 3);
    CHECK(after[0].first == doctest::Approx(0.7f));
    CHECK(after[0].second == doctest::Approx(0.05f));
    CHECK(after[1].first == doctest::Approx(0.7f + 0.8f));
    CHECK(after[1].second == doctest::Approx(0.05f - 0.15f));
    CHECK(after[2].first == doctest::Approx(0.7f + 0.8f + 0.9f));
    CHECK(after[2].second == doctest::Approx(0.05f - 0.15f + 0.2f));

    // It shows in the snapshot for the camera.
    Snapshot snapshot;
    sim->writeSnapshot(snapshot);
    const ThingView* view = snapshot.find(pawn);
    FEEL_REQUIRE(view != nullptr);
    CHECK(view->kickUp > 0.0f);
}

TEST_CASE("Feel: skill steadies the kick, and fighters get none") {
    auto sim = feelSim();
    const Entity steady = feelPlayer(*sim, 20, 40, "Gun_BoltActionRifle", 20);
    const Entity novice = feelPlayer(*sim, 20, 44, "Gun_BoltActionRifle", 0);
    for (const Entity pawn : {steady, novice}) {
        sim->submit(ShootCommand{pawn, kNoEntity, 60.5f, sim->things().registry().get<Position>(pawn).y, 1.0f, true});
    }
    sim->step();
    float steadyUp = 0.0f;
    float noviceUp = 0.0f;
    float side = 0.0f;
    sim->combat().kickOf(steady, steadyUp, side);
    sim->combat().kickOf(novice, noviceUp, side);
    CHECK(noviceUp == doctest::Approx(3.0f));
    CHECK(steadyUp == doctest::Approx(1.5f));  // 2.5% less a level, 50% at 20

    // A fighter's aimed shot adds no kick.
    const Entity raider = sim->things().spawn("Human", CellPos{20, 60, 0});
    sim->combat().equip(raider, sim->things().make(sim->defs().idOf("Things", "Gun_Revolver")));
    sim->submit(ShootCommand{raider, kNoEntity, 60.5f, 60.5f, 1.0f, false});
    sim->stepMany(120);
    float raiderUp = 1.0f;
    sim->combat().kickOf(raider, raiderUp, side);
    CHECK(raiderUp == 0.0f);
}

TEST_CASE("Feel: the kick turns the shot up and to the right of the aim") {
    auto sim = feelSim();
    const Entity pawn = feelPlayer(*sim, 20, 40, "Gun_Revolver");
    Rng rng(1);
    // Aimed east along the ground, no spread: a kick of 0.1 up rises, 0.1 right turns toward -y (south).
    const Entity plain = sim->combat().fire(pawn, 60.5f, 40.5f, 1.45f, 0.0f, rng);
    const Entity kicked = sim->combat().fire(pawn, 60.5f, 40.5f, 1.45f, 0.0f, rng, 0.1f, 0.1f);
    FEEL_REQUIRE(plain != kNoEntity);
    FEEL_REQUIRE(kicked != kNoEntity);
    const Projectile& a = sim->things().registry().get<Projectile>(plain);
    const Projectile& b = sim->things().registry().get<Projectile>(kicked);
    CHECK(b.vz > a.vz + 0.1f * a.vx * 0.9f);
    CHECK(b.vy < a.vy - 0.1f * a.vx * 0.9f);
}

TEST_CASE("Feel: the kick holds, wears off, and the spray starts over") {
    auto sim = feelSim();
    const Entity pawn = feelPlayer(*sim, 20, 40, "Gun_AssaultRifle");
    const auto burst = [&] {
        sim->submit(ShootCommand{pawn, kNoEntity, 60.5f, 40.5f, 1.0f, true});
        for (int i = 0; i < 40; ++i) {
            sim->step();
            if (sim->combat().shotReadyIn(pawn) > 0 && sim->combat().rollPhase(pawn) == RollPhase::Idle) {
                const bool firing = std::any_of(sim->events().begin(), sim->events().end(), [&](const Event& e) {
                    return e.type == EventType::ShotFired;
                });
                if (firing) {
                    return;
                }
            }
        }
    };
    burst();
    float up = 0.0f;
    float side = 0.0f;
    sim->combat().kickOf(pawn, up, side);
    CHECK(up == doctest::Approx(0.7f));
    sim->stepMany(3);
    float held = 0.0f;
    sim->combat().kickOf(pawn, held, side);
    CHECK(held == doctest::Approx(up));  // holding
    sim->stepMany(30);
    float gone = 1.0f;
    sim->combat().kickOf(pawn, gone, side);
    CHECK(gone == 0.0f);  // worn off
    sim->stepMany(60);    // the gun cools down

    // The next spray starts at the first entry again.
    burst();
    sim->combat().kickOf(pawn, up, side);
    CHECK(up == doctest::Approx(0.7f));
}

TEST_CASE("Feel: a dodge roll goes through start-up, roll and recovery, and moves the pawn by the sim") {
    auto sim = feelSim();
    const Entity pawn = feelPlayer(*sim, 40, 40, "Gun_Revolver");
    const float startX = feelPos(*sim, pawn).x;
    const float startY = feelPos(*sim, pawn).y;
    const uint64_t id = sim->submit(DodgeRollCommand{pawn, 3.0f, 0.0f});  // any length: east
    sim->step();
    CHECK_FALSE(feelFailed(*sim, id));
    bool sawEvent = false;
    for (const Event& e : sim->events()) {
        if (e.type == EventType::PawnAction && e.action == PawnAction::Roll && e.thing == pawn) {
            sawEvent = true;
            CHECK(e.x == doctest::Approx(1.0f));
            CHECK(e.amount == doctest::Approx(27.0f));
        }
    }
    CHECK(sawEvent);

    // Steps 1 to 3 are start-up: the same place, shootable. (The command's own step counted as 1.)
    CHECK(sim->combat().rollPhase(pawn) == RollPhase::StartUp);
    CHECK(feelProbe(*sim, pawn) == ProjectileHitKind::Pawn);
    sim->stepMany(2);
    CHECK(feelPos(*sim, pawn).x == doctest::Approx(startX));
    CHECK(sim->combat().rollPhase(pawn) == RollPhase::Rolling);

    // Rolling: 15 steps, bullets pass through for the first 12.
    int proof = 0;
    int rolling = 0;
    while (sim->combat().rollPhase(pawn) == RollPhase::Rolling) {
        sim->step();
        ++rolling;
        if (sim->combat().hitProof(pawn)) {
            ++proof;
            CHECK(feelProbe(*sim, pawn) != ProjectileHitKind::Pawn);
        }
        if (rolling > 40) {
            break;
        }
    }
    CHECK(rolling == 15);
    CHECK(proof == 12);
    CHECK(feelPos(*sim, pawn).x == doctest::Approx(startX + 4.0f).epsilon(0.001));
    CHECK(feelPos(*sim, pawn).y == doctest::Approx(startY));

    // Recovery: 9 steps standing still and shootable, then the wait, then free.
    CHECK(sim->combat().rollPhase(pawn) == RollPhase::Recovery);
    CHECK(feelProbe(*sim, pawn) == ProjectileHitKind::Pawn);
    int recovery = 0;
    while (sim->combat().rollPhase(pawn) == RollPhase::Recovery && recovery < 40) {
        sim->step();
        ++recovery;
    }
    CHECK(recovery == 9);
    CHECK(sim->combat().rollPhase(pawn) == RollPhase::Idle);
    const uint64_t tooSoon = sim->submit(DodgeRollCommand{pawn, 0.0f, 1.0f});
    sim->step();
    CHECK(feelFailed(*sim, tooSoon));  // the wait
    sim->stepMany(6);
    const uint64_t again = sim->submit(DodgeRollCommand{pawn, 0.0f, 1.0f});
    sim->step();
    CHECK_FALSE(feelFailed(*sim, again));
    CHECK(sim->combat().rollPhase(pawn) == RollPhase::StartUp);
}

TEST_CASE("Feel: the sim moves a rolling pawn; the player's position is ignored and shooting is off") {
    auto sim = feelSim();
    const Entity pawn = feelPlayer(*sim, 40, 40, "Gun_Revolver");
    const float startX = feelPos(*sim, pawn).x;
    const uint64_t id = sim->submit(DodgeRollCommand{pawn, 0.0f, 0.0f});  // no direction: back from where it faces (east)
    sim->step();
    CHECK_FALSE(feelFailed(*sim, id));
    float dirX = 0.0f;
    float dirY = 0.0f;
    FEEL_REQUIRE(sim->combat().rollDirection(pawn, dirX, dirY));
    CHECK(dirX == doctest::Approx(-1.0f));

    // The player keeps sending a position, even one far away; nothing fails and nothing follows it.
    uint64_t move = 0;
    uint64_t shoot = 0;
    for (int i = 0; i < 10; ++i) {
        move = sim->submit(DirectMoveCommand{pawn, 90.0f, 90.0f});
        sim->step();
        CHECK_FALSE(feelFailed(*sim, move));
    }
    shoot = sim->submit(ShootCommand{pawn, kNoEntity, 60.5f, 40.5f, 1.0f, true});
    sim->step();
    CHECK(feelFailed(*sim, shoot));
    CHECK(feelPos(*sim, pawn).x < startX);
    CHECK(feelPos(*sim, pawn).y == doctest::Approx(40.5f));

    // A snapshot says what the animation needs.
    Snapshot snapshot;
    sim->writeSnapshot(snapshot);
    const ThingView* view = snapshot.find(pawn);
    FEEL_REQUIRE(view != nullptr);
    CHECK(view->rollPhase == 2);
    CHECK(view->rollDirX == doctest::Approx(-1.0f));
    CHECK(view->rollStepsLeft > 0);
    CHECK(view->hitProof);

    // Then the player has the pawn back.
    sim->stepMany(40);
    CHECK(sim->combat().rollPhase(pawn) == RollPhase::Idle);
    const uint64_t after = sim->submit(DirectMoveCommand{pawn, feelPos(*sim, pawn).x + 0.3f, feelPos(*sim, pawn).y});
    const float before = feelPos(*sim, pawn).x;
    sim->step();
    CHECK_FALSE(feelFailed(*sim, after));
    CHECK(feelPos(*sim, pawn).x == doctest::Approx(before + 0.3f));
}

TEST_CASE("Feel: a roll ends a shot being aimed or fired, keeps a reload, and is refused when it can't be done") {
    auto sim = feelSim();
    const Entity pawn = feelPlayer(*sim, 40, 40, "Gun_AssaultRifle");
    sim->submit(ShootCommand{pawn, kNoEntity, 80.5f, 40.5f, 1.0f, true});
    sim->step();  // the first shot of the burst
    const uint64_t id = sim->submit(DodgeRollCommand{pawn, 0.0f, 1.0f});
    sim->step();
    CHECK_FALSE(feelFailed(*sim, id));
    int shots = 0;
    for (int i = 0; i < 12; ++i) {
        sim->step();
        shots += std::any_of(sim->events().begin(), sim->events().end(),
                             [&](const Event& e) { return e.type == EventType::ShotFired; })
                     ? 1
                     : 0;
    }
    CHECK(shots == 0);  // the rest of the burst is off

    // Refused: an uncontrolled pawn, a downed one, a prone one, one already rolling.
    const Entity free = sim->things().spawn("Human", CellPos{10, 10, 0});
    const uint64_t a = sim->submit(DodgeRollCommand{free, 1.0f, 0.0f});
    const uint64_t b = sim->submit(DodgeRollCommand{pawn, 1.0f, 0.0f});  // already rolling or waiting
    sim->step();
    CHECK(feelFailed(*sim, a));
    CHECK(feelFailed(*sim, b));
    sim->stepMany(60);
    sim->submit(SetStanceCommand{pawn, Stance::Prone});
    const uint64_t prone = sim->submit(DodgeRollCommand{pawn, 1.0f, 0.0f});
    sim->step();
    CHECK(feelFailed(*sim, prone));
    sim->submit(SetStanceCommand{pawn, Stance::Standing});
    sim->step();

    // A reload goes on through a roll.
    sim->submit(ReloadCommand{pawn});
    sim->step();
    CHECK(sim->combat().roundsLeft(pawn) < sim->combat().magazineSize(pawn));
    CHECK(sim->combat().reloading(pawn));
    sim->submit(DodgeRollCommand{pawn, 1.0f, 0.0f});
    sim->stepMany(3);
    CHECK(sim->combat().reloading(pawn));
}

TEST_CASE("Feel: a roll stops at the edge of the map") {
    auto sim = feelSim();
    const Entity pawn = feelPlayer(*sim, 1, 40);
    sim->submit(DodgeRollCommand{pawn, -1.0f, 0.0f});
    sim->stepMany(40);
    CHECK(feelPos(*sim, pawn).x >= 0.0f);
    CHECK(feelPos(*sim, pawn).x < 1.6f);
    CHECK(sim->combat().rollPhase(pawn) == RollPhase::Idle);
}

TEST_CASE("Feel: gun kick and a roll in progress survive a save and load, and replay") {
    auto sim = feelSim();
    const Entity shooter = feelPlayer(*sim, 20, 40, "Gun_AssaultRifle");
    const Entity roller = feelPlayer(*sim, 40, 50);
    sim->submit(ShootCommand{shooter, kNoEntity, 60.5f, 40.5f, 1.0f, true});
    sim->stepMany(12);
    sim->submit(DodgeRollCommand{roller, 1.0f, 1.0f});
    sim->stepMany(8);  // mid-roll, the kick holding or wearing off
    FEEL_REQUIRE(sim->combat().rollPhase(roller) == RollPhase::Rolling);

    const std::string json = saveToJson(*sim);
    CHECK(json.find("\"feel\"") != std::string::npos);
    SaveLoadResult loaded = loadSim(saveSim(*sim), feelDefs());
    FEEL_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->stateHash() == sim->stateHash());
    CHECK(loaded.sim->combat().rollPhase(roller) == RollPhase::Rolling);
    CHECK(loaded.sim->combat().rollStepsLeft(roller) == sim->combat().rollStepsLeft(roller));
    sim->stepMany(100);
    loaded.sim->stepMany(100);
    CHECK(loaded.sim->stateHash() == sim->stateHash());

    // The command is in replay files by name.
    const Replay recorded = recordReplay(*sim);
    const std::string text = writeReplay(recorded);
    CHECK(text.find("\"DodgeRoll\"") != std::string::npos);
    Replay reread;
    std::string error;
    FEEL_REQUIRE(readReplay(text, reread, error));
    CHECK(writeReplay(reread) == text);
}

TEST_CASE("Feel: the player's recoil and roll change the state hash") {
    auto a = feelSim();
    auto b = feelSim();
    const Entity pa = feelPlayer(*a, 40, 40, "Gun_Revolver");
    const Entity pb = feelPlayer(*b, 40, 40, "Gun_Revolver");
    CHECK(a->stateHash() == b->stateHash());
    a->submit(DodgeRollCommand{pa, 1.0f, 0.0f});
    a->step();
    b->step();
    CHECK(a->stateHash() != b->stateHash());
    (void)pb;
}
