// Drugs, tolerance, addiction, withdrawal, overdose and drug policies (docs/design/drugs.md).
#include "ColonySim/Bills.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Drugs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/MentalBreaks.h"
#include "ColonySim/Mood.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Recreation.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define DRUG_REQUIRE(...)                   \
    do {                                     \
        const bool drugOk = (__VA_ARGS__);  \
        CHECK(drugOk);                       \
        if (!drugOk) {                       \
            return;                          \
        }                                    \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> drugDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

struct DrugFixture {
    Sim sim;
    int64_t tick = 1000;  // the "now" doses are rolled at; each dose moves it on

    DrugFixture() : sim(settings(), drugDefs()) {
        for (int y = 0; y < sim.map().height(); ++y) {
            for (int x = 0; x < sim.map().width(); ++x) {
                sim.plants().remove(CellPos{x, y, 0});
            }
        }
        sim.mentalBreaks().setNatural(false);  // withdrawals run for days, and a low mood must not end them with a break
        sim.stepMany(9 * kSecondsPerHour);  // daytime
    }
    static SimSettings settings() {
        SimSettings s;
        s.seed = 5;
        s.mapSize = 64;
        s.wildAnimals = 0;
        s.flatMap = true;
        s.checkpointEvery = 0;
        return s;
    }
    entt::registry& reg() { return sim.things().registry(); }
    DefId thingDef(const char* name) const { return sim.defs().idOf("Things", name); }
    DefId hediff(const char* name) const { return sim.defs().idOf("Hediffs", name); }
    Entity colonist(CellPos cell = CellPos{10, 10, 0}) {
        const Entity pawn = sim.things().spawn("Human", cell);
        Colonist c;
        c.skills.push_back(SkillRecord{"Cooking", 8, Passion::None, 0.0f, false});
        reg().emplace<Colonist>(pawn, c);
        sim.jobs().addWorker(pawn);
        sim.step();
        return pawn;
    }
    bool dose(Entity pawn, const char* drug) {
        tick += 7919;
        return sim.drugs().takeDose(pawn, thingDef(drug), tick);
    }
    float sev(Entity pawn, const char* name) { return sim.health().environmentSeverity(pawn, hediff(name)); }
    // Steps `days` game days, keeping the pawn fed and the dishes done.
    void days(Entity pawn, double days) {
        const int64_t total = static_cast<int64_t>(days * 86400.0);
        for (int64_t s = 0; s < total; ++s) {
            if (s % 600 == 0 && reg().valid(pawn) && reg().all_of<Needs>(pawn)) {
                reg().get<Needs>(pawn).food = 1.0f;
            }
            sim.step();
        }
    }
    bool saw(EventType type, Entity pawn) {
        return std::any_of(sim.events().begin(), sim.events().end(), [&](const Event& e) { return e.type == type && e.thing == pawn; });
    }
    const ThoughtView* thought(Entity pawn, const char* name) {
        thoughts_ = sim.mood().thoughts(pawn, sim.time().seconds);
        for (const ThoughtView& t : thoughts_) {
            if (t.def == name) {
                return &t;
            }
        }
        return nullptr;
    }
    std::vector<ThoughtView> thoughts_;
};

}  // namespace

TEST_CASE("Drugs: the defs load, every drug has its chemical, high and recipe") {
    DrugFixture f;
    const DrugSystem& drugs = f.sim.drugs();
    CHECK(drugs.kinds().size() == 10);
    CHECK(drugs.chemicals().size() == 8);
    CHECK(drugs.policies().size() == 6);
    for (const DrugKind& k : drugs.kinds()) {
        CHECK(k.high.valid());
        CHECK(k.chemical >= 0);
    }
    for (const char* recipe : {"BrewBeer", "MakeSmokeleafJoint", "MakePsychiteTea", "MakeFlake", "MakeYayo", "MakeGoJuice", "MakeWakeUp", "MakePenoxycyline"}) {
        CHECK(f.sim.defs().find("Recipes", recipe) != nullptr);
    }
    CHECK(f.sim.defs().find("Things", "DrugLab") != nullptr);
    CHECK(f.sim.defs().find("Things", "FermentingBarrel") != nullptr);
}

TEST_CASE("Drugs: a beer gives a short high, rest, joy and tolerance, and the tolerance wears off") {
    DrugFixture f;
    const Entity pawn = f.colonist();
    f.reg().get<Joy>(pawn).level = 0.2f;
    DRUG_REQUIRE(f.dose(pawn, "Beer"));
    CHECK(f.sev(pawn, "BeerHigh") == doctest::Approx(0.15f));
    CHECK(f.reg().get<Joy>(pawn).level == doctest::Approx(0.37f));
    CHECK(f.sim.drugs().tolerance(pawn, "Alcohol") == doctest::Approx(0.016f));
    // The second beer is weaker, by 1 - 0.75 x tolerance.
    DRUG_REQUIRE(f.dose(pawn, "Beer"));
    CHECK(f.sev(pawn, "BeerHigh") == doctest::Approx(0.15f + 0.15f * (1.0f - 0.75f * 0.016f)).epsilon(0.001));
    // 75% a day: it is gone in a day.
    f.days(pawn, 1.0);
    CHECK(f.sev(pawn, "BeerHigh") == doctest::Approx(0.0f));
    CHECK(f.sim.drugs().tolerance(pawn, "Alcohol") == doctest::Approx(0.032f - 0.016f).epsilon(0.02));
}

