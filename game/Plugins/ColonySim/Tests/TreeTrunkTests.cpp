#include "ColonySim/Combat.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Pathfinder.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Save.h"
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

// docs/design/structure-damage.md "Trees in the way" and "Falling trees": tree trunks are solid posts that paths avoid
// and walkers steer round; a tree taken down says which way it falls.

#define TRUNK_REQUIRE(...)     \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> trunkTestDefs() {
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
        return db;
    }();
    return defs;
}

// A bare 64 m field with no plants: each test puts its own trees where it wants them.
std::unique_ptr<Sim> trunkTestSim(uint64_t seed = 31) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = 64;
    settings.flatMap = true;
    auto sim = std::make_unique<Sim>(settings, trunkTestDefs());
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            sim->plants().remove(CellPos{x, y, 0});
        }
    }
    return sim;
}

CellPos trunkTestTree(Sim& sim, int x, int y, float growth = 1.0f) {
    const CellPos c{x, y, 0};
    sim.plants().remove(c);
    CHECK(sim.plants().place(c, sim.defs().idOf("Things", "Plant_TreeOak"), growth));
    return c;
}

Entity trunkTestPawn(Sim& sim, float x, float y) {
    const Entity pawn = sim.things().spawn("Human", CellPos{static_cast<int>(x), static_cast<int>(y), 0});
    Position& pos = sim.things().registry().get<Position>(pawn);
    pos.x = x;
    pos.y = y;
    return pawn;
}

// How far the pawn's body is from the nearest trunk's bark (negative: inside it), metres.
float trunkTestClearance(const Sim& sim, Entity pawn) {
    const Position& pos = sim.things().registry().get<Position>(pawn);
    float best = 1.0e9f;
    for (int y = static_cast<int>(pos.y) - 2; y <= static_cast<int>(pos.y) + 2; ++y) {
        for (int x = static_cast<int>(pos.x) - 2; x <= static_cast<int>(pos.x) + 2; ++x) {
            float tx = 0.0f;
            float ty = 0.0f;
            float tr = 0.0f;
            if (sim.map().trunkAt(CellPos{x, y, 0}, tx, ty, tr)) {
                best = std::min(best, std::sqrt((pos.x - tx) * (pos.x - tx) + (pos.y - ty) * (pos.y - ty)) - tr - kTrunkBodyRadius);
            }
        }
    }
    return best;
}

// Walks the pawn along a path from the pathfinder, smoothed as jobs do; returns the steps it took (-1: never arrived or
// gave up, -2: stuck until the watchdog let it walk through) and the closest it came to any trunk.
int trunkTestWalk(Sim& sim, Entity pawn, CellPos goal, float& closest, int maxSteps = 3000) {
    const Position& pos = sim.things().registry().get<Position>(pawn);
    std::vector<CellPos> path;
    if (!sim.pathfinder().findPath(CellPos{static_cast<int>(pos.x), static_cast<int>(pos.y), 0}, goal, path)) {
        return -1;
    }
    smoothPath(sim.map(), pos.x, pos.y, path);
    sim.things().registry().get<Mover>(pawn).walkTo(pos.x, pos.y, std::move(path));
    closest = 1.0e9f;
    for (int step = 0; step < maxSteps; ++step) {
        sim.step();
        closest = std::min(closest, trunkTestClearance(sim, pawn));
        const Mover& mover = sim.things().registry().get<Mover>(pawn);
        if (mover.phaseSteps > 0) {
            return -2;  // stuck: the watchdog let it walk through
        }
        if (mover.arrived() && mover.vx == 0.0f && mover.vy == 0.0f) {
            return mover.gaveUp ? -1 : step;
        }
    }
    return -1;
}

