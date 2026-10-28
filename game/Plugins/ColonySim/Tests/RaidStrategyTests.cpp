#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Raids.h"
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
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace colony;

#define STRAT_REQUIRE(...)     \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

// These tests check docs/design/raid-strategies.md (task F12).
namespace {

std::shared_ptr<const DefDatabase> stratDefs() {
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

std::unique_ptr<Sim> stratSim(uint64_t seed = 11, int size = 128) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = size;
    settings.flatMap = true;
    settings.scenario = "Crashlanded";
    return std::make_unique<Sim>(settings, stratDefs());
}

std::vector<Entity> stratColonists(Sim& sim) {
    std::vector<Entity> out;
    for (const auto [pawn, colonist] : sim.things().registry().view<const Colonist>().each()) {
        if (sim.fighters().factionOf(pawn) == kPlayerFaction) {
            out.push_back(pawn);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<Entity> stratRaiders(Sim& sim) {
    std::vector<Entity> out;
    for (const Entity pawn : sim.things().registry().view<const Raider>()) {
        out.push_back(pawn);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool stratFailed(const Sim& sim) {
    return std::any_of(sim.events().begin(), sim.events().end(), [](const Event& e) { return e.type == EventType::CommandFailed; });
}

const Event* stratEvent(const Sim& sim, EventType type) {
    for (const Event& e : sim.events()) {
        if (e.type == type) {
            return &e;
        }
    }
    return nullptr;
}

// A closed ring of walls round the colonists, three cells out, with 60 hit points each so sapping is quick.
std::vector<Entity> stratWallRing(Sim& sim) {
    const entt::registry& reg = sim.things().registry();
    int x0 = 1000, y0 = 1000, x1 = -1000, y1 = -1000;
    for (const Entity c : stratColonists(sim)) {
        const CellPos cell = cellOf(reg.get<Position>(c));
        x0 = std::min(x0, cell.x);
        y0 = std::min(y0, cell.y);
        x1 = std::max(x1, cell.x);
        y1 = std::max(y1, cell.y);
    }
    x0 -= 3;
    y0 -= 3;
    x1 += 3;
    y1 += 3;
    std::vector<Entity> walls;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            if (x == x0 || x == x1 || y == y0 || y == y1) {
                const Entity wall = sim.things().spawn("Wall", CellPos{x, y, 0});
                if (wall != kNoEntity) {
                    if (HitPoints* hp = sim.things().registry().try_get<HitPoints>(wall)) {
                        hp->current = 60;
                        hp->max = 60;
                    }
                    walls.push_back(wall);
                }
            }
        }
    }
    return walls;
}

// Takes every loose item off the map, so a bait is the only loot there is.
void stratClearLoot(Sim& sim) {
    std::vector<Entity> items;
    for (const auto [e, thing, pos, stack] : sim.things().registry().view<const Thing, const Position, const StackCount>().each()) {
        if (thing.category == ThingCategory::Item) {
            items.push_back(e);
        }
    }
    for (const Entity e : items) {
        sim.things().destroy(e);
    }
}

int stratStanding(const Sim& sim, const std::vector<Entity>& things) {
    int n = 0;
    for (const Entity e : things) {
        n += sim.things().exists(e) ? 1 : 0;
    }
    return n;
}

}  // namespace

TEST_CASE("Raid strategies: names round-trip and bad commands fail") {
    for (int i = 0; i < kRaidStrategyCount; ++i) {
        RaidStrategy back = RaidStrategy::Assault;
        CHECK(raidStrategyFromName(raidStrategyName(static_cast<RaidStrategy>(i)), back));
        CHECK(static_cast<int>(back) == i);
    }
    RaidStrategy ignored = RaidStrategy::Assault;
    CHECK_FALSE(raidStrategyFromName("Siege", ignored));

    auto sim = stratSim();
    sim->submit(StartStrategyRaidCommand{"Pirate", 100.0f, -1, "Siege", -1.0f});     // no such strategy
    sim->submit(StartStrategyRaidCommand{"Pirate", 100.0f, -1, "Assault", 30.0f});   // no such hour
    sim->submit(StartStrategyRaidCommand{"Nobody", 100.0f, -1, "Assault", -1.0f});   // no such faction
    sim->step();
    CHECK(std::count_if(sim->events().begin(), sim->events().end(), [](const Event& e) { return e.type == EventType::CommandFailed; }) == 3);
    CHECK(sim->raids().state().squads.empty());
}

TEST_CASE("Raid strategies: Auto picks sappers for a walled colony and never drop pods for a small raid") {
    auto open = stratSim(12, 128);
    int sappers = 0;
    int pods = 0;
    for (int i = 0; i < 60; ++i) {
        const RaidStrategy s = open->raids().pickStrategy(60.0f);
        sappers += s == RaidStrategy::Sappers ? 1 : 0;
        pods += s == RaidStrategy::DropPods ? 1 : 0;
    }
    CHECK(pods == 0);          // under dropPodsMinPoints
    CHECK(sappers > 0);        // 12 of 98 weight: 60 draws miss it with chance under 1 in 100000
    CHECK(sappers < 30);

    auto walled = stratSim(13, 128);
    stratWallRing(*walled);
    walled->step();
    // Walls shut out everything but sappers and pods, and sappers are much likelier.
    int dug = 0;
    for (int i = 0; i < 40; ++i) {
        const RaidStrategy s = walled->raids().pickStrategy(300.0f);
        CHECK((s == RaidStrategy::Sappers || s == RaidStrategy::DropPods));
        dug += s == RaidStrategy::Sappers ? 1 : 0;
    }
    CHECK(dug > 20);
    // A plain assault has no way in.
    walled->submit(StartRaidCommand{"Pirate", 100.0f, 3});
    walled->step();
    CHECK(stratFailed(*walled));
}

TEST_CASE("Raid strategies: drop pods land the raiders round the colony, walls or not") {
    auto sim = stratSim(21, 160);
    stratWallRing(*sim);
    sim->step();
    sim->submit(StartStrategyRaidCommand{"Pirate", 300.0f, -1, "DropPods", -1.0f});
    sim->step();
    STRAT_REQUIRE(!stratFailed(*sim));
    const std::vector<Entity> raiders = stratRaiders(*sim);
    STRAT_REQUIRE(raiders.size() >= 3);
    int landed = 0;
    for (const Event& e : sim->events()) {
        landed += e.type == EventType::DropPodLanded ? 1 : 0;
    }
    CHECK(landed == static_cast<int>(raiders.size()));
    STRAT_REQUIRE(sim->raids().state().squads.size() == 1);
    const Squad& squad = sim->raids().state().squads[0];
    CHECK(squad.strategy == RaidStrategy::DropPods);
    // Every pod is 8 to 24 m from the squad's target (the colonist nearest the map's middle).
    const entt::registry& reg = sim->things().registry();
    for (const Entity r : raiders) {
        const Position& p = reg.get<Position>(r);
        const float d = std::hypot(p.x - (static_cast<float>(squad.target.x) + 0.5f), p.y - (static_cast<float>(squad.target.y) + 0.5f));
        CHECK(d >= 7.9f);
        CHECK(d <= 24.6f);
    }
    Snapshot snap;
    sim->writeSnapshot(snap);
    STRAT_REQUIRE(snap.squads.size() == 1);
    CHECK(snap.squads[0].strategy == static_cast<uint8_t>(RaidStrategy::DropPods));
}

TEST_CASE("Raid strategies: sappers dig through the wall that shuts the colony in") {
    auto sim = stratSim(31, 128);
    const std::vector<Entity> walls = stratWallRing(*sim);
    STRAT_REQUIRE(!walls.empty());
    sim->step();
    sim->submit(StartStrategyRaidCommand{"Pirate", 200.0f, 3, "Sappers", -1.0f});
    sim->step();
    STRAT_REQUIRE(!stratFailed(*sim));
    STRAT_REQUIRE(sim->raids().state().squads.size() == 1);
    CHECK(sim->raids().state().squads[0].strategy == RaidStrategy::Sappers);
    const int before = stratStanding(*sim, walls);

    bool breaking = false;
    bool snapshotShowsIt = false;
    for (int i = 0; i < 3000 && stratStanding(*sim, walls) == before; ++i) {
        sim->step();
        breaking = breaking || stratEvent(*sim, EventType::SappersBreaking) != nullptr;
        if (!snapshotShowsIt && i % 50 == 0) {
            Snapshot snap;
            sim->writeSnapshot(snap);
            snapshotShowsIt = !snap.squads.empty() && snap.squads[0].breaching;
        }
    }
    CHECK(breaking);
    CHECK(snapshotShowsIt);
    CHECK(stratStanding(*sim, walls) < before);  // a wall was dug out
}

TEST_CASE("Raid strategies: a night raid waits for its hour, and a save made while it waits plays on the same") {
    auto sim = stratSim(41, 128);
    // 0.05 h is 180 game seconds.
    sim->submit(StartStrategyRaidCommand{"Pirate", 100.0f, 3, "Assault", 0.05f});
    sim->stepMany(100);
    CHECK(sim->raids().state().squads.empty());
    STRAT_REQUIRE(sim->raids().state().pending.size() == 1);
    CHECK(sim->raids().state().pending[0].at == 180);

    const std::vector<uint8_t> bytes = saveSim(*sim);
    SaveLoadResult loaded = loadSim(bytes, stratDefs());
    STRAT_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->stateHash() == sim->stateHash());
    CHECK(loaded.sim->raids().state().pending.size() == 1);

    bool arrived = false;
    for (int i = 0; i < 200; ++i) {
        sim->step();
        loaded.sim->step();
        CHECK(sim->stateHash() == loaded.sim->stateHash());
        if (const Event* e = stratEvent(*sim, EventType::RaidArrived)) {
            arrived = true;
            CHECK(sim->time().seconds >= 180);
            CHECK(e->cell.x <= 10);
        }
    }
    CHECK(arrived);
    CHECK(sim->raids().state().pending.empty());
    CHECK(sim->raids().state().squads.size() == 1);
}

TEST_CASE("Raid strategies: thieves lift the most valuable loose item and leave the map with it") {
    auto sim = stratSim(51, 128);
    // 300 silver near the west edge, where the raid walks in; the colonists are in the middle.
    stratClearLoot(*sim);
    const Entity silver = sim->things().spawn("Silver", CellPos{24, 64, 0}, 300);
    STRAT_REQUIRE(silver != kNoEntity);
    sim->submit(StartStrategyRaidCommand{"Pirate", 60.0f, 3, "Thieves", -1.0f});
    sim->step();
    STRAT_REQUIRE(!stratFailed(*sim));
    STRAT_REQUIRE(sim->raids().state().squads.size() == 1);
    CHECK(sim->raids().state().squads[0].strategy == RaidStrategy::Thieves);

    bool stolen = false;
    bool left = false;
    for (int i = 0; i < 1500 && !left; ++i) {
        sim->step();
        if (const Event* e = stratEvent(*sim, EventType::ItemStolen)) {
            stolen = true;
            CHECK(e->count > 0);
            CHECK(e->def == sim->defs().idOf("Things", "Silver"));
        }
        left = left || stratEvent(*sim, EventType::PawnLeftMap) != nullptr;
    }
    CHECK(stolen);
    CHECK(left);
    // The silver is gone from the map: the thief took it all (300 silver weigh far less than a thief carries).
    int silverLeft = 0;
    const DefId silverDef = sim->defs().idOf("Things", "Silver");
    for (const auto [e, thing, pos, stack] : sim->things().registry().view<const Thing, const Position, const StackCount>().each()) {
        silverLeft += thing.def == silverDef && e != kNoEntity ? stack.count : 0;
    }
    CHECK(silverLeft == 0);
}

TEST_CASE("Raid strategies: kidnappers carry a downed colonist off the map, and the colony can't rescue it") {
    auto sim = stratSim(62, 128);
    const std::vector<Entity> colonists = stratColonists(*sim);
    STRAT_REQUIRE(colonists.size() == 6);
    entt::registry& reg = sim->things().registry();
    // Down a colonist (not the protagonist) and leave it near the west edge, ahead of the raid.
    Entity victim = kNoEntity;
    for (const Entity c : colonists) {
        if (!reg.get<Colonist>(c).protagonist) {
            victim = c;
            break;
        }
    }
    STRAT_REQUIRE(victim != kNoEntity);
    sim->step();
    for (int i = 0; i < 200 && !sim->health().incapacitated(victim); ++i) {
        sim->submit(DamagePawnCommand{victim, "Blunt", 4.0f, -1});
        sim->step();
    }
    STRAT_REQUIRE(sim->health().incapacitated(victim));
    STRAT_REQUIRE(!reg.get<Health>(victim).dead);
    Position& vp = reg.get<Position>(victim);
    vp.x = 20.5f;
    vp.y = 64.5f;

    sim->submit(StartStrategyRaidCommand{"Pirate", 120.0f, 3, "Kidnappers", -1.0f});
    sim->step();
    STRAT_REQUIRE(!stratFailed(*sim));
    bool lifted = false;
    bool gone = false;
    for (int i = 0; i < 2000 && !gone; ++i) {
        sim->step();
        for (const Event& e : sim->events()) {
            if (e.type == EventType::PawnKidnapped && e.message == "lifted") {
                lifted = true;
                CHECK(e.target == victim);
            }
            if (e.type == EventType::PawnKidnapped && e.message == "left the map") {
                gone = true;
            }
        }
        if (lifted && !gone) {
            // While it is carried it is the kidnapper's: it stays with it and is not shot.
            CHECK(reg.all_of<Kidnapped>(victim));
            CHECK_FALSE(reg.get<Health>(victim).dead);
        }
    }
    CHECK(lifted);
    CHECK(gone);
    CHECK_FALSE(sim->things().exists(victim));
}

TEST_CASE("Raid strategies: the commands go through a replay file and play back the same game") {
    auto sim = stratSim(71, 128);
    sim->submit(StartStrategyRaidCommand{"Pirate", 150.0f, 2, "Thieves", -1.0f});
    sim->submit(StartStrategyRaidCommand{"Pirate", 150.0f, 1, "Assault", 0.1f});
    sim->stepMany(400);
    const std::string text = writeReplay(recordReplay(*sim));
    CHECK(text.find("StartStrategyRaid") != std::string::npos);
    Replay back;
    std::string error;
    STRAT_REQUIRE(readReplay(text, back, error));
    Sim played(back.settings, stratDefs());
    playReplay(back, played, sim->time().seconds);
    CHECK(played.stateHash() == sim->stateHash());
}

TEST_CASE("Raid strategies: a save in the middle of a theft loads and plays on the same") {
    auto sim = stratSim(81, 128);
    stratClearLoot(*sim);
    sim->things().spawn("Silver", CellPos{24, 64, 0}, 100);
    sim->submit(StartStrategyRaidCommand{"Pirate", 60.0f, 3, "Thieves", -1.0f});
    // Run until a thief carries something.
    bool carrying = false;
    for (int i = 0; i < 1500 && !carrying; ++i) {
        sim->step();
        for (const Entity r : stratRaiders(*sim)) {
            carrying = carrying || sim->things().registry().get<Raider>(r).loot != kNoEntity;
        }
    }
    STRAT_REQUIRE(carrying);
    const std::vector<uint8_t> bytes = saveSim(*sim);
    SaveLoadResult loaded = loadSim(bytes, stratDefs());
    STRAT_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->stateHash() == sim->stateHash());
    for (int i = 0; i < 300; ++i) {
        sim->step();
        loaded.sim->step();
        if (sim->stateHash() != loaded.sim->stateHash()) {
            MESSAGE("hashes differ " << i << " steps after loading");
            CHECK(false);
            break;
        }
    }
}