TEST_CASE("Drugs: many beers make a pawn drunk (slow, dull), then a hangover") {
    DrugFixture f;
    const Entity pawn = f.colonist();
    const float consciousness = f.sim.health().capacityValue(pawn, "Consciousness");
    for (int i = 0; i < 6; ++i) {
        DRUG_REQUIRE(f.dose(pawn, "Beer"));
    }
    f.sim.step();
    CHECK(f.sev(pawn, "BeerHigh") > 0.7f);  // hammered
    CHECK(f.sim.health().capacityValue(pawn, "Consciousness") < consciousness - 0.4f);
    const ThoughtView* drunk = f.thought(pawn, "DrunkBeer");
    DRUG_REQUIRE(drunk != nullptr);
    CHECK(drunk->offset == doctest::Approx(26.0f));
    f.days(pawn, 1.5);
    CHECK(f.sev(pawn, "BeerHigh") == doctest::Approx(0.0f));
    CHECK(f.sev(pawn, "Hangover") > 0.0f);
    CHECK(f.thought(pawn, "HangoverMood") != nullptr);
    f.days(pawn, 1.0);
    CHECK(f.sev(pawn, "Hangover") == doctest::Approx(0.0f));
}

TEST_CASE("Drugs: a social drug cannot addict below its tolerance, and can above it") {
    DrugFixture f;
    const Entity pawn = f.colonist();
    for (int i = 0; i < 3; ++i) {  // 3 x 3% = 9%, under the joint's 15%
        DRUG_REQUIRE(f.dose(pawn, "SmokeleafJoint"));
    }
    CHECK_FALSE(f.sim.drugs().addicted(pawn, "Smokeleaf"));

    // Used to it: 50% tolerance makes 2% into 10% a dose.
    ChemicalState& cs = f.sim.drugs().states()[pawn].chemicals.front();
    cs.tolerance = 0.5f;
    int doses = 0;
    while (!f.sim.drugs().addicted(pawn, "Smokeleaf") && doses < 400) {
        f.sim.drugs().states()[pawn].chemicals.front().tolerance = 0.5f;
        DRUG_REQUIRE(f.dose(pawn, "SmokeleafJoint"));
        ++doses;
    }
    CHECK(f.sim.drugs().addicted(pawn, "Smokeleaf"));
    CHECK(doses > 1);
    CHECK(f.sim.drugs().progress(pawn, "Smokeleaf") == doctest::Approx(0.5f));
    CHECK(f.sim.drugs().need(pawn, "Smokeleaf") == doctest::Approx(0.9f));
}

TEST_CASE("Drugs: an addiction's need empties, withdrawal sets in, and it heals if left alone") {
    DrugFixture f;
    const Entity pawn = f.colonist();
    f.sim.drugs().states()[pawn].policy = "NoDrugs";  // it cannot find its fix anyway: nothing is on the map
    ChemicalState alcohol;
    alcohol.chemical = "Alcohol";
    alcohol.addicted = true;
    alcohol.need = 0.9f;
    alcohol.progress = 0.5f;
    f.sim.drugs().states()[pawn].chemicals.push_back(alcohol);
    f.days(pawn, 1.0);
    CHECK(f.sim.drugs().need(pawn, "Alcohol") == doctest::Approx(0.4f).epsilon(0.01));
    CHECK(f.sev(pawn, "AlcoholWithdrawal") == doctest::Approx(0.0f));
    f.days(pawn, 1.0);  // empty after 1.8 days
    CHECK(f.sim.drugs().need(pawn, "Alcohol") == doctest::Approx(0.0f));
    CHECK(f.sev(pawn, "AlcoholWithdrawal") > 0.0f);
    f.days(pawn, 1.0);
    CHECK(f.sev(pawn, "AlcoholWithdrawal") == doctest::Approx(1.0f));
    const ThoughtView* mood = f.thought(pawn, "AlcoholWithdrawalMood");
    DRUG_REQUIRE(mood != nullptr);
    CHECK(mood->offset == doctest::Approx(-35.0f));
    CHECK(f.sim.health().capacityValue(pawn, "Manipulation") <= 0.5f + 0.001f);
    // About 15 days from 50% at 3.33% a day: skip to the last half day.
    f.sim.drugs().states()[pawn].chemicals.front().progress = 0.99f;
    f.days(pawn, 0.6);
    CHECK_FALSE(f.sim.drugs().addicted(pawn, "Alcohol"));
    f.days(pawn, 1.0);  // the withdrawal fades out in half a day
    CHECK(f.sev(pawn, "AlcoholWithdrawal") == doctest::Approx(0.0f));
}