// Walks the pawn straight at the cells given (no pathfinder, no smoothing); the steps it took to stand still at the last,
// -1 if it never did or gave up, -2 if it was stuck until the watchdog let it walk through.
int trunkTestStraight(Sim& sim, Entity pawn, std::vector<CellPos> cells, int maxSteps = 900) {
    const Position& pos = sim.things().registry().get<Position>(pawn);
    sim.things().registry().get<Mover>(pawn).walkTo(pos.x, pos.y, std::move(cells));
    for (int step = 0; step < maxSteps; ++step) {
        sim.step();
        const Mover& mover = sim.things().registry().get<Mover>(pawn);
        if (mover.phaseSteps > 0) {
            return -2;
        }
        if (mover.arrived() && mover.vx == 0.0f && mover.vy == 0.0f) {
            return mover.gaveUp ? -1 : step;
        }
    }
    return -1;
}

}  // namespace

TEST_CASE("Trees in the way: the map's trunk layer follows the trees as they grow, die and load") {
    auto sim = trunkTestSim();
    const CellPos c = trunkTestTree(*sim, 20, 20, 1.0f);
    float tx = 0.0f;
    float ty = 0.0f;
    float tr = 0.0f;
    TRUNK_REQUIRE(sim->map().trunkAt(c, tx, ty, tr));
    float px = 0.0f;
    float py = 0.0f;
    float pr = 0.0f;
    float ph = 0.0f;
    TRUNK_REQUIRE(sim->plants().trunkOf(c, px, py, pr, ph));
    CHECK(tx == px);
    CHECK(ty == py);
    CHECK(std::fabs(tr - pr) <= 0.005f);
    CHECK(std::fabs(tr - 0.28f) <= 0.005f);  // an oak's trunk, Plant_TreeOak visual.stem.radius
    CHECK(sim->map().trunkCount() == 1);

    // A sapling is thinner; it thickens as it grows.
    sim->plants().setGrowth(c, 0.0f);
    TRUNK_REQUIRE(sim->map().trunkAt(c, tx, ty, tr));
    CHECK(std::fabs(tr - 0.28f * 0.2f) <= 0.006f);

    // Grass and bushes are no trunks.
    sim->plants().place(CellPos{22, 20, 0}, sim->defs().idOf("Things", "Plant_Grass"), 1.0f);
    CHECK_FALSE(sim->map().hasTrunk(CellPos{22, 20, 0}));

    // Felled, cut down or killed by damage: gone from the layer.
    sim->plants().remove(c);
    CHECK_FALSE(sim->map().hasTrunk(c));
    const CellPos hurt = trunkTestTree(*sim, 25, 25);
    CHECK(sim->map().hasTrunk(hurt));
    CHECK(sim->plants().damage(hurt, 100000));
    CHECK_FALSE(sim->map().hasTrunk(hurt));
    CHECK(sim->map().trunkCount() == 0);

    // A save keeps the trees, and so the trunks (a loaded game starts from a fresh map with its own wild trees).
    const CellPos kept = trunkTestTree(*sim, 30, 12);
    SaveLoadResult loaded = loadSim(saveSim(*sim), trunkTestDefs());
    TRUNK_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->map().hasTrunk(kept));
    CHECK(loaded.sim->map().trunkCount() == 1);
}

TEST_CASE("Trees in the way: a path goes round a lone tree") {
    auto sim = trunkTestSim();
    CHECK(sim->pathfinder().treeCost() > 3.0f);  // RimWorld's 42 ticks: about 3.2 m
    CHECK(sim->pathfinder().treeCost() < 3.5f);
    const CellPos tree = trunkTestTree(*sim, 30, 30);
    std::vector<CellPos> path;
    TRUNK_REQUIRE(sim->pathfinder().findPath(CellPos{20, 30, 0}, CellPos{40, 30, 0}, path));
    CHECK(std::find(path.begin(), path.end(), tree) == path.end());
    // The best path steps round it, one cell aside...
    TRUNK_REQUIRE(sim->pathfinder().findPathFullGrid(CellPos{20, 30, 0}, CellPos{40, 30, 0}, path));
    CHECK(std::find(path.begin(), path.end(), tree) == path.end());
    for (const CellPos& c : path) {
        CHECK(std::abs(c.y - 30) <= 1);
    }
    // ...where without the cost it would walk straight through the tree's cell.
    sim->pathfinder().setTreeCost(0.0f);
    TRUNK_REQUIRE(sim->pathfinder().findPathFullGrid(CellPos{20, 30, 0}, CellPos{40, 30, 0}, path));
    CHECK(std::find(path.begin(), path.end(), tree) != path.end());
}

