#include "ColonySim/Animals.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Health.h"
#include "ColonySim/Monsters.h"
#include "ColonySim/Raids.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace colony;

// These tests check docs/design/monsters.md.

#define MONSTER_REQUIRE(...)   \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> monsterDefs() {
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

std::unique_ptr<Sim> monsterSim(uint64_t seed = 11, int size = 128, bool colonists = true) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = size;
    settings.flatMap = true;
    settings.scenario = colonists ? "Crashlanded" : "";
    return std::make_unique<Sim>(settings, monsterDefs());
}

size_t monsterCount(Sim& sim, const char* def) {
    const DefId id = sim.defs().idOf("Things", def);
    size_t n = 0;
    for (const auto [e, thing] : sim.things().registry().view<const Thing>().each()) {
        n += thing.def == id && sim.things().spawned(e) ? 1 : 0;
    }
    return n;
}

int monsterJelly(Sim& sim) {
    const DefId id = sim.defs().idOf("Things", "InsectJelly");
    int n = 0;
    for (const auto [e, thing, stack] : sim.things().registry().view<const Thing, const StackCount>().each()) {
        n += thing.def == id && sim.things().spawned(e) ? stack.count : 0;
    }
    return n;
}

std::vector<Entity> monsterPawnsOf(Sim& sim, FactionId side) {
    std::vector<Entity> out;
    for (const auto [e, faction] : sim.things().registry().view<const Faction>().each()) {
        if (faction.id == side) {
            out.push_back(e);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

TEST_CASE("Monsters: the wiki's numbers are in the defs") {
    const auto defs = monsterDefs();
    const Def& scyther = defs->get(defs->idOf("Things", "Mech_Scyther"));
    CHECK(scyther.number("race.baseHealthScale") == doctest::Approx(1.32));
    CHECK(scyther.number("race.armor.sharp") == doctest::Approx(0.4));
    CHECK(scyther.number("race.combatPower") == 150);
    CHECK(scyther.number("stats.moveSpeed") * 30 == doctest::Approx(4.7).epsilon(0.001));
    CHECK(defs->get(defs->idOf("Things", "Mech_Lancer")).text("race.weapon") == "Gun_ChargeLance");
    CHECK(defs->get(defs->idOf("Things", "Mech_Centipede")).number("race.baseBodySize") == doctest::Approx(3.0));
    CHECK(defs->get(defs->idOf("Things", "Megaspider")).number("race.combatPower") == 150);
    CHECK(defs->get(defs->idOf("Things", "Megascarab")).number("race.armor.sharp") == doctest::Approx(0.72));
    CHECK(defs->get(defs->idOf("Things", "Hive")).number("stats.maxHitPoints") == 130);
}

TEST_CASE("Monsters: mechanoids neither bleed nor hurt, and their hulls turn blows") {
    auto sim = monsterSim();
    const Entity mech = sim->things().spawn("Mech_Scyther", CellPos{20, 20, 0});
    const Entity human = sim->things().spawn("Human", CellPos{25, 20, 0});
    MONSTER_REQUIRE(mech != kNoEntity);
    MONSTER_REQUIRE(human != kNoEntity);
    Rng rng(5);
    const DefId cut = sim->health().damageTypeId("Cut");
    for (int i = 0; i < 3; ++i) {
        sim->health().applyDamage(mech, cut, 6.0f, -1, rng);
        sim->health().applyDamage(human, cut, 6.0f, -1, rng);
    }
    sim->step();
    HealthReport mechReport, humanReport;
    MONSTER_REQUIRE(sim->healthReport(mech, mechReport));
    MONSTER_REQUIRE(sim->healthReport(human, humanReport));
    CHECK(humanReport.bleedPerDay > 0.0f);
    CHECK(mechReport.bleedPerDay == 0.0f);
    CHECK(mechReport.pain == 0.0f);
    // Armor: a scyther's 40% sharp armor deflects about a fifth of hits and halves another fifth.
    const DefId bullet = sim->health().damageTypeId("Bullet");
    int deflected = 0, halved = 0, humanStopped = 0;
    for (uint64_t seed = 1; seed <= 600; ++seed) {
        const PawnHitResult r = sim->combat().hitPawn(mech, 0, bullet, 10.0f, 0.0f, seed);
        deflected += r.deflected ? 1 : 0;
        halved += !r.deflected && r.amount < 10.0f ? 1 : 0;
        const PawnHitResult h = sim->combat().hitPawn(human, 0, bullet, 10.0f, 0.0f, seed);
        humanStopped += h.amount < 10.0f ? 1 : 0;
    }
    CHECK(deflected > 600 * 0.14);
    CHECK(deflected < 600 * 0.26);
    CHECK(halved > 600 * 0.14);
    CHECK(halved < 600 * 0.26);
    CHECK(humanStopped == 0);
}

TEST_CASE("Monsters: an infestation grows mounds and sends insects after the colony") {
    auto sim = monsterSim();
    MONSTER_REQUIRE(sim->setupErrors().empty());
    std::string error;
    MONSTER_REQUIRE(sim->monsters().infest(900.0f, error));
    CHECK(sim->monsters().hiveCount() >= 2);
    const size_t insects = monsterCount(*sim, "Megascarab") + monsterCount(*sim, "Spelopede") + monsterCount(*sim, "Megaspider");
    CHECK(insects >= 3);
    sim->step();
    bool hostile = false;
    for (const auto [e, animal, thing] : sim->things().registry().view<const Animal, const Thing>().each()) {
        (void)thing;
        hostile = hostile || animal.temper == AnimalTemper::Manhunter;
        (void)e;
    }
    CHECK(hostile);
    // No jelly yet; a day and a half later every mound has made some, and the nest has bred.
    CHECK(monsterJelly(*sim) == 0);
    const size_t hives = sim->monsters().hiveCount();
    sim->stepMany(kSecondsPerDay * 3 / 2);
    CHECK(monsterJelly(*sim) >= 20 * static_cast<int>(hives));
}

TEST_CASE("Monsters: a destroyed mound leaves 30 jelly, and a mound takes no more than 40 around it") {
    // No colonists: they pick up the landing's guns and shoot the mound down in the first hour.
    auto sim = monsterSim(11, 128, false);
    const Entity hive = sim->monsters().spawnHive(CellPos{60, 60, 0});
    MONSTER_REQUIRE(hive != kNoEntity);
    sim->stepMany(kSecondsPerDay * 4);
    CHECK(monsterJelly(*sim) <= 60);  // 40 cap plus at most one batch that came in as the cap was checked
    CHECK(monsterJelly(*sim) >= 20);
    const int before = monsterJelly(*sim);
    sim->submit(ExplodeCommand{60.5f, 60.5f, 1.9f, 400.0f, "Bomb", 0.1f});  // a blast kills it inside a step
    sim->stepMany(3);
    CHECK(monsterJelly(*sim) == before + 30);
    CHECK(sim->monsters().hiveCount() == 0);
}

TEST_CASE("Monsters: a Remnant cluster walks in armed and fights the colony") {
    auto sim = monsterSim();
    std::string error;
    MONSTER_REQUIRE(sim->monsters().remnantCluster(400.0f, 3, error));
    const FactionId side = sim->raids().factionId("Remnant");
    const std::vector<Entity> mechs = monsterPawnsOf(*sim, side);
    MONSTER_REQUIRE(mechs.size() >= 2);
    bool armed = false;
    for (const Entity m : mechs) {
        armed = armed || sim->things().registry().all_of<Gear>(m);
    }
    CHECK(armed);
    const Position start = sim->things().registry().get<Position>(mechs[0]);
    sim->stepMany(1200);
    const Position now = sim->things().registry().get<Position>(mechs[0]);
    CHECK(std::hypot(now.x - start.x, now.y - start.y) > 5.0f);
    // The colonists see them and the alarm sounds.
    sim->stepMany(1800);
    CHECK(sim->raids().state().alarm);
}

TEST_CASE("Monsters: the threat command, a bad kind, and saving") {
    auto sim = monsterSim();
    sim->submit(StartThreatCommand{"Infestation", 500.0f, -1});
    sim->submit(StartThreatCommand{"Locusts", 0.0f, -1});
    sim->step();
    size_t failed = 0;
    for (const Event& e : sim->events()) {
        failed += e.type == EventType::CommandFailed ? 1 : 0;
    }
    CHECK(failed == 1);
    CHECK(sim->monsters().hiveCount() >= 1);
    sim->stepMany(100);
    const std::vector<uint8_t> bytes = saveSim(*sim);
    SaveLoadResult loaded = loadSim(bytes, monsterDefs());
    MONSTER_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->monsters().hiveCount() == sim->monsters().hiveCount());
    sim->stepMany(200);
    loaded.sim->stepMany(200);
    CHECK(loaded.sim->stateHash() == sim->stateHash());
}
