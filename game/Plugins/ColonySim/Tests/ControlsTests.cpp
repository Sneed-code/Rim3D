// The player-controls rework (sneed 2026-11-17, docs/design/controls.md), with the game's own data: AI pawns sprint
// only in a fight, fleeing or an emergency, and the player's colonist picks resources up by walking past them.
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"
#include "ColonySim/Zones.h"

#include <doctest.h>

#include <memory>
#include <string>
#include <vector>

using namespace colony;

#define CTRL_REQUIRE(...)                                    \
    do {                                                     \
        const bool ctrlOk = static_cast<bool>(__VA_ARGS__);  \
        CHECK_MESSAGE(ctrlOk, #__VA_ARGS__);                 \
        if (!ctrlOk) {                                       \
            return;                                          \
        }                                                    \
    } while (false)

namespace {

std::shared_ptr<const DefDatabase> ctrlDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return db;
    }();
    return defs;
}

std::unique_ptr<Sim> ctrlSim() { return std::make_unique<Sim>(SimSettings{11, 48, 0, 0, true}, ctrlDefs()); }

Entity ctrlColonist(Sim& sim, CellPos cell) {
    const Entity pawn = sim.things().spawn("Human", cell);
    sim.jobs().addWorker(pawn);
    Colonist colonist;
    colonist.firstName = "Test";
    sim.things().registry().emplace<Colonist>(pawn, colonist);
    return pawn;
}

int ctrlBagCount(const Sim& sim, Entity pawn, const char* def) {
    const Gear* gear = sim.things().registry().try_get<Gear>(pawn);
    int count = 0;
    if (gear != nullptr) {
        for (const Entity item : gear->inventory) {
            if (sim.things().registry().get<Thing>(item).def == sim.defs().idOf("Things", def)) {
                count += sim.things().registry().get<StackCount>(item).count;
            }
        }
    }
    return count;
}

}  // namespace

TEST_CASE("Controls: rescuing and carrying a downed friend are emergencies that sprint; the player's own pawn never") {
    auto sim = ctrlSim();
    const Entity pawn = ctrlColonist(*sim, CellPos{10, 10, 0});
    sim->step();
    CHECK_FALSE(sim->pawnSprinting(pawn));  // at work or idle: it jogs
    sim->things().registry().get<Worker>(pawn).job.type = JobType::Rescue;
    CHECK(sim->pawnSprinting(pawn));
    sim->things().registry().get<Worker>(pawn).job.type = JobType::ExtinguishFire;
    CHECK(sim->pawnSprinting(pawn));
    sim->things().registry().get<Worker>(pawn).job.type = JobType::None;

    // Drafted with no enemy about: still a jog; carrying a downed friend to cover: a sprint.
    sim->submit(DraftCommand{pawn, true});
    sim->step();
    CTRL_REQUIRE(sim->things().registry().all_of<Fighter>(pawn));
    CHECK_FALSE(sim->pawnSprinting(pawn));
    sim->things().registry().get<Fighter>(pawn).mode = FightMode::Rescue;
    CHECK(sim->pawnSprinting(pawn));

    // The player's own colonist: Unreal's sprint toggle sets its speed, never the sim.
    sim->submit(ControlPawnCommand{pawn});
    sim->step();
    CHECK_FALSE(sim->pawnSprinting(pawn));
}

TEST_CASE("Controls: the player's colonist picks up resources it walks past, but not chunks or stockpiled things") {
    auto sim = ctrlSim();
    const Entity pawn = ctrlColonist(*sim, CellPos{10, 10, 0});
    sim->submit(ControlPawnCommand{pawn});
    sim->step();
    CTRL_REQUIRE(sim->things().registry().all_of<PlayerControl>(pawn));
    CHECK(sim->jobs().autoPickUpRadius() > 0.0f);
    CHECK(sim->jobs().autoPicksUp(sim->defs().idOf("Things", "Steel")));
    CHECK(sim->jobs().autoPicksUp(sim->defs().idOf("Things", "WoodLog")));
    CHECK_FALSE(sim->jobs().autoPicksUp(sim->defs().idOf("Things", "ChunkGranite")));
    CHECK_FALSE(sim->jobs().autoPicksUp(sim->defs().idOf("Things", "MealSimple")));

    const Entity steel = sim->things().spawn("Steel", CellPos{11, 10, 0}, 20);
    const Entity wood = sim->things().spawn("WoodLog", CellPos{10, 11, 0}, 10);
    const Entity chunk = sim->things().spawn("ChunkGranite", CellPos{9, 10, 0}, 1);
    const Entity far = sim->things().spawn("Steel", CellPos{20, 10, 0}, 5);
    // A stockpile's steel stays where it is.
    sim->submit(CreateStockpileCommand{CellRect{10, 9, 1, 1, 0}});
    sim->step();
    const Entity stored = sim->things().spawn("Steel", CellPos{10, 9, 0}, 7);
    CTRL_REQUIRE(steel != kNoEntity && wood != kNoEntity && chunk != kNoEntity && far != kNoEntity && stored != kNoEntity);
    sim->stepMany(30);

    CHECK(ctrlBagCount(*sim, pawn, "Steel") == 20);
    CHECK(ctrlBagCount(*sim, pawn, "WoodLog") == 10);
    CHECK(sim->things().spawned(chunk));
    CHECK(sim->things().spawned(far));
    CHECK(sim->things().spawned(stored));

    // Walking up to the far stack takes it too.
    for (int i = 0; i < 40; ++i) {
        sim->submit(DirectMoveCommand{pawn, 10.5f + 0.25f * static_cast<float>(i), 10.5f});
        sim->step();
    }
    CHECK(ctrlBagCount(*sim, pawn, "Steel") == 25);
}