TEST_CASE("Drugs: luciferium addicts at once, never heals, and kills about ten days after the last dose runs out") {
    DrugFixture f;
    const Entity pawn = f.colonist();
    f.sim.drugs().states()[pawn].policy = "NoDrugs";
    DRUG_REQUIRE(f.dose(pawn, "Luciferium"));
    CHECK(f.sim.drugs().addicted(pawn, "Luciferium"));
    CHECK(f.sev(pawn, "LuciferiumHigh") == doctest::Approx(0.9f));
    f.sim.step();
    CHECK(f.sim.health().capacityValue(pawn, "BloodFiltration") > 1.5f);
    f.days(pawn, 5.0);
    CHECK(f.sim.drugs().need(pawn, "Luciferium") > 0.1f);
    CHECK_FALSE(f.sim.health().incapacitated(pawn));
    f.days(pawn, 2.0);  // empty after 6 days
    CHECK(f.sim.drugs().need(pawn, "Luciferium") == doctest::Approx(0.0f));
    CHECK(f.sev(pawn, "LuciferiumWithdrawal") > 0.0f);
    CHECK(f.sim.drugs().addicted(pawn, "Luciferium"));
    f.days(pawn, 12.0);
    const Health& health = f.reg().get<Health>(pawn);
    CHECK(health.dead);
    CHECK(health.cause == DeathCause::Withdrawal);
}

TEST_CASE("Drugs: a dose of luciferium every five days keeps a pawn well") {
    DrugFixture f;
    const Entity pawn = f.colonist();
    f.sim.drugs().states()[pawn].policy = "NoDrugs";
    for (int round = 0; round < 6; ++round) {
        DRUG_REQUIRE(f.dose(pawn, "Luciferium"));
        f.days(pawn, 5.0);
    }
    CHECK_FALSE(f.reg().get<Health>(pawn).dead);
    CHECK(f.sev(pawn, "LuciferiumWithdrawal") == doctest::Approx(0.0f));
    CHECK(f.sim.drugs().need(pawn, "Luciferium") > 0.0f);
}

TEST_CASE("Drugs: hard drugs overdose, and too many kill") {
    DrugFixture f;
    const Entity pawn = f.colonist();
    f.sim.drugs().states()[pawn].policy = "NoDrugs";
    for (int i = 0; i < 3; ++i) {
        DRUG_REQUIRE(f.dose(pawn, "Yayo"));
    }
    CHECK(f.sev(pawn, "Overdose") >= 0.5f);  // 3 x 0.18 at least
    CHECK(f.saw(EventType::DrugOverdose, pawn));
    for (int i = 0; i < 4; ++i) {
        f.dose(pawn, "Yayo");
    }
    f.sim.stepMany(3);
    const Health& health = f.reg().get<Health>(pawn);
    CHECK(health.dead);
    CHECK(health.cause == DeathCause::Overdose);
}

TEST_CASE("Drugs: social drugs never overdose, and an overdose fades by a day") {
    DrugFixture f;
    const Entity pawn = f.colonist();
    f.sim.drugs().states()[pawn].policy = "NoDrugs";
    for (int i = 0; i < 12; ++i) {
        DRUG_REQUIRE(f.dose(pawn, "Beer"));
    }
    CHECK(f.sev(pawn, "Overdose") == doctest::Approx(0.0f));
    DRUG_REQUIRE(f.dose(pawn, "WakeUp"));
    const float overdose = f.sev(pawn, "Overdose");
    CHECK(overdose >= 0.18f);
    f.days(pawn, 1.0);
    CHECK(f.sev(pawn, "Overdose") == doctest::Approx(0.0f));
}

TEST_CASE("Drugs: go-juice speeds a pawn, wake-up fills its rest, a stimulant slows tiring") {
    DrugFixture f;
    const Entity pawn = f.colonist();
    const float moving = f.sim.health().capacityValue(pawn, "Moving");
    DRUG_REQUIRE(f.dose(pawn, "GoJuice"));
    f.sim.step();
    CHECK(f.sim.health().capacityValue(pawn, "Moving") == doctest::Approx(moving + 0.5f).epsilon(0.01));
    CHECK(f.sim.drugs().restFallFactor(pawn) == doctest::Approx(1.0f));
    f.reg().get<Needs>(pawn).rest = 0.2f;
    DRUG_REQUIRE(f.dose(pawn, "WakeUp"));
    CHECK(f.reg().get<Needs>(pawn).rest == doctest::Approx(1.0f));
    CHECK(f.sim.drugs().restFallFactor(pawn) == doctest::Approx(0.8f));
}

TEST_CASE("Drugs: the default policy has a bored colonist walk to a beer and drink it") {
    DrugFixture f;
    const Entity pawn = f.colonist(CellPos{10, 10, 0});
    f.sim.things().spawn("Beer", CellPos{14, 10, 0});
    f.reg().get<Joy>(pawn).level = 0.1f;
    bool took = false;
    for (int i = 0; i < 600 && !took; ++i) {
        f.sim.step();
        took = f.saw(EventType::PawnTookDrug, pawn);
    }
    CHECK(took);
    CHECK(f.sim.drugs().tolerance(pawn, "Alcohol") > 0.0f);
    CHECK(f.sim.things().registry().view<Thing>().size() >= 1);
}

