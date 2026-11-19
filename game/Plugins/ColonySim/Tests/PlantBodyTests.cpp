#include "ColonySim/BodyMotion.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Plants.h"
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

// docs/design/plant-bodies.md. sneed (2026-11-16): hard things block bodies; grass, bushes, crops and leaves bend and
// don't stop anyone, only slow a little; small loose props get shoved by weight.

// Evaluates its condition once (placing a plant twice in a cell fails the second time).
#define PLANTB_REQUIRE(...)                     \
    do {                                        \
        const bool plantbOk = (__VA_ARGS__);    \
        CHECK(plantbOk);                        \
        if (!plantbOk) {                        \
            return;                             \
        }                                       \
    } while (0)

namespace {

// The game's real data: the plants' path costs and trunks, the items' masses and `minor` flags are what is tested.
std::shared_ptr<const DefDatabase> plantBodyDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        namespace fs = std::filesystem;
        const fs::path root = fs::path(COLONY_DATA_DIR);
        std::vector<fs::path> files;
        for (const auto& entry : fs::recursive_directory_iterator(root / "defs")) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());
        std::vector<DefSource> sources;
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

std::unique_ptr<Sim> plantBodySimPtr(uint64_t seed) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = 64;
    settings.flatMap = true;
    return std::make_unique<Sim>(settings, plantBodyDefs());
}

// No wild plants at all, so each test puts down only what it means to.
void plantBodyClear(Sim& sim) {
    for (int y = 0; y < sim.map().height(); ++y) {
        for (int x = 0; x < sim.map().width(); ++x) {
            sim.plants().remove(CellPos{x, y, 0});
        }
    }
}

bool plantBodyPlace(Sim& sim, int x, int y, const char* def, float growth = 1.0f) {
    return sim.plants().place(CellPos{x, y, 0}, sim.defs().idOf("Things", def), growth);
}

Entity plantBodyPawn(Sim& sim, float x, float y) {
    const Entity pawn = sim.things().spawn("Human", CellPos{static_cast<int>(x), static_cast<int>(y), 0});
    Position& pos = sim.things().registry().get<Position>(pawn);
    pos.x = x;
    pos.y = y;
    return pawn;
}

const Position& plantBodyPos(const Sim& sim, Entity e) { return sim.things().registry().get<Position>(e); }

void plantBodyWalk(Sim& sim, Entity pawn, std::vector<CellPos> path) {
    const Position& pos = sim.things().registry().get<Position>(pawn);
    sim.things().registry().get<Mover>(pawn).walkTo(pos.x, pos.y, std::move(path));
}

void plantBodyControl(Sim& sim, Entity pawn) {
    (void)sim.things().registry().get_or_emplace<Colonist>(pawn);
    sim.jobs().addWorker(pawn);
    sim.submit(ControlPawnCommand{pawn});
    sim.step();
}

float plantBodyOverlap(const Sim& sim, Entity pawn) {
    const Position& p = plantBodyPos(sim, pawn);
    return trunkOverlap(sim.plants(), p.x, p.y, sim.combat().motion().trunkRadius(pawn), p.level);
}

}  // namespace

