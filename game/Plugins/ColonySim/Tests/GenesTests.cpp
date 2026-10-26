#include "doctest.h"

#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Genes.h"
#include "ColonySim/Health.h"
#include "ColonySim/Looks.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Temperature.h"
#include "ColonySim/Things.h"

#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define GENES_REQUIRE(...)     \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> genesTestDefs() {
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

PawnGenes genesOfLineage(const std::string& lineage) {
    PawnGenes genes;
    std::string error;
    CHECK(makeGenes(*genesTestDefs(), lineage, genes, error));
    CHECK(error.empty());
    return genes;
}

SimSettings genesSettings(const char* scenario = "Crashlanded") {
    SimSettings settings;
    settings.seed = 11;
    settings.mapSize = 96;
    settings.flatMap = true;
    settings.scenario = scenario;
    return settings;
}

std::vector<Entity> genesColonists(const Sim& sim) {
    std::vector<Entity> out;
    for (const auto [pawn, colonist] : sim.things().registry().view<const Colonist>().each()) {
        out.push_back(pawn);
    }
    return out;
}

}  // namespace

TEST_CASE("genes: every lineage's genes exist, the baseline has none, and the effects add up") {
    const auto defs = genesTestDefs();
    for (const char* lineage : {"Baseline", "Emberkin", "Frostkin", "Warbred", "Ashborn", "Delver"}) {
        PawnGenes genes;
        std::string error;
        CHECK_MESSAGE(makeGenes(*defs, lineage, genes, error), lineage << ": " << error);
        CHECK(genes.genes.size() == defs->find("Lineages", lineage)->count("genes"));
    }
    const PawnGenes baseline = genesOfLineage("Baseline");
    CHECK(baseline.genes.empty());
    CHECK(baseline.effects.moveSpeed == 0.0f);
    CHECK(baseline.effects.foodRate == 1.0f);
    CHECK(baseline.effects.damageTaken == 1.0f);

    const PawnGenes ember = genesOfLineage("Emberkin");
    CHECK(ember.effects.moveSpeed > 0.0f);          // very fast runner
    CHECK(ember.effects.comfyMinC == 10.0f);        // cold weakness
    CHECK(ember.effects.comfyMaxC == 20.0f);        // heat super-tolerant
    CHECK(ember.effects.woundHealing == 0.5f);      // slow wound healing
    CHECK(ember.effects.moodOffset < 0.0f);         // unhappy
    CHECK(geneAptitude(ember, "Plants") == -4);
    CHECK(geneAptitude(ember, "Mining") == 0);

    const PawnGenes frost = genesOfLineage("Frostkin");
    // The wiki: a naked Frostkin is comfortable down to -6 C, a baseline human down to 16 C.
    CHECK(frost.effects.comfyMinC == -22.0f);
    CHECK(frost.effects.damageTaken == 0.75f);      // robust
    CHECK(frost.effects.foodRate == doctest::Approx(1.25f));
    CHECK(geneAptitude(frost, "Mining") == -8);

    const PawnGenes warbred = genesOfLineage("Warbred");
    CHECK(warbred.effects.foodRate == doctest::Approx(0.8f));
    CHECK(warbred.effects.woundHealing == 2.0f);
    CHECK(geneAptitude(warbred, "Shooting") == 8);

    const PawnGenes delver = genesOfLineage("Delver");
    CHECK(geneAptitude(delver, "Mining") == 8);
    CHECK(delver.effects.moveSpeed < 0.0f);
    CHECK(geneStat(delver, "meleeDamage", 1.0f) == 1.5f);
    CHECK(geneStat(delver, "noSuchStat", 3.0f) == 3.0f);

    PawnGenes none;
    std::string error;
    CHECK_FALSE(makeGenes(*defs, "NoSuchLineage", none, error));
    CHECK(error.find("NoSuchLineage") != std::string::npos);
}

