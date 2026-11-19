// sneed's races (2026-11-19, docs/design/races.md): baseline, Impid (Emberkin), Orcoid, drow and cat-strain,
// as lineages whose genes the sim reads: learning, work, light, aim, damage, hit points, blood, heat,
// falls, penalties, joiners' races and the drow's night schedule.

#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Genes.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Lights.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

#define RACES_REQUIRE(...)                                   \
    do {                                                     \
        const bool racesOk = static_cast<bool>(__VA_ARGS__); \
        CHECK_MESSAGE(racesOk, #__VA_ARGS__);                \
        if (!racesOk) {                                      \
            return;                                          \
        }                                                    \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> racesDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

SimSettings racesSettings() {
    SimSettings settings;
    settings.seed = 23;
    settings.mapSize = 96;
    settings.flatMap = true;
    settings.scenario = "Crashlanded";
    return settings;
}

// The colonists, the protagonist first.
std::vector<Entity> racesColonists(const Sim& sim) {
    std::vector<Entity> out;
    for (const auto [pawn, colonist] : sim.things().registry().view<const Colonist>().each()) {
        out.push_back(pawn);
    }
    return out;
}

float racesPain(Sim& sim, Entity pawn) {
    HealthReport report;
    sim.healthReport(pawn, report);
    return report.pain;
}

}  // namespace

TEST_CASE("races: the lineages carry sneed's numbers") {
    const auto defs = racesDefs();
    PawnGenes g;
    std::string error;
    CHECK(makeGenes(*defs, "Orcoid", g, error));
    CHECK(g.effects.meleeDamage == doctest::Approx(1.1f));
    CHECK(g.effects.healthScale == doctest::Approx(1.1f));
    CHECK(g.effects.learning == doctest::Approx(0.9f));
    CHECK(g.effects.workSpeed == doctest::Approx(0.9f));
    CHECK(makeGenes(*defs, "Drow", g, error));
    CHECK(g.effects.rangedDamage == doctest::Approx(1.1f));
    CHECK(g.effects.darkVision);
    CHECK(g.effects.darkWorkSpeed == doctest::Approx(1.1f));
    CHECK(g.effects.sunWorkSpeed == doctest::Approx(0.9f));
    CHECK(g.effects.sunShootingAccuracy == doctest::Approx(0.9f));
    CHECK(g.effects.noOutdoorsNeed);
    CHECK(g.effects.nocturnal);
    CHECK(makeGenes(*defs, "CatStrain", g, error));
    CHECK(g.effects.jumpHeight == 2.0f);
    CHECK(g.effects.climbSpeed == 2.0f);
    CHECK(g.effects.environmentDamage == doctest::Approx(0.8f));
    CHECK(g.effects.debuffFactor == doctest::Approx(1.33f));
    // A penalty a third worse: 25% off becomes 33.25% off; bonuses are left alone.
    CHECK(geneWorsen(0.75f, 1.33f) == doctest::Approx(0.6675f));
    CHECK(geneWorsen(1.2f, 1.33f) == 1.2f);
    CHECK(geneWorsen(0.1f, 1.33f) == doctest::Approx(0.0f).epsilon(0.01));
    // Drow work: +10% in the dark, -10% in sunlight, both with the Orcoid-free base of 1.
    CHECK(geneWorkFactor(g.effects, 0.1f, false) == 1.0f);  // the cat has neither
    CHECK(makeGenes(*defs, "Drow", g, error));
    CHECK(geneWorkFactor(g.effects, 0.1f, false) == doctest::Approx(1.1f));
    CHECK(geneWorkFactor(g.effects, 1.0f, true) == doctest::Approx(0.9f));
    CHECK(geneWorkFactor(g.effects, 1.0f, false) == 1.0f);  // lit indoors: neither
}

TEST_CASE("races: Orcoid have a tenth more hit points and blood, cat-strain feel pain a third worse") {
    Sim sim(racesSettings(), racesDefs());
    const std::vector<Entity> colonists = racesColonists(sim);
    RACES_REQUIRE(colonists.size() >= 4);
    const Entity plain = colonists[1];
    const Entity orc = colonists[2];
    const Entity cat = colonists[3];
    sim.submit(SetLineageCommand{orc, "Orcoid"});
    sim.submit(SetLineageCommand{cat, "CatStrain"});
    sim.step();
    entt::registry& reg = sim.things().registry();
    CHECK(reg.get<Health>(orc).lineageHp == doctest::Approx(1.1f));
    CHECK(reg.get<Health>(plain).lineageHp == 1.0f);

    for (const Entity pawn : {plain, orc, cat}) {
        sim.submit(DamagePawnCommand{pawn, "Cut", 12.0f, 0});
    }
    sim.step();
    const float plainPain = racesPain(sim, plain);
    RACES_REQUIRE(plainPain > 0.0f);
    CHECK(racesPain(sim, cat) / plainPain == doctest::Approx(1.33f).epsilon(0.03));
    // The same wound is a smaller share of a bigger body: less pain, and less blood lost a day.
    CHECK(racesPain(sim, orc) < plainPain);
    // A tenth more hit points: the same wounds are a smaller share. The plain pawn, made an Orcoid now, with
    // the same wounds and the same clothes, loses a tenth less of its struck zone.
    const auto least = [&](Entity pawn) {
        float out = 1.0f;
        for (const float z : reg.get<Health>(pawn).zones) {
            out = std::min(out, z);
        }
        return out;
    };
    const float asHuman = least(plain);
    const float humanBleed = reg.get<Health>(plain).bleedPerDay;
    RACES_REQUIRE(asHuman < 1.0f);
    sim.submit(SetLineageCommand{plain, "Orcoid"});
    sim.step();
    CHECK(1.0f - least(plain) == doctest::Approx((1.0f - asHuman) / 1.1f).epsilon(0.03));
    if (humanBleed > 0.0f) {
        CHECK(reg.get<Health>(plain).bleedPerDay <= humanBleed / 1.09f);
    }
}

TEST_CASE("races: Orcoid hit harder in melee and work slower; drow work faster in the dark and slower in the sun") {
    Sim sim(racesSettings(), racesDefs());
    const std::vector<Entity> colonists = racesColonists(sim);
    RACES_REQUIRE(colonists.size() >= 4);
    const Entity orc = colonists[2];
    const Entity drow = colonists[3];
    entt::registry& reg = sim.things().registry();
    // Both in the same open-air cell, so the light and the sun are the same for both.
    const Position spot = reg.get<Position>(colonists[1]);
    const auto place = [&]() {
        for (const Entity pawn : {orc, drow}) {
            Position& p = reg.get<Position>(pawn);
            p.x = spot.x;
            p.y = spot.y;
            p.level = spot.level;
        }
    };
    place();
    RACES_REQUIRE(sim.lights().underOpenSky(cellOf(spot)));
    // Each against itself as a baseline human, a moment before.
    const float orcMelee = sim.combat().traitMeleeDamage(orc);
    const float orcWork = sim.jobs().workFactor(orc);
    const float drowWork = sim.jobs().workFactor(drow);
    sim.submit(SetLineageCommand{orc, "Orcoid"});
    sim.submit(SetLineageCommand{drow, "Drow"});
    sim.step();
    place();
    CHECK(sim.combat().traitMeleeDamage(orc) / orcMelee == doctest::Approx(1.1f));
    CHECK(sim.jobs().workFactor(orc) / orcWork == doctest::Approx(0.9f).epsilon(0.01));
    const auto drowExpected = [&]() {
        const CellPos c = cellOf(spot);
        // Dark vision: none of the dim light's slowdown that a human has.
        const float dim = 1.0f / sim.lights().workSpeedAt(c);
        if (sim.lights().skyGlow() > 0.3f) {
            return 0.9f * dim;  // outdoors in the sun
        }
        // In the dark: a tenth faster on top.
        return sim.lights().lightAt(c) < 0.3f ? 1.1f * dim : dim;
    };
    CHECK(sim.jobs().workFactor(drow) / drowWork == doctest::Approx(drowExpected()).epsilon(0.01));

    // The other half of the day: a baseline's factor there, then the drow's, back to back.
    const bool day = sim.lights().skyGlow() > 0.3f;
    for (int i = 0; i < 48 && (sim.lights().skyGlow() > 0.3f) == day; ++i) {
        sim.stepMany(1800);
    }
    CHECK((sim.lights().skyGlow() > 0.3f) != day);
    place();
    const float drowNow = sim.jobs().workFactor(drow);
    sim.submit(SetLineageCommand{drow, "Baseline"});
    sim.step();
    place();
    const float asHuman = sim.jobs().workFactor(drow);
    RACES_REQUIRE(asHuman > 0.0f);
    CHECK(drowNow / asHuman == doctest::Approx(drowExpected()).epsilon(0.02));
}

TEST_CASE("races: joiners and raiders roll a race, the drow's schedule is turned round") {
    Sim sim(racesSettings(), racesDefs());
    std::map<std::string, int> counts;
    for (uint64_t seed = 1; seed <= 2000; ++seed) {
        ++counts[sim.joinerLineage(seed)];
    }
    CHECK(counts.size() == 5);
    CHECK(counts["Baseline"] > 1000);
    CHECK(counts["Baseline"] < 1400);
    for (const char* other : {"Emberkin", "Orcoid", "Drow", "CatStrain"}) {
        CAPTURE(other);
        CHECK(counts[other] > 120);
        CHECK(counts[other] < 290);
    }
    CHECK(sim.joinerLineage(77) == sim.joinerLineage(77));

    // The player and the starting colonists are human.
    for (const Entity pawn : racesColonists(sim)) {
        const PawnGenes* genes = sim.genesOf(pawn);
        RACES_REQUIRE(genes != nullptr);
        CHECK(genes->genes == std::vector<std::string>{"QuickLearner"});
    }

    const Entity drow = racesColonists(sim)[2];
    sim.submit(SetLineageCommand{drow, "Drow"});
    sim.step();
    sim.giveNocturnalSchedule(drow);
    const Schedule standard = sim.needs().rules().standardSchedule();
    const Schedule* schedule = sim.things().registry().try_get<Schedule>(drow);
    RACES_REQUIRE(schedule != nullptr);
    for (int hour = 0; hour < 24; ++hour) {
        CAPTURE(hour);
        CHECK(schedule->hours[static_cast<size_t>(hour)] == standard.hours[static_cast<size_t>((hour + 12) % 24)]);
    }
    CHECK(schedule->hours[12] == ScheduleKind::Sleep);  // asleep at noon
    CHECK(schedule->hours[0] != ScheduleKind::Sleep);   // up at midnight
}
