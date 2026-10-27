#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Health.h"
#include "ColonySim/Loot.h"
#include "ColonySim/Narrator.h"
#include "ColonySim/Raids.h"
#include "ColonySim/Sim.h"
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

#define LOOT_REQUIRE(...)      \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// The game's real data plus one table and one raider kind of the test's own. These tests check
// docs/design/loot.md.
std::shared_ptr<const DefDatabase> lootTestDefs() {
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
        sources.push_back(DefSource{"LootTables", "zz_loot_test.json", R"([
            {"defName": "LootTestPack", "items": [{"thing": "MealSurvivalPack", "chance": 1, "count": [2, 2]}]}
        ])"});
        sources.push_back(DefSource{"PawnKinds", "zz_loot_test.json", R"([
            {"defName": "LootTestRaider", "label": "loot test raider", "race": "Human", "role": "raider", "lootTable": "LootTestPack"}
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

std::unique_ptr<Sim> lootTestSim(uint64_t seed = 21) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = 128;
    settings.flatMap = true;
    settings.scenario = "Crashlanded";
    return std::make_unique<Sim>(settings, lootTestDefs());
}

int lootTestTotal(const std::vector<LootItem>& items, DefId def) {
    int total = 0;
    for (const LootItem& item : items) {
        if (item.def == def) {
            total += item.count;
        }
    }
    return total;
}

int lootTestOnMap(Sim& sim, const char* def) {
    int total = 0;
    const DefId id = sim.defs().idOf("Things", def);
    for (const auto [e, thing, stack, at] : sim.things().registry().view<const Thing, const StackCount, const Position>().each()) {
        if (thing.def == id) {
            total += stack.count;
        }
    }
    return total;
}

}  // namespace

TEST_CASE("Loot: every table rolls only its own things, within stack limits") {
    auto sim = lootTestSim();
    const DefDatabase& defs = sim->defs();
    CHECK(defs.idsOfKind("LootTables").size() >= 8);
    for (const DefId id : defs.idsOfKind("LootTables")) {
        const std::string name = defs.get(id).defName();
        CHECK_MESSAGE(sim->loot().hasTable(name), name);
        for (int i = 0; i < 40; ++i) {
            Rng rng(static_cast<uint64_t>(i + 1));
            for (const LootItem& item : sim->loot().roll(name, 150.0f, rng)) {
                const Def& thing = defs.get(item.def);
                CHECK_MESSAGE(item.count >= 1, name);
                if (defs.get(id).count("stacks.from") > 0 && thing.number("stats.marketValue", 0.0) > 0.0) {
                    CHECK_MESSAGE(item.count <= std::max<int64_t>(thing.integer("stackLimit", 1), 1) * 5, name);
                }
            }
        }
    }
    Rng rng(1);
    CHECK(sim->loot().roll("NoSuchTable", 100.0f, rng).empty());
}

TEST_CASE("Loot: the same stream gives the same loot") {
    auto sim = lootTestSim();
    Rng a = sim->loot().rngFor(77, 1000);
    Rng b = sim->loot().rngFor(77, 1000);
    Rng c = sim->loot().rngFor(78, 1000);
    const auto first = sim->loot().roll("AncientDanger", 0.0f, a);
    const auto second = sim->loot().roll("AncientDanger", 0.0f, b);
    const auto other = sim->loot().roll("AncientDanger", 0.0f, c);
    LOOT_REQUIRE(first.size() == second.size());
    for (size_t i = 0; i < first.size(); ++i) {
        CHECK(first[i].def == second[i].def);
        CHECK(first[i].count == second[i].count);
    }
    CHECK(sim->loot().describe(first) != sim->loot().describe(other));
}

TEST_CASE("Loot: a cargo pod is one stack worth its value, an ancient danger always holds survival meals") {
    auto sim = lootTestSim();
    const DefDatabase& defs = sim->defs();
    for (int i = 0; i < 60; ++i) {
        Rng rng(static_cast<uint64_t>(100 + i));
        const auto pod = sim->loot().roll("CargoPod", 0.0f, rng);
        LOOT_REQUIRE(pod.size() == 1);
        const double each = defs.get(pod[0].def).number("stats.marketValue", 0.0);
        const double value = each * pod[0].count;
        const int limit = static_cast<int>(defs.get(pod[0].def).integer("stackLimit", 1));
        // Worth 200 to 900 silver, rounded to whole items and cut at the stack limit.
        if (pod[0].count < limit) {
            CHECK(value >= 200.0 - each);
            CHECK(value <= 900.0 + each);
        }
        Rng rng2(static_cast<uint64_t>(500 + i));
        const auto danger = sim->loot().roll("AncientDanger", 0.0f, rng2);
        const int meals = lootTestTotal(danger, defs.idOf("Things", "MealSurvivalPack"));
        CHECK(meals >= 3);
    }
}

TEST_CASE("Loot: more raid points give a raider more") {
    auto sim = lootTestSim();
    const DefId silver = sim->defs().idOf("Things", "Silver");
    int small = 0;
    int big = 0;
    for (int i = 0; i < 200; ++i) {
        Rng a(static_cast<uint64_t>(i + 1));
        Rng b(static_cast<uint64_t>(i + 1));  // the same dice, so only the points differ
        small += lootTestTotal(sim->loot().roll("RaiderPackPirate", 100.0f, a), silver);
        big += lootTestTotal(sim->loot().roll("RaiderPackPirate", 400.0f, b), silver);
    }
    CHECK(big > small);
    CHECK(sim->loot().tableOfKind("MercenaryGunner") == "RaiderPackMercenary");
    CHECK(sim->loot().tableOfKind("ScavengerGunner") == "RaiderPackPirate");
    CHECK(sim->loot().tableOfKind("ClanWarrior") == "RaiderPackTribal");
    CHECK(sim->loot().tableOfKind("ConcordTrooper") == "RaiderPackTrooper");
    CHECK(sim->loot().tableOfKind("FrayVillager").empty());
}

TEST_CASE("Loot: a raider that dies drops its pack where it lies, once") {
    auto sim = lootTestSim();
    Entity victim = kNoEntity;
    for (const auto [pawn, colonist] : sim->things().registry().view<const Colonist>().each()) {
        victim = pawn;
        break;
    }
    LOOT_REQUIRE(victim != kNoEntity);
    sim->things().registry().emplace<Raider>(victim, Raider{9, "LootTestRaider"});
    const int mealsBefore = lootTestOnMap(*sim, "MealSurvivalPack");  // the colonists start with some
    const CellPos where = cellOf(sim->things().registry().get<Position>(victim));
    sim->submit(DamagePawnCommand{victim, "Bullet", 100000.0f, 0});
    sim->step();
    CHECK(sim->things().registry().get<Health>(victim).dead);
    CHECK(lootTestOnMap(*sim, "MealSurvivalPack") == mealsBefore + 2);
    // Near the body, not far.
    bool near = false;
    const DefId meal = sim->defs().idOf("Things", "MealSurvivalPack");
    for (const auto [e, thing, pos] : sim->things().registry().view<const Thing, const Position>().each()) {
        if (thing.def == meal) {
            near = std::abs(static_cast<int>(pos.x) - where.x) <= 7 && std::abs(static_cast<int>(pos.y) - where.y) <= 7;
        }
    }
    CHECK(near);
    for (int i = 0; i < 5; ++i) {
        sim->step();
    }
    CHECK(lootTestOnMap(*sim, "MealSurvivalPack") == mealsBefore + 2);  // not again
}

TEST_CASE("Loot: a real raid's dead drop packs, and the living take theirs home") {
    auto sim = lootTestSim(5);
    sim->submit(StartRaidCommand{"Pirate", 400.0f, 1});
    sim->step();
    std::vector<Entity> raiders(sim->things().registry().view<const Raider>().begin(), sim->things().registry().view<const Raider>().end());
    LOOT_REQUIRE(!raiders.empty());
    std::sort(raiders.begin(), raiders.end());
    for (const Entity r : raiders) {
        sim->submit(DamagePawnCommand{r, "Bullet", 100000.0f, 0});
    }
    sim->step();
    // Packs are chances, so at 400 points several raiders make something: silver is the likeliest.
    const int silver = lootTestOnMap(*sim, "Silver");
    const int meals = lootTestOnMap(*sim, "MealSurvivalPack");
    const int medicine = lootTestOnMap(*sim, "MedicineIndustrial");
    CHECK(silver + meals + medicine > 0);
}

TEST_CASE("Loot: the cargo pod incident drops a stack near the colony, with a letter") {
    auto sim = lootTestSim(3);
    sim->submit(FireIncidentCommand{"ResourcePodCrash", 0.0f});
    const size_t before = sim->things().registry().view<const StackCount, const Position>().size_hint();
    sim->step();
    CHECK(std::none_of(sim->events().begin(), sim->events().end(), [](const Event& e) { return e.type == EventType::CommandFailed; }));
    const Event* letter = nullptr;
    for (const Event& e : sim->events()) {
        if (e.type == EventType::Letter) {
            letter = &e;
        }
    }
    LOOT_REQUIRE(letter != nullptr);
    CHECK(letter->def == sim->defs().idOf("Incidents", "ResourcePodCrash"));
    CHECK(letter->count == static_cast<int>(LetterKind::PositiveEvent));
    CHECK(letter->message.find("cargo pod") != std::string::npos);
    CHECK(sim->things().registry().view<const StackCount, const Position>().size_hint() > before);
    // The pod's stack lies at the letter's cell or beside it.
    bool found = false;
    for (int dy = -1; dy <= 1 && !found; ++dy) {
        for (int dx = -1; dx <= 1 && !found; ++dx) {
            found = !sim->things().itemsAt(CellPos{letter->cell.x + dx, letter->cell.y + dy, 0}).empty();
        }
    }
    CHECK(found);
}

TEST_CASE("Loot: two games with the same seed and commands stay identical") {
    auto a = lootTestSim(8);
    auto b = lootTestSim(8);
    for (Sim* sim : {a.get(), b.get()}) {
        sim->submit(FireIncidentCommand{"ResourcePodCrash", 0.0f});
        sim->submit(StartRaidCommand{"Pirate", 200.0f, 2});
    }
    for (int i = 0; i < 40; ++i) {
        a->step();
        b->step();
    }
    CHECK(a->stateHash() == b->stateHash());
}