TEST_CASE("Drugs: no-drugs policy keeps the beer, a happy colonist leaves it, and hard drugs are not for joy") {
    {
        DrugFixture f;
        const Entity pawn = f.colonist(CellPos{10, 10, 0});
        f.sim.submit(SetDrugPolicyCommand{pawn, "NoDrugs"});
        f.sim.things().spawn("Beer", CellPos{14, 10, 0});
        f.reg().get<Joy>(pawn).level = 0.1f;
        bool took = false;
        for (int i = 0; i < 900 && !took; ++i) {
            f.sim.step();
            took = f.saw(EventType::PawnTookDrug, pawn);
        }
        CHECK_FALSE(took);
    }
    {
        DrugFixture f;
        const Entity pawn = f.colonist(CellPos{10, 10, 0});
        f.sim.things().spawn("Beer", CellPos{14, 10, 0});
        f.reg().get<Joy>(pawn).level = 1.0f;
        f.sim.stepMany(300);
        const DrugState* none = f.sim.drugs().state(pawn);
    CHECK((none == nullptr || none->last.empty()));
    }
    {
        DrugFixture f;
        const Entity pawn = f.colonist(CellPos{10, 10, 0});
        f.sim.things().spawn("Yayo", CellPos{14, 10, 0});
        f.reg().get<Joy>(pawn).level = 0.1f;
        f.sim.stepMany(900);
        const DrugState* none = f.sim.drugs().state(pawn);
    CHECK((none == nullptr || none->last.empty()));
    }
}

TEST_CASE("Drugs: an addict takes its drug even under a no-drugs policy, and a policy can allow more") {
    DrugFixture f;
    const Entity pawn = f.colonist(CellPos{10, 10, 0});
    f.sim.submit(SetDrugPolicyCommand{pawn, "NoDrugs"});
    f.sim.step();
    ChemicalState alcohol;
    alcohol.chemical = "Alcohol";
    alcohol.addicted = true;
    alcohol.need = 0.1f;
    alcohol.progress = 0.5f;
    f.sim.drugs().states()[pawn].chemicals.push_back(alcohol);
    f.sim.things().spawn("Beer", CellPos{14, 10, 0});
    bool took = false;
    for (int i = 0; i < 900 && !took; ++i) {
        f.sim.step();
        took = f.saw(EventType::PawnTookDrug, pawn);
    }
    CHECK(took);
    CHECK(f.sim.drugs().need(pawn, "Alcohol") > 0.8f);

    // Switching the beer off for addiction in the pawn's own policy keeps it from the bottle.
    SetDrugPolicyEntryCommand off;
    off.pawn = pawn;
    off.drug = "Beer";
    off.forAddiction = false;
    f.sim.submit(off);
    f.sim.step();
    f.sim.drugs().states()[pawn].chemicals.front().need = 0.05f;
    f.sim.things().spawn("Beer", CellPos{14, 12, 0});
    f.sim.stepMany(900);
    CHECK(f.sim.drugs().need(pawn, "Alcohol") < 0.1f);
}

TEST_CASE("Drugs: a scheduled drug is taken when due and not again before the frequency has passed") {
    DrugFixture f;
    const Entity pawn = f.colonist(CellPos{10, 10, 0});
    f.reg().get<Joy>(pawn).level = 1.0f;
    f.sim.submit(SetDrugPolicyCommand{pawn, "OneDrinkPerDay"});
    f.sim.things().spawn("Beer", CellPos{14, 10, 0}, 3);
    int doses = 0;
    for (int i = 0; i < 3000; ++i) {
        f.sim.step();
        for (const Event& e : f.sim.events()) {
            doses += e.type == EventType::PawnTookDrug && e.thing == pawn ? 1 : 0;
        }
        f.reg().get<Joy>(pawn).level = 1.0f;
    }
    CHECK(doses == 1);
}

TEST_CASE("Drugs: the player can order a dose, and bad orders fail") {
    DrugFixture f;
    const Entity pawn = f.colonist(CellPos{10, 10, 0});
    const Entity beer = f.sim.things().spawn("Beer", CellPos{13, 10, 0});
    const Entity steel = f.sim.things().spawn("Steel", CellPos{13, 12, 0});
    f.sim.submit(TakeDrugCommand{pawn, steel});
    f.sim.step();
    CHECK(std::any_of(f.sim.events().begin(), f.sim.events().end(), [](const Event& e) { return e.type == EventType::CommandFailed; }));
    f.sim.submit(TakeDrugCommand{pawn, beer});
    bool took = false;
    for (int i = 0; i < 300 && !took; ++i) {
        f.sim.step();
        took = f.saw(EventType::PawnTookDrug, pawn);
    }
    CHECK(took);
    CHECK_FALSE(f.reg().valid(beer));
    f.sim.submit(SetDrugPolicyCommand{pawn, "NoSuchPolicy"});
    f.sim.step();
    CHECK(std::any_of(f.sim.events().begin(), f.sim.events().end(), [](const Event& e) { return e.type == EventType::CommandFailed; }));
}

