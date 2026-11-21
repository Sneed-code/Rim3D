#include "doctest.h"

#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Looks.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

#define NG_REQUIRE(...)        \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

// Tests for docs/design/new-game.md: the Crash Landed scenario and the character screen's edits, on the real data.
namespace {

std::shared_ptr<const DefDatabase> newGameDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        for (const std::string& e : errors) {
            MESSAGE(e);
        }
        return db;
    }();
    return defs;
}

SimSettings newGameSettings(uint64_t seed) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = 128;
    settings.checkpointEvery = 0;
    settings.scenario = "CrashLanded";
    return settings;
}

std::vector<Entity> newGameColonists(const Sim& sim) {
    std::vector<Entity> out;
    for (const auto [e, c] : sim.things().registry().view<const Colonist>().each()) {
        (void)c;
        out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](Entity a, Entity b) { return entt::to_entity(a) < entt::to_entity(b); });
    return out;
}

// Items of `def` lying within `radius` metres of `at` (the landing spot), stacks added up.
int newGameItemsNear(const Sim& sim, const char* def, Position at, float radius) {
    int total = 0;
    const DefId id = sim.defs().idOf("Things", def);
    for (const auto [e, thing, stack, pos] : sim.things().registry().view<const Thing, const StackCount, const Position>().each()) {
        (void)e;
        const float dx = pos.x - at.x;
        const float dy = pos.y - at.y;
        if (thing.def == id && std::sqrt(dx * dx + dy * dy) <= radius) {
            total += stack.count;
        }
    }
    return total;
}

std::map<uint64_t, std::string> newGameFailures(const Sim& sim) {
    std::map<uint64_t, std::string> failed;
    for (const Event& e : sim.events()) {
        if (e.type == EventType::CommandFailed) {
            failed[e.command] = e.message;
        }
    }
    return failed;
}

}  // namespace

TEST_CASE("Crash Landed: the player and two human strangers with sneed's starting kit") {
    const auto defs = newGameDefs();
    const Def* scenario = defs->find("Scenarios", "CrashLanded");
    NG_REQUIRE(scenario != nullptr);
    CHECK(scenario->flag("menu"));
    CHECK(scenario->text("biome") == "TropicalRainforest");

    Sim sim(newGameSettings(7), defs);
    CHECK(sim.setupErrors().empty());
    for (const std::string& e : sim.setupErrors()) {
        MESSAGE(e);
    }
    const std::vector<Entity> people = newGameColonists(sim);
    NG_REQUIRE(people.size() == 3);
    int protagonists = 0;
    for (const Entity e : people) {
        const Colonist& c = sim.things().registry().get<Colonist>(e);
        protagonists += c.protagonist ? 1 : 0;
        const PawnLooks* looks = sim.things().registry().try_get<PawnLooks>(e);
        NG_REQUIRE(looks != nullptr);
        CHECK(looks->lineage == "Baseline");  // humans only for now
        CHECK(looks->creator.empty());
    }
    CHECK(protagonists == 1);
    CHECK(sim.things().registry().get<Colonist>(people[0]).protagonist);

    // The kit lies beside them: nothing more, nothing less (only one gun, carried by nobody).
    const Position at = sim.things().registry().get<Position>(people[0]);
    CHECK(newGameItemsNear(sim, "MealSurvivalPack", at, 12.0f) == 50);
    CHECK(newGameItemsNear(sim, "Silver", at, 12.0f) == 200);
    CHECK(newGameItemsNear(sim, "Steel", at, 12.0f) == 50);
    CHECK(newGameItemsNear(sim, "ComponentIndustrial", at, 12.0f) == 30);
    CHECK(newGameItemsNear(sim, "Gun_BoltActionRifle", at, 12.0f) == 1);
    CHECK(newGameItemsNear(sim, "WoodLog", at, 12.0f) == 0);
    for (const Entity e : people) {
        const Gear* gear = sim.things().registry().try_get<Gear>(e);
        CHECK((gear == nullptr || gear->weapon == kNoEntity));
    }
}

