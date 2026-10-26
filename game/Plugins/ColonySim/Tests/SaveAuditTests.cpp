// The save audit: a busy colony with every system running, saved and loaded at several moments;
// the loaded sim and the original run on side by side and must stay the same. State a system
// forgets to save shows up as the two drifting apart (docs/design/saves.md, "The audit").
#include "ColonySim/Animals.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Narrator.h"
#include "ColonySim/Random.h"
#include "ColonySim/Prisoners.h"
#include "ColonySim/Husbandry.h"
#include "ColonySim/Health.h"
#include "ColonySim/Raids.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

namespace {

std::shared_ptr<const DefDatabase> auditDefs() {
    auto db = std::make_shared<DefDatabase>();
    std::vector<std::string> errors;
    DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
    return db;
}

std::vector<Entity> auditColonists(const Sim& sim) {
    std::vector<Entity> out;
    for (const auto [pawn, colonist] : sim.things().registry().view<const Colonist>().each()) {
        (void)colonist;
        out.push_back(pawn);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<Entity> auditAnimals(const Sim& sim) {
    std::vector<Entity> out;
    for (const auto [e, animal] : sim.things().registry().view<const Animal>().each()) {
        (void)animal;
        out.push_back(e);
    }
    std::sort(out.begin(), out.end());
    return out;
}

CellPos auditAt(const Sim& sim, int dx, int dy) {
    const CellPos landing = sim.stockpileCell().value_or(CellPos{sim.map().width() / 2, sim.map().height() / 2, 0});
    return CellPos{landing.x + dx, landing.y + dy, 0};
}

// What the player and the world do to a busy colony, by game second. Called on the original and
// on the loaded copy with the same ranges, so both get the same commands. Commands that fail are
// fine (they fail in both).
void auditSession(Sim& sim, int64_t from, int64_t to) {
    const std::vector<Entity> people = auditColonists(sim);
    const std::vector<Entity> animals = auditAnimals(sim);
    for (int64_t i = from; i < to; ++i) {
        const int64_t t = i;
        if (t == 2) {
            sim.submit(SetNarratorCommand{"Orrin", ""});
            sim.submit(UnlockResearchCommand{"Electricity"});
            sim.submit(SetResearchCommand{"Smithing"});
            sim.submit(CreateStockpileCommand{CellRect{auditAt(sim, -9, 0).x, auditAt(sim, -9, 0).y, 5, 5, 0}});
            sim.submit(CreateGrowingZoneCommand{CellRect{auditAt(sim, 6, -8).x, auditAt(sim, 6, -8).y, 6, 4, 0}, "Plant_Rice"});
            for (int k = 0; k < 4; ++k) {
                sim.submit(PlaceBlueprintCommand{"Wall", "Steel", auditAt(sim, 3 + k, 3), Rot4::North});
                sim.submit(PlaceBlueprintCommand{"Wall", "Steel", auditAt(sim, 3 + k, 7), Rot4::North});
            }
            sim.submit(PlaceBlueprintCommand{"FueledStove", "", auditAt(sim, 5, 5), Rot4::North});
            sim.submit(PlaceBlueprintCommand{"Bed", "Steel", auditAt(sim, 3, 5), Rot4::North});
            sim.submit(PlaceBlueprintCommand{"CraftingSpot", "", auditAt(sim, 6, 5), Rot4::North});
            sim.submit(PlaceFloorCommand{CellRect{auditAt(sim, 3, 4).x, auditAt(sim, 3, 4).y, 4, 3, 0}, "WoodFloor"});
            sim.submit(SpawnThingCommand{"Steel", auditAt(sim, -3, 2), 120});
            sim.submit(SpawnThingCommand{"MealSurvivalPack", auditAt(sim, -8, 1), 40});
            sim.submit(SpawnThingCommand{"WoodLog", auditAt(sim, -3, 4), 100});
        }
        if (t == 30) {
            sim.submit(DesignateCommand{CellRect{auditAt(sim, -6, -6).x, auditAt(sim, -6, -6).y, 12, 12, 0}, DesignationKind::CutPlant});
            sim.submit(DesignateCommand{CellRect{auditAt(sim, 0, 10).x, auditAt(sim, 0, 10).y, 10, 6, 0}, DesignationKind::HarvestPlant});
            sim.submit(DesignateCommand{CellRect{auditAt(sim, 12, 0).x, auditAt(sim, 12, 0).y, 6, 6, 0}, DesignationKind::Mine});
            if (people.size() > 1) {
                sim.submit(SetWorkPriorityCommand{people[1], "Cooking", 1});
                sim.submit(SetOutfitCommand{people[1], 2});
                sim.submit(SetMedicalCareCommand{people[0], "Best"});
            }
        }
        if (t == 700) {
            for (const Entity a : animals) {
                sim.submit(MarkAnimalCommand{a, "Tame", true});
            }
            sim.submit(MarkAnimalsInAreaCommand{CellRect{0, 0, sim.map().width(), sim.map().height(), 0}, "Hunt"});
            sim.submit(SetWeatherCommand{"Rain", 3.0f});
        }
        if (t == 1500) {
            for (const Entity a : animals) {
                sim.submit(SetAnimalNameCommand{a, "Pip"});
                sim.submit(SetTrainingCommand{a, "Obedience", true});
            }
            sim.submit(CreateAnimalZoneCommand{CellRect{auditAt(sim, -9, 8).x, auditAt(sim, -9, 8).y, 5, 5, 0}});
            if (!people.empty()) {
                sim.submit(AddTalentXpCommand{people[0], 5000});
                sim.submit(SpendTalentPointCommand{people[0], "Marksman"});
                sim.submit(DraftCommand{people[0], true});
            }
            sim.submit(SoundAlarmCommand{true});
        }
        if (t == 2400) {
            sim.submit(SoundAlarmCommand{false});
            if (!people.empty()) {
                sim.submit(DraftCommand{people[0], false});
            }
            sim.submit(IgniteCommand{auditAt(sim, 8, -6), 0.0f, kNoEntity, false});
            if (people.size() > 2) {
                sim.submit(StartMentalBreakCommand{people[2], "WanderSad"});
            }
        }
        // Incidents, one every while, so each one's state is live somewhere in the run.
        static const char* const kIncidents[] = {"WandererJoin",   "ResourcePodCrash", "TraderCaravanArrival", "VisitorGroup",
                                                 "RaidEnemy",      "Disease_Flu",      "MadAnimal",            "ManhunterPack",
                                                 "ColdSnap",       "Eclipse",          "OrbitalTraderArrival", "RefugeePodCrash",
                                                 "ToxicFallout",   "HeatWave",         "Blight",               "PrisonBreak",
                                                 "AnimalSelfTame", "HerdMigration",    "ShortCircuit",         "Aurora"};
        if (t >= 3000 && t % 1500 == 0) {
            const size_t n = static_cast<size_t>((t - 3000) / 1500) % (sizeof kIncidents / sizeof kIncidents[0]);
            sim.submit(FireIncidentCommand{kIncidents[n], 0.0f});
        }
        sim.step();
    }
}

SimSettings auditSettings(uint64_t seed) {
    SimSettings settings{seed, 128, 0, 0};
    settings.scenario = "Crashlanded";
    settings.wildAnimals = 150;
    return settings;
}

// With AUDIT_DUMP=<folder> set, a difference leaves both sims' JSON dumps there (a.json, b.json) to diff.
void auditDump(const Sim& a, const Sim& b) {
    const char* dir = std::getenv("AUDIT_DUMP");
    if (dir == nullptr) {
        return;
    }
    const std::pair<const char*, const Sim*> both[] = {{"a.json", &a}, {"b.json", &b}};
    for (const auto& [name, sim] : both) {
        if (FILE* file = std::fopen((std::string(dir) + "/" + name).c_str(), "w")) {
            const std::string json = saveToJson(*sim);
            std::fwrite(json.data(), 1, json.size(), file);
            std::fclose(file);
        }
    }
}

// Saves `original`, loads the file, then runs both for `ahead` more seconds with the same
// commands and compares at every step block. Returns the first difference in words, or "".
std::string auditFork(Sim& original, const std::shared_ptr<const DefDatabase>& defs, int64_t ahead) {
    const std::vector<uint8_t> bytes = saveSim(original);
    SaveLoadResult loaded = loadSim(bytes, defs);
    if (loaded.sim == nullptr) {
        auditDump(original, original);
        return "load failed: " + loaded.error;
    }
    if (!loaded.warnings.empty()) {
        return "load warning: " + loaded.warnings.front();
    }
    Sim& copy = *loaded.sim;
    if (copy.stateHash() != original.stateHash()) {
        auditDump(original, copy);
        return "state hash differs right after loading";
    }
    if (saveSim(copy) != bytes) {
        return "saving the loaded sim gives another file";
    }
    const int64_t start = original.time().seconds;
    const char* blockText = std::getenv("AUDIT_BLOCK");  // a smaller block finds the second a drift starts
    const int64_t kBlock = blockText != nullptr ? std::max(1, std::atoi(blockText)) : 150;
    for (int64_t at = start; at < start + ahead; at += kBlock) {
        const int64_t to = std::min(start + ahead, at + kBlock);
        auditSession(original, at, to);
        auditSession(copy, at, to);
        if (copy.stateHash() != original.stateHash()) {
            auditDump(original, copy);
            return "state hash differs at second " + std::to_string(to) + ", " + std::to_string(to - start) + " after the load";
        }
        if (saveSim(copy) != saveSim(original)) {
            auditDump(original, copy);
            return "the saved files differ at second " + std::to_string(to) + ", " + std::to_string(to - start) + " after the load";
        }
    }
    return "";
}

// Seeds differ in map, people and animals, so the incidents and fights land differently.
void auditRun(uint64_t seed, const std::vector<int64_t>& gaps) {
    const auto defs = auditDefs();
    Sim original(auditSettings(seed), defs);
    CHECK_MESSAGE(original.setupErrors().empty(), "seed " << seed << " has no start: " << (original.setupErrors().empty() ? "" : original.setupErrors().front()));
    if (!original.setupErrors().empty() || auditColonists(original).empty()) {
        return;
    }
    for (const Entity pawn : auditColonists(original)) {
        original.combat().equip(pawn, original.things().make(original.defs().idOf("Things", "Gun_AssaultRifle")));
    }
    // Before the first step: a prison bed with a downed enemy to take in, and a few tame animals.
    {
        const Entity bed = original.things().spawn("Bed", auditAt(original, -4, 9));
        std::string error;
        original.submit(SetPrisonBedCommand{bed, true});
        // The enemy's spot can be rock or water on some seeds; then there is no prisoner.
        const Entity enemy = original.things().spawn("Human", auditAt(original, 14, 14));
        if (enemy != kNoEntity) {
            original.fighters().setFaction(enemy, 2, error);
            Rng rng(5);
            const std::vector<std::string> labels = original.health().partLabels(original.things().registry().get<Thing>(enemy).def);
            for (const char* part : {"torso", "left leg", "right leg", "left arm"}) {
                const auto it = std::find(labels.begin(), labels.end(), part);
                original.health().applyDamage(enemy, original.health().damageTypeId("Blunt"), 20.0f,
                                              static_cast<int>(it - labels.begin()), rng);
            }
        }
        const Entity handler = auditColonists(original).front();
        for (int k = 0; k < 4; ++k) {
            const Entity hen = original.things().spawn(k < 2 ? "Chicken" : "Muffalo", auditAt(original, -6 + k, 12));
            original.stepMany(3);
            if (hen != kNoEntity && original.husbandry().markTame(hen, true, error)) {
                for (int tries = 0; tries < 200 && !original.husbandry().isTame(hen); ++tries) {
                    original.husbandry().finishTame(handler, hen, 20);
                }
            }
        }
    }
    // Moments chosen to land in the middle of things: building, farming, hunting, a raid, a fire, a break.
    for (const int64_t gap : gaps) {
        auditSession(original, original.time().seconds, original.time().seconds + gap);
        const int64_t moment = original.time().seconds;
        const std::string problem = auditFork(original, defs, 900);
        CHECK_MESSAGE(problem.empty(), "seed " << seed << ", saved at second " << moment << ": " << problem);
    }
}

}  // namespace

TEST_CASE("Save audit: a busy colony saved and loaded at many moments plays on the same") {
    auditRun(42, {1, 700, 1300, 1500, 2000, 3000, 4000, 8000, 12000});
}

TEST_CASE("Save audit: other seeds, saved in the middle of incidents") {
    auditRun(7, {900, 2100, 1500, 2500, 3300, 3700, 6000, 9000});
    auditRun(2024, {600, 1900, 2800, 1700, 3100, 4100, 7000, 11000});
    // Seed 14 once saved two jobs holding one bed cell, a file the loader refused; seed 1 weathered one step differently after a load.
    auditRun(14, {47500, 900, 700});
    auditRun(1, {16300, 200});
}

// The soak: AUDIT_SOAK=<seeds> ./colonysim_tests -tc="Save audit: soak" saves a busy colony of each seed about twenty times.
TEST_CASE("Save audit: soak") {
    const char* seeds = std::getenv("AUDIT_SOAK");
    const int count = seeds != nullptr ? std::atoi(seeds) : 0;
    for (int seed = 1; seed <= count; ++seed) {
        if (const char* only = std::getenv("AUDIT_SEED"); only != nullptr && std::atoi(only) != seed) {
            continue;
        }
        auditRun(static_cast<uint64_t>(seed), std::vector<int64_t>(20, 1777 + 211 * (seed % 5)));
    }
}