TEST_CASE("Drugs: state saves and loads, and the loaded game goes on the same") {
    DrugFixture f;
    const Entity pawn = f.colonist();
    f.dose(pawn, "Beer");
    f.dose(pawn, "Luciferium");
    f.sim.submit(SetDrugPolicyCommand{pawn, "Unrestricted"});
    SetDrugPolicyEntryCommand edit;
    edit.pawn = pawn;
    edit.drug = "Flake";
    edit.forJoy = false;
    edit.scheduled = true;
    edit.daysFrequency = 2.5f;
    edit.moodBelow = 0.4f;
    f.sim.submit(edit);
    f.days(pawn, 0.25);

    const std::vector<uint8_t> bytes = saveSim(f.sim);
    SaveLoadResult loaded = loadSim(bytes, drugDefs());
    DRUG_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->stateHash() == f.sim.stateHash());
    CHECK(saveSim(*loaded.sim) == bytes);
    const DrugState* state = loaded.sim->drugs().state(pawn);
    DRUG_REQUIRE(state != nullptr);
    CHECK(state->policy == "Unrestricted");
    CHECK(state->edits.size() == 1);
    CHECK(state->chemicals.size() == 2);
    CHECK(loaded.sim->drugs().addicted(pawn, "Luciferium"));
    for (int i = 0; i < 600; ++i) {
        f.sim.step();
        loaded.sim->step();
    }
    CHECK(loaded.sim->stateHash() == f.sim.stateHash());
}

TEST_CASE("Drugs: two runs of the same game give the same hashes") {
    uint64_t hashes[2] = {0, 0};
    for (uint64_t& hash : hashes) {
        DrugFixture f;
        const Entity pawn = f.colonist(CellPos{10, 10, 0});
        f.sim.things().spawn("Beer", CellPos{14, 10, 0}, 4);
        f.sim.submit(SetDrugPolicyCommand{pawn, "Unrestricted"});
        f.reg().get<Joy>(pawn).level = 0.1f;
        f.days(pawn, 0.3);
        hash = f.sim.stateHash();
    }
    CHECK(hashes[0] == hashes[1]);
}

TEST_CASE("Drugs: a barrel brews beer from hops and a drug lab rolls joints from leaves") {
    DrugFixture f;
    f.colonist(CellPos{10, 10, 0});
    f.sim.submit(SpawnThingCommand{"FermentingBarrel", CellPos{12, 10, 0}, 1});
    f.sim.submit(SpawnThingCommand{"DrugLab", CellPos{12, 14, 0}, 1});
    f.sim.step();
    Entity barrel = kNoEntity;
    Entity lab = kNoEntity;
    for (const Event& e : f.sim.events()) {
        if (e.type == EventType::ThingSpawned && f.sim.defs().get(e.def).defName() == "FermentingBarrel") {
            barrel = e.thing;
        }
        if (e.type == EventType::ThingSpawned && f.sim.defs().get(e.def).defName() == "DrugLab") {
            lab = e.thing;
        }
    }
    DRUG_REQUIRE(barrel != kNoEntity && lab != kNoEntity);
    f.sim.things().spawn("Hops", CellPos{11, 12, 0}, 25);
    f.sim.things().spawn("SmokeleafLeaves", CellPos{11, 16, 0}, 4);
    f.sim.submit(AddBillCommand{barrel, "BrewBeer", BillMode::Count, 5});  // refused: beer brewing is not researched
    f.sim.step();
    bool refused = false;
    for (const Event& e : f.sim.events()) {
        refused = refused || (e.type == EventType::CommandFailed && e.message.find("needs the research") != std::string::npos);
    }
    CHECK(refused);
    f.sim.submit(UnlockResearchCommand{"BeerBrewing"});
    f.sim.submit(AddBillCommand{barrel, "BrewBeer", BillMode::Count, 5});
    f.sim.submit(AddBillCommand{lab, "MakeSmokeleafJoint", BillMode::Count, 1});
    int beers = 0;
    int joints = 0;
    for (int i = 0; i < 6000 && (beers < 5 || joints < 1); ++i) {
        f.sim.step();
        beers = joints = 0;
        for (const auto [item, thing, stack] : f.reg().view<const Thing, const StackCount>().each()) {
            beers += f.sim.defs().get(thing.def).defName() == "Beer" ? stack.count : 0;
            joints += f.sim.defs().get(thing.def).defName() == "SmokeleafJoint" ? stack.count : 0;
        }
    }
    CHECK(beers == 5);
    CHECK(joints == 1);
}

// ---- Side effects: pain, work speed, hunger, chemical damage, binges, ambrosia, penoxycyline ----

namespace {

// Runs the drug system alone through `checks` of its staggered rolls: the seconds when (now + pawn) is a multiple of
// 600 and of 216 (5400 is both), so a stretch of days costs a few thousand calls, not millions of steps.
template <typename Fn>
void drugRolls(Sim& sim, Entity pawn, int checks, Fn each) {
    const int64_t id = static_cast<int64_t>(entt::to_integral(pawn));
    int64_t base = 10 * 86400;
    base += (5400 - (base + id) % 5400) % 5400;
    for (int i = 0; i < checks; ++i) {
        sim.drugs().step(base + static_cast<int64_t>(i) * 5400);
        if (!each(i)) {
            return;
        }
    }
}

int drugCount(Sim& sim, EventType type, Entity pawn) {
    return static_cast<int>(std::count_if(sim.events().begin(), sim.events().end(), [&](const Event& e) { return e.type == type && e.thing == pawn; }));
}

}  // namespace