TEST_CASE("Crash Landed: the character screen renames, changes gender and stores the creator's look, before the start only") {
    const auto defs = newGameDefs();
    Sim sim(newGameSettings(11), defs);
    NG_REQUIRE(newGameColonists(sim).size() == 3);
    const Entity player = newGameColonists(sim)[0];
    const Colonist before = sim.things().registry().get<Colonist>(player);
    const std::string childhood = before.childhood;

    EditColonistCommand rename;
    rename.slot = 0;
    rename.firstName = "Elara";
    rename.nickName = "Ell";
    rename.lastName = "Rivera";
    rename.female = before.female ? 0 : 1;
    rename.setCreator = true;
    rename.creator = "v=1;height=1.04;weight=-0.2;hair=3;";
    const uint64_t good = sim.submit(rename);
    EditColonistCommand badSlot;
    badSlot.slot = 3;
    const uint64_t bad = sim.submit(badSlot);
    EditColonistCommand longName;
    longName.slot = 1;
    longName.firstName = std::string(25, 'a');
    const uint64_t tooLong = sim.submit(longName);
    sim.step();
    const auto failed = newGameFailures(sim);
    CHECK(failed.count(good) == 0);
    CHECK(failed.count(bad) == 1);
    CHECK(failed.count(tooLong) == 1);

    const std::vector<Entity> people = newGameColonists(sim);
    NG_REQUIRE(people.size() == 3);
    CHECK(people[0] == player);  // edited in place, not remade
    const Colonist& after = sim.things().registry().get<Colonist>(player);
    CHECK(after.firstName == "Elara");
    CHECK(after.nickName == "Ell");
    CHECK(after.lastName == "Rivera");
    CHECK(after.female != before.female);
    CHECK(after.childhood == childhood);  // the rest stays
    CHECK(after.protagonist);
    const PawnLooks& looks = sim.things().registry().get<PawnLooks>(player);
    CHECK(looks.female == after.female);
    CHECK(looks.creator == rename.creator);
    // The snapshot shows it to Unreal.
    Snapshot snap;
    sim.writeSnapshot(snap);
    bool seen = false;
    for (const ThingView& view : snap.things) {
        if (view.id == player && view.looks >= 0) {
            seen = snap.looks[static_cast<size_t>(view.looks)].creator == rename.creator;
        }
    }
    CHECK(seen);

    // Same seed and same edits give the same game; a save and a replay keep them.
    Sim twin(newGameSettings(11), defs);
    twin.submit(rename);
    twin.submit(badSlot);
    twin.submit(longName);
    twin.step();
    CHECK(twin.stateHash() == sim.stateHash());
    SaveLoadResult loaded = loadSim(saveSim(sim), defs);
    CHECK(loaded.error.empty());
    NG_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->stateHash() == sim.stateHash());
    CHECK(loaded.sim->things().registry().get<PawnLooks>(player).creator == rename.creator);
    Replay replay;
    std::string error;
    CHECK(readReplay(writeReplay(recordReplay(sim)), replay, error));
    int edits = 0;
    for (const LoggedCommand& logged : replay.commands) {
        if (const auto* edit = std::get_if<EditColonistCommand>(&logged.command)) {
            edits += 1;
            if (edit->slot == 0) {
                CHECK(edit->creator == rename.creator);
                CHECK(edit->firstName == "Elara");
            }
        }
    }
    CHECK(edits == 3);

    // Once the game runs it is too late.
    sim.stepMany(3);
    EditColonistCommand late;
    late.slot = 0;
    late.firstName = "Late";
    const uint64_t lateId = sim.submit(late);
    sim.step();
    CHECK(newGameFailures(sim).count(lateId) == 1);
    CHECK(sim.things().registry().get<Colonist>(player).firstName == "Elara");
}

TEST_CASE("Crash Landed: the character screen's edits apply at once before the start and replay the same") {
    const auto defs = newGameDefs();
    Sim sim(newGameSettings(5), defs);
    sim.submit(RerollColonistCommand{1});
    CHECK(sim.applyPregameCommands());
    CHECK(sim.time().steps == 0);
    CHECK(sim.time().seconds == 0);
    EditColonistCommand edit;
    edit.slot = 1;
    edit.firstName = "Mina";
    edit.setCreator = true;
    edit.creator = "seed=4;female=1;";
    sim.submit(edit);
    sim.submit(RerollColonistCommand{2});
    CHECK(sim.applyPregameCommands());
    CHECK(sim.things().registry().get<Colonist>(newGameColonists(sim)[1]).firstName == "Mina");
    sim.step();
    CHECK_FALSE(sim.applyPregameCommands());

    // A replay (all of them in its first step) comes out the same.
    Sim twin(newGameSettings(5), defs);
    twin.submit(RerollColonistCommand{1});
    twin.submit(edit);
    twin.submit(RerollColonistCommand{2});
    twin.step();
    CHECK(twin.stateHash() == sim.stateHash());
}
