// The day length (docs/design/day-length.md): a player option from 48 real minutes (the
// default) to 2 hours. Steps stay real time, so walking, fighting and work keep their speed; the
// calendar (needs, growth, the clock) moves on 48 of every `dayMinutes` steps.

#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Melee.h"
#include "ColonySim/Needs.h"
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

#define DAYLEN_REQUIRE(...)    \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> dayLengthDefs() {
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
        return db;
    }();
    return defs;
}

std::unique_ptr<Sim> dayLengthSim(int dayMinutes, int size = 64) {
    SimSettings settings;
    settings.seed = 21;
    settings.mapSize = size;
    settings.flatMap = true;
    settings.dayMinutes = dayMinutes;
    return std::make_unique<Sim>(settings, dayLengthDefs());
}

Entity dayLengthPawn(Sim& sim, float x, float y) {
    const Entity pawn = sim.things().spawn("Human", CellPos{static_cast<int>(x), static_cast<int>(y), 0});
    Position& pos = sim.things().registry().get<Position>(pawn);
    pos.x = x;
    pos.y = y;
    return pawn;
}

int dayLengthCount(const Sim& sim, EventType type) {
    return static_cast<int>(std::count_if(sim.events().begin(), sim.events().end(), [&](const Event& e) { return e.type == type; }));
}

}  // namespace

TEST_CASE("Day length: the 48-minute day moves the calendar every step, a longer one on 48 of every N steps") {
    auto normal = dayLengthSim(48);
    normal->stepMany(1000);
    CHECK(normal->time().seconds == 1000);
    CHECK(normal->time().steps == 1000);
    CHECK(normal->dayMinutes() == 48);

    auto twoHours = dayLengthSim(120);
    twoHours->stepMany(1000);
    CHECK(twoHours->time().steps == 1000);
    CHECK(twoHours->time().seconds == 400);  // 48 / 120 of the steps
    CHECK(twoHours->dayMinutes() == 120);

    // A whole game day takes 120 real minutes of steps at 1x.
    auto oneDay = dayLengthSim(120, 32);
    oneDay->stepMany(static_cast<int64_t>(120) * 60 * kStepsPerRealSecondAt1x);
    CHECK(oneDay->time().seconds == kSecondsPerDay);

    // Out of range settings are held to 48 to 120.
    CHECK(dayLengthSim(10, 32)->dayMinutes() == 48);
    CHECK(dayLengthSim(500, 32)->dayMinutes() == 120);
}

TEST_CASE("Day length: changed in play by command, refused outside 48 to 120, and shown in the snapshot") {
    auto sim = dayLengthSim(48, 32);
    sim->stepMany(10);
    sim->submit(SetDayLengthCommand{30});
    sim->step();
    CHECK(dayLengthCount(*sim, EventType::CommandFailed) == 1);
    sim->submit(SetDayLengthCommand{121});
    sim->step();
    CHECK(dayLengthCount(*sim, EventType::CommandFailed) == 1);
    CHECK(sim->dayMinutes() == 48);

    sim->submit(SetDayLengthCommand{96});
    sim->step();
    CHECK(dayLengthCount(*sim, EventType::CommandFailed) == 0);
    CHECK(sim->dayMinutes() == 96);
    const int64_t seconds = sim->time().seconds;
    const int64_t steps = sim->time().steps;
    sim->stepMany(200);
    CHECK(sim->time().steps == steps + 200);
    CHECK(sim->time().seconds == seconds + 100);  // half speed

    Snapshot snap;
    sim->writeSnapshot(snap);
    CHECK(snap.dayMinutes == 96);
    CHECK(snap.steps == sim->time().steps);
    CHECK(snap.gameSeconds == sim->time().seconds);
}

TEST_CASE("Day length: hunger and rest follow the calendar, so a 2-hour day makes them last 2.5 times as many steps") {
    auto normal = dayLengthSim(48);
    auto longDay = dayLengthSim(120);
    const Entity a = dayLengthPawn(*normal, 20.5f, 20.5f);
    const Entity b = dayLengthPawn(*longDay, 20.5f, 20.5f);
    normal->jobs().addWorker(a);
    longDay->jobs().addWorker(b);
    DAYLEN_REQUIRE(normal->things().registry().all_of<Needs>(a));
    DAYLEN_REQUIRE(longDay->things().registry().all_of<Needs>(b));

    normal->stepMany(3600);
    longDay->stepMany(3600);
    const float fellNormal = 1.0f - normal->things().registry().get<Needs>(a).food;
    const float fellLong = 1.0f - longDay->things().registry().get<Needs>(b).food;
    CHECK(fellNormal > 0.0f);
    CHECK(fellLong == doctest::Approx(fellNormal * 0.4f).epsilon(0.02));

    // The same calendar time gives the same hunger.
    longDay->stepMany(5400);  // 9000 steps in all: 3600 calendar seconds
    CHECK(longDay->time().seconds == 3600);
    CHECK(1.0f - longDay->things().registry().get<Needs>(b).food == doctest::Approx(fellNormal).epsilon(0.001));
}

