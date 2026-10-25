#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Talents.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define TAL_REQUIRE(...)       \
    do {                       \
        CHECK((__VA_ARGS__));  \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> talDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

Sim talSim() { return Sim(SimSettings{5, 48, 0, 0, true}, talDefs()); }

// A human; the protagonist if asked.
Entity talPawn(Sim& sim, CellPos cell, bool protagonist) {
    const Entity pawn = sim.things().spawn("Human", cell);
    Colonist& c = sim.things().registry().get_or_emplace<Colonist>(pawn);
    c.protagonist = protagonist;
    return pawn;
}

std::string talLastFailure(const Sim& sim) {
    std::string out;
    for (const Event& e : sim.events()) {
        if (e.type == EventType::CommandFailed) {
            out = e.message;
        }
    }
    return out;
}

}  // namespace

TEST_CASE("Talents: the defs load and XP turns into points") {
    Sim sim = talSim();
    const TalentSystem& talents = sim.talents();
    TAL_REQUIRE(talents.rules().present);
    CHECK(talents.talents().size() == 7);
    CHECK(talents.xpForPoints(0) == 0);
    CHECK(talents.xpForPoints(1) == 100);
    CHECK(talents.xpForPoints(2) == 225);  // 100 + 125

    const Entity hero = talPawn(sim, CellPos{10, 10, 0}, true);
    const Entity other = talPawn(sim, CellPos{12, 10, 0}, false);
    sim.submit(AddTalentXpCommand{hero, 99});
    sim.submit(AddTalentXpCommand{other, 50});
    sim.step();
    CHECK(sim.talents().pointsEarned(hero) == 0);
    CHECK(talLastFailure(sim) == "only the protagonist has talents");
    sim.submit(AddTalentXpCommand{hero, 1});
    sim.step();
    CHECK(sim.talents().pointsFree(hero) == 1);
    const bool sawPoint = std::any_of(sim.events().begin(), sim.events().end(), [&](const Event& e) {
        return e.type == EventType::TalentPoint && e.thing == hero && e.amount == doctest::Approx(1.0f);
    });
    CHECK(sawPoint);
    sim.submit(AddTalentXpCommand{hero, 125});
    sim.step();
    CHECK(sim.talents().pointsEarned(hero) == 2);
    CHECK(sim.talents().state(other) == nullptr);
}

TEST_CASE("Talents: spending points follows cost, requirements and top rank") {
    Sim sim = talSim();
    const Entity hero = talPawn(sim, CellPos{10, 10, 0}, true);
    sim.submit(SpendTalentPointCommand{hero, "FastBuilder"});
    sim.step();
    CHECK(talLastFailure(sim).find("not enough") != std::string::npos);

    // 100 + 125 + 150 + 175 + 200 = 750 XP is five points.
    sim.submit(AddTalentXpCommand{hero, 750});
    sim.submit(SpendTalentPointCommand{hero, "NoSuchTalent"});
    sim.step();
    CHECK(talLastFailure(sim) == "no talent 'NoSuchTalent'");
    CHECK(sim.talents().pointsFree(hero) == 5);

    sim.submit(SpendTalentPointCommand{hero, "MasterBuilder"});
    sim.step();
    CHECK(talLastFailure(sim).find("needs FastBuilder") != std::string::npos);
    for (int i = 0; i < 3; ++i) {
        sim.submit(SpendTalentPointCommand{hero, "FastBuilder"});
    }
    sim.step();
    CHECK(talLastFailure(sim).empty());
    CHECK(sim.talents().rank(hero, "FastBuilder") == 3);
    CHECK(sim.talents().pointsFree(hero) == 2);
    sim.submit(SpendTalentPointCommand{hero, "MasterBuilder"});  // costs 2
    sim.step();
    CHECK(sim.talents().rank(hero, "MasterBuilder") == 1);
    CHECK(sim.talents().pointsFree(hero) == 0);
    sim.submit(SpendTalentPointCommand{hero, "Haggler"});
    sim.step();
    CHECK(talLastFailure(sim).find("not enough") != std::string::npos);

    // The top rank is a limit.
    sim.submit(AddTalentXpCommand{hero, 100000});
    sim.stepMany(1);
    for (int i = 0; i < 3; ++i) {
        sim.submit(SpendTalentPointCommand{hero, "FastBuilder"});
    }
    sim.step();
    CHECK(sim.talents().rank(hero, "FastBuilder") == 5);
    CHECK(talLastFailure(sim).find("top rank") != std::string::npos);
}

