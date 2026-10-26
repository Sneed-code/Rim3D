#include "ColonySim/Clock.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Narrator.h"
#include "ColonySim/Raids.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"

#include <doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace colony;

#define NARRATOR_REQUIRE(...)  \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// The game's real data. These tests check docs/design/narrator.md.
std::shared_ptr<const DefDatabase> narratorTestDefs() {
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

// A flat map with the Crashlanded colonists in the middle.
std::unique_ptr<Sim> narratorTestSim(uint64_t seed = 5, int size = 96) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = size;
    settings.flatMap = true;
    settings.scenario = "Crashlanded";
    return std::make_unique<Sim>(settings, narratorTestDefs());
}

// A narrator on its own clock, over a sim's colony: `days` of incident checks go by in no time.
// Every incident in `incidents` gets a worker that only writes down when it came.
struct NarratorTestRun {
    std::unique_ptr<Sim> sim = narratorTestSim();
    GameTime time;
    NarratorSystem narrator{sim->raids(), sim->defs(), time, 77};
    std::vector<Event> events;
    std::vector<std::pair<std::string, double>> came;  // incident, day

    explicit NarratorTestRun(const std::vector<std::string>& incidents) {
        narrator.setEventLog(&events);
        for (const std::string& name : incidents) {
            narrator.setWorker(name, [this, name](IncidentFiring& f) {
                came.emplace_back(name, static_cast<double>(time.seconds) / static_cast<double>(kSecondsPerDay));
                f.text = name;
                return true;
            });
        }
    }
    void run(double days) {
        const auto end = time.seconds + static_cast<int64_t>(days * static_cast<double>(kSecondsPerDay));
        const int64_t every = narrator.rules().checkSeconds;
        while (time.seconds < end) {
            time.seconds += every;
            narrator.step();
        }
    }
    std::vector<double> daysOf(const std::string& name) const {
        std::vector<double> out;
        for (const auto& [n, d] : came) {
            if (n == name) {
                out.push_back(d);
            }
        }
        return out;
    }
};

