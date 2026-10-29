#include "ColonySim/Animals.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Health.h"
#include "ColonySim/Husbandry.h"
#include "ColonySim/Raids.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <memory>
#include <string>

using namespace colony;

namespace {

// The game's real defs.
std::shared_ptr<const DefDatabase> closeShotDefs() {
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
}

std::unique_ptr<Sim> closeShotSim(const std::shared_ptr<const DefDatabase>& defs) {
    SimSettings settings;
    settings.seed = 7;
    settings.mapSize = 64;
    settings.flatMap = true;
    settings.checkpointEvery = 0;
    auto sim = std::make_unique<Sim>(settings, defs);
    for (int y = 0; y < sim->map().height(); ++y) {
        for (int x = 0; x < sim->map().width(); ++x) {
            sim->plants().remove(CellPos{x, y, 0});
        }
    }
    sim->stepMany(12 * kSecondsPerHour - sim->time().seconds);
    return sim;
}

Entity closeShotSpawn(Sim& sim, const char* def, float x, float y, const char* gun = nullptr) {
    const Entity e = sim.things().spawn(def, CellPos{static_cast<int>(x), static_cast<int>(y), 0});
    if (e != kNoEntity) {
        Position& p = sim.things().registry().get<Position>(e);
        p.x = x;
        p.y = y;
        if (gun != nullptr) {
            sim.combat().equip(e, sim.things().make(sim.defs().idOf("Things", gun)));
        }
        (void)sim.things().registry().get_or_emplace<Colonist>(e);
    }
    return e;
}

}  // namespace

namespace {

// A mad squirrel starts `gap` metres from a colonist with a revolver; how the fight goes.
void closeShotSquirrel(float gap, bool second) {
    const auto defs = closeShotDefs();
    auto sim = closeShotSim(defs);
    const Entity squirrel = sim->things().spawn("Squirrel", CellPos{30, 30, 0});
    const Entity colonist = closeShotSpawn(*sim, "Human", 30.5f + gap, 30.5f, "Gun_Revolver");
    const Entity friendPawn = second ? closeShotSpawn(*sim, "Human", 30.5f + gap, 37.5f, "Gun_Revolver") : kNoEntity;
    CHECK(squirrel != kNoEntity);
    CHECK(colonist != kNoEntity);
    if (squirrel == kNoEntity || colonist == kNoEntity) {
        return;
    }
    Position& sp = sim->things().registry().get<Position>(squirrel);
    sp.x = 30.5f;
    sp.y = 30.5f;
    const bool enraged = sim->animals().enrage(squirrel, AnimalTemper::Manhunter, colonist);
    CHECK(enraged);
    int shots = 0;
    int hitsOnSquirrel = 0;
    int wounds = 0;
    int friendlyHits = 0;
    int i = 0;
    const auto out = [&]() { return sim->things().registry().get<Health>(squirrel).dead || sim->things().registry().get<Health>(squirrel).downed; };
    for (; i < 1800 && !out(); ++i) {
        sim->step();
        for (const Event& e : sim->events()) {
            shots += e.type == EventType::ShotFired ? 1 : 0;
            hitsOnSquirrel += e.type == EventType::PawnInjured && e.thing == squirrel ? 1 : 0;
            wounds += e.type == EventType::PawnInjured && (e.thing == colonist || e.thing == friendPawn) ? 1 : 0;
            friendlyHits += e.type == EventType::PawnInjured && (e.thing == colonist || e.thing == friendPawn) && e.message == "Gunshot" ? 1 : 0;
        }
    }
    MESSAGE("gap " << gap << ": " << i << " s, shots " << shots << ", hits on the squirrel " << hitsOnSquirrel << ", wounds on colonists "
                   << wounds << " (gunshots " << friendlyHits << ")");
    CHECK(out());
    CHECK(friendlyHits == 0);
}

}  // namespace

TEST_CASE("Close shots: a colonist with a revolver kills a mad squirrel at its feet") {
    closeShotSquirrel(0.9f, false);
    closeShotSquirrel(0.9f, true);
}

TEST_CASE("Close shots: the colony's own animal gone mad is an enemy: the alarm sounds and it is shot") {
    // Seed 42 of the year run: a squirrel that had joined the colony went mad and bit a colonist in bed to death, with the
    // alarm off and an armed colonist beside it.
    const auto defs = closeShotDefs();
    auto sim = closeShotSim(defs);
    const Entity squirrel = sim->things().spawn("Squirrel", CellPos{30, 30, 0});
    const Entity colonist = closeShotSpawn(*sim, "Human", 34.5f, 30.5f, "Gun_Revolver");
    CHECK(squirrel != kNoEntity);
    CHECK(colonist != kNoEntity);
    if (squirrel == kNoEntity || colonist == kNoEntity) {
        return;
    }
    CHECK(sim->husbandry().joinColony(squirrel));
    sim->stepMany(60);
    CHECK_FALSE(sim->raids().state().alarm);
    const bool enraged = sim->animals().enrage(squirrel, AnimalTemper::Manhunter, colonist);
    CHECK(enraged);
    bool alarm = false;
    const Health& h = sim->things().registry().get<Health>(squirrel);
    for (int i = 0; i < 1800 && !h.dead && !h.downed; ++i) {
        sim->step();
        alarm = alarm || sim->raids().state().alarm;
    }
    CHECK(alarm);
    CHECK((h.dead || h.downed));
}
