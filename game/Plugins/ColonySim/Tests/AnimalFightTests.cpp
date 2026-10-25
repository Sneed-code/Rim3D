#include "ColonySim/Animals.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Health.h"
#include "ColonySim/Melee.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Raids.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
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

// These tests check docs/design/animal-fights.md (task N5).

#define FIGHT_REQUIRE(...)     \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// The game's real defs; `from` and `to` swap one text in AnimalRules.json (a test that wants a chance to be 1 or 0).
std::shared_ptr<const DefDatabase> fightTestDefs(const std::string& from = std::string(), const std::string& to = std::string()) {
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
        std::string body = text.str();
        if (!from.empty() && file.filename() == "AnimalRules.json") {
            const size_t at = body.find(from);
            CHECK(at != std::string::npos);
            if (at != std::string::npos) {
                body.replace(at, from.size(), to);
            }
        }
        sources.push_back(DefSource{file.parent_path().filename().string(), file.filename().string(), body});
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
}

std::unique_ptr<Sim> fightSim(const std::shared_ptr<const DefDatabase>& defs, int hour = 18, uint64_t seed = 7, int size = 96) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = size;
    settings.flatMap = true;
    settings.checkpointEvery = 0;
    auto sim = std::make_unique<Sim>(settings, defs);
    for (int y = 0; y < sim->map().height(); ++y) {
        for (int x = 0; x < sim->map().width(); ++x) {
            sim->plants().remove(CellPos{x, y, 0});
        }
    }
    sim->stepMany(static_cast<int64_t>(hour) * kSecondsPerHour - sim->time().seconds);  // 9 in the morning: the deer are about; 18: wolves and deer are both awake
    return sim;
}

Entity fightSpawn(Sim& sim, const char* def, float x, float y) {
    const Entity e = sim.things().spawn(def, CellPos{static_cast<int>(x), static_cast<int>(y), 0});
    if (e != kNoEntity) {
        Position& p = sim.things().registry().get<Position>(e);
        p.x = x;
        p.y = y;
    }
    return e;
}

Entity fightColonist(Sim& sim, float x, float y, const char* gun = nullptr) {
    const Entity pawn = fightSpawn(sim, "Human", x, y);
    if (gun != nullptr && pawn != kNoEntity) {
        sim.combat().equip(pawn, sim.things().make(sim.defs().idOf("Things", gun)));
    }
    return pawn;
}

Animal& fightAnimal(Sim& sim, Entity e) { return sim.things().registry().get<Animal>(e); }

float fightDistance(Sim& sim, Entity a, Entity b) {
    const Position& p = sim.things().registry().get<Position>(a);
    const Position& q = sim.things().registry().get<Position>(b);
    return std::sqrt((p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y));
}

bool fightDead(Sim& sim, Entity e) { return !sim.things().registry().valid(e) || sim.things().registry().get<Health>(e).dead; }

// Dead, or downed and no more a threat.
bool fightOut(Sim& sim, Entity e) {
    return fightDead(sim, e) || sim.things().registry().get<Health>(e).downed;
}

bool fightInjured(Sim& sim, Entity e) { return sim.things().registry().valid(e) && !sim.things().registry().get<Health>(e).injuries.empty(); }

// Lets the animals settle (they start with a random first look), without stepping on to anything that happens.
void fightSettle(Sim& sim) { sim.stepMany(60); }

// A wolf or a boar, hurt by a colonist, in one blow: what `hurt` does with the roll.
void fightHurt(Sim& sim, Entity animal, Entity by) { sim.animals().hurt(animal, by); }

}  // namespace

TEST_CASE("Animal fights: shots hit animals now that they have hitboxes, and a manhunter is an enemy of the fighter AI") {
    const auto defs = fightTestDefs();
    auto sim = fightSim(defs);
    const Entity boar = fightSpawn(*sim, "WildBoar", 60, 40);
    const Entity colonist = fightColonist(*sim, 40, 40, "Gun_AssaultRifle");
    FIGHT_REQUIRE((boar != kNoEntity && colonist != kNoEntity));
    fightSettle(*sim);
    // Calm, the boar is nobody's enemy: a drafted rifleman leaves it alone.
    sim->submit(DraftCommand{colonist, true});
    sim->stepMany(300);
    CHECK_FALSE(fightInjured(*sim, boar));
    CHECK_FALSE(sim->animals().hostile(boar));
    // Manhunter, it is shot at and dies before it reaches the rifleman, or after a bite or two.
    const bool enraged = sim->animals().enrage(boar, AnimalTemper::Manhunter, colonist);
    FIGHT_REQUIRE(enraged);
    CHECK(sim->animals().hostile(boar));
    CHECK(sim->animals().stats().manhunters == 1);
    sim->stepMany(900);
    CHECK(fightDead(*sim, boar));
}

