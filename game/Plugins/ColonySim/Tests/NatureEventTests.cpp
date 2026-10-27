#include "ColonySim/Animals.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Health.h"
#include "ColonySim/Husbandry.h"
#include "ColonySim/Lights.h"
#include "ColonySim/Map.h"
#include "ColonySim/NatureEvents.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Raids.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"
#include "ColonySim/Weather.h"

#include <doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace colony;

#define NATURE_REQUIRE(...)          \
    do {                             \
        const bool natureOk = (__VA_ARGS__); \
        CHECK(natureOk);             \
        if (!natureOk) {             \
            return;                  \
        }                            \
    } while (0)

namespace {

// The game's real data. These tests check docs/design/nature-events.md.
std::shared_ptr<const DefDatabase> natureTestDefs() {
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

// The Crashlanded colonists on a flat 128 m map with the Temperate forest's weather.
std::unique_ptr<Sim> natureTestSim(uint64_t seed = 21, int wildAnimals = 0) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = 128;
    settings.flatMap = true;
    settings.scenario = "Crashlanded";
    settings.wildAnimals = wildAnimals;
    return std::make_unique<Sim>(settings, natureTestDefs());
}

// Makes an incident happen now; the letter it sent (the last Letter event) is returned through `letter`.
bool natureFire(Sim& sim, const std::string& incident, std::string* letter = nullptr) {
    std::string error;
    const size_t before = sim.events().size();
    const bool ok = sim.narrator().fire(incident, 0.0f, error);
    if (!ok) {
        MESSAGE(incident << ": " << error);
    }
    if (letter != nullptr) {
        letter->clear();
        for (size_t i = before; i < sim.events().size(); ++i) {
            if (sim.events()[i].type == EventType::Letter) {
                *letter = sim.events()[i].message;
            }
        }
    }
    return ok;
}

size_t natureThings(const Sim& sim) {
    return sim.things().registry().view<const Thing>().size();
}

size_t naturePlantsOf(const Sim& sim, const char* def) {
    return sim.plants().countOf(sim.defs().idOf("Things", def));
}

void natureRun(Sim& sim, int64_t seconds) {
    for (int64_t i = 0; i < seconds; ++i) {
        sim.step();
    }
}

}  // namespace

TEST_CASE("Nature events: a wanderer walks in from the edge and joins") {
    auto sim = natureTestSim();
    const int before = sim->raids().colonistCount();
    std::string letter;
    NATURE_REQUIRE(natureFire(*sim, "WandererJoin", &letter));
    CHECK(sim->raids().colonistCount() == before + 1);
    CHECK(letter.find("wanderer") != std::string::npos);
    // Dressed like the rest, and given work by the job system (they walk about on their own).
    natureRun(*sim, 600);
    CHECK(sim->raids().colonistCount() == before + 1);
}

TEST_CASE("Nature events: a crashed pod holds a hurt survivor who is downed") {
    auto sim = natureTestSim();
    const int before = sim->raids().colonistCount();
    NATURE_REQUIRE(natureFire(*sim, "RefugeePodCrash"));
    CHECK(sim->raids().colonistCount() == before + 1);
    int downed = 0;
    for (const auto [e, colonist] : sim->things().registry().view<const Colonist>().each()) {
        const Health& h = sim->things().registry().get<Health>(e);
        downed += h.downed && !h.dead ? 1 : 0;
    }
    CHECK(downed == 1);
}

TEST_CASE("Nature events: a cargo pod drops a stack of something useful") {
    auto sim = natureTestSim();
    const size_t before = natureThings(*sim);
    std::string letter;
    NATURE_REQUIRE(natureFire(*sim, "ResourcePodCrash", &letter));
    CHECK(natureThings(*sim) > before);
    CHECK(letter.find("cargo pod") != std::string::npos);
}

TEST_CASE("Nature events: a herd comes in from the edge, and an animal joins the colony") {
    auto sim = natureTestSim(21, 100);
    const size_t before = sim->animals().livingCount();
    NATURE_REQUIRE(natureFire(*sim, "HerdMigration"));
    CHECK(sim->animals().livingCount() >= before + 8);
    // Wild animals are about: the nearest calm one joins.
    const uint64_t tamedBefore = sim->husbandry().stats().tamed;
    std::string letter;
    NATURE_REQUIRE(natureFire(*sim, "AnimalSelfTame", &letter));
    CHECK(sim->husbandry().stats().tamed == tamedBefore + 1);
    CHECK(letter.find("joined") != std::string::npos);
}

TEST_CASE("Nature events: berries and healroot sprout") {
    auto sim = natureTestSim();
    const size_t berries = naturePlantsOf(*sim, "Plant_Berry");
    NATURE_REQUIRE(natureFire(*sim, "BerryBloom"));
    CHECK(naturePlantsOf(*sim, "Plant_Berry") >= berries + 4);
    const size_t roots = naturePlantsOf(*sim, "Plant_Healroot");
    NATURE_REQUIRE(natureFire(*sim, "HealrootBloom"));
    CHECK(naturePlantsOf(*sim, "Plant_Healroot") >= roots + 4);
}

