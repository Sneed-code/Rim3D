// Work groups (docs/design/ui-layout.md, the Work tab): the player puts colonists in named groups to sort and filter
// the work grid. SetWorkGroupCommand sets or clears a colonist's group; saves and replays keep it.

#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace colony;

#define WORKGROUP_REQUIRE(...) \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> workGroupDefs() {
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

std::unique_ptr<Sim> workGroupSim() {
    SimSettings settings;
    settings.seed = 33;
    settings.mapSize = 32;
    settings.flatMap = true;
    return std::make_unique<Sim>(settings, workGroupDefs());
}

Entity workGroupColonist(Sim& sim, CellPos cell) {
    const Entity pawn = sim.things().spawn("Human", cell);
    Colonist c;
    c.firstName = "Ada";
    sim.things().registry().emplace<Colonist>(pawn, c);
    sim.jobs().addWorker(pawn, {}, false);
    return pawn;
}

int workGroupFailures(const Sim& sim) {
    return static_cast<int>(
        std::count_if(sim.events().begin(), sim.events().end(), [](const Event& e) { return e.type == EventType::CommandFailed; }));
}

}  // namespace

TEST_CASE("Work groups: a colonist is put in a group, moved and taken out again") {
    auto sim = workGroupSim();
    const Entity pawn = workGroupColonist(*sim, CellPos{10, 10, 0});
    WORKGROUP_REQUIRE(sim->colonistOf(pawn) != nullptr);
    CHECK(sim->colonistOf(pawn)->workGroup.empty());

    sim->submit(SetWorkGroupCommand{pawn, "Growers"});
    sim->step();
    CHECK(sim->colonistOf(pawn)->workGroup == "Growers");
    sim->submit(SetWorkGroupCommand{pawn, "Fighters"});
    sim->step();
    CHECK(sim->colonistOf(pawn)->workGroup == "Fighters");
    sim->submit(SetWorkGroupCommand{pawn, ""});
    sim->step();
    CHECK(sim->colonistOf(pawn)->workGroup.empty());
    CHECK(workGroupFailures(*sim) == 0);
}

TEST_CASE("Work groups: refused for a pawn that is no colonist and for a name over 24 characters") {
    auto sim = workGroupSim();
    const Entity pawn = workGroupColonist(*sim, CellPos{10, 10, 0});
    const Entity stranger = sim->things().spawn("Human", CellPos{12, 10, 0});
    // Events last one step, so each step's failures are counted on their own.
    sim->submit(SetWorkGroupCommand{stranger, "Miners"});
    sim->step();
    CHECK(workGroupFailures(*sim) == 1);
    sim->submit(SetWorkGroupCommand{pawn, std::string(25, 'x')});
    sim->step();
    CHECK(workGroupFailures(*sim) == 1);
    CHECK(sim->colonistOf(pawn)->workGroup.empty());
    sim->submit(SetWorkGroupCommand{pawn, std::string(24, 'x')});
    sim->step();
    CHECK(workGroupFailures(*sim) == 0);
    CHECK(sim->colonistOf(pawn)->workGroup == std::string(24, 'x'));
}

TEST_CASE("Work groups: a save keeps the group and the state hash") {
    auto sim = workGroupSim();
    const Entity pawn = workGroupColonist(*sim, CellPos{10, 10, 0});
    const uint64_t before = sim->stateHash();
    sim->submit(SetWorkGroupCommand{pawn, "Builders"});
    sim->step();
    CHECK(sim->stateHash() != before);
    const std::vector<uint8_t> bytes = saveSim(*sim);
    SaveLoadResult loaded = loadSim(bytes, workGroupDefs());
    WORKGROUP_REQUIRE(loaded.sim != nullptr);
    WORKGROUP_REQUIRE(loaded.sim->colonistOf(pawn) != nullptr);
    CHECK(loaded.sim->colonistOf(pawn)->workGroup == "Builders");
    CHECK(loaded.sim->stateHash() == sim->stateHash());
}

TEST_CASE("Work groups: a replay logs the command and plays it back the same") {
    auto sim = workGroupSim();
    const Entity pawn = workGroupColonist(*sim, CellPos{10, 10, 0});
    sim->stepMany(5);
    sim->submit(SetWorkGroupCommand{pawn, "Cooks"});
    sim->stepMany(5);
    const Replay replay = recordReplay(*sim);
    WORKGROUP_REQUIRE(!replay.commands.empty());
    const std::string text = writeReplay(replay);
    CHECK(text.find("SetWorkGroup") != std::string::npos);
    Replay read;
    std::string error;
    WORKGROUP_REQUIRE(readReplay(text, read, error));
    WORKGROUP_REQUIRE(!read.commands.empty());
    const auto* command = std::get_if<SetWorkGroupCommand>(&read.commands.back().command);
    WORKGROUP_REQUIRE(command != nullptr);
    CHECK(command->pawn == pawn);
    CHECK(command->group == "Cooks");
}