TEST_CASE("Plant bodies: a tree's trunk stands where the tree is drawn, as wide as its growth") {
    auto simOwner = plantBodySimPtr(5);
    Sim& sim = *simOwner;
    plantBodyClear(sim);
    PLANTB_REQUIRE(plantBodyPlace(sim, 20, 20, "Plant_TreeTeak", 1.0f));
    PLANTB_REQUIRE(plantBodyPlace(sim, 22, 20, "Plant_TreeTeak", 0.1f));
    PLANTB_REQUIRE(plantBodyPlace(sim, 24, 20, "Plant_TallGrass", 1.0f));
    float x = 0.0f;
    float y = 0.0f;
    float r = 0.0f;
    PLANTB_REQUIRE(sim.plants().trunkAt(CellPos{20, 20, 0}, x, y, r));
    float dx = 0.0f;
    float dy = 0.0f;
    Plants::stemOffset(sim.map().index(CellPos{20, 20, 0}), dx, dy);
    CHECK(x == doctest::Approx(20.5f + dx));
    CHECK(y == doctest::Approx(20.5f + dy));
    CHECK(std::fabs(dx) <= 0.35f);
    CHECK(std::fabs(dy) <= 0.35f);
    CHECK(r == doctest::Approx(0.3f));  // teak: plant.trunkRadius at full growth
    float young = 0.0f;
    PLANTB_REQUIRE(sim.plants().trunkAt(CellPos{22, 20, 0}, x, y, young));
    CHECK(young < r * 0.5f);  // a sapling is thin
    CHECK_FALSE(sim.plants().trunkAt(CellPos{24, 20, 0}, x, y, r));  // grass bends: no trunk
    CHECK_FALSE(sim.plants().trunkAt(CellPos{26, 20, 0}, x, y, r));  // nothing there
    // The same hash Unreal's TerrainSubsystem uses (cell index 0 and 1000 pinned, so a change on one side shows).
    Plants::stemOffset(0, dx, dy);
    const float pinned0[2] = {dx, dy};
    Plants::stemOffset(1000, dx, dy);
    CHECK(pinned0[0] != doctest::Approx(dx));
    CHECK(Plants::drawnSize(0) == doctest::Approx(0.2f));
    CHECK(Plants::drawnSize(kPlantFullGrowth) == doctest::Approx(1.0f));
    CHECK(Plants::drawnSize(kPlantFullGrowth / 2) == doctest::Approx(0.6f));
}

TEST_CASE("Plant bodies: grass, bushes and crops slow walking only a little, more as they grow; trees not at all") {
    auto simOwner = plantBodySimPtr(5);
    Sim& sim = *simOwner;
    plantBodyClear(sim);
    PLANTB_REQUIRE(plantBodyPlace(sim, 10, 10, "Plant_TallGrass", 1.0f));
    PLANTB_REQUIRE(plantBodyPlace(sim, 11, 10, "Plant_TallGrass", 0.2f));
    PLANTB_REQUIRE(plantBodyPlace(sim, 12, 10, "Plant_Bush", 1.0f));
    PLANTB_REQUIRE(plantBodyPlace(sim, 13, 10, "Plant_TreeTeak", 1.0f));
    const Plants& plants = sim.plants();
    // RimWorld's path cost (tall grass 14, bush 30) times a tenth: 1 / (1 + cost * 0.1 * 0.077).
    CHECK(plants.walkFactor(CellPos{10, 10, 0}) == doctest::Approx(1.0f / (1.0f + 14.0f * 0.1f * 0.077f)).epsilon(0.001));
    CHECK(plants.walkFactor(CellPos{12, 10, 0}) == doctest::Approx(1.0f / (1.0f + 30.0f * 0.1f * 0.077f)).epsilon(0.001));
    CHECK(plants.walkFactor(CellPos{12, 10, 0}) > 0.8f);  // never more than a fifth slower
    CHECK(plants.walkFactor(CellPos{11, 10, 0}) > plants.walkFactor(CellPos{10, 10, 0}));
    CHECK(plants.walkFactor(CellPos{13, 10, 0}) == doctest::Approx(1.0f));  // its trunk is solid instead
    CHECK(plants.walkFactor(CellPos{14, 10, 0}) == doctest::Approx(1.0f));

    // A man walking through a field of bushes takes a little longer than over bare ground, and is never stopped.
    const auto walkSteps = [](bool bushes) {
        auto sOwner = plantBodySimPtr(5);
    Sim& s = *sOwner;
        plantBodyClear(s);
        if (bushes) {
            for (int y = 28; y <= 32; ++y) {
                for (int x = 10; x <= 50; ++x) {
                    plantBodyPlace(s, x, y, "Plant_Bush", 1.0f);
                }
            }
        }
        const Entity pawn = plantBodyPawn(s, 10.5f, 30.5f);
        plantBodyWalk(s, pawn, {CellPos{50, 30, 0}});
        int steps = 0;
        while (!s.things().registry().get<Mover>(pawn).arrived() && steps < 3000) {
            s.step();
            ++steps;
        }
        return steps;
    };
    const int bare = walkSteps(false);
    const int bush = walkSteps(true);
    CHECK(bare < 3000);
    CHECK(bush < 3000);
    CHECK(bush > bare);
    CHECK(static_cast<float>(bush) < static_cast<float>(bare) * 1.3f);
}