TEST_CASE("Trees in the way: smoothing never cuts a line through a trunk") {
    auto sim = trunkTestSim();
    // A scatter of trees across the way.
    for (int x = 24; x <= 36; x += 3) {
        for (int y = 26; y <= 34; y += 2) {
            trunkTestTree(*sim, x + (y % 4 == 0 ? 1 : 0), y);
        }
    }
    std::vector<CellPos> path;
    TRUNK_REQUIRE(sim->pathfinder().findPath(CellPos{18, 30, 0}, CellPos{42, 30, 0}, path));
    smoothPath(sim->map(), 18.5f, 30.5f, path);
    CellPos at{18, 30, 0};
    float x = 18.5f;
    float y = 30.5f;
    int skips = 0;
    for (const CellPos& c : path) {
        float nx = 0.0f;
        float ny = 0.0f;
        cellWalkPoint(sim->map(), c, nx, ny);
        // A line the smoothing made (it skipped cells) keeps clear of every trunk; a step to the next cell is the path's own.
        if (std::max(std::abs(c.x - at.x), std::abs(c.y - at.y)) > 1) {
            CHECK(trunksClearOfLine(sim->map(), x, y, nx, ny, kPawnRadius));
            ++skips;
        }
        at = c;
        x = nx;
        y = ny;
    }
    CHECK(skips > 0);
}

TEST_CASE("Trees in the way: a walker steers round a trunk on its line and never walks into it") {
    auto sim = trunkTestSim();
    trunkTestTree(*sim, 30, 30);
    const Entity pawn = trunkTestPawn(*sim, 20.5f, 30.5f);
    // Straight at the tree, past the pathfinder: the steering alone must take it round.
    const Position& pos = sim->things().registry().get<Position>(pawn);
    sim->things().registry().get<Mover>(pawn).walkTo(pos.x, pos.y, {CellPos{40, 30, 0}});
    float closest = 1.0e9f;
    bool arrived = false;
    for (int step = 0; step < 1500 && !arrived; ++step) {
        sim->step();
        closest = std::min(closest, trunkTestClearance(*sim, pawn));
        arrived = sim->things().registry().get<Mover>(pawn).arrived();
    }
    CHECK(arrived);
    CHECK_FALSE(sim->things().registry().get<Mover>(pawn).gaveUp);
    CHECK(closest >= -0.01f);
    CHECK(pos.x > 39.0f);
}