TEST_CASE("Animal fights: gunfire frightens animals nearby, not a wolf, and a herd runs together") {
    const auto defs = fightTestDefs();
    auto sim = fightSim(defs);
    const Entity deerA = fightSpawn(*sim, "Deer", 40, 40);
    const Entity deerB = fightSpawn(*sim, "Deer", 42, 41);
    const Entity farDeer = fightSpawn(*sim, "Deer", 85, 85);
    const Entity wolf = fightSpawn(*sim, "TimberWolf", 38, 42);
    const Entity shooter = fightColonist(*sim, 30, 40, "Gun_AssaultRifle");
    const Entity target = fightColonist(*sim, 30, 20);
    FIGHT_REQUIRE((deerA != kNoEntity && deerB != kNoEntity && farDeer != kNoEntity && wolf != kNoEntity && shooter != kNoEntity && target != kNoEntity));
    fightSettle(*sim);
    CHECK(fightAnimal(*sim, deerA).mode != AnimalMode::Flee);
    sim->submit(SetFactionCommand{target, 5});  // an enemy to shoot at, away from the deer
    sim->submit(DraftCommand{shooter, true});
    bool shot = false;
    for (int i = 0; i < 400 && !shot; ++i) {
        sim->step();
        for (const Event& e : sim->events()) {
            shot = shot || e.type == EventType::ShotFired;
        }
    }
    FIGHT_REQUIRE(shot);
    sim->step();
    CHECK(fightAnimal(*sim, deerA).mode == AnimalMode::Flee);
    CHECK(fightAnimal(*sim, deerB).mode == AnimalMode::Flee);
    CHECK(fightAnimal(*sim, farDeer).mode != AnimalMode::Flee);
    CHECK(fightAnimal(*sim, wolf).mode != AnimalMode::Flee);
    CHECK(sim->animals().stats().scares >= 2);
    // They run, and later stand about again.
    const Position before = sim->things().registry().get<Position>(deerA);
    sim->stepMany(100);
    const Position& after = sim->things().registry().get<Position>(deerA);
    CHECK(std::hypot(after.x - before.x, after.y - before.y) > 3.0f);
    sim->stepMany(600);
    CHECK(fightAnimal(*sim, deerA).mode != AnimalMode::Flee);
}

TEST_CASE("Animal fights: a hurt boar turns manhunter and bites, a hurt deer only runs") {
    const auto defs = fightTestDefs("\"manhunterChanceFlagged\": 0.5", "\"manhunterChanceFlagged\": 1");
    auto sim = fightSim(defs);
    const Entity boar = fightSpawn(*sim, "WildBoar", 50, 40);
    const Entity deer = fightSpawn(*sim, "Deer", 50, 50);
    const Entity colonist = fightColonist(*sim, 44, 40);
    FIGHT_REQUIRE((boar != kNoEntity && deer != kNoEntity && colonist != kNoEntity));
    fightSettle(*sim);
    fightHurt(*sim, deer, colonist);
    CHECK(fightAnimal(*sim, deer).temper == AnimalTemper::Calm);
    CHECK(fightAnimal(*sim, deer).mode == AnimalMode::Flee);
    fightHurt(*sim, boar, colonist);
    CHECK(fightAnimal(*sim, boar).temper == AnimalTemper::Manhunter);
    CHECK(fightAnimal(*sim, boar).enemy == colonist);
    CHECK(sim->animals().stats().manhunters == 1);
    // It runs at the colonist and bites.
    bool swung = false;
    bool bit = false;
    for (int i = 0; i < 600 && !bit; ++i) {
        sim->step();
        for (const Event& e : sim->events()) {
            if (e.type == EventType::MeleeSwing && e.thing == boar) {
                swung = true;
            }
            if (e.type == EventType::MeleeHit && e.thing == boar && e.target == colonist && e.hit == ProjectileHitKind::Pawn) {
                bit = true;
            }
        }
    }
    CHECK(swung);
    CHECK(bit);
    CHECK(fightInjured(*sim, colonist));
    // The view says so.
    Snapshot snap;
    sim->writeSnapshot(snap);
    const ThingView* view = snap.find(boar);
    FIGHT_REQUIRE(view != nullptr);
    CHECK(view->animalTemper == static_cast<int>(AnimalTemper::Manhunter));
}