TEST_CASE("Plant bodies: a man walking past a row of trees goes round each trunk and never through one") {
    auto simOwner = plantBodySimPtr(5);
    Sim& sim = *simOwner;
    plantBodyClear(sim);
    // Trees on every other cell of the line he walks.
    for (int x = 14; x <= 40; x += 2) {
        PLANTB_REQUIRE(plantBodyPlace(sim, x, 30, "Plant_TreeTeak", 1.0f));
    }
    const Entity pawn = plantBodyPawn(sim, 10.5f, 30.5f);
    plantBodyWalk(sim, pawn, {CellPos{46, 30, 0}});
    int steps = 0;
    float worst = 0.0f;
    while (!sim.things().registry().get<Mover>(pawn).arrived() && steps < 4000) {
        sim.step();
        ++steps;
        if (sim.things().registry().get<Mover>(pawn).ghostSteps == 0) {
            worst = std::max(worst, plantBodyOverlap(sim, pawn));
        }
    }
    CHECK(steps < 4000);
    CHECK(worst < 0.01f);
}

TEST_CASE("Plant bodies: dense jungle never traps anyone") {
    auto simOwner = plantBodySimPtr(5);
    Sim& sim = *simOwner;
    plantBodyClear(sim);
    // A tree in every cell of a 12 x 12 block: some gaps between trunks are narrower than a man.
    for (int y = 24; y < 36; ++y) {
        for (int x = 24; x < 36; ++x) {
            plantBodyPlace(sim, x, y, "Plant_TreeTeak", 1.0f);
        }
    }
    const Entity pawn = plantBodyPawn(sim, 20.5f, 30.5f);
    std::vector<CellPos> path;
    for (int x = 21; x <= 40; ++x) {
        path.push_back(CellPos{x, 30, 0});
    }
    plantBodyWalk(sim, pawn, path);
    int steps = 0;
    int ghosting = 0;
    float worst = 0.0f;
    while (!sim.things().registry().get<Mover>(pawn).arrived() && steps < 6000) {
        sim.step();
        ++steps;
        if (sim.things().registry().get<Mover>(pawn).ghostSteps > 0) {
            ++ghosting;
        } else {
            worst = std::max(worst, plantBodyOverlap(sim, pawn));
        }
    }
    // Most of the way it threads between the trunks; it squeezes past only where they stand too close.
    CHECK(ghosting < steps / 2);
    CHECK(steps < 6000);
    CHECK(plantBodyPos(sim, pawn).x > 39.0f);
}

TEST_CASE("Plant bodies: a roll into a tree stops at the trunk") {
    auto simOwner = plantBodySimPtr(5);
    Sim& sim = *simOwner;
    plantBodyClear(sim);
    PLANTB_REQUIRE(plantBodyPlace(sim, 33, 30, "Plant_TreeTeak", 1.0f));
    float tx = 0.0f;
    float ty = 0.0f;
    float tr = 0.0f;
    PLANTB_REQUIRE(sim.plants().trunkAt(CellPos{33, 30, 0}, tx, ty, tr));
    const Entity pawn = plantBodyPawn(sim, 30.5f, ty);
    plantBodyControl(sim, pawn);
    sim.submit(DodgeRollCommand{pawn, 1.0f, 0.0f});
    for (int i = 0; i < 60; ++i) {
        sim.step();
        CHECK(plantBodyOverlap(sim, pawn) < 1e-3f);
    }
    const float r = sim.combat().motion().trunkRadius(pawn);
    CHECK(plantBodyPos(sim, pawn).x < tx);
    CHECK(plantBodyPos(sim, pawn).x > tx - r - tr - 0.06f);  // right up against it, not short of it
}