TEST_CASE("Drugs: pain is cut by the high, work speed rises with wake-up and go-juice, smokeleaf makes a pawn hungrier") {
    DrugFixture f;
    const Entity pawn = f.colonist();
    DRUG_REQUIRE(f.sim.health().addInjury(pawn, 0, f.hediff("Cut"), 12.0f));
    HealthReport before;
    DRUG_REQUIRE(f.sim.health().report(pawn, before));
    DRUG_REQUIRE(before.pain > 0.05f);
    CHECK(f.sim.health().workSpeedFactor(pawn) == doctest::Approx(1.0f));

    DRUG_REQUIRE(f.dose(pawn, "Yayo"));
    f.sim.step();
    HealthReport yayo;
    f.sim.health().report(pawn, yayo);
    CHECK(yayo.pain == doctest::Approx(before.pain * 0.5f).epsilon(0.01));
    f.sim.health().setEnvironmentSeverity(pawn, f.hediff("YayoHigh"), 0.0f);

    DRUG_REQUIRE(f.dose(pawn, "GoJuice"));
    f.sim.step();
    HealthReport juice;
    f.sim.health().report(pawn, juice);
    CHECK(juice.pain == doctest::Approx(before.pain * 0.1f).epsilon(0.01));
    CHECK(f.sim.health().workSpeedFactor(pawn) == doctest::Approx(1.2f));
    DRUG_REQUIRE(f.dose(pawn, "WakeUp"));
    f.sim.step();
    CHECK(f.sim.health().workSpeedFactor(pawn) == doctest::Approx(1.8f));  // +20% and +50% multiply
    f.sim.health().setEnvironmentSeverity(pawn, f.hediff("GoJuiceHigh"), 0.0f);
    f.sim.health().setEnvironmentSeverity(pawn, f.hediff("WakeUpHigh"), 0.0f);
    f.sim.step();
    CHECK(f.sim.health().workSpeedFactor(pawn) == doctest::Approx(1.0f));
}

TEST_CASE("Drugs: smokeleaf takes 20% off pain and makes the food fall 30% faster") {
    DrugFixture f;
    const Entity a = f.colonist(CellPos{10, 10, 0});
    const Entity b = f.colonist(CellPos{12, 10, 0});
    DRUG_REQUIRE(f.sim.health().addInjury(a, 0, f.hediff("Cut"), 12.0f));
    HealthReport before;
    f.sim.health().report(a, before);
    DRUG_REQUIRE(f.dose(a, "SmokeleafJoint"));
    f.sim.step();
    HealthReport stoned;
    f.sim.health().report(a, stoned);
    CHECK(stoned.pain == doctest::Approx(before.pain * 0.8f).epsilon(0.01));
    CHECK(f.sim.health().hungerFactor(a) == doctest::Approx(1.3f));
    CHECK(f.sim.health().hungerFactor(b) == doctest::Approx(1.0f));
    f.reg().get<Needs>(a).food = 0.9f;
    f.reg().get<Needs>(b).food = 0.9f;
    f.sim.stepMany(3 * kSecondsPerHour);
    const float lostA = 0.9f - f.reg().get<Needs>(a).food;
    const float lostB = 0.9f - f.reg().get<Needs>(b).food;
    CHECK(lostB > 0.0f);
    CHECK(lostA == doctest::Approx(lostB * 1.3f).epsilon(0.05));
}

TEST_CASE("Drugs: drunk pawns feel less pain, the more the drunker") {
    DrugFixture f;
    const Entity pawn = f.colonist();
    DRUG_REQUIRE(f.sim.health().addInjury(pawn, 0, f.hediff("Cut"), 12.0f));
    HealthReport sober;
    f.sim.health().report(pawn, sober);
    for (int i = 0; i < 6; ++i) {
        DRUG_REQUIRE(f.dose(pawn, "Beer"));
    }
    f.sim.step();
    HealthReport drunk;
    f.sim.health().report(pawn, drunk);
    CHECK(drunk.pain < sober.pain * 0.85f);
}

TEST_CASE("Drugs: heavy tolerance hurts the liver for good, light use does not") {
    DrugFixture f;
    const Entity pawn = f.colonist();
    const DefId human = f.reg().get<Thing>(pawn).def;
    const std::vector<int> livers = f.sim.health().partsOfDef(human, "Liver");
    DRUG_REQUIRE(livers.size() == 1);
    const float full = f.sim.health().partMaxHitPoints(pawn, livers[0]);
    ChemicalState alcohol;
    alcohol.chemical = "Alcohol";
    alcohol.tolerance = 0.3f;
    f.sim.drugs().states()[pawn].chemicals.push_back(alcohol);
    drugRolls(f.sim, pawn, 4000, [&](int) { return true; });
    CHECK(drugCount(f.sim, EventType::ChemicalDamage, pawn) == 0);
    CHECK(f.sim.health().partHitPoints(pawn, livers[0]) == doctest::Approx(full));

    f.sim.drugs().states()[pawn].chemicals.front().tolerance = 1.0f;
    drugRolls(f.sim, pawn, 30000, [&](int) { return drugCount(f.sim, EventType::ChemicalDamage, pawn) < 2; });
    CHECK(drugCount(f.sim, EventType::ChemicalDamage, pawn) >= 1);
    CHECK(f.sim.health().partHitPoints(pawn, livers[0]) < full * 0.9f);
    HealthReport report;
    f.sim.health().report(pawn, report);
    const bool listed = std::any_of(report.injuries.begin(), report.injuries.end(),
                                    [](const HealthReport::InjuryLine& l) { return l.injury == "chemical damage" && l.permanent; });
    CHECK(listed);
    CHECK(f.sim.health().partHitPoints(pawn, livers[0]) > 0.0f);
}