TEST_CASE("Animal fights: a hurt wolf fights back for a while and then calms down") {
    const auto defs = fightTestDefs("\"manhunterChancePredator\": 0.2", "\"manhunterChancePredator\": 0");
    auto sim = fightSim(defs);
    const Entity wolf = fightSpawn(*sim, "TimberWolf", 50, 40);
    const Entity colonist = fightColonist(*sim, 45, 40);
    FIGHT_REQUIRE((wolf != kNoEntity && colonist != kNoEntity));
    fightSettle(*sim);
    fightHurt(*sim, wolf, colonist);
    CHECK(fightAnimal(*sim, wolf).temper == AnimalTemper::Revenge);
    CHECK(sim->animals().stats().revenges == 1);
    CHECK(sim->animals().hostile(wolf));
    sim->stepMany(120);
    CHECK(fightDistance(*sim, wolf, colonist) < 2.0f);
    // After its time (and with nobody left to fight) it is calm again.
    sim->health().kill(colonist, DeathCause::BloodLoss);
    sim->stepMany(sim->animals().rules().revengeSteps + 60);
    CHECK(fightAnimal(*sim, wolf).temper == AnimalTemper::Calm);
    CHECK_FALSE(sim->animals().hostile(wolf));
    CHECK(fightAnimal(*sim, wolf).mode != AnimalMode::Chase);
}

TEST_CASE("Animal fights: an animal that is not a predator never fights back") {
    const auto defs = fightTestDefs("\"manhunterChanceOther\": 0.02", "\"manhunterChanceOther\": 0");
    auto sim = fightSim(defs);
    const Entity alpaca = fightSpawn(*sim, "Alpaca", 50, 40);
    const Entity colonist = fightColonist(*sim, 45, 40);
    FIGHT_REQUIRE((alpaca != kNoEntity && colonist != kNoEntity));
    fightSettle(*sim);
    fightHurt(*sim, alpaca, colonist);
    CHECK(fightAnimal(*sim, alpaca).temper == AnimalTemper::Calm);
    CHECK(fightAnimal(*sim, alpaca).mode == AnimalMode::Flee);
    CHECK(sim->animals().stats().manhunters == 0);
}

TEST_CASE("Animal fights: a hungry wolf hunts a deer, kills it and eats it") {
    const auto defs = fightTestDefs();
    auto sim = fightSim(defs);
    const Entity wolf = fightSpawn(*sim, "TimberWolf", 40, 40);
    const Entity deer = fightSpawn(*sim, "Deer", 55, 40);
    FIGHT_REQUIRE((wolf != kNoEntity && deer != kNoEntity));
    fightSettle(*sim);
    fightAnimal(*sim, wolf).food = 0.05f;
    fightAnimal(*sim, wolf).foodAt = sim->time().seconds;
    bool hunting = false;
    for (int i = 0; i < 6000 && sim->animals().stats().meals == 0; ++i) {
        sim->step();
        hunting = hunting || sim->animals().stats().hunts > 0;
    }
    CHECK(hunting);
    CHECK(sim->animals().stats().kills == 1);
    CHECK(sim->animals().stats().meals == 1);
    CHECK_FALSE(sim->things().registry().valid(deer));  // eaten, corpse gone
    CHECK(fightAnimal(*sim, wolf).food > 0.5f);
    CHECK(fightAnimal(*sim, wolf).temper == AnimalTemper::Calm);  // prey is not the colony's business
    CHECK_FALSE(sim->animals().hostile(wolf));
}

TEST_CASE("Animal fights: a fed wolf, and a wolf next to prey that is too big, leave things alone") {
    const auto defs = fightTestDefs();
    auto sim = fightSim(defs);
    const Entity wolf = fightSpawn(*sim, "TimberWolf", 40, 40);
    const Entity deer = fightSpawn(*sim, "Deer", 48, 40);
    const Entity muffalo = fightSpawn(*sim, "Muffalo", 40, 50);
    FIGHT_REQUIRE((wolf != kNoEntity && deer != kNoEntity && muffalo != kNoEntity));
    fightSettle(*sim);
    fightAnimal(*sim, wolf).food = sim->animals().foodCapacity(wolf);
    sim->stepMany(600);
    CHECK(sim->animals().stats().hunts == 0);
    CHECK_FALSE(fightInjured(*sim, deer));
    // Hungry but with only a muffalo (body size 2.4 against 0.9, and the limit is 1.5 times) near: no hunt.
    sim->things().destroy(deer);
    fightAnimal(*sim, wolf).food = 0.0f;
    fightAnimal(*sim, wolf).foodAt = sim->time().seconds;
    fightAnimal(*sim, wolf).huntAfter = 0;
    sim->stepMany(900);
    CHECK(sim->animals().stats().hunts == 0);
    CHECK_FALSE(fightInjured(*sim, muffalo));
}