TEST_CASE("Talents: ranks change the stats the systems ask for") {
    Sim sim = talSim();
    const Entity hero = talPawn(sim, CellPos{10, 10, 0}, true);
    const Entity other = talPawn(sim, CellPos{12, 10, 0}, false);
    const float build = sim.jobs().constructionSpeed(hero);
    const DefId steel = sim.defs().idOf("Things", "Silver");
    TAL_REQUIRE(steel.valid());
    const int carry = sim.jobs().carryLimit(hero, steel);
    CHECK(sim.talents().statFactor(hero, "buildSpeed") == doctest::Approx(1.0));

    sim.submit(AddTalentXpCommand{hero, 100000});
    sim.step();
    for (const char* name : {"FastBuilder", "FastBuilder", "SteadyAim", "PackMule", "Haggler"}) {
        sim.submit(SpendTalentPointCommand{hero, name});
    }
    sim.step();
    CHECK(talLastFailure(sim).empty());
    CHECK(sim.talents().statFactor(hero, "buildSpeed") == doctest::Approx(1.2));
    CHECK(sim.talents().statFactor(hero, "aimSpread") == doctest::Approx(0.94));
    CHECK(sim.talents().statFactor(hero, "carryCapacity") == doctest::Approx(1.2));
    CHECK(sim.talents().statFactor(hero, "tradePrice") == doctest::Approx(0.96));
    CHECK(sim.jobs().constructionSpeed(hero) == doctest::Approx(build * 1.2f));
    CHECK(sim.jobs().carryLimit(hero, steel) > carry);
    // Everyone else is unchanged.
    CHECK(sim.talents().statFactor(other, "buildSpeed") == doctest::Approx(1.0));
    CHECK(sim.jobs().carryLimit(other, steel) == carry);
}

TEST_CASE("Talents: finished jobs give the protagonist XP, other pawns none") {
    Sim sim = talSim();
    const Entity hero = talPawn(sim, CellPos{10, 10, 0}, true);
    const Entity other = talPawn(sim, CellPos{12, 10, 0}, false);
    // Fake the job ending the way the job system reports it.
    std::vector<Event> events;
    for (const Entity pawn : {hero, other}) {
        Event e;
        e.type = EventType::JobEnded;
        e.thing = pawn;
        e.job = JobType::BuildFrame;
        e.message = "done";
        events.push_back(e);
    }
    Event failed = events[0];
    failed.message = "failed: nothing to do";
    events.push_back(failed);
    std::vector<Event> out;
    sim.talents().awardJobXp(events, 0, out);
    TAL_REQUIRE(sim.talents().state(hero) != nullptr);
    CHECK(sim.talents().state(hero)->xp == 6);
    CHECK(sim.talents().state(other) == nullptr);
}

TEST_CASE("Talents: the state saves, loads and replays the same") {
    Sim sim = talSim();
    const Entity hero = talPawn(sim, CellPos{10, 10, 0}, true);
    sim.submit(AddTalentXpCommand{hero, 400});
    sim.step();
    sim.submit(SpendTalentPointCommand{hero, "SteadyAim"});
    sim.submit(SpendTalentPointCommand{hero, "PackMule"});
    sim.stepMany(10);
    CHECK(sim.talents().rank(hero, "SteadyAim") == 1);

    const std::vector<uint8_t> bytes = saveSim(sim);
    SaveLoadResult loaded = loadSim(bytes, talDefs());
    if (!loaded.error.empty()) MESSAGE(loaded.error);
    TAL_REQUIRE(loaded.error.empty());
    TAL_REQUIRE(loaded.sim != nullptr);
    Sim& copy = *loaded.sim;
    CHECK(copy.stateHash() == sim.stateHash());
    CHECK(copy.talents().state(hero)->xp == 400);
    CHECK(copy.talents().rank(hero, "PackMule") == 1);
    CHECK(saveSim(copy) == bytes);
    CHECK(saveToJson(sim).find("\"talents\"") != std::string::npos);

    // The commands go through a replay file.
    const Replay recorded = recordReplay(sim);
    Replay reread;
    std::string error;
    TAL_REQUIRE(readReplay(writeReplay(recorded), reread, error));
    CHECK(writeReplay(reread) == writeReplay(recorded));

    // The snapshot has the protagonist's progress.
    Snapshot snapshot;
    sim.writeSnapshot(snapshot);
    TAL_REQUIRE(snapshot.talents.size() == 1);
    CHECK(snapshot.talents[0].xp == 400);
    CHECK(snapshot.talents[0].pointsFree == 1);  // three points earned, two spent
    CHECK(snapshot.talents[0].xpForNextPoint == 550);
    CHECK(snapshot.talents[0].ranks.size() == 2);
}