const Event* narratorTestEvent(const Sim& sim, EventType type) {
    for (const Event& e : sim.events()) {
        if (e.type == type) {
            return &e;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("Narrator: off until one is picked, then Orrin's big threats come on his cycle") {
    NarratorTestRun run({"RaidEnemy"});
    NARRATOR_REQUIRE(run.sim->setupErrors().empty());
    CHECK(run.narrator.state().narrator.empty());
    CHECK(run.narrator.state().difficulty == "StriveToSurvive");
    run.run(20.0);
    CHECK(run.came.empty());  // no narrator: nothing comes by itself

    std::string error;
    CHECK_FALSE(run.narrator.setNarrator("Cassandra", "", error));
    CHECK(error.find("no narrator") != std::string::npos);
    CHECK_FALSE(run.narrator.setNarrator("Orrin", "Hard", error));
    NARRATOR_REQUIRE(run.narrator.setNarrator("Orrin", "", error));
    run.run(61.0);
    const std::vector<double> raids = run.daysOf("RaidEnemy");
    // Turned on at day 20, past his first threat day: a cycle of 1.5 days on and 4.6 off from then,
    // one big threat in each "on" part.
    NARRATOR_REQUIRE(raids.size() >= 9);
    CHECK(raids.size() <= 11);
    for (size_t i = 0; i < raids.size(); ++i) {
        const double cycleStart = 20.0 + 6.1 * static_cast<double>(i);
        CHECK(raids[i] >= cycleStart - 0.02);
        CHECK(raids[i] <= cycleStart + 1.5 + 0.02);
    }
    // Each one says so in a ThreatBig letter with its threat points.
    NARRATOR_REQUIRE(!run.events.empty());
    CHECK(run.events[0].type == EventType::Letter);
    CHECK(run.events[0].count == static_cast<int>(LetterKind::ThreatBig));
    CHECK(run.events[0].amount >= 35.0f);
    CHECK(run.narrator.state().incidents == raids.size());
}

TEST_CASE("Narrator: from the start, Orrin's first raid comes on day 5 and Maren's on day 8") {
    NarratorTestRun orrin({"RaidEnemy"});
    std::string error;
    NARRATOR_REQUIRE(orrin.narrator.setNarrator("Orrin", "", error));
    orrin.run(12.0);
    const std::vector<double> o = orrin.daysOf("RaidEnemy");
    NARRATOR_REQUIRE(o.size() == 2);
    CHECK(o[0] >= 5.0);
    CHECK(o[0] <= 6.5 + 0.02);
    CHECK(o[1] >= 11.1 - 0.02);

    NarratorTestRun maren({"RaidEnemy"});
    NARRATOR_REQUIRE(maren.narrator.setNarrator("Maren", "", error));
    maren.run(60.0);
    const std::vector<double> m = maren.daysOf("RaidEnemy");
    NARRATOR_REQUIRE(!m.empty());
    CHECK(m[0] >= 8.0);
    CHECK(m[0] <= 9.0 + 0.02);
    // About nine days apart: fewer than Orrin's ten in the same time.
    CHECK(m.size() >= 5);
    CHECK(m.size() <= 7);
}

TEST_CASE("Narrator: Jinx rolls every category on its own, and earliest days and cooldowns hold") {
    NarratorTestRun run({"RaidEnemy", "MadAnimal", "Aurora", "Eclipse"});
    std::string error;
    NARRATOR_REQUIRE(run.narrator.setNarrator("Jinx", "", error));
    run.run(300.0);
    // Big threats: a mean 5 days between from day 5, about 59 in 295 days.
    const std::vector<double> raids = run.daysOf("RaidEnemy");
    CHECK(raids.size() >= 35);
    CHECK(raids.size() <= 90);
    NARRATOR_REQUIRE(!raids.empty());
    CHECK(raids[0] >= 5.0);
    // Mad animals: never before day 3, never within 15 days of the last one.
    const std::vector<double> mad = run.daysOf("MadAnimal");
    NARRATOR_REQUIRE(mad.size() >= 5);
    CHECK(mad[0] >= 3.0);
    for (size_t i = 1; i < mad.size(); ++i) {
        CHECK(mad[i] - mad[i - 1] >= 15.0 - 0.001);
    }
    // Misc (mean 1.35 days) is shared between the two that can happen: both come, 10 days apart at least.
    const std::vector<double> aurora = run.daysOf("Aurora");
    const std::vector<double> eclipse = run.daysOf("Eclipse");
    CHECK(aurora.size() >= 8);
    CHECK(eclipse.size() >= 8);
    for (size_t i = 1; i < aurora.size(); ++i) {
        CHECK(aurora[i] - aurora[i - 1] >= 10.0 - 0.001);
    }
    // Nothing without a worker ever came.
    for (const auto& [name, day] : run.came) {
        CHECK((name == "RaidEnemy" || name == "MadAnimal" || name == "Aurora" || name == "Eclipse"));
        (void)day;
    }
    // Jinx scales each threat's points by 0.5 to 1.5: they are not all the same.
    float lo = 1e9f;
    float hi = 0.0f;
    for (const Event& e : run.events) {
        if (e.count == static_cast<int>(LetterKind::ThreatBig)) {
            lo = std::min(lo, e.amount);
            hi = std::max(hi, e.amount);
        }
    }
    CHECK(hi > lo);
}

TEST_CASE("Narrator: threat points follow the difficulty and the narrator; wealth counts the colonists") {
    auto sim = narratorTestSim();
    NARRATOR_REQUIRE(sim->setupErrors().empty());
    // Six colonists at 1,750 each, on top of their starting things (wiki "Raid points": creatures count).
    CHECK(sim->raids().colonyWealth() > 6 * 1750.0f);
    CHECK(sim->raids().colonistCount() == 6);
    const float base = sim->raids().threatPoints();
    CHECK(base > 35.0f);  // over the minimum, so the factors show
    CHECK(sim->narrator().threatPoints() == doctest::Approx(base));  // Strive to survive, no narrator

    sim->submit(SetNarratorCommand{"Orrin", "LosingIsFun"});
    sim->step();
    CHECK(narratorTestEvent(*sim, EventType::CommandFailed) == nullptr);
    CHECK(sim->narrator().threatPoints() == doctest::Approx(base * 2.2f).epsilon(0.01));
    sim->submit(SetNarratorCommand{"Maren", ""});  // keeps the difficulty
    sim->step();
    CHECK(sim->narrator().state().difficulty == "LosingIsFun");
    CHECK(sim->narrator().threatPoints() == doctest::Approx(base * 2.2f * 0.85f).epsilon(0.01));
    sim->submit(SetNarratorCommand{"Orrin", "Peaceful"});
    sim->step();
    CHECK(sim->narrator().threatPoints() == doctest::Approx(35.0f));  // 0.1 of it is under the minimum

    Snapshot snap;
    sim->writeSnapshot(snap);
    CHECK(snap.narrator.narrator == sim->defs().idOf("Narrators", "Orrin"));
    CHECK(snap.narrator.difficulty == sim->defs().idOf("Difficulties", "Peaceful"));
    CHECK(snap.narrator.threatPoints == doctest::Approx(35.0f));
    CHECK(snap.narrator.wealth > 6 * 1750.0f);

    sim->submit(SetNarratorCommand{"", ""});
    sim->step();
    sim->writeSnapshot(snap);
    CHECK_FALSE(snap.narrator.narrator.valid());
    sim->submit(SetNarratorCommand{"Orrin", "Brutal"});
    sim->step();
    CHECK(narratorTestEvent(*sim, EventType::CommandFailed) != nullptr);
}

TEST_CASE("Narrator: the population pushes wanderers and survivors") {
    NarratorTestRun run({});
    // Day 0: the push has not faded in yet.
    CHECK(run.narrator.populationFactor() == doctest::Approx(1.0f));
    run.run(10.0);
    // Six colonists from day 10: 1 + (0.7 - 1) on the wiki's curve between 5 (1.0) and 9 (0.4).
    CHECK(run.narrator.populationFactor() == doctest::Approx(0.85f));
    run.narrator.setWorker("WandererJoin", [](IncidentFiring&) { return true; });
    CHECK(run.narrator.weight("WandererJoin") == doctest::Approx(0.85f));
    CHECK(run.narrator.weight("ResourcePodCrash") == doctest::Approx(0.0f));  // no worker yet
}

TEST_CASE("Narrator: a raid comes as the first incident, with its letter") {
    auto sim = narratorTestSim(9, 128);
    sim->submit(FireIncidentCommand{"RaidEnemy", 200.0f});
    sim->step();
    CHECK(narratorTestEvent(*sim, EventType::CommandFailed) == nullptr);
    const Event* letter = narratorTestEvent(*sim, EventType::Letter);
    NARRATOR_REQUIRE(letter != nullptr);
    CHECK(letter->def == sim->defs().idOf("Incidents", "RaidEnemy"));
    CHECK(letter->count == static_cast<int>(LetterKind::ThreatBig));
    CHECK(letter->amount == doctest::Approx(200.0f));
    CHECK(letter->message.find("Raid") != std::string::npos);
    NARRATOR_REQUIRE(sim->raids().state().squads.size() == 1);
    const Squad& squad = sim->raids().state().squads.front();
    CHECK(letter->squad == squad.id);
    CHECK(letter->cell == squad.entry);
    CHECK(squad.points == doctest::Approx(200.0f));
    CHECK((squad.faction == "Pirate" || squad.faction == "ClanSavage"));

    // Unknown incidents, and ones nothing can carry out yet, fail in plain words.
    sim->submit(FireIncidentCommand{"Meteorite", 0.0f});
    sim->step();
    CHECK(narratorTestEvent(*sim, EventType::CommandFailed) != nullptr);
    sim->submit(FireIncidentCommand{"Aurora", 0.0f});
    sim->step();
    const Event* failed = narratorTestEvent(*sim, EventType::CommandFailed);
    NARRATOR_REQUIRE(failed != nullptr);
    CHECK(failed->message.find("aurora") != std::string::npos);
}

TEST_CASE("Narrator: saves and loads, and both games go on the same") {
    auto sim = narratorTestSim(13, 64);
    sim->submit(SetNarratorCommand{"Jinx", "BloodAndDust"});
    sim->submit(FireIncidentCommand{"RaidEnemy", 50.0f});
    for (int i = 0; i < 2 * 1440 + 3; ++i) {
        sim->step();
    }
    const NarratorState before = sim->narrator().state();
    CHECK(before.narrator == "Jinx");
    CHECK(before.fired.size() == 1);

    const std::vector<uint8_t> bytes = saveSim(*sim);
    SaveLoadResult loaded = loadSim(bytes, narratorTestDefs());
    NARRATOR_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.error.empty());
    const NarratorState after = loaded.sim->narrator().state();
    CHECK(after.narrator == "Jinx");
    CHECK(after.difficulty == "BloodAndDust");
    CHECK(after.rngState == before.rngState);
    NARRATOR_REQUIRE(after.fired.size() == 1);
    CHECK(after.fired[0].incident == "RaidEnemy");
    CHECK(after.incidents == before.incidents);
    CHECK(loaded.sim->stateHash() == sim->stateHash());
    CHECK(saveSim(*loaded.sim) == bytes);
    for (int i = 0; i < 3000; ++i) {
        sim->step();
        loaded.sim->step();
    }
    CHECK(loaded.sim->stateHash() == sim->stateHash());

    // The commands go through the replay file and back.
    const std::string json = writeReplay(recordReplay(*sim));
    CHECK(json.find("SetNarrator") != std::string::npos);
    CHECK(json.find("FireIncident") != std::string::npos);
    Replay replay;
    std::string error;
    NARRATOR_REQUIRE(readReplay(json, replay, error));
    CHECK(replay.commands.size() == sim->commandLog().size());
}