TEST_CASE("Nature events: a cold snap and a heat wave move the outdoor temperature for their time only") {
    auto sim = natureTestSim();
    natureRun(*sim, 60);
    const double base = sim->weather().temperatureAt(sim->time().seconds + 4 * 86400);
    NATURE_REQUIRE(natureFire(*sim, "ColdSnap"));
    const auto active = sim->nature().active();
    NATURE_REQUIRE(active.size() == 1);
    CHECK(active[0].incident == "ColdSnap");
    const double days = static_cast<double>(active[0].endsAt - active[0].startsAt) / 86400.0;
    CHECK(days >= 2.0);
    CHECK(days <= 5.0);
    CHECK(sim->weather().temperatureAt(active[0].startsAt + 3600) - sim->weather().temperatureAt(active[0].endsAt + 3600) < -15.0);
    Snapshot snap;
    sim->writeSnapshot(snap);
    CHECK(snap.narrator.conditions.size() == 1);
    // Not twice at once.
    CHECK_FALSE(natureFire(*sim, "ColdSnap"));
    // Over, it is gone from the snapshot and the offset with it.
    natureRun(*sim, active[0].endsAt - sim->time().seconds + 10);
    CHECK(sim->nature().active().empty());
    sim->writeSnapshot(snap);
    CHECK(snap.narrator.conditions.empty());
    CHECK(std::abs(sim->nature().temperatureOffset(sim->time().seconds)) < 1e-9);
    (void)base;

    NATURE_REQUIRE(natureFire(*sim, "HeatWave"));
    CHECK(sim->nature().temperatureOffset(sim->time().seconds) > 10.0);
}

TEST_CASE("Nature events: an eclipse dims the sun and an aurora lights the night") {
    auto sim = natureTestSim();
    // Wait for noon.
    natureRun(*sim, 12 * 3600 - sim->time().seconds % 86400);
    const float noon = sim->lights().skyGlow();
    CHECK(noon > 0.5f);
    NATURE_REQUIRE(natureFire(*sim, "Eclipse"));
    CHECK(sim->lights().skyGlow() < noon * 0.2f);
    const auto eclipse = sim->nature().active();
    NATURE_REQUIRE(eclipse.size() == 1);
    natureRun(*sim, eclipse[0].endsAt - sim->time().seconds + 5);
    CHECK(sim->nature().skyFactor() == 1.0f);

    // At midnight an open cell is dark; under the aurora it is lit, a roofed one stays dark.
    natureRun(*sim, 86400 - sim->time().seconds % 86400 + 1);
    const CellPos open{sim->map().width() / 2, sim->map().height() / 2, 0};
    CHECK(sim->lights().lightAt(open) < 0.2f);
    NATURE_REQUIRE(natureFire(*sim, "Aurora"));
    CHECK(sim->lights().lightAt(open) >= 0.4f - 1e-4f);
    CHECK(sim->lights().skyGlow() < 0.2f);  // the sun's own glow (and solar power) is not touched
}

TEST_CASE("Nature events: the spells and the rest are saved, loaded and replay the same") {
    auto sim = natureTestSim(33, 100);
    sim->submit(SetNarratorCommand{"Jinx", ""});
    NATURE_REQUIRE(natureFire(*sim, "ColdSnap"));
    NATURE_REQUIRE(natureFire(*sim, "WandererJoin"));
    natureRun(*sim, 3000);
    const std::vector<uint8_t> bytes = saveSim(*sim);
    SaveLoadResult loaded = loadSim(bytes, natureTestDefs());
    NATURE_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.error.empty());
    CHECK(loaded.sim->nature().active().size() == sim->nature().active().size());
    CHECK(loaded.sim->stateHash() == sim->stateHash());
    CHECK(saveSim(*loaded.sim) == bytes);
    for (int i = 0; i < 2000; ++i) {
        sim->step();
        loaded.sim->step();
    }
    CHECK(loaded.sim->stateHash() == sim->stateHash());
    CHECK(loaded.sim->weather().outdoorTemperature() == sim->weather().outdoorTemperature());
}

TEST_CASE("Nature events: Jinx sends the good news by himself over a year") {
    auto sim = natureTestSim(5, 100);
    sim->submit(SetNarratorCommand{"Jinx", "Peaceful"});
    const std::vector<std::string> good = {"WandererJoin", "ResourcePodCrash", "RefugeePodCrash", "HerdMigration", "AnimalSelfTame",
                                           "ColdSnap", "HeatWave", "Eclipse", "Aurora", "BerryBloom", "HealrootBloom"};
    for (int64_t i = 0; i < 40 * 86400; ++i) {
        sim->step();
    }
    const NarratorState state = sim->narrator().state();
    CHECK(state.incidents >= 10);
    size_t kinds = 0;
    for (const std::string& name : good) {
        for (const IncidentFired& f : state.fired) {
            kinds += f.incident == name ? 1 : 0;
        }
    }
    CHECK(kinds >= 4);
}