TEST_CASE("Day length: walking keeps its real-time speed") {
    auto normal = dayLengthSim(48);
    auto longDay = dayLengthSim(120);
    const Entity a = dayLengthPawn(*normal, 10.5f, 10.5f);
    const Entity b = dayLengthPawn(*longDay, 10.5f, 10.5f);
    for (Sim* sim : {normal.get(), longDay.get()}) {
        const Entity pawn = sim == normal.get() ? a : b;
        sim->jobs().addWorker(pawn);
        sim->submit(DraftCommand{pawn, true});
        sim->step();
        sim->submit(MoveOrderCommand{pawn, CellPos{50, 10, 0}});
        sim->stepMany(120);  // four real seconds at 1x
    }
    const Position& pa = normal->things().registry().get<Position>(a);
    const Position& pb = longDay->things().registry().get<Position>(b);
    CHECK(pa.x > 20.0f);  // it walked a good way
    CHECK(pb.x == doctest::Approx(pa.x).epsilon(0.001));
    CHECK(pb.y == doctest::Approx(pa.y).epsilon(0.001));
}

TEST_CASE("Day length: a melee swing takes the same steps whatever the day length") {
    int first = -1;
    for (const int minutes : {48, 120}) {
        CAPTURE(minutes);
        auto sim = dayLengthSim(minutes);
        const Entity attacker = dayLengthPawn(*sim, 30.5f, 30.5f);  // fists: 60 steps a swing at Melee 10
        const Entity target = dayLengthPawn(*sim, 31.3f, 30.5f);
        sim->submit(MeleeCommand{attacker, target, MeleeMode::Attack});
        sim->step();
        DAYLEN_REQUIRE(dayLengthCount(*sim, EventType::MeleeSwing) == 1);
        int steps = 1;
        while (!sim->melee().ready(attacker) && steps < 200) {
            sim->step();
            ++steps;
        }
        // No skills: Melee 4, 0.88 of the pace (real-time-combat.md): 68 steps at RimWorld's pace; a person's fists swing
        // at half of that with no pause (MeleeRules unarmedTimeFactor, unarmedGapSteps: sneed 2026-11-24), about 34.
        CHECK(steps >= 32);
        CHECK(steps <= 37);
        if (first < 0) {
            first = steps;
        }
        CHECK(steps == first);
    }
}

TEST_CASE("Day length: a save keeps the step clock, the day length and the calendar's share") {
    auto sim = dayLengthSim(48, 32);
    sim->stepMany(7);
    sim->submit(SetDayLengthCommand{100});
    sim->stepMany(33);
    const std::vector<uint8_t> bytes = saveSim(*sim);
    SaveLoadResult loaded = loadSim(bytes, dayLengthDefs());
    DAYLEN_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->time().steps == sim->time().steps);
    CHECK(loaded.sim->time().seconds == sim->time().seconds);
    CHECK(loaded.sim->dayMinutes() == 100);
    CHECK(loaded.sim->stateHash() == sim->stateHash());
    sim->stepMany(50);
    loaded.sim->stepMany(50);
    CHECK(loaded.sim->time().seconds == sim->time().seconds);
    CHECK(loaded.sim->stateHash() == sim->stateHash());
}

TEST_CASE("Day length: a replay of a longer day plays back the same, the command logged by step") {
    auto sim = dayLengthSim(72, 32);
    sim->stepMany(30);
    sim->submit(SetDayLengthCommand{120});
    sim->stepMany(70);
    const Replay replay = recordReplay(*sim);
    CHECK(replay.settings.dayMinutes == 72);
    DAYLEN_REQUIRE(!replay.commands.empty());
    CHECK(replay.commands.back().step == 30);  // the step it was applied in, not the calendar second
    CHECK(replay.endStep == 100);

    Replay read;
    std::string error;
    DAYLEN_REQUIRE(readReplay(writeReplay(replay), read, error));
    CHECK(read.settings.dayMinutes == 72);
    Sim again(read.settings, dayLengthDefs());
    playReplay(read, again, read.endStep);
    CHECK(again.time().steps == sim->time().steps);
    CHECK(again.time().seconds == sim->time().seconds);
    CHECK(again.dayMinutes() == 120);
    CHECK(again.stateHash() == sim->stateHash());
}