TEST_CASE("Animal fights: a hungry wolf hunts a lone colonist, and not one with company") {
    const auto defs = fightTestDefs();
    {
        auto sim = fightSim(defs);
        const Entity wolf = fightSpawn(*sim, "TimberWolf", 40, 40);
        const Entity colonist = fightColonist(*sim, 55, 40);
        FIGHT_REQUIRE((wolf != kNoEntity && colonist != kNoEntity));
        fightSettle(*sim);
        fightAnimal(*sim, wolf).food = 0.0f;
        fightAnimal(*sim, wolf).foodAt = sim->time().seconds;
        fightAnimal(*sim, wolf).huntAfter = 0;
        bool hunted = false;
        for (int i = 0; i < 600 && !hunted; ++i) {
            sim->step();
            hunted = fightAnimal(*sim, wolf).temper == AnimalTemper::Hunting;
        }
        CHECK(hunted);
        CHECK(fightAnimal(*sim, wolf).enemy == colonist);
        CHECK(sim->animals().hostile(wolf));
        sim->stepMany(300);
        CHECK(fightInjured(*sim, colonist));  // it got to him and bit
    }
    {
        auto sim = fightSim(defs);
        const Entity wolf = fightSpawn(*sim, "TimberWolf", 40, 40);
        const Entity colonist = fightColonist(*sim, 55, 40);
        const Entity friendPawn = fightColonist(*sim, 58, 41);
        FIGHT_REQUIRE((wolf != kNoEntity && colonist != kNoEntity && friendPawn != kNoEntity));
        fightSettle(*sim);
        fightAnimal(*sim, wolf).food = 0.0f;
        fightAnimal(*sim, wolf).foodAt = sim->time().seconds;
        fightAnimal(*sim, wolf).huntAfter = 0;
        sim->stepMany(600);
        CHECK(sim->animals().stats().hunts == 0);
        CHECK_FALSE(fightInjured(*sim, colonist));
    }
}

TEST_CASE("Animal fights: riflemen shoot a manhunting wolf and the colony alarm sounds") {
    const auto defs = fightTestDefs();
    auto sim = fightSim(defs);
    const Entity wolf = fightSpawn(*sim, "TimberWolf", 40, 40);
    const Entity colonist = fightColonist(*sim, 90, 40, "Gun_AssaultRifle");
    const Entity second = fightColonist(*sim, 90, 43, "Gun_AssaultRifle");
    FIGHT_REQUIRE((wolf != kNoEntity && colonist != kNoEntity && second != kNoEntity));
    (void)sim->things().registry().get_or_emplace<Colonist>(colonist);  // the alarm calls up colonists
    (void)sim->things().registry().get_or_emplace<Colonist>(second);
    fightSettle(*sim);
    const bool enraged = sim->animals().enrage(wolf, AnimalTemper::Manhunter, colonist);
    FIGHT_REQUIRE(enraged);
    bool alarm = false;
    for (int i = 0; i < 900 && !fightOut(*sim, wolf); ++i) {
        sim->step();
        alarm = alarm || sim->raids().state().alarm;
    }
    CHECK(alarm);
    CHECK(fightOut(*sim, wolf));  // shot down
}

TEST_CASE("Animal fights: saving in the middle of a hunt and loading plays on the same") {
    const auto defs = fightTestDefs();
    auto sim = fightSim(defs);
    const Entity wolf = fightSpawn(*sim, "TimberWolf", 40, 40);
    const Entity deer = fightSpawn(*sim, "Deer", 56, 40);
    const Entity boar = fightSpawn(*sim, "WildBoar", 60, 60);
    const Entity colonist = fightColonist(*sim, 62, 60);
    FIGHT_REQUIRE((wolf != kNoEntity && deer != kNoEntity && boar != kNoEntity && colonist != kNoEntity));
    fightSettle(*sim);
    fightAnimal(*sim, wolf).food = 0.0f;
    fightAnimal(*sim, wolf).foodAt = sim->time().seconds;
    sim->animals().enrage(boar, AnimalTemper::Manhunter, colonist);
    sim->stepMany(40);
    FIGHT_REQUIRE(fightAnimal(*sim, wolf).mode == AnimalMode::Chase);
    const std::vector<uint8_t> bytes = saveSim(*sim);
    SaveLoadResult loaded = loadSim(bytes, defs);
    FIGHT_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.error.empty());
    CHECK(loaded.sim->stateHash() == sim->stateHash());
    CHECK(loaded.sim->animals().stats().hunts == sim->animals().stats().hunts);
    CHECK(saveSim(*loaded.sim) == bytes);
    sim->stepMany(3000);
    loaded.sim->stepMany(3000);
    CHECK(loaded.sim->stateHash() == sim->stateHash());
    CHECK(loaded.sim->animals().stats().meals == sim->animals().stats().meals);
}
