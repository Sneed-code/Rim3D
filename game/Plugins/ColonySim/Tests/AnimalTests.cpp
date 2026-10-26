#include "ColonySim/Animals.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Looks.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define ANIMAL_REQUIRE(...)    \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// The game's real defs, with one text swapped in the AnimalRules file when `from` is given (a test
// that wants an animal to leave at once makes the chance 1).
std::shared_ptr<const DefDatabase> animalTestDefs(const std::string& from = std::string(), const std::string& to = std::string()) {
    namespace fs = std::filesystem;
    std::vector<DefSource> sources;
    std::error_code ec;
    const fs::path root = fs::path(COLONY_DATA_DIR) / "defs";
    for (const fs::directory_entry& kindDir : fs::directory_iterator(root, ec)) {
        if (!kindDir.is_directory()) {
            continue;
        }
        for (const fs::directory_entry& file : fs::directory_iterator(kindDir.path(), ec)) {
            if (file.path().extension() != ".json") {
                continue;
            }
            std::ifstream in(file.path(), std::ios::binary);
            std::stringstream text;
            text << in.rdbuf();
            std::string body = text.str();
            if (!from.empty() && file.path().filename() == "AnimalRules.json") {
                const size_t at = body.find(from);
                CHECK(at != std::string::npos);
                if (at != std::string::npos) {
                    body.replace(at, from.size(), to);
                }
            }
            sources.push_back({kindDir.path().filename().string(), file.path().filename().string(), body});
        }
    }
    std::string tuning;
    {
        std::ifstream in(fs::path(COLONY_DATA_DIR) / "tuning.json", std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        tuning = text.str();
    }
    auto db = std::make_shared<DefDatabase>();
    std::vector<std::string> errors;
    DefDatabase::build(sources, tuning, *db, errors);
    CHECK(errors.empty());
    return db;
}

SimSettings animalTestSettings(int size, int wild, uint64_t seed = 5) {
    SimSettings s;
    s.seed = seed;
    s.mapSize = size;
    s.wildAnimals = wild;
    s.checkpointEvery = 0;
    return s;
}

// A small flat all-soil map with no plants at all, so a test places exactly what it wants.
SimSettings animalFlat(int size = 64) {
    SimSettings s = animalTestSettings(size, 0);
    s.flatMap = true;
    return s;
}

void animalClearPlants(Sim& sim) {
    for (int y = 0; y < sim.map().height(); ++y) {
        for (int x = 0; x < sim.map().width(); ++x) {
            sim.plants().remove(CellPos{x, y, 0});
        }
    }
}

// Steps an empty sim to the start of a game hour, so a test can place its animals by day.
void animalGoToHour(Sim& sim, int hour) {
    sim.stepMany(static_cast<int64_t>(hour) * kSecondsPerHour - sim.time().seconds);
}

std::vector<Entity> animalsOf(const Sim& sim) {
    std::vector<Entity> out;
    for (const Entity e : sim.things().registry().view<const Animal>()) {
        out.push_back(e);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string animalName(const Sim& sim, Entity e) {
    return sim.defs().get(sim.things().registry().get<Thing>(e).def).defName();
}

}  // namespace

TEST_CASE("Animals: a spawned deer is a pawn with a body and a side, and no job") {
    Sim sim(animalFlat(), animalTestDefs());
    sim.submit(SpawnThingCommand{"Deer", CellPos{20, 20, 0}});
    sim.step();
    const std::vector<Entity> animals = animalsOf(sim);
    ANIMAL_REQUIRE(animals.size() == 1);
    const entt::registry& reg = sim.things().registry();
    const Entity deer = animals[0];
    CHECK(reg.all_of<Health, Mover, Posture>(deer));
    CHECK_FALSE(reg.all_of<Worker>(deer));    // the job system leaves it alone
    CHECK_FALSE(reg.all_of<Colonist>(deer));
    CHECK_FALSE(reg.all_of<PawnLooks>(deer));  // not humanlike

    sim.stepMany(5);
    ANIMAL_REQUIRE(reg.all_of<Faction>(deer));
    CHECK(reg.get<Faction>(deer).id == kWildFaction);
    CHECK(sim.fighters().factionOf(deer) == kWildFaction);

    Snapshot snap;
    sim.writeSnapshot(snap);
    const ThingView* view = snap.find(deer);
    ANIMAL_REQUIRE(view != nullptr);
    CHECK(view->category == ThingCategory::Pawn);
    CHECK(view->lifeStage == 2);  // a spawned animal is an adult
    CHECK(view->bodySize == doctest::Approx(1.2f));
    CHECK(view->food > 0.0f);
    CHECK(view->food <= 1.0f);
    CHECK(view->faction == kWildFaction);
    CHECK(sim.animals().lifeStage(deer) == 2);
    CHECK(sim.animals().lifeStage(kNoEntity) == -1);
    CHECK(sim.animals().livingCount() == 1);
}

TEST_CASE("Animals: the map is filled with herds by the biome's density and species") {
    Sim sim(animalTestSettings(256, 100), animalTestDefs());
    const AnimalSystem& animals = sim.animals();
    ANIMAL_REQUIRE(animals.populated());
    CHECK(animals.wildWeight() >= animals.targetWeight());
    CHECK(animals.wildWeight() < animals.targetWeight() + 25.0f);  // one more group at most
    CHECK(animals.livingCount() > 30);

    // Only the temperate forest's wild animals, never domestic or zero-commonality ones.
    const std::set<std::string> allowed = {"Hare", "Squirrel", "Deer", "WildBoar", "Muffalo", "Alpaca",
                                           "TimberWolf", "GrizzlyBear", "Cougar"};
    std::set<std::string> seen;
    std::map<uint32_t, int> herdSizes;
    const entt::registry& reg = sim.things().registry();
    for (const Entity e : animalsOf(sim)) {
        const std::string name = animalName(sim, e);
        CHECK_MESSAGE(allowed.count(name) == 1, name);
        seen.insert(name);
        const Animal& a = reg.get<Animal>(e);
        CHECK(a.wild);
        CHECK(sim.map().walkable(cellOf(reg.get<Position>(e))));
        CHECK(sim.animals().lifeStage(e) == 2);  // placed animals are grown
        if (a.herd != 0) {
            ++herdSizes[a.herd];
        }
    }
    CHECK(seen.size() >= 4);
    CHECK(seen.count("Deer") + seen.count("Hare") + seen.count("WildBoar") > 0);
    ANIMAL_REQUIRE(!herdSizes.empty());
    for (const auto& [herd, size] : herdSizes) {
        (void)herd;
        CHECK(size >= 1);
        CHECK(size <= 10);  // the biggest herd in the defs (muffalo)
    }

    // A tenth of the density gives about a tenth of the animals; none gives none.
    Sim sparse(animalTestSettings(256, 10), animalTestDefs());
    CHECK(sparse.animals().livingCount() > 0);
    CHECK(sparse.animals().livingCount() < sim.animals().livingCount() / 3);
    Sim none(animalTestSettings(256, 0), animalTestDefs());
    CHECK(none.animals().livingCount() == 0);
    CHECK_FALSE(none.animals().populated());
}

TEST_CASE("Animals: no animal starts near a colonist") {
    SimSettings settings = animalTestSettings(256, 300);
    settings.scenario = "Crashlanded";
    Sim sim(settings, animalTestDefs());
    ANIMAL_REQUIRE(sim.setupErrors().empty());
    const entt::registry& reg = sim.things().registry();
    ANIMAL_REQUIRE(!reg.view<const Colonist>().empty());
    ANIMAL_REQUIRE(sim.animals().livingCount() > 0);
    const float clear = sim.animals().rules().startClearRadius;
    for (const Entity colonist : reg.view<const Colonist>()) {
        const Position& c = reg.get<Position>(colonist);
        for (const Entity animal : animalsOf(sim)) {
            const Position& p = reg.get<Position>(animal);
            const float d2 = (p.x - c.x) * (p.x - c.x) + (p.y - c.y) * (p.y - c.y);
            CHECK(d2 >= clear * clear - 1.0f);
        }
    }
}

TEST_CASE("Animals: the same seed gives the same animals, another seed other ones") {
    const auto defs = animalTestDefs();
    Sim a(animalTestSettings(128, 100, 11), defs);
    Sim b(animalTestSettings(128, 100, 11), defs);
    Sim c(animalTestSettings(128, 100, 12), defs);
    CHECK(a.stateHash() == b.stateHash());
    CHECK(a.stateHash() != c.stateHash());
    a.stepMany(6 * kSecondsPerHour);
    b.stepMany(6 * kSecondsPerHour);
    CHECK(a.stateHash() == b.stateHash());
    CHECK(a.animals().livingCount() > 0);
}

TEST_CASE("Animals: by night the day animals sleep and the night animals do not") {
    Sim sim(animalTestSettings(256, 100), animalTestDefs());
    sim.stepMany(300);  // second 300 is just past midnight
    int dayAsleep = 0;
    int dayAwake = 0;
    int nightAsleep = 0;
    int nightAwake = 0;
    for (const Entity e : animalsOf(sim)) {
        const std::string name = animalName(sim, e);
        const bool nocturnal = name == "TimberWolf" || name == "Cougar";
        const bool asleep = sim.animals().asleep(e);
        (nocturnal ? (asleep ? nightAsleep : nightAwake) : (asleep ? dayAsleep : dayAwake)) += 1;
    }
    CHECK(dayAsleep > 0);
    CHECK(dayAwake == 0);
    CHECK(nightAsleep == 0);

    // A sleeper is lying still and the snapshot says so.
    Snapshot snap;
    sim.writeSnapshot(snap);
    int still = 0;
    for (const Entity e : animalsOf(sim)) {
        const ThingView* v = snap.find(e);
        ANIMAL_REQUIRE(v != nullptr);
        if (v->asleep) {
            still += v->vx == 0.0f && v->vy == 0.0f ? 1 : 0;
        }
    }
    CHECK(still == dayAsleep);
    (void)nightAwake;

    // By day the sleepers are up (the sleep hours end by 07:00) and the herds have moved.
    std::vector<Position> before;
    for (const Entity e : animalsOf(sim)) {
        before.push_back(sim.things().registry().get<Position>(e));
    }
    animalGoToHour(sim, 10);
    int awake = 0;
    int moved = 0;
    const std::vector<Entity> animals = animalsOf(sim);
    for (size_t i = 0; i < animals.size(); ++i) {
        const std::string name = animalName(sim, animals[i]);
        if (name != "TimberWolf" && name != "Cougar") {
            awake += sim.animals().asleep(animals[i]) ? 0 : 1;
        }
        const Position& p = sim.things().registry().get<Position>(animals[i]);
        moved += (p.x != before[i].x || p.y != before[i].y) ? 1 : 0;
    }
    CHECK(awake > 0);
    CHECK(moved > static_cast<int>(animals.size()) / 2);
}

TEST_CASE("Animals: a hungry deer walks to a plant and eats it") {
    Sim sim(animalFlat(), animalTestDefs());
    animalClearPlants(sim);
    animalGoToHour(sim, 9);
    const Entity deer = sim.things().spawn("Deer", CellPos{20, 20, 0});
    ANIMAL_REQUIRE(deer != kNoEntity);
    sim.stepMany(60);  // its first look sets it up
    Animal& a = sim.things().registry().get<Animal>(deer);
    ANIMAL_REQUIRE(a.started);
    a.food = 0.0f;
    const CellPos grass{26, 20, 0};
    ANIMAL_REQUIRE(sim.plants().place(grass, sim.defs().idOf("Things", "Plant_Grass"), 1.0f));

    bool grazedEvent = false;
    for (int i = 0; i < 400 && sim.plants().has(grass); ++i) {
        sim.step();
        for (const Event& e : sim.events()) {
            grazedEvent = grazedEvent || (e.type == EventType::PlantRemoved && e.message == "grazed" && e.cell == grass);
        }
    }
    CHECK_FALSE(sim.plants().has(grass));
    CHECK(grazedEvent);
    CHECK(sim.animals().stats().grazed == 1);
    CHECK(sim.things().registry().get<Animal>(deer).food > 0.4f);  // grass gives 0.5 nutrition
    CHECK(sim.animals().foodShare(deer) > 0.3f);
}

TEST_CASE("Animals: a fed deer leaves plants alone, and wolves never graze") {
    Sim sim(animalFlat(), animalTestDefs());
    animalClearPlants(sim);
    animalGoToHour(sim, 9);
    const Entity deer = sim.things().spawn("Deer", CellPos{20, 20, 0});
    const Entity wolf = sim.things().spawn("TimberWolf", CellPos{40, 40, 0});
    ANIMAL_REQUIRE(deer != kNoEntity);
    ANIMAL_REQUIRE(wolf != kNoEntity);
    sim.stepMany(60);
    sim.things().registry().get<Animal>(deer).food = 1.2f;  // full
    for (int dy = -3; dy <= 3; ++dy) {
        for (int dx = -3; dx <= 3; ++dx) {
            sim.plants().place(CellPos{20 + dx, 22 + dy, 0}, sim.defs().idOf("Things", "Plant_Grass"), 1.0f);
            sim.plants().place(CellPos{40 + dx, 42 + dy, 0}, sim.defs().idOf("Things", "Plant_Grass"), 1.0f);
        }
    }
    const size_t before = sim.plants().count();
    sim.stepMany(600);
    // The deer fell from 1.0 of its capacity by little in ten minutes; the wolf hunts when it is hungry (N5) but never grazes.
    CHECK(sim.plants().count() == before);
    CHECK(sim.animals().stats().grazed == 0);
    CHECK(sim.animals().foodShare(wolf) < 1.0f);  // wild predators get hungry too
}

TEST_CASE("Animals: a herd stays together") {
    Sim sim(animalFlat(96), animalTestDefs());
    animalClearPlants(sim);
    animalGoToHour(sim, 9);
    std::vector<Entity> herd;
    for (int i = 0; i < 6; ++i) {
        const Entity e = sim.things().spawn("Muffalo", CellPos{40 + i, 40, 0});
        ANIMAL_REQUIRE(e != kNoEntity);
        sim.things().registry().get<Animal>(e).herd = 7;
        herd.push_back(e);
    }
    CHECK(sim.animals().leaderOf(herd[3]) == herd[0]);  // the lowest id
    sim.stepMany(1800);
    // Half an hour on, they have wandered but every one is within the herd's pull of the leader.
    const entt::registry& reg = sim.things().registry();
    const Position& lead = reg.get<Position>(herd[0]);
    float widest = 0.0f;
    for (const Entity e : herd) {
        const Position& p = reg.get<Position>(e);
        widest = std::max(widest, std::hypot(p.x - lead.x, p.y - lead.y));
    }
    CHECK(widest < 25.0f);
    const Position start{40.5f, 40.5f, 0};
    CHECK(std::hypot(lead.x - start.x, lead.y - start.y) > 2.0f);  // the leader did wander

    // When the leader dies the next one leads.
    sim.health().kill(herd[0], DeathCause::BloodLoss);
    CHECK(sim.animals().leaderOf(herd[3]) == herd[1]);
}

TEST_CASE("Animals: a wild herd that falls short is refilled from the edges") {
    Sim sim(animalTestSettings(128, 100, 3), animalTestDefs());
    ANIMAL_REQUIRE(sim.animals().populated());
    const std::vector<Entity> animals = animalsOf(sim);
    for (size_t i = 0; i < animals.size(); ++i) {
        if (i % 5 != 0) {
            sim.things().destroy(animals[i]);  // four in five are gone
        }
    }
    const float low = sim.animals().wildWeight();
    CHECK(low < sim.animals().targetWeight() * 0.5f);
    sim.stepMany(36 * kSecondsPerHour);
    CHECK(sim.animals().stats().arrived > 0);
    CHECK(sim.animals().wildWeight() > low);
}

TEST_CASE("Animals: an animal by the edge can walk off the map") {
    // With the leave chance at 1 every animal within reach of an edge goes at its next decision.
    const auto defs = animalTestDefs("\"leaveChance\": 0.001", "\"leaveChance\": 1");
    Sim sim(animalFlat(64), defs);
    animalClearPlants(sim);
    animalGoToHour(sim, 9);
    const Entity near = sim.things().spawn("Deer", CellPos{3, 30, 0});
    const Entity far = sim.things().spawn("Deer", CellPos{32, 32, 0});
    ANIMAL_REQUIRE(near != kNoEntity);
    ANIMAL_REQUIRE(far != kNoEntity);
    sim.stepMany(300);
    CHECK_FALSE(sim.things().exists(near));
    CHECK(sim.things().exists(far));
    CHECK(sim.animals().stats().left == 1);
}

TEST_CASE("Animals: a corpse lies where it fell and then rots away") {
    const auto defs = animalTestDefs("\"corpseDays\": 15", "\"corpseDays\": 1");
    Sim sim(animalFlat(64), defs);
    animalGoToHour(sim, 9);
    const Entity deer = sim.things().spawn("Deer", CellPos{30, 30, 0});
    ANIMAL_REQUIRE(deer != kNoEntity);
    sim.stepMany(60);
    ANIMAL_REQUIRE(sim.health().kill(deer, DeathCause::BloodLoss));
    sim.stepMany(kSecondsPerDay / 2);
    ANIMAL_REQUIRE(sim.things().exists(deer));
    Snapshot snap;
    sim.writeSnapshot(snap);
    const ThingView* view = snap.find(deer);
    ANIMAL_REQUIRE(view != nullptr);
    CHECK(view->dead);
    CHECK(sim.animals().livingCount() == 0);
    const Position where = sim.things().registry().get<Position>(deer);
    sim.stepMany(kSecondsPerDay);
    CHECK_FALSE(sim.things().exists(deer));
    CHECK(sim.animals().stats().cleared == 1);
    (void)where;
}

TEST_CASE("Animals: hostile fighters take no animal for an enemy") {
    Sim sim(animalFlat(), animalTestDefs());
    animalClearPlants(sim);
    animalGoToHour(sim, 9);
    const Entity raider = sim.things().spawn("Human", CellPos{20, 20, 0});
    const Entity deer = sim.things().spawn("Deer", CellPos{26, 20, 0});
    const Entity colonist = sim.things().spawn("Human", CellPos{15, 20, 0});
    ANIMAL_REQUIRE(raider != kNoEntity);
    ANIMAL_REQUIRE(deer != kNoEntity);
    ANIMAL_REQUIRE(colonist != kNoEntity);
    sim.submit(SetFactionCommand{raider, 2});
    sim.stepMany(120);
    const Fighter* fighter = sim.things().registry().try_get<Fighter>(raider);
    ANIMAL_REQUIRE(fighter != nullptr);
    CHECK(fighter->target != deer);
    for (const KnownEnemy& k : fighter->known) {
        CHECK(k.pawn != deer);
    }
    CHECK(std::any_of(fighter->known.begin(), fighter->known.end(), [&](const KnownEnemy& k) { return k.pawn == colonist; }));
}

TEST_CASE("Animals: saving and loading keeps the herds, their state and the game going the same") {
    const auto defs = animalTestDefs();
    Sim original(animalTestSettings(128, 100, 21), defs);
    original.stepMany(5 * kSecondsPerHour + 1800);  // the sleepers are waking and walking
    const std::vector<Entity> animals = animalsOf(original);
    ANIMAL_REQUIRE(animals.size() > 5);
    original.health().kill(animals[2], DeathCause::BloodLoss);  // a corpse waiting to rot
    original.submit(SpawnThingCommand{"Cow", CellPos{60, 60, 0}});
    original.stepMany(3000);

    const std::vector<uint8_t> bytes = saveSim(original);
    SaveLoadResult loaded = loadSim(bytes, defs);
    ANIMAL_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.error.empty());
    CHECK(loaded.warnings.empty());
    Sim& copy = *loaded.sim;
    CHECK(copy.stateHash() == original.stateHash());
    CHECK(copy.settings().wildAnimals == 100);
    CHECK(copy.animals().populated());
    CHECK(copy.animals().livingCount() == original.animals().livingCount());
    CHECK(copy.animals().stats().grazed == original.animals().stats().grazed);
    CHECK(saveSim(copy) == bytes);
    CHECK(saveToJson(original).find("\"animals\"") != std::string::npos);

    // On from the save the two games stay the same, through a day's walking, grazing and a night's sleep.
    original.stepMany(6 * kSecondsPerHour);
    copy.stepMany(6 * kSecondsPerHour);
    CHECK(copy.stateHash() == original.stateHash());
    CHECK(copy.animals().stats().grazed == original.animals().stats().grazed);
    CHECK(copy.animals().stats().grazed > 0);
    CHECK(saveSim(copy) == saveSim(original));
}

TEST_CASE("Animals: a replay with wild animals plays back to the same hashes") {
    const auto defs = animalTestDefs();
    SimSettings settings = animalTestSettings(96, 100, 8);
    settings.checkpointEvery = 1800;
    Sim original(settings, defs);
    original.stepMany(500);
    original.submit(SpawnThingCommand{"Deer", CellPos{40, 40, 0}});
    original.stepMany(3 * kSecondsPerHour);

    const Replay recorded = recordReplay(original);
    CHECK(recorded.settings.wildAnimals == 100);
    Replay loaded;
    std::string error;
    ANIMAL_REQUIRE(readReplay(writeReplay(recorded), loaded, error));
    CHECK(loaded.settings.wildAnimals == 100);

    Sim replayed(loaded.settings, defs);
    playReplay(loaded, replayed, loaded.endStep);
    const ReplayCheck check = checkReplay(loaded, replayed);
    CHECK(check.compared > 3);
    CHECK(check.matched());
}