TEST_CASE("genes: the hunger rate follows the metabolism curve between its points") {
    const auto defs = genesTestDefs();
    DefDatabase db;
    std::vector<std::string> errors;
    const std::vector<DefSource> sources = {
        {"Rules", "t.json", R"([{"defName": "GeneRules", "hungerRateByMetabolicEfficiency": [[-1, 1.25], [0, 1.0], [2, 0.8]]}])"},
        {"Genes", "t.json", R"([{"defName": "G"}])"},
        {"Lineages", "t.json", R"([{"defName": "L1", "genes": ["G"], "metabolicEfficiency": 1}, {"defName": "L9", "genes": ["G"], "metabolicEfficiency": 9}, {"defName": "L-5", "genes": ["G"], "metabolicEfficiency": -5}])"}};
    CHECK(DefDatabase::build(sources, "", db, errors));
    PawnGenes genes;
    std::string error;
    CHECK(makeGenes(db, "L1", genes, error));
    CHECK(genes.effects.foodRate == doctest::Approx(0.9f));
    CHECK(makeGenes(db, "L9", genes, error));
    CHECK(genes.effects.foodRate == doctest::Approx(0.8f));
    CHECK(makeGenes(db, "L-5", genes, error));
    CHECK(genes.effects.foodRate == doctest::Approx(1.25f));
}

TEST_CASE("genes: looks read the colours and horns from the lineage's genes") {
    const auto defs = genesTestDefs();
    PawnLooks baseline, ember, ash;
    std::string error;
    CHECK(generateLooks(*defs, 42, true, "Baseline", baseline, error));
    CHECK(generateLooks(*defs, 42, true, "Emberkin", ember, error));
    CHECK(generateLooks(*defs, 42, true, "Ashborn", ash, error));
    CHECK(ember.skin.r - ember.skin.g > baseline.skin.r - baseline.skin.g + 0.15f);
    CHECK(ember.hair.r < 0.15f);
    CHECK(std::fabs(ash.hair.r - 0.45f) < 0.01f);  // gray hair
    CHECK(std::fabs(ash.skin.r - ash.skin.b) < 0.12f);

    const Def* emberDef = defs->find("Lineages", "Emberkin");
    const Def* horns = lineageGeneWith(*defs, *emberDef, "visual.horns.length");
    GENES_REQUIRE(horns != nullptr);
    CHECK(horns->defName() == "MiniHorns");
    CHECK(lineageGeneWith(*defs, *defs->find("Lineages", "Baseline"), "visual.horns.length") == nullptr);
}

TEST_CASE("genes: pawns get their lineage's genes, a changed lineage changes them, and a save rebuilds them") {
    const auto defs = genesTestDefs();
    Sim sim(genesSettings(), defs);
    CHECK(sim.setupErrors().empty());
    const std::vector<Entity> colonists = genesColonists(sim);
    GENES_REQUIRE(colonists.size() >= 2);
    const Entity pawn = colonists[1];
    const PawnGenes* genes = sim.genesOf(pawn);
    GENES_REQUIRE(genes != nullptr);
    CHECK(genes->genes.empty());

    sim.submit(SetLineageCommand{pawn, "Frostkin"});
    sim.step();
    genes = sim.genesOf(pawn);
    GENES_REQUIRE(genes != nullptr);
    CHECK(genes->effects.comfyMinC == -22.0f);

    float min = 0.0f, max = 0.0f;
    float otherMin = 0.0f, otherMax = 0.0f;
    GENES_REQUIRE(sim.temperature().comfortRange(pawn, min, max));
    GENES_REQUIRE(sim.temperature().comfortRange(colonists[2], otherMin, otherMax));
    CHECK(min == doctest::Approx(otherMin - 22.0f));
    CHECK(max == doctest::Approx(otherMax));

    // Things that aren't humanlike have no genes.
    sim.submit(SpawnThingCommand{"Steel", CellPos{10, 10, 0}, 5});
    sim.step();

    const std::vector<uint8_t> bytes = saveSim(sim);
    SaveLoadResult loaded = loadSim(bytes, defs);
    GENES_REQUIRE(loaded.sim != nullptr);
    const PawnGenes* again = loaded.sim->genesOf(pawn);
    GENES_REQUIRE(again != nullptr);
    CHECK(again->genes == genes->genes);
    CHECK(again->effects.comfyMinC == -22.0f);
    CHECK(again->effects.foodRate == doctest::Approx(1.25f));
    CHECK(loaded.sim->stateHash() == sim.stateHash());
}