TEST_CASE("Drugs: becoming addicted to psychite may hurt a kidney at once, a major overdose hurts the brain, wake-up risks the heart") {
    DrugFixture f;
    int hurt = 0;
    for (int i = 0; i < 40; ++i) {
        const Entity pawn = f.colonist(CellPos{5 + i % 20, 5 + i / 20, 0});
        ChemicalState psychite;
        psychite.chemical = "Psychite";
        psychite.tolerance = 1.0f;
        f.sim.drugs().states()[pawn].chemicals.push_back(psychite);
        f.dose(pawn, "PsychiteTea");  // at full tolerance, a certain addiction
        if (f.sim.drugs().addicted(pawn, "Psychite") && drugCount(f.sim, EventType::ChemicalDamage, pawn) > 0) {
            ++hurt;
        }
    }
    CHECK(hurt >= 1);
    CHECK(hurt <= 20);  // about 15% of 40

    const Entity pawn = f.colonist(CellPos{30, 30, 0});
    const DefId human = f.reg().get<Thing>(pawn).def;
    const std::vector<int> brain = f.sim.health().partsOfDef(human, "Brain");
    DRUG_REQUIRE(brain.size() == 1);
    const float brainMax = f.sim.health().partMaxHitPoints(pawn, brain[0]);
    f.sim.drugs().states()[pawn] = DrugState();
    drugRolls(f.sim, pawn, 6000, [&](int) {
        f.sim.health().setEnvironmentSeverity(pawn, f.hediff("Overdose"), 0.8f);
        return f.sim.health().partHitPoints(pawn, brain[0]) >= brainMax;
    });
    CHECK(f.sim.health().partHitPoints(pawn, brain[0]) < brainMax);

    const Entity runner = f.colonist(CellPos{40, 30, 0});
    const std::vector<int> heart = f.sim.health().partsOfDef(human, "Heart");
    DRUG_REQUIRE(heart.size() == 1);
    const float heartMax = f.sim.health().partMaxHitPoints(runner, heart[0]);
    f.dose(runner, "WakeUp");
    drugRolls(f.sim, runner, 200000, [&](int) {
        f.sim.health().setEnvironmentSeverity(runner, f.hediff("WakeUpHigh"), 0.5f);
        return f.sim.health().partHitPoints(runner, heart[0]) >= heartMax;
    });
    CHECK(f.sim.health().partHitPoints(runner, heart[0]) < heartMax);
}

TEST_CASE("Drugs: an addict in a bad mood binges on its drug, whatever its policy; the protagonist and a walked pawn never do") {
    DrugFixture f;
    const Entity pawn = f.colonist(CellPos{10, 10, 0});
    f.sim.submit(SetDrugPolicyCommand{pawn, "NoDrugs"});
    f.sim.step();
    ChemicalState alcohol;
    alcohol.chemical = "Alcohol";
    alcohol.addicted = true;
    alcohol.need = 0.9f;
    alcohol.progress = 0.5f;
    f.sim.drugs().states()[pawn].chemicals.push_back(alcohol);

    // A stable mood: no binge (the need is full, so no withdrawal either).
    drugRolls(f.sim, pawn, 3000, [&](int) { return true; });
    CHECK_FALSE(f.sim.drugs().bingeing(pawn));
    // A major mood band: a social drug's addict binges in a day or two.
    f.reg().get<Mood>(pawn).band = MoodBand::Major;
    drugRolls(f.sim, pawn, 3000, [&](int) { return !f.sim.drugs().bingeing(pawn); });
    CHECK(f.sim.drugs().bingeing(pawn));
    CHECK(drugCount(f.sim, EventType::DrugBinge, pawn) == 1);
    f.sim.drugs().states()[pawn].bingeUntil = f.sim.time().seconds + 3 * kSecondsPerHour;  // the rolls ran ahead of the clock

    // On the binge it drinks every beer it finds despite the no-drugs policy.
    f.sim.things().spawn("Beer", CellPos{14, 10, 0}, 3);
    f.sim.things().spawn("Beer", CellPos{14, 11, 0}, 3);
    int doses = 0;
    for (int i = 0; i < 1500; ++i) {
        f.sim.step();
        doses += drugCount(f.sim, EventType::PawnTookDrug, pawn);
    }
    CHECK(doses >= 2);

    // It ends by itself.
    f.days(pawn, 0.2);
    CHECK_FALSE(f.sim.drugs().bingeing(pawn));

    // The protagonist never binges.
    const Entity hero = f.colonist(CellPos{20, 20, 0});
    f.reg().get<Health>(hero).protagonist = true;
    f.sim.drugs().states()[hero].chemicals.push_back(alcohol);
    f.reg().get<Mood>(hero).band = MoodBand::Extreme;
    drugRolls(f.sim, hero, 3000, [&](int) { return true; });
    CHECK_FALSE(f.sim.drugs().bingeing(hero));
    CHECK_FALSE(f.sim.drugs().startBinge(hero, "Alcohol", 5));
    // Nor a pawn the player walks.
    const Entity walked = f.colonist(CellPos{22, 20, 0});
    f.reg().emplace<PlayerControl>(walked);
    CHECK_FALSE(f.sim.drugs().startBinge(walked, "Alcohol", 5));
}