TEST_CASE("Trees in the way: a pawn inside a trunk walks off it, and a goal beside a trunk is reached from every side") {
    // Found by the stairs tests and the raid soak: a pawn spawned against a trunk froze there, and fighters stood for good a
    // step from a goal cell beside a tree (every way on cost more than standing).
    {
        auto sim = trunkTestSim();
        const CellPos tree = trunkTestTree(*sim, 30, 30);
        float tx = 0.0f;
        float ty = 0.0f;
        float tr = 0.0f;
        TRUNK_REQUIRE(sim->map().trunkAt(tree, tx, ty, tr));
        const Entity pawn = trunkTestPawn(*sim, tx, ty - 0.15f);  // in the bark, walking on through it
        CHECK(trunkTestClearance(*sim, pawn) < 0.0f);
        CHECK(trunkTestStraight(*sim, pawn, {CellPos{30, 34, 0}}) >= 0);
        CHECK(trunkTestClearance(*sim, pawn) >= -0.01f);
    }
    // A goal cell holding a trunk, walked into from a spot in each cell round it.
    int reached = 0;
    int tries = 0;
    for (int side = 0; side < 8; ++side) {
        for (int spot = 0; spot < 3; ++spot) {
            auto sim = trunkTestSim();
            const CellPos goal = trunkTestTree(*sim, 30, 30);
            const float off = 0.2f + 0.3f * static_cast<float>(spot);
            const Entity pawn = trunkTestPawn(*sim, 30.0f + static_cast<float>(Map::kStepDx[side]) + off,
                                              30.0f + static_cast<float>(Map::kStepDy[side]) + (1.0f - off));
            if (trunkTestClearance(*sim, pawn) < 0.0f) {
                continue;
            }
            ++tries;
            const int steps = trunkTestStraight(*sim, pawn, {goal});
            reached += steps >= 0 ? 1 : 0;
            CHECK_MESSAGE(steps >= 0, "from side " << side << " spot " << spot);
            CHECK(trunkTestClearance(*sim, pawn) >= -0.01f);
            const Position& pos = sim->things().registry().get<Position>(pawn);
            CHECK(CellPos{static_cast<int>(pos.x), static_cast<int>(pos.y), 0} == goal);
        }
    }
    CHECK(tries >= 16);
    CHECK(reached == tries);

    // Like the raid soak's (seed 1): a fighter just north of its goal cell, whose young tree stands a little south of the
    // cell's middle.
    int found = 0;
    for (int y = 8; y < 56 && found < 3; ++y) {
        for (int x = 8; x < 56 && found < 3; ++x) {
            float px = 0.0f;
            float py = 0.0f;
            Map::plantSpot(y * 64 + x, x, y, px, py);
            if (std::fabs(px - (static_cast<float>(x) + 0.5f)) > 0.06f || std::fabs(py - (static_cast<float>(y) + 0.34f)) > 0.06f) {
                continue;
            }
            ++found;
            auto sim = trunkTestSim();
            const CellPos goal = trunkTestTree(*sim, x, y, 0.55f);
            const Entity pawn = trunkTestPawn(*sim, static_cast<float>(x) + 0.06f, static_cast<float>(y) + 1.01f);
            const int steps = trunkTestStraight(*sim, pawn, {goal});
            CHECK_MESSAGE(steps >= 0, "goal " << x << "," << y);
            const Position& pos = sim->things().registry().get<Position>(pawn);
            CHECK(CellPos{static_cast<int>(pos.x), static_cast<int>(pos.y), 0} == goal);
        }
    }
    CHECK(found > 0);
}

TEST_CASE("Trees in the way: a pawn walks to a tree's own cell and to one beside it in a thick forest") {
    auto sim = trunkTestSim();
    // A thick forest: a grown oak in two cells of every five from x 24 to 36 (a jungle has about one in five).
    for (int y = 20; y <= 40; ++y) {
        for (int x = 24; x <= 36; ++x) {
            if ((x * 7 + y * 3) % 5 < 2) {
                trunkTestTree(*sim, x, y);
            }
        }
    }
    const Entity pawn = trunkTestPawn(*sim, 18.5f, 30.5f);
    float closest = 0.0f;
    // Right through the forest to the far side.
    CHECK(trunkTestWalk(*sim, pawn, CellPos{44, 31, 0}, closest) > 0);
    CHECK(closest >= -0.01f);
    // Into the middle, to a cell with a tree in it (the woodcutter's or a hauler's goal).
    const CellPos treeCell{33, 30, 0};
    TRUNK_REQUIRE(sim->map().hasTrunk(treeCell));
    CHECK(trunkTestWalk(*sim, pawn, treeCell, closest) > 0);
    CHECK(closest >= -0.01f);
    const Position& pos = sim->things().registry().get<Position>(pawn);
    CHECK(static_cast<int>(std::floor(pos.x)) == treeCell.x);
    CHECK(static_cast<int>(std::floor(pos.y)) == treeCell.y);
}

