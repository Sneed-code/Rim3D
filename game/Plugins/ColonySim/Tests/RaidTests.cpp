#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
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
    // Crashlanded: silver, steel, wood, components, medicine and meals, well under 10,000.
    CHECK(wealth > 4000.0f);
    CHECK(wealth < 10000.0f);
    // Six colonists at 15 points each (wealth under 10,000), day 0 (x0.7), adaptation 0.8.
    CHECK(sim->raids().threatPoints() == doctest::Approx(6 * 15 * 0.7 * 0.8).epsilon(0.001));

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
    auto sim = raidTestSim(24, 160);  // (a few seeds see the raiders hang back; the check below is about the usual case)
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