TEST_CASE("Drugs: withdrawal can force a binge at any mood, and a binge with nothing to take ends at once") {
    DrugFixture f;
    const Entity pawn = f.colonist(CellPos{10, 10, 0});
    ChemicalState smoke;
    smoke.chemical = "Smokeleaf";
    smoke.addicted = true;
    smoke.need = 0.0f;
    smoke.progress = 0.5f;
    f.sim.drugs().states()[pawn].chemicals.push_back(smoke);
    drugRolls(f.sim, pawn, 4000, [&](int) {
        f.sim.health().setEnvironmentSeverity(pawn, f.hediff("SmokeleafWithdrawal"), 1.0f);
        return !f.sim.drugs().bingeing(pawn);
    });
    CHECK(f.sim.drugs().bingeing(pawn));
    CHECK(drugCount(f.sim, EventType::DrugBinge, pawn) == 1);

    // Nothing to smoke on the map: the first look ends it.
    int ended = 0;
    for (int i = 0; i < 120; ++i) {
        f.sim.step();
        ended += drugCount(f.sim, EventType::DrugBinge, pawn);
    }
    CHECK_FALSE(f.sim.drugs().bingeing(pawn));
    CHECK(ended == 1);
}

TEST_CASE("Drugs: ambrosia feeds, cheers and is mildly addictive; penoxycyline keeps three diseases away for 5.5 days") {
    DrugFixture f;
    const Entity pawn = f.colonist();
    f.reg().get<Needs>(pawn).food = 0.5f;
    f.reg().get<Joy>(pawn).level = 0.2f;
    DRUG_REQUIRE(f.dose(pawn, "Ambrosia"));
    f.sim.step();
    CHECK(f.reg().get<Needs>(pawn).food == doctest::Approx(0.7f).epsilon(0.02));
    CHECK(f.reg().get<Joy>(pawn).level == doctest::Approx(0.7f).epsilon(0.02));
    CHECK(f.sim.drugs().tolerance(pawn, "Ambrosia") == doctest::Approx(0.032f));
    const ThoughtView* high = f.thought(pawn, "AmbrosiaMood");
    DRUG_REQUIRE(high != nullptr);
    CHECK(high->offset == doctest::Approx(5.0f));
    CHECK_FALSE(f.sim.drugs().addicted(pawn, "Ambrosia"));  // under the 15% tolerance that addiction needs
    f.days(pawn, 0.8);
    CHECK(f.sev(pawn, "AmbrosiaHigh") == doctest::Approx(0.0f));  // 16 hours

    const Entity patient = f.colonist(CellPos{20, 20, 0});
    CHECK_FALSE(f.sim.drugs().preventsDisease(patient, "Malaria"));
    DRUG_REQUIRE(f.dose(patient, "Penoxycyline"));
    CHECK(f.sim.drugs().preventsDisease(patient, "Malaria"));
    CHECK(f.sim.drugs().preventsDisease(patient, "Plague"));
    CHECK(f.sim.drugs().preventsDisease(patient, "SleepingSickness"));
    CHECK_FALSE(f.sim.drugs().preventsDisease(patient, "Flu"));
    CHECK_FALSE(f.sim.drugs().addicted(patient, "Penoxycyline"));
    f.days(patient, 5.0);
    CHECK(f.sim.drugs().preventsDisease(patient, "Malaria"));
    f.days(patient, 0.6);
    CHECK_FALSE(f.sim.drugs().preventsDisease(patient, "Malaria"));
}

TEST_CASE("Drugs: a binge saves and loads") {
    DrugFixture f;
    const Entity pawn = f.colonist();
    DRUG_REQUIRE(f.sim.drugs().startBinge(pawn, "Alcohol", 100000));
    const std::vector<uint8_t> bytes = saveSim(f.sim);
    SaveLoadResult loaded = loadSim(bytes, drugDefs());
    DRUG_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->drugs().bingeing(pawn));
    const DrugState* state = loaded.sim->drugs().state(pawn);
    DRUG_REQUIRE(state != nullptr);
    CHECK(state->bingeChemical == "Alcohol");
    CHECK(state->bingeUntil == f.sim.drugs().state(pawn)->bingeUntil);
    CHECK(loaded.sim->stateHash() == f.sim.stateHash());
    CHECK(saveSim(*loaded.sim) == bytes);
}
