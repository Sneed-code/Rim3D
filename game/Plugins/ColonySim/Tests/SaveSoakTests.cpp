// A save taken in the middle of a real colony, with wild animals, fires and raids going on, must
// play on exactly like the game that was never saved. SaveTests covers the hauling fixture; this
// covers the whole game on its real data, which is where a system that keeps state outside the
// save, a value worked out at the end of a step and not saved, or a reference held across the
// destruction of another thing shows up. Each case found a bug that SaveTests could not see.
#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Designations.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

namespace {

std::shared_ptr<const DefDatabase> saveSoakDefs() {
    static std::shared_ptr<const DefDatabase> defs;
    if (!defs) {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        const bool ok = DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        for (const std::string& e : errors) MESSAGE(e);
        CHECK(ok);
        defs = db;
    }
    return defs;
}

// The nth colonist by id (the registry's own order changes with a load).
Entity saveSoakColonist(Sim& sim, size_t nth) {
    std::vector<Entity> all;
    for (const auto [pawn, colonist] : sim.things().registry().view<const Colonist>().each()) {
        all.push_back(pawn);
    }
    std::sort(all.begin(), all.end());
    return nth < all.size() ? all[nth] : kNoEntity;
}

// Saves `original` now, loads the copy and plays both `steps` more steps, comparing the state
// hash after every step. `script` runs on both before each step.
template <typename Script>
void saveSoakFork(Sim& original, int64_t steps, const char* what, Script script) {
    SaveLoadResult loaded = loadSim(saveSim(original), saveSoakDefs());
    CHECK_MESSAGE(loaded.error.empty(), what, ": ", loaded.error);
    if (loaded.sim == nullptr) {
        return;
    }
    CHECK_MESSAGE(loaded.warnings.empty(), what);
    Sim& copy = *loaded.sim;
    CHECK_MESSAGE(copy.stateHash() == original.stateHash(), what, ": the hash differs straight after loading");
    for (int64_t i = 0; i < steps; ++i) {
        script(original);
        script(copy);
        original.step();
        copy.step();
        if (copy.stateHash() != original.stateHash()) {
            FAIL_CHECK(std::string(what) << ": the copy's state hash differs " << (i + 1) << " steps after the save");
            return;
        }
    }
}

void saveSoakNothing(Sim&) {}

std::unique_ptr<Sim> saveSoakColony(uint64_t seed, int wildAnimals) {
    SimSettings settings{seed, 256, 0, 0};
    settings.scenario = "Crashlanded";
    settings.wildAnimals = wildAnimals;
    auto sim = std::make_unique<Sim>(settings, saveSoakDefs());
    CHECK(sim->setupErrors().empty());
    return sim;
}

}  // namespace

TEST_CASE("Save: a crashlanded colony with animals and a raid plays on the same after a save at awkward moments") {
    const auto sim = saveSoakColony(42, 100);
    sim->submit(StartRaidCommand{"Pirate", 400.0f, -1});
    // Half a game day is a whole number of minutes, hours and spoilage rounds: everything that
    // runs on a timer fires on the first step after the load.
    while (sim->time().seconds < 43200) {
        sim->step();
    }
    saveSoakFork(*sim, 3000, "at half a day", saveSoakNothing);
    // And at a moment on no timer's beat, with the raid under way.
    while (sim->time().seconds < 90007) {
        sim->step();
    }
    saveSoakFork(*sim, 3000, "at 90007 s", saveSoakNothing);
}

TEST_CASE("Save: a predator that finishes eating a corpse is not lost by a save") {
    // At 107544 s on this seed a wolf finishes the alpaca it killed: the corpse is destroyed and the
    // wolf goes idle. The wolf's state used to be written through a reference the destruction had
    // already invalidated, so the game that kept running and the one loaded from a save disagreed.
    const auto sim = saveSoakColony(42, 100);
    while (sim->time().seconds < 100001) {
        sim->step();
    }
    saveSoakFork(*sim, 8000, "at 100001 s", saveSoakNothing);
}

TEST_CASE("Save: colonists, hunts, fires and breaks play on the same after a save") {
    // What a player might do, at fixed times. A deer burnt in the grass fire is downed when the
    // save is made; loading used to stop it walking, which the game that went on never did.
    const auto script = [](Sim& s) {
        const int64_t t = s.time().seconds;
        const auto cellOfColonist = [&](size_t nth) {
            const Entity pawn = saveSoakColonist(s, nth);
            return pawn == kNoEntity ? CellPos{128, 128, 0} : cellOf(s.things().registry().get<Position>(pawn));
        };
        if (t == 0) {
            const CellPos c = cellOfColonist(0);
            s.submit(SetHomeAreaCommand{CellRect{c.x - 25, c.y - 25, 50, 50, 0}, true});
            s.submit(CreateGrowingZoneCommand{CellRect{c.x + 6, c.y + 6, 8, 8, 0}, "Plant_Rice"});
            s.submit(DesignateCommand{CellRect{c.x - 30, c.y - 30, 60, 60, 0}, DesignationKind::HarvestPlant});
            s.submit(DesignateCommand{CellRect{c.x - 20, c.y - 20, 40, 40, 0}, DesignationKind::CutPlant});
        }
        if (t == 3000) {
            s.submit(MarkAnimalsInAreaCommand{CellRect{0, 0, 256, 256, 0}, "Hunt", true});
        }
        if (t == 12000) {
            const CellPos c = cellOfColonist(1);
            s.submit(IgniteCommand{CellPos{c.x + 7, c.y, 0}, 3.0f, kNoEntity, true});
        }
        if (t == 20000) {
            s.submit(StartRaidCommand{"Pirate", 500.0f, -1});
        }
        if (t == 26000) {
            s.submit(StartMentalBreakCommand{saveSoakColonist(s, 2), "InsultingSpree"});
        }
        if (t == 40000) {
            s.submit(MarkAnimalsInAreaCommand{CellRect{0, 0, 256, 256, 0}, "Tame", true});
        }
        if (t == 60000) {
            s.submit(StartMentalBreakCommand{saveSoakColonist(s, 3), "WanderSad"});
            s.submit(SetWeatherCommand{"Rain"});
        }
        if (t == 80000) {
            const CellPos c = cellOfColonist(4);
            s.submit(StartRaidCommand{"Pirate", 900.0f, -1});
            s.submit(IgniteCommand{CellPos{c.x + 2, c.y + 2, 0}, 2.0f, kNoEntity, true});
        }
    };
    const auto sim = saveSoakColony(99, 250);
    while (sim->time().seconds < 100312) {
        script(*sim);
        sim->step();
    }
    saveSoakFork(*sim, 4000, "at 100312 s", script);
}
