#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Monsters.h"
#include "ColonySim/Raids.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Rooms.h"
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
#include <tuple>
#include <vector>

using namespace colony;

#define RAID_REQUIRE(...)      \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// The game's real data plus a test wall (the walls in the data need stuff). These tests check
// docs/design/raids.md.
std::shared_ptr<const DefDatabase> raidTestDefs() {
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
        sources.push_back(DefSource{"Things", "zz_raid_test.json", R"([
            {"defName": "RaidTestWall", "category": "Building", "passability": "Impassable", "fillPercent": 1.0, "stats": {"maxHitPoints": 3000}},
            {"defName": "RaidTestDoor", "category": "Building", "thingClass": "Building_Door", "passability": "PassThroughOnly", "fillPercent": 1.0}
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

// A flat map with the Crashlanded colonists in the middle.
std::unique_ptr<Sim> raidTestSim(uint64_t seed = 11, int size = 128) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = size;
    settings.flatMap = true;
    settings.scenario = "Crashlanded";
    return std::make_unique<Sim>(settings, raidTestDefs());
}

std::vector<Entity> raidTestColonists(Sim& sim) {
    std::vector<Entity> out;
    for (const auto [pawn, colonist] : sim.things().registry().view<const Colonist>().each()) {
        if (sim.fighters().factionOf(pawn) == kPlayerFaction) {
            out.push_back(pawn);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<Entity> raidTestRaiders(Sim& sim) {
    std::vector<Entity> out;
    for (const Entity pawn : sim.things().registry().view<const Raider>()) {
        out.push_back(pawn);
    }
    std::sort(out.begin(), out.end());
    return out;
}

void raidTestArm(Sim& sim, Entity pawn, const char* gun = "Gun_AssaultRifle") {
    sim.combat().equip(pawn, sim.things().make(sim.defs().idOf("Things", gun)));
}

bool raidTestFailed(const Sim& sim) {
    return std::any_of(sim.events().begin(), sim.events().end(), [](const Event& e) { return e.type == EventType::CommandFailed; });
}

const Event* raidTestEvent(const Sim& sim, EventType type) {
    for (const Event& e : sim.events()) {
        if (e.type == type) {
            return &e;
        }
    }
    return nullptr;
}

float raidTestDist(Sim& sim, Entity a, Entity b) {
    const Position& p = sim.things().registry().get<Position>(a);
    const Position& q = sim.things().registry().get<Position>(b);
    return std::hypot(p.x - q.x, p.y - q.y);
}

void raidTestHeal(Sim& sim, Entity pawn) {
    sim.things().registry().replace<Health>(pawn, Health{});
    sim.health().refresh(pawn);
}

}  // namespace

TEST_CASE("Raids: raid points follow RimWorld's wealth and colonist curves") {
    auto sim = raidTestSim();
    RAID_REQUIRE(sim->setupErrors().empty());
    const float wealth = sim->raids().colonyWealth();
    // Crashlanded: silver, steel, wood, components, medicine and meals, plus the six colonists at
    // 1,750 each (T1: the wiki counts creatures): a little over 14,000, where wealth points start.
    CHECK(wealth > 4000.0f + 6 * 1750.0f);
    CHECK(wealth < 20000.0f);
    // The wiki's curves: wealth points from 0 at 14,000 to 2,400 at 400,000; 15 points a colonist
    // up to 10,000, rising to 140 at 400,000. Then day 0 (x0.7) and adaptation 0.8.
    const float wealthPoints = std::max(0.0f, (wealth - 14000.0f) / 386000.0f * 2400.0f);
    const float perColonist = 15.0f + (wealth - 10000.0f) / 390000.0f * 125.0f;
    CHECK(sim->raids().threatPoints() == doctest::Approx((wealthPoints + 6 * perColonist) * 0.7 * 0.8).epsilon(0.001));

    // Rich colony, later: 400,000 wealth gives 2,400 points plus 140 a colonist.
    SimSettings settings;
    settings.seed = 3;
    settings.mapSize = 64;
    settings.flatMap = true;
    Sim rich(settings, raidTestDefs());
    rich.things().spawn("Silver", CellPos{10, 10, 0}, 400);
    CHECK(rich.raids().colonyWealth() == doctest::Approx(400.0f));
    CHECK(rich.raids().threatPoints() == doctest::Approx(35.0f));  // the minimum
}

TEST_CASE("Raids: a raid buys pirates with its points and walks in at the edge") {
    auto sim = raidTestSim();
    sim->submit(StartRaidCommand{"Pirate", 300.0f, 3});
    sim->step();
    RAID_REQUIRE(!raidTestFailed(*sim));
    const Event* arrived = raidTestEvent(*sim, EventType::RaidArrived);
    RAID_REQUIRE(arrived != nullptr);
    CHECK(arrived->message == "Pirate");
    CHECK(arrived->amount == doctest::Approx(300.0f));
    CHECK(arrived->cell.x <= 10);  // the west edge

    const std::vector<Entity> raiders = raidTestRaiders(*sim);
    RAID_REQUIRE(!raiders.empty());
    CHECK(arrived->count == static_cast<int>(raiders.size()));
    float power = 0.0f;
    const entt::registry& reg = sim->things().registry();
    for (const Entity r : raiders) {
        CHECK(sim->fighters().factionOf(r) == 2);
        CHECK(reg.all_of<Colonist, Fighter, Health>(r));
        CHECK(reg.get<Fighter>(r).advances);
        CHECK(reg.get<Fighter>(r).hasGoal);
        CHECK(reg.get<Gear>(r).weapon != kNoEntity);  // every gunner has a gun
        CHECK(std::find(reg.get<Colonist>(r).disabledWorkTags.begin(), reg.get<Colonist>(r).disabledWorkTags.end(), "Violent") ==
              reg.get<Colonist>(r).disabledWorkTags.end());
        CHECK(reg.get<Position>(r).x < 20.0f);
        power += static_cast<float>(sim->defs().find("PawnKinds", reg.get<Raider>(r).kind)->number("combatPower"));
    }
    CHECK(power <= 300.0f);
    CHECK(power > 300.0f - 50.0f);  // it buys until not even the cheapest kind is left
    RAID_REQUIRE(sim->raids().state().squads.size() == 1);
    CHECK(sim->raids().state().squads[0].state == SquadState::Advance);

    Snapshot snap;
    sim->writeSnapshot(snap);
    RAID_REQUIRE(snap.squads.size() == 1);
    CHECK(snap.squads[0].members == static_cast<int>(raiders.size()));
    CHECK(snap.find(raiders[0])->squad == snap.squads[0].id);

    // Bad commands fail.
    sim->submit(StartRaidCommand{"Nobody", 100.0f, -1});
    sim->submit(StartRaidCommand{"Pirate", 100.0f, 7});
    sim->step();
    CHECK(std::count_if(sim->events().begin(), sim->events().end(), [](const Event& e) { return e.type == EventType::CommandFailed; }) == 2);
}

TEST_CASE("Raids: raiders head for the colony, the alarm sounds and the colony fights back") {
    auto sim = raidTestSim(21, 160);
    const std::vector<Entity> colonists = raidTestColonists(*sim);
    RAID_REQUIRE(colonists.size() == 6);
    for (const Entity c : colonists) {
        raidTestArm(*sim, c);
    }
    sim->stepMany(5);
    sim->submit(StartRaidCommand{"Pirate", 150.0f, 1});
    sim->step();
    const std::vector<Entity> raiders = raidTestRaiders(*sim);
    RAID_REQUIRE(!raiders.empty());
    const float startDist = raidTestDist(*sim, raiders[0], colonists[0]);

    bool alarm = false;
    int64_t alarmAt = -1;
    int shotsByColony = 0;
    for (int i = 0; i < 1800 && !alarm; ++i) {
        sim->step();
        if (const Event* e = raidTestEvent(*sim, EventType::AlarmChanged)) {
            alarm = e->message.rfind("on", 0) == 0;
            alarmAt = sim->time().seconds;
        }
    }
    RAID_REQUIRE(alarm);
    CHECK(sim->raids().state().alarm);
    CHECK_FALSE(sim->raids().state().manual);
    CHECK(raidTestDist(*sim, raiders[0], colonists[0]) < startDist - 20.0f);  // they came for the colony
    MESSAGE("alarm after " << alarmAt << " s");

    // Every colonist (all Defenders) is called up: off work, fighting, not drafted.
    sim->stepMany(20);
    const entt::registry& reg = sim->things().registry();
    for (const Entity c : colonists) {
        if (sim->health().incapacitated(c)) {
            continue;
        }
        RAID_REQUIRE(reg.all_of<Fighter>(c));
        CHECK(reg.get<Fighter>(c).defending);
        CHECK_FALSE(reg.get<Fighter>(c).drafted);
        CHECK(reg.get<Worker>(c).job.type == JobType::None);
    }

    // The fight runs to an end: the raid is beaten or runs, the alarm stops by itself, and
    // everyone left standing goes back to work.
    bool over = false;
    for (int i = 0; i < 6000 && !over; ++i) {
        sim->step();
        for (const Event& e : sim->events()) {
            if (e.type == EventType::ShotFired && sim->fighters().factionOf(e.thing) == kPlayerFaction) {
                ++shotsByColony;
            }
        }
        over = sim->raids().state().squads.empty() && !sim->raids().state().alarm;
    }
    CHECK(shotsByColony > 0);
    RAID_REQUIRE(over);
    sim->stepMany(5);
    for (const Entity c : colonists) {
        if (!sim->health().incapacitated(c)) {
            CHECK_FALSE(reg.all_of<Fighter>(c));
        }
    }
}

TEST_CASE("Raids: Shelter never goes into a room with a brood mound in it") {
    // The year run (docs/design/colony-scenario.md "Seventh pass"): an infestation comes up under overhead mountain, which
    // counts as indoors, and unarmed colonists running for the nearest roofed cell ran into the hive's cave.
    auto sim = raidTestSim(31, 128);
    const std::vector<Entity> colonists = raidTestColonists(*sim);
    RAID_REQUIRE(colonists.size() == 6);
    const entt::registry& reg = sim->things().registry();
    const Position home = reg.get<Position>(colonists[0]);
    const int x0 = static_cast<int>(home.x) + 10;
    const int y0 = static_cast<int>(home.y) - 3;
    for (int y = y0; y <= y0 + 6; ++y) {
        for (int x = x0; x <= x0 + 6; ++x) {
            if (x == x0 || y == y0 || x == x0 + 6 || y == y0 + 6) {
                sim->things().spawn(x == x0 && y == y0 + 3 ? "Door" : "Wall", CellPos{x, y, 0});
            }
        }
    }
    sim->step();
    const CellPos inside{x0 + 3, y0 + 3, 0};
    RAID_REQUIRE(sim->rooms().indoors(inside));
    const Entity hive = sim->monsters().spawnHive(CellPos{x0 + 5, y0 + 5, 0});
    RAID_REQUIRE(hive != kNoEntity);
    sim->submit(MarkShelterCommand{inside, false});
    sim->submit(SetDefenseRoleCommand{colonists[1], static_cast<uint8_t>(DefenseRole::Shelter), false, CellPos{}});
    sim->submit(SoundAlarmCommand{true});
    sim->stepMany(400);
    RAID_REQUIRE(reg.all_of<Fighter>(colonists[1]));
    CHECK(reg.get<Fighter>(colonists[1]).mode == FightMode::Shelter);
    const Position& at = reg.get<Position>(colonists[1]);
    const Position& mound = reg.get<Position>(hive);
    MESSAGE("the sheltering colonist is " << std::hypot(at.x - mound.x, at.y - mound.y) << " m from the mound");
    CHECK(std::hypot(at.x - mound.x, at.y - mound.y) >= 19.0f);
    const bool inTheMoundsRoom = sim->rooms().indoors(cellOf(at)) && sim->rooms().roomOf(cellOf(at)) == sim->rooms().roomOf(inside);
    CHECK_FALSE(inTheMoundsRoom);
}

TEST_CASE("Raids: a sheltering colonist moves on when an enemy comes close and can see it") {
    // The year run (docs/design/colony-scenario.md "Eighth pass"): with no roofed cell to run to, unarmed colonists waited
    // in the open where they first fled, and a raider walked up and shot them there. Now a shelterer an enemy can see
    // within shelterDanger picks a new place.
    auto sim = raidTestSim(31, 128);
    const std::vector<Entity> colonists = raidTestColonists(*sim);
    RAID_REQUIRE(colonists.size() == 6);
    const entt::registry& reg = sim->things().registry();
    for (const Entity c : colonists) {
        sim->submit(SetDefenseRoleCommand{c, static_cast<uint8_t>(DefenseRole::Shelter), false, CellPos{}});
    }
    sim->submit(StartRaidCommand{"Pirate", 100.0f, 3});
    sim->step();
    const std::vector<Entity> raiders = raidTestRaiders(*sim);
    RAID_REQUIRE(!raiders.empty());
    // The first time the raider aims at a sheltering colonist, that colonist picks a new place within a few alarm checks.
    bool aimed = false;
    bool movedOn = false;
    CellPos firstGoal;
    Entity watched = kNoEntity;
    int64_t aimedAt = 0;
    for (int64_t i = 0; i < 12 * kSecondsPerHour && !movedOn; ++i) {
        sim->step();
        if (!reg.valid(raiders[0]) || !reg.all_of<Fighter>(raiders[0])) {
            break;
        }
        if (!aimed) {
            const Entity target = reg.get<Fighter>(raiders[0]).target;
            const Fighter* theirs = target != kNoEntity && reg.valid(target) ? reg.try_get<Fighter>(target) : nullptr;
            if (theirs != nullptr && theirs->mode == FightMode::Shelter && theirs->hasGoal && !sim->health().incapacitated(target)) {
                aimed = true;
                watched = target;
                firstGoal = theirs->goal;
                aimedAt = i;
            }
        } else {
            const Fighter* f = reg.valid(watched) ? reg.try_get<Fighter>(watched) : nullptr;
            movedOn = f != nullptr && f->goal != firstGoal;
            if (i - aimedAt > 60) {
                break;
            }
        }
    }
    RAID_REQUIRE(aimed);
    CHECK(movedOn);
}

TEST_CASE("Raids: Shelter goes to the marked room, Keep working works until enemies come close") {
    auto sim = raidTestSim(31, 128);
    const std::vector<Entity> colonists = raidTestColonists(*sim);
    RAID_REQUIRE(colonists.size() == 6);
    const entt::registry& reg = sim->things().registry();
    const Position home = reg.get<Position>(colonists[0]);
    // A 5 by 5 room (inside) 10 m east of the colonists, door on its west wall.
    const int x0 = static_cast<int>(home.x) + 10;
    const int y0 = static_cast<int>(home.y) - 3;
    for (int y = y0; y <= y0 + 6; ++y) {
        for (int x = x0; x <= x0 + 6; ++x) {
            const bool edge = x == x0 || y == y0 || x == x0 + 6 || y == y0 + 6;
            if (edge) {
                sim->things().spawn(x == x0 && y == y0 + 3 ? "Door" : "Wall", CellPos{x, y, 0});
            }
        }
    }
    sim->step();
    const CellPos inside{x0 + 3, y0 + 3, 0};
    RAID_REQUIRE(sim->rooms().indoors(inside));

    sim->submit(MarkShelterCommand{inside, false});
    sim->submit(SetDefenseRoleCommand{colonists[1], static_cast<uint8_t>(DefenseRole::Shelter), false, CellPos{}});
    sim->submit(SetDefenseRoleCommand{colonists[2], static_cast<uint8_t>(DefenseRole::KeepWorking), false, CellPos{}});
    sim->submit(SetDefenseRoleCommand{colonists[3], static_cast<uint8_t>(DefenseRole::Defender), true, CellPos{static_cast<int>(home.x) - 6, static_cast<int>(home.y), 0}});
    sim->submit(MarkShelterCommand{CellPos{2, 2, 0}, false});                                   // outdoors: fails
    sim->submit(SetDefenseRoleCommand{colonists[1], static_cast<uint8_t>(DefenseRole::Shelter), true, inside});  // a spot for a Shelter: fails
    sim->step();
    CHECK(std::count_if(sim->events().begin(), sim->events().end(), [](const Event& e) { return e.type == EventType::CommandFailed; }) == 2);
    CHECK(sim->raids().roleOf(colonists[1]) == DefenseRole::Shelter);
    CHECK(sim->raids().roleOf(colonists[0]) == DefenseRole::Defender);

    // The player sounds the alarm with no enemy about: it stays on until the player stops it.
    sim->submit(SoundAlarmCommand{true});
    sim->stepMany(400);
    CHECK(sim->raids().state().alarm);
    CHECK(sim->raids().state().manual);
    RAID_REQUIRE(reg.all_of<Fighter>(colonists[1]));
    CHECK(reg.get<Fighter>(colonists[1]).mode == FightMode::Shelter);
    const Position& sheltered = reg.get<Position>(colonists[1]);
    CHECK(sim->rooms().indoors(cellOf(sheltered)));
    CHECK_FALSE(reg.all_of<Fighter>(colonists[2]));  // keeps working: no enemy near
    RAID_REQUIRE(reg.all_of<Fighter>(colonists[3]));
    CHECK(reg.get<Fighter>(colonists[3]).anchor == CellPos{static_cast<int>(home.x) - 6, static_cast<int>(home.y), 0});
    const Position& atSpot = reg.get<Position>(colonists[3]);
    CHECK(std::hypot(atSpot.x - (home.x - 6.0f), atSpot.y - home.y) < 4.0f);
    CHECK(reg.all_of<Fighter>(colonists[0]));

    Snapshot snap;
    sim->writeSnapshot(snap);
    CHECK(snap.alarm);
    CHECK(snap.alarmManual);
    CHECK(snap.find(colonists[1])->sheltering);
    CHECK(snap.find(colonists[1])->defenseRole == 1);
    CHECK(snap.find(colonists[0])->defending);

    // Drafting a called-up colonist hands it to the player; the rest go back to work when the
    // alarm stops.
    sim->submit(DraftCommand{colonists[0], true});
    sim->step();
    CHECK_FALSE(raidTestFailed(*sim));
    CHECK(reg.get<Fighter>(colonists[0]).drafted);
    sim->submit(SoundAlarmCommand{false});
    sim->stepMany(3);
    CHECK_FALSE(sim->raids().state().alarm);
    CHECK_FALSE(reg.all_of<Fighter>(colonists[1]));
    CHECK_FALSE(reg.all_of<Fighter>(colonists[3]));
    CHECK(reg.all_of<Fighter>(colonists[0]));  // still drafted

    // An enemy within 20 m of the Keep working colonist draws it into the fight.
    const Position worker = reg.get<Position>(colonists[2]);
    const Entity enemy = sim->things().spawn("Human", CellPos{static_cast<int>(worker.x) + 12, static_cast<int>(worker.y), 0});
    raidTestArm(*sim, enemy, "Gun_Revolver");
    sim->submit(SetFactionCommand{enemy, 2});
    bool drawnIn = false;
    for (int i = 0; i < 60 && !drawnIn; ++i) {
        raidTestHeal(*sim, enemy);
        sim->step();
        drawnIn = reg.all_of<Fighter>(colonists[2]);
    }
    CHECK(sim->raids().state().alarm);  // the colony saw it: the alarm sounded by itself
    CHECK(drawnIn);
}

TEST_CASE("Raids: a squad that loses half its pawns routs and runs off the map") {
    SimSettings settings;
    settings.seed = 41;
    settings.mapSize = 96;
    settings.flatMap = true;
    Sim sim(settings, raidTestDefs());
    sim.submit(StartRaidCommand{"Pirate", 400.0f, 2});
    sim.step();
    std::vector<Entity> raiders = raidTestRaiders(sim);
    RAID_REQUIRE(raiders.size() >= 4);
    sim.stepMany(60);
    RAID_REQUIRE(sim.raids().state().squads.size() == 1);
    CHECK(sim.raids().state().squads[0].state == SquadState::Advance);

    // Half of them go down.
    const size_t half = (raiders.size() + 1) / 2;
    for (size_t i = 0; i < half; ++i) {
        sim.submit(DamagePawnCommand{raiders[i], "Bullet", 500.0f, 0});
    }
    bool routed = false;
    for (int i = 0; i < 40 && !routed; ++i) {
        sim.step();
        if (const Event* e = raidTestEvent(sim, EventType::SquadChanged)) {
            routed = e->message == "Rout";
        }
    }
    RAID_REQUIRE(routed);
    const entt::registry& reg = sim.things().registry();
    for (size_t i = half; i < raiders.size(); ++i) {
        CHECK(reg.get<Fighter>(raiders[i]).mode == FightMode::Flee);
    }
    // They run for the nearest edge and leave; the squad is over when nobody is left standing.
    int left = 0;
    for (int i = 0; i < 3000 && !sim.raids().state().squads.empty(); ++i) {
        sim.step();
        for (const Event& e : sim.events()) {
            left += e.type == EventType::PawnLeftMap ? 1 : 0;
        }
    }
    CHECK(left == static_cast<int>(raiders.size() - half));
    CHECK(sim.raids().state().squads.empty());
    for (size_t i = half; i < raiders.size(); ++i) {
        CHECK_FALSE(reg.valid(raiders[i]));
    }
}

TEST_CASE("Raids: a squad whose members are all badly hurt runs, though none is down") {
    SimSettings settings;
    settings.seed = 43;
    settings.mapSize = 96;
    settings.flatMap = true;
    Sim sim(settings, raidTestDefs());
    sim.submit(StartRaidCommand{"Pirate", 400.0f, 2});
    sim.step();
    const std::vector<Entity> raiders = raidTestRaiders(sim);
    RAID_REQUIRE(raiders.size() >= 4);
    sim.stepMany(60);
    RAID_REQUIRE(sim.raids().state().squads.size() == 1);
    CHECK(sim.raids().state().squads[0].state == SquadState::Advance);
    // Everyone is hurt but on their feet: pain and a limp, nobody downed.
    bool routed = false;
    for (int i = 0; i < 90 && !routed; ++i) {
        for (const Entity r : raiders) {
            Health& h = sim.things().registry().get<Health>(r);
            h.pain = 0.5f;
            h.movingFactor = 0.5f;
        }
        sim.step();
        if (const Event* e = raidTestEvent(sim, EventType::SquadChanged)) {
            routed = e->message == "Rout";
        }
    }
    CHECK(routed);
    for (const Entity r : raiders) {
        CHECK_FALSE(sim.health().incapacitated(r));
    }
}

TEST_CASE("Raids: a squad of unhurt pawns doesn't run") {
    SimSettings settings;
    settings.seed = 43;
    settings.mapSize = 96;
    settings.flatMap = true;
    Sim sim(settings, raidTestDefs());
    sim.submit(StartRaidCommand{"Pirate", 400.0f, 2});
    sim.step();
    sim.stepMany(150);
    RAID_REQUIRE(sim.raids().state().squads.size() == 1);
    CHECK(sim.raids().state().squads[0].state != SquadState::Rout);
}

TEST_CASE("Raids: a lone raider facing a stronger colony falls back") {
    auto sim = raidTestSim(51, 128);
    const std::vector<Entity> colonists = raidTestColonists(*sim);
    RAID_REQUIRE(colonists.size() == 6);
    for (const Entity c : colonists) {
        raidTestArm(*sim, c);
    }
    sim->submit(StartRaidCommand{"Pirate", 40.0f, 0});  // less than any kind costs: one pawn
    sim->step();
    const std::vector<Entity> raiders = raidTestRaiders(*sim);
    RAID_REQUIRE(raiders.size() == 1);
    bool fellBack = false;
    for (int i = 0; i < 2400 && !fellBack; ++i) {
        raidTestHeal(*sim, raiders[0]);
        sim->step();
        fellBack = !sim->raids().state().squads.empty() && sim->raids().state().squads[0].state == SquadState::FallBack;
    }
    CHECK(fellBack);
    if (fellBack) {
        CHECK(sim->things().registry().get<Fighter>(raiders[0]).retreat);
    }
}

TEST_CASE("Raids: a raid is the same after a save and load, and through a replay") {
    auto sim = raidTestSim(61, 128);
    for (const Entity c : raidTestColonists(*sim)) {
        raidTestArm(*sim, c);
    }
    sim->submit(SetDefenseRoleCommand{raidTestColonists(*sim)[1], static_cast<uint8_t>(DefenseRole::KeepWorking), false, CellPos{}});
    sim->submit(StartRaidCommand{"Pirate", 200.0f, -1});
    sim->stepMany(500);
    const std::vector<uint8_t> bytes = saveSim(*sim);
    SaveLoadResult loaded = loadSim(bytes, raidTestDefs());
    RAID_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->stateHash() == sim->stateHash());
    sim->submit(SoundAlarmCommand{true});
    loaded.sim->submit(SoundAlarmCommand{true});
    for (int i = 0; i < 600; ++i) {
        sim->step();
        loaded.sim->step();
        if (sim->stateHash() != loaded.sim->stateHash()) {
            MESSAGE("hashes differ " << i << " steps after loading");
            CHECK(false);
            break;
        }
    }
    CHECK(saveToJson(*sim).find("\"raids\"") != std::string::npos);

}

TEST_CASE("Raids: the raid commands go through a replay file and play back the same game") {
    auto sim = raidTestSim(71, 128);
    sim->submit(SetDefenseRoleCommand{raidTestColonists(*sim)[2], static_cast<uint8_t>(DefenseRole::Shelter), false, CellPos{}});
    sim->submit(StartRaidCommand{"Pirate", 150.0f, 2});
    sim->stepMany(200);
    sim->submit(SoundAlarmCommand{true});
    sim->submit(MarkShelterCommand{CellPos{1, 1, 0}, true});  // fails: nothing marked; failed ones are replayed too
    sim->stepMany(300);
    const std::string text = writeReplay(recordReplay(*sim));
    for (const char* name : {"StartRaid", "SetDefenseRole", "SoundAlarm", "MarkShelter"}) {
        CHECK(text.find(name) != std::string::npos);
    }
    Replay back;
    std::string error;
    RAID_REQUIRE(readReplay(text, back, error));
    Sim played(back.settings, raidTestDefs());
    playReplay(back, played, sim->time().seconds);
    CHECK(played.stateHash() == sim->stateHash());
}

TEST_CASE("Raids: wild animals near the colony don't sound the alarm and aren't a raid's target") {
    auto sim = raidTestSim(81, 128);
    const std::vector<Entity> colonists = raidTestColonists(*sim);
    RAID_REQUIRE(colonists.size() == 6);
    const Position& at = sim->things().registry().get<Position>(colonists[0]);
    sim->submit(SpawnThingCommand{"Deer", CellPos{static_cast<int>(at.x) + 4, static_cast<int>(at.y), 0}});
    sim->stepMany(120);
    CHECK_FALSE(raidTestFailed(*sim));
    CHECK_FALSE(sim->raids().state().alarm);
    // A raid's target is still one of the colonists, not the deer.
    sim->submit(StartRaidCommand{"Pirate", 100.0f, 0});
    sim->stepMany(40);
    RAID_REQUIRE(!sim->raids().state().squads.empty());
    const Squad& squad = sim->raids().state().squads.front();
    RAID_REQUIRE(squad.hasTarget);
    bool onColonist = false;
    for (const Entity c : colonists) {
        const Position& p = sim->things().registry().get<Position>(c);
        onColonist = onColonist || (std::abs(static_cast<int>(p.x) - squad.target.x) <= 1 &&
                                    std::abs(static_cast<int>(p.y) - squad.target.y) <= 1);
    }
    CHECK(onColonist);
}

TEST_CASE("Raids: adaptation falls when a colonist dies or goes down and climbs back, and the difficulty sets its weight") {
    auto sim = raidTestSim();
    RAID_REQUIRE(sim->setupErrors().empty());
    CHECK(sim->raids().state().adaptDays == doctest::Approx(0.0f));
    CHECK(sim->raids().adaptationFactor() == doctest::Approx(0.8f));  // the wiki's start value
    const std::vector<Entity> colonists = raidTestColonists(*sim);
    RAID_REQUIRE(colonists.size() == 6);
    sim->stepMany(10);  // the colony's books are open

    // One goes down: 6 days lost at 6 colonists (wiki: 8 at 1, 6 at 2 and 3, 5 at 11).
    sim->things().registry().get<Health>(colonists[1]).downed = true;
    sim->stepMany(5);
    const float afterDown = sim->raids().state().adaptDays;
    CHECK(afterDown < -4.0f);
    CHECK(afterDown > -8.0f);
    sim->stepMany(20);
    CHECK(sim->raids().state().adaptDays == doctest::Approx(afterDown));  // only once

    // One dies: 30 days lost (the wiki's value from 0 to 11 colonists).
    CHECK(sim->health().kill(colonists[0], DeathCause::BloodLoss));
    sim->stepMany(5);
    CHECK(sim->raids().state().adaptDays == doctest::Approx(afterDown - 30.0f));
    // Less adapted than at the start: the curve gives under 0.8.
    CHECK(sim->raids().adaptationFactor() < 0.5f);

    // The difficulty's impact blends the factor with 1: 40% (Losing is fun) of the way to it.
    sim->raids().setAdaptation(1.0f, 0.4f);
    CHECK(sim->raids().adaptationFactor() > 0.7f);
    CHECK(sim->raids().adaptationFactor() < 0.85f);
    sim->raids().setAdaptation(1.0f, 0.0f);
    CHECK(sim->raids().adaptationFactor() == doctest::Approx(1.0f));
    sim->raids().setAdaptation(1.0f, 1.0f);

    // Half a day on, the days climb by 3 a day while below 0 (halved to 1.5 a 12 hours).
    const float before = sim->raids().state().adaptDays;
    while (sim->time().seconds < 13 * 3600) {
        sim->stepMany(600);
    }
    CHECK(sim->raids().state().adaptDays == doctest::Approx(before + 1.5f));

    // It is saved.
    SaveLoadResult loaded = loadSim(saveSim(*sim), raidTestDefs());
    RAID_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->raids().state().adaptDays == doctest::Approx(sim->raids().state().adaptDays));
    CHECK(loaded.sim->stateHash() == sim->stateHash());
}

TEST_CASE("Raids: adaptation does not climb above 0 in the first 30 days") {
    auto sim = raidTestSim();
    RAID_REQUIRE(sim->setupErrors().empty());
    while (sim->time().seconds < 2 * 86400) {
        sim->stepMany(600);
    }
    CHECK(sim->raids().state().adaptDays == doctest::Approx(0.0f));
}

TEST_CASE("Raids: a colonist too hungry to stand at its post leaves the alarm to eat, and comes back fed") {
    // C25: an infestation kept the alarm on for days and the colonists starved at their posts.
    auto sim = raidTestSim(21, 160);
    const std::vector<Entity> colonists = raidTestColonists(*sim);
    RAID_REQUIRE(colonists.size() == 6);
    for (const Entity c : colonists) {
        raidTestArm(*sim, c);
    }
    sim->stepMany(5);
    sim->submit(SoundAlarmCommand{true});
    sim->stepMany(40);
    entt::registry& reg = sim->things().registry();
    const Entity hungry = colonists[0];
    RAID_REQUIRE(reg.all_of<Fighter>(hungry));
    reg.get<Needs>(hungry).food = 0.05f;  // ravenous: under NeedRules food.ravenousBelow (0.125)
    CHECK(sim->raids().excusedToEat(hungry));
    sim->stepMany(40);
    CHECK_FALSE(reg.all_of<Fighter>(hungry));
    for (size_t i = 1; i < colonists.size(); ++i) {
        CHECK(reg.all_of<Fighter>(colonists[i]));  // the fed stay at their posts
    }
    reg.get<Needs>(hungry).food = 0.9f;
    sim->stepMany(40);
    CHECK(reg.all_of<Fighter>(hungry));
    CHECK(reg.get<Fighter>(hungry).defending);

    // But not with an enemy close by: the year run's hungry colonists ate while a raider shot them from a few metres.
    sim->submit(StartRaidCommand{"Pirate", 100.0f, 3});
    sim->step();
    const std::vector<Entity> raiders = raidTestRaiders(*sim);
    RAID_REQUIRE(!raiders.empty());
    // Put the raider 6 m from the hungry colonist (a question about where pawns stand; no step runs).
    const Position& a = reg.get<Position>(hungry);
    Position& r = reg.get<Position>(raiders[0]);
    const Position away = r;
    r.x = a.x + 6.0f;
    r.y = a.y;
    reg.get<Needs>(hungry).food = 0.05f;
    CHECK_FALSE(sim->raids().excusedToEat(hungry));
    r = away;  // far off again: let off to eat
    CHECK(sim->raids().excusedToEat(hungry));
}

TEST_CASE("Raids: a colonist resting in bed to get over an illness stays in bed when the alarm sounds") {
    // Found by the year run (docs/design/colony-scenario.md "Ninth pass"): a raid two days after plague and malaria
    // called the sick out of bed, and they died fighting at a fraction of their strength. A player leaves them undrafted.
    auto sim = raidTestSim(21, 160);
    const std::vector<Entity> colonists = raidTestColonists(*sim);
    RAID_REQUIRE(colonists.size() == 6);
    for (const Entity c : colonists) {
        raidTestArm(*sim, c);
    }
    entt::registry& reg = sim->things().registry();
    const Entity sick = colonists[0];
    const Position& at = reg.get<Position>(sick);
    sim->things().spawn("Bed", CellPos{static_cast<int>(at.x) + 3, static_cast<int>(at.y), 0});
    sim->submit(ContractDiseaseCommand{sick, "Plague", 0.4f, -1});
    bool resting = false;
    for (int i = 0; i < 1200 && !resting; ++i) {
        sim->step();
        resting = reg.get<Worker>(sick).job.type == JobType::BedRest;
    }
    RAID_REQUIRE(resting);
    sim->submit(SoundAlarmCommand{true});
    sim->stepMany(40);
    CHECK_FALSE(reg.all_of<Fighter>(sick));
    CHECK(reg.get<Worker>(sick).job.type == JobType::BedRest);
    for (size_t i = 1; i < colonists.size(); ++i) {
        CHECK(reg.all_of<Fighter>(colonists[i]));  // the healthy are called up
    }
}

TEST_CASE("Raids: the alarm's home is the middle of the colony's own buildings, not ruins, geysers or brood mounds") {
    auto sim = raidTestSim();
    for (int x = 60; x < 64; ++x) {
        sim->things().spawn("RaidTestWall", CellPos{x, 60, 0});
    }
    float x0 = 0.0f;
    float y0 = 0.0f;
    CHECK(sim->raids().colonyMiddle(x0, y0));
    // The map's own buildings far off: a ruin's ancient walls, a steam geyser and a brood mound.
    for (int y = 100; y < 120; ++y) {
        sim->things().spawn("AncientWall", CellPos{115, y, 0});
    }
    CHECK(sim->things().spawn("SteamGeyser", CellPos{10, 10, 0}) != kNoEntity);
    CHECK(sim->things().spawn("Hive", CellPos{100, 20, 0}) != kNoEntity);
    float x1 = 0.0f;
    float y1 = 0.0f;
    CHECK(sim->raids().colonyMiddle(x1, y1));
    CHECK(x1 == doctest::Approx(x0));
    CHECK(y1 == doctest::Approx(y0));
    // A building of the colony's does move it, and the alarm takes it as home.
    sim->things().spawn("RaidTestWall", CellPos{90, 60, 0});
    float x2 = 0.0f;
    float y2 = 0.0f;
    CHECK(sim->raids().colonyMiddle(x2, y2));
    CHECK(x2 > x1 + 1.0f);
    sim->submit(SoundAlarmCommand{true});
    sim->step();
    CHECK(sim->raids().state().hasHome);
    CHECK(sim->raids().state().homeX == doctest::Approx(x2));
    CHECK(sim->raids().state().homeY == doctest::Approx(y2));
}

namespace {

// A squad of pirates that has just routed (half of them shot down). Returns the ones still running.
std::vector<Entity> raidTestRouted(Sim& sim) {
    sim.submit(StartRaidCommand{"Pirate", 400.0f, 2});
    sim.step();
    std::vector<Entity> raiders = raidTestRaiders(sim);
    if (raiders.size() < 4) {
        return {};
    }
    sim.stepMany(60);
    const size_t half = (raiders.size() + 1) / 2;
    for (size_t i = 0; i < half; ++i) {
        sim.submit(DamagePawnCommand{raiders[i], "Bullet", 500.0f, 0});
    }
    for (int i = 0; i < 40; ++i) {
        sim.step();
    }
    std::vector<Entity> running;
    for (size_t i = half; i < raiders.size(); ++i) {
        const Fighter* f = sim.things().registry().try_get<Fighter>(raiders[i]);
        if (f != nullptr && f->mode == FightMode::Flee) {
            running.push_back(raiders[i]);
        }
    }
    return running;
}

}  // namespace

TEST_CASE("Raids: a routed raider caught by an enemy at its heels turns and fights to the death") {
    // sneed (docs/design/real-time-combat.md): raiders run at half lost, but fight back when caught.
    SimSettings settings;
    settings.seed = 41;
    settings.mapSize = 96;
    settings.flatMap = true;
    Sim sim(settings, raidTestDefs());
    const std::vector<Entity> running = raidTestRouted(sim);
    RAID_REQUIRE(running.size() >= 2);
    entt::registry& reg = sim.things().registry();
    const Entity caught = running[0];
    const Entity far = running[1];
    // A colonist with a club right behind the first; one shooting the second from 20 m.
    const Position& cp = reg.get<Position>(caught);
    const Entity chaser = sim.things().spawn("Human", CellPos{static_cast<int>(cp.x) + 2, static_cast<int>(cp.y), 0});
    raidTestArm(sim, chaser, "Melee_Club");
    const Position& fp = reg.get<Position>(far);
    const Entity shooter = sim.things().spawn("Human", CellPos{static_cast<int>(fp.x), static_cast<int>(fp.y) + 20, 0});
    RAID_REQUIRE(raidTestDist(sim, chaser, caught) <= 4.0f);
    RAID_REQUIRE(raidTestDist(sim, shooter, far) > 4.0f);
    sim.combat().noteHurt(caught, chaser);
    sim.combat().noteHurt(far, shooter);
    CHECK(reg.get<Raider>(caught).cornered);
    CHECK(reg.get<Fighter>(caught).mode == FightMode::Fight);
    CHECK_FALSE(reg.get<Raider>(far).cornered);  // shot from afar: it keeps running
    CHECK(reg.get<Fighter>(far).mode == FightMode::Flee);
    CHECK(sim.raids().corneredCount() == 1);
    // The squad's next thoughts (still routed) don't send it running again, and it doesn't leave the map.
    for (int i = 0; i < 90; ++i) {
        sim.step();
    }
    CHECK(reg.valid(caught));
    CHECK(reg.get<Fighter>(caught).mode == FightMode::Fight);
}

TEST_CASE("Raids: a routed raider walled in with no way off the map fights instead") {
    SimSettings settings;
    settings.seed = 41;
    settings.mapSize = 96;
    settings.flatMap = true;
    Sim sim(settings, raidTestDefs());
    const std::vector<Entity> running = raidTestRouted(sim);
    RAID_REQUIRE(!running.empty());
    entt::registry& reg = sim.things().registry();
    const Entity boxed = running[0];
    // Put it in a closed box of walls in the middle of the map.
    Position& p = reg.get<Position>(boxed);
    p.x = 48.5f;
    p.y = 48.5f;
    reg.get<Mover>(boxed).path.clear();
    reg.get<Mover>(boxed).next = 0;
    for (int y = 46; y <= 50; ++y) {
        for (int x = 46; x <= 50; ++x) {
            if (x == 46 || x == 50 || y == 46 || y == 50) {
                sim.things().spawn("Wall", CellPos{x, y, 0});
            }
        }
    }
    bool walledIn = false;
    for (int i = 0; i < 60; ++i) {
        sim.step();
        for (const Event& e : sim.events()) {
            walledIn = walledIn || (e.type == EventType::PawnAction && e.action == PawnAction::Cornered && e.thing == boxed &&
                                    e.message == "walled in");
        }
    }
    CHECK(walledIn);
    CHECK(reg.get<Raider>(boxed).cornered);
    CHECK(reg.get<Fighter>(boxed).mode == FightMode::Fight);
}

TEST_CASE("Raids: a working colonist hurt by an enemy drops its job: armed it fights back, unarmed it runs") {
    // sneed (docs/design/real-time-combat.md phase 3): anyone shot at goes for cover and shoots back.
    const auto run = [](bool armed, bool silenced, bool medic) {
        auto sim = raidTestSim(71, 192);
        sim->stepMany(5);
        const std::vector<Entity> colonists = raidTestColonists(*sim);
        const Entity colonist = colonists.empty() ? kNoEntity : colonists.front();
        if (colonist == kNoEntity) {
            return std::make_tuple(false, false, false, false);
        }
        entt::registry& reg = sim->things().registry();
        if (Gear* g = reg.try_get<Gear>(colonist); g != nullptr && g->weapon != kNoEntity) {
            sim->combat().unequip(colonist, g->weapon);
        }
        if (armed) {
            raidTestArm(*sim, colonist, "Gun_Revolver");
        }
        if (medic) {
            sim->submit(SetDefenseRoleCommand{colonist, 3, false, CellPos{}});  // Medic
        }
        // An enemy 70 m off, beyond anyone's sight (60 m), so the alarm doesn't sound by itself first.
        const Position& p = reg.get<Position>(colonist);
        const int rx = static_cast<int>(p.x) + 70 < 188 ? static_cast<int>(p.x) + 70 : static_cast<int>(p.x) - 70;
        const Entity raider = sim->things().spawn("Human", CellPos{rx, static_cast<int>(p.y), 0});
        raidTestArm(*sim, raider, "Gun_BoltActionRifle");
        sim->submit(SetFactionCommand{raider, 2});
        sim->step();
        if (silenced) {
            sim->submit(SoundAlarmCommand{true});
            sim->step();
            sim->submit(SoundAlarmCommand{false});  // with an enemy on the map: it stays silent
            sim->step();
        }
        if (reg.all_of<Fighter>(colonist) || reg.all_of<Fighter>(raider) == false) {
            MESSAGE("setup: colonist already fighting or raider not a fighter");
        }
        sim->combat().noteHurt(colonist, raider);
        sim->step();
        bool tookCover = false;
        for (const Event& e : sim->events()) {
            tookCover = tookCover || (e.type == EventType::PawnAction && e.action == PawnAction::TakeCover && e.thing == colonist);
        }
        const Fighter* f = reg.try_get<Fighter>(colonist);
        const bool knows = f != nullptr && std::any_of(f->known.begin(), f->known.end(), [&](const KnownEnemy& k) { return k.pawn == raider; });
        return std::make_tuple(tookCover, f != nullptr && f->mode == FightMode::Fight && knows, f != nullptr && f->mode == FightMode::Shelter,
                               sim->raids().state().alarm);
    };
    const auto [armedCover, armedFights, armedShelters, armedAlarm] = run(true, false, false);
    CHECK(armedCover);
    CHECK(armedFights);  // fights back with its attacker known
    CHECK_FALSE(armedShelters);
    CHECK(armedAlarm);  // being shot sounds the alarm
    const auto [bareCover, bareFights, bareShelters, bareAlarm] = run(false, false, false);
    CHECK(bareCover);
    CHECK_FALSE(bareFights);
    CHECK(bareShelters);  // runs for shelter
    const auto [quietCover, quietFights, quietShelters, quietAlarm] = run(true, true, false);
    CHECK_FALSE(quietCover);  // the player silenced the alarm: left alone
    const auto [medicCover, medicFights, medicShelters, medicAlarm] = run(true, false, true);
    CHECK_FALSE(medicCover);  // a medic keeps to its duty (field care)
    (void)bareAlarm;
    (void)quietFights;
    (void)quietShelters;
    (void)quietAlarm;
    (void)medicFights;
    (void)medicShelters;
    (void)medicAlarm;
}