TEST_CASE("Plant bodies: the player walking into a trunk slides round it, through grass freely") {
    auto simOwner = plantBodySimPtr(5);
    Sim& sim = *simOwner;
    plantBodyClear(sim);
    PLANTB_REQUIRE(plantBodyPlace(sim, 33, 30, "Plant_TreeTeak", 1.0f));
    for (int x = 25; x <= 31; ++x) {
        plantBodyPlace(sim, x, 30, "Plant_TallGrass", 1.0f);
    }
    float tx = 0.0f;
    float ty = 0.0f;
    float tr = 0.0f;
    PLANTB_REQUIRE(sim.plants().trunkAt(CellPos{33, 30, 0}, tx, ty, tr));
    const Entity pawn = plantBodyPawn(sim, 25.5f, ty + 0.05f);
    plantBodyControl(sim, pawn);
    // Walk straight at the tree and past it, as Unreal sends one position a step.
    for (int i = 0; i < 400; ++i) {
        const Position& p = plantBodyPos(sim, pawn);
        sim.submit(DirectMoveCommand{pawn, p.x + 0.05f, p.y});
        sim.step();
        CHECK(plantBodyOverlap(sim, pawn) < 1e-3f);
    }
    CHECK(plantBodyPos(sim, pawn).x > tx + 1.0f);  // it got past (slid round), not stuck behind it
    // In tall grass the player's move factor is a little under the same pawn's on bare ground.
    sim.things().registry().get<Position>(pawn).x = 28.5f;
    sim.things().registry().get<Position>(pawn).y = 30.5f;
    Snapshot snap;
    sim.writeSnapshot(snap);
    for (const ThingView& v : snap.things) {
        if (v.id == pawn) {
            CHECK(v.moveFactor < 0.95f);
            CHECK(v.moveFactor > 0.8f);
        }
    }
}

TEST_CASE("Plant bodies: loose props are shoved by weight and stay in their cell; heavy stacks are not") {
    auto simOwner = plantBodySimPtr(5);
    Sim& sim = *simOwner;
    plantBodyClear(sim);
    BodyMotionSystem& motion = sim.combat().motion();
    const Entity rifle = sim.things().spawn("Gun_BoltActionRifle", CellPos{30, 30, 0});
    const Entity steel = sim.things().spawn("Steel", CellPos{30, 32, 0}, 75);
    PLANTB_REQUIRE(rifle != kNoEntity);
    PLANTB_REQUIRE(steel != kNoEntity);
    CHECK(motion.propMass(rifle) > 0.0f);
    CHECK(motion.propMass(rifle) < 10.0f);
    CHECK(motion.propMass(steel) == doctest::Approx(37.5f));  // 75 x 0.5 kg: over half a man, stays put
    const Position rifleAt = plantBodyPos(sim, rifle);
    const Position steelAt = plantBodyPos(sim, steel);

    const Entity pawn = plantBodyPawn(sim, 25.5f, 30.5f);
    plantBodyWalk(sim, pawn, {CellPos{35, 30, 0}});
    const Entity pawn2 = plantBodyPawn(sim, 25.5f, 32.5f);
    plantBodyWalk(sim, pawn2, {CellPos{35, 32, 0}});
    bool moved = false;
    for (int i = 0; i < 600; ++i) {
        sim.step();
        const Position& p = plantBodyPos(sim, rifle);
        moved = moved || std::fabs(p.x - rifleAt.x) + std::fabs(p.y - rifleAt.y) > 0.05f;
        // Always inside its own cell, so storage and hauling still find it.
        CHECK(cellOf(p) == CellPos{30, 30, 0});
        CHECK(sim.things().itemsAt(CellPos{30, 30, 0}).size() == 1);
    }
    CHECK(moved);
    CHECK(sim.combat().motion().propShoves() > 0);
    CHECK(plantBodyPos(sim, steel).x == doctest::Approx(steelAt.x));
    CHECK(plantBodyPos(sim, steel).y == doctest::Approx(steelAt.y));
    // The snapshot draws the rifle where it was pushed to.
    Snapshot snap;
    sim.writeSnapshot(snap);
    for (const ThingView& v : snap.things) {
        if (v.id == rifle) {
            CHECK(v.x == doctest::Approx(plantBodyPos(sim, rifle).x));
        }
    }
}

TEST_CASE("Plant bodies: the same seed gives the same game with trees, props and bodies") {
    const auto run = [] {
        auto simOwner = plantBodySimPtr(11);
    Sim& sim = *simOwner;
        for (int x = 14; x <= 40; x += 3) {
            plantBodyPlace(sim, x, 30, "Plant_TreeTeak", 1.0f);
        }
        sim.things().spawn("Gun_BoltActionRifle", CellPos{20, 30, 0});
        const Entity pawn = plantBodyPawn(sim, 10.5f, 30.5f);
        plantBodyWalk(sim, pawn, {CellPos{46, 30, 0}});
        sim.stepMany(900);
        return sim.stateHash();
    };
    CHECK(run() == run());
}