TEST_CASE("genes: hunger, damage, walking speed and skills follow the genes") {
    const auto defs = genesTestDefs();
    Sim sim(genesSettings(), defs);
    const std::vector<Entity> colonists = genesColonists(sim);
    GENES_REQUIRE(colonists.size() >= 4);
    const Entity plain = colonists[1];
    const Entity frost = colonists[2];
    const Entity ember = colonists[3];
    sim.submit(SetLineageCommand{frost, "Frostkin"});
    sim.submit(SetLineageCommand{ember, "Emberkin"});
    // Warm weather for both (the Emberkin's range starts at 22 degrees), so cold doesn't slow either.
    sim.setOutdoorOverride(25.0f);
    sim.step();

    // Hunger: both start full and fall at their own rate.
    const float plainStart = sim.things().registry().get<Needs>(plain).food;
    const float frostStart = sim.things().registry().get<Needs>(frost).food;
    sim.stepMany(7200);
    const float plainDrop = plainStart - sim.things().registry().get<Needs>(plain).food;
    const float frostDrop = frostStart - sim.things().registry().get<Needs>(frost).food;
    GENES_REQUIRE(plainDrop > 0.0f);
    CHECK(frostDrop / plainDrop == doctest::Approx(1.25f).epsilon(0.02));

    // Damage: robust takes three quarters of a hit.
    const auto severity = [&](Entity pawn) {
        HealthReport report;
        sim.healthReport(pawn, report);
        float total = 0.0f;
        for (const auto& injury : report.injuries) {
            total += injury.severity;
        }
        return total;
    };
    const float plainBefore = severity(plain);
    const float frostBefore = severity(frost);
    sim.submit(DamagePawnCommand{plain, "Cut", 8.0f, 0});
    sim.submit(DamagePawnCommand{frost, "Cut", 8.0f, 0});
    sim.step();
    const float plainHurt = severity(plain) - plainBefore;
    const float frostHurt = severity(frost) - frostBefore;
    GENES_REQUIRE(plainHurt > 0.0f);
    CHECK(frostHurt / plainHurt == doctest::Approx(0.75f).epsilon(0.02));

    // Walking: drafted and sent along a free row; once up to speed the very fast runner covers
    // more ground in the same time.
    const auto posOf = [&](Entity pawn) { return sim.things().registry().get<Position>(pawn); };
    for (const Entity pawn : {plain, ember}) {
        sim.submit(DraftCommand{pawn, true});
    }
    sim.step();
    for (const Entity pawn : {plain, ember}) {
        const Position at = posOf(pawn);
        sim.submit(MoveOrderCommand{pawn, CellPos{static_cast<int>(at.x) > 40 ? 5 : 90, static_cast<int>(at.y), 0}});
    }
    sim.stepMany(30);  // up to speed
    const Position plainFrom = posOf(plain);
    const Position emberFrom = posOf(ember);
    sim.stepMany(40);
    const float plainMoved = std::fabs(posOf(plain).x - plainFrom.x);
    const float emberMoved = std::fabs(posOf(ember).x - emberFrom.x);
    CHECK(plainMoved > 1.0f);
    CHECK(emberMoved > plainMoved * 1.1f);
}

TEST_CASE("genes: a scenario with lineage weights rolls the colonists' lineages, never the protagonist's") {
    const auto defs = genesTestDefs();
    std::map<std::string, int> seen;
    std::vector<std::string> first;
    for (int seed = 1; seed <= 12; ++seed) {
        SimSettings settings = genesSettings("CrashlandedMixed");
        settings.seed = static_cast<uint64_t>(seed);
        Sim sim(settings, defs);
        CHECK(sim.setupErrors().empty());
        for (const Entity pawn : genesColonists(sim)) {
            const PawnLooks* looks = sim.looksOf(pawn);
            GENES_REQUIRE(looks != nullptr);
            if (sim.colonistOf(pawn)->protagonist) {
                CHECK(looks->lineage == "Baseline");
            }
            ++seen[looks->lineage];
            const PawnGenes* genes = sim.genesOf(pawn);
            GENES_REQUIRE(genes != nullptr);
            CHECK(genes->genes == genesOfLineage(looks->lineage).genes);
        }
        if (seed == 3) {
            Sim again(settings, defs);
            CHECK(again.stateHash() == sim.stateHash());
        }
    }
    CHECK(seen["Baseline"] > 20);
    int others = 0;
    for (const auto& entry : seen) {
        others += entry.first == "Baseline" ? 0 : entry.second;
    }
    CHECK(others >= 3);
    // The plain scenario still gives everyone the baseline.
    Sim plain(genesSettings(), defs);
    for (const Entity pawn : genesColonists(plain)) {
        CHECK(plain.looksOf(pawn)->lineage == "Baseline");
    }
}