TEST_CASE("Trees in the way: a pawn criss-crosses a jungle-thick wood and never gets stuck") {
    auto sim = trunkTestSim();
    // A tree in about one cell of three from x 16 to 48 (a jungle has about one in five), oaks and thicker teaks, grown
    // and half grown.
    const DefId oak = sim->defs().idOf("Things", "Plant_TreeOak");
    const DefId teak = sim->defs().idOf("Things", "Plant_TreeTeak");
    uint64_t h = 12345;
    for (int y = 10; y <= 54; ++y) {
        for (int x = 16; x <= 48; ++x) {
            h = h * 6364136223846793005ULL + 1442695040888963407ULL;
            if ((h >> 33) % 3 == 0) {
                sim->plants().place(CellPos{x, y, 0}, (h >> 40) % 3 == 0 ? teak : oak, (h >> 45) % 2 == 0 ? 1.0f : 0.5f);
            }
        }
    }
    const Entity pawn = trunkTestPawn(*sim, 10.5f, 30.5f);
    const CellPos goals[] = {{54, 30, 0}, {10, 14, 0}, {54, 50, 0}, {30, 31, 0}, {12, 52, 0}, {47, 12, 0}, {20, 40, 0}, {40, 20, 0}};
    for (const CellPos& goal : goals) {
        if (sim->map().hasTrunk(goal)) {
            continue;
        }
        float closest = 0.0f;
        const int steps = trunkTestWalk(*sim, pawn, goal, closest);
        CHECK_MESSAGE(steps > 0, "walking to " << goal.x << "," << goal.y << " gave " << steps);
        CHECK(closest >= -0.01f);
    }
}

TEST_CASE("Trees in the way: a wall of trees with no way round is walked through, between the trunks") {
    auto sim = trunkTestSim();
    // A corridor three cells wide with walls on both sides and a young tree in every cell of one column.
    for (int x = 20; x <= 40; ++x) {
        sim->things().spawn("Wall", CellPos{x, 28, 0});
        sim->things().spawn("Wall", CellPos{x, 32, 0});
    }
    for (int y = 29; y <= 31; ++y) {
        trunkTestTree(*sim, 30, y, 0.0f);
    }
    std::vector<CellPos> path;
    TRUNK_REQUIRE(sim->pathfinder().findPath(CellPos{21, 30, 0}, CellPos{39, 30, 0}, path));
    bool throughTree = false;
    for (const CellPos& c : path) {
        throughTree = throughTree || sim->map().hasTrunk(c);
    }
    CHECK(throughTree);
    const Entity pawn = trunkTestPawn(*sim, 21.5f, 30.5f);
    float closest = 0.0f;
    CHECK(trunkTestWalk(*sim, pawn, CellPos{39, 30, 0}, closest) > 0);
    CHECK(closest >= -0.01f);
}

TEST_CASE("Falling trees: a tree killed by a blow or a shot falls away from where it was struck") {
    auto sim = trunkTestSim();
    const CellPos c = trunkTestTree(*sim, 30, 30);
    float tx = 0.0f;
    float ty = 0.0f;
    float tr = 0.0f;
    float th = 0.0f;
    TRUNK_REQUIRE(sim->plants().trunkOf(c, tx, ty, tr, th));
    // Struck on its south side, hard enough to kill it.
    TraceHit hit;
    hit.kind = ProjectileHitKind::Plant;
    hit.cell = c;
    hit.x = tx;
    hit.y = ty - tr;
    hit.z = 1.0f;
    size_t seen = sim->events().size();
    CHECK(sim->combat().damageStruck(hit, 100000.0f, true) > 0);
    CHECK_FALSE(sim->plants().has(c));
    const Event* fell = nullptr;
    for (size_t i = seen; i < sim->events().size(); ++i) {
        const Event& e = sim->events()[i];
        if (e.type == EventType::PlantRemoved && e.cell == c) {
            fell = &e;
        }
    }
    TRUNK_REQUIRE(fell != nullptr);
    CHECK(fell->message == "broken");
    CHECK(fell->def == sim->defs().idOf("Things", "Plant_TreeOak"));
    CHECK(fell->y > 0.99f);  // north, away from the blow
    CHECK(fell->count == static_cast<int>(kPlantFullGrowth));

    // A blow that doesn't kill it says nothing.
    const CellPos other = trunkTestTree(*sim, 34, 30);
    hit.cell = other;
    seen = sim->events().size();
    CHECK(sim->combat().damageStruck(hit, 5.0f, true) > 0);
    CHECK(sim->plants().has(other));
    for (size_t i = seen; i < sim->events().size(); ++i) {
        CHECK(sim->events()[i].type != EventType::PlantRemoved);
    }
}
