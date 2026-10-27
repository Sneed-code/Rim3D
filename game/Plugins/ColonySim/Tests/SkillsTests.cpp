#include "FuelTestHelper.h"
#include "ColonySim/Bills.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Mood.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Skills.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define SKILLS_REQUIRE(...)                  \
    do {                                     \
        const bool skillsOk = (__VA_ARGS__); \
        CHECK(skillsOk);                     \
        if (!skillsOk) {                     \
            return;                          \
        }                                    \
    } while (0)

namespace {

// The game's real data: SkillRules, the recipes and the thoughts are part of what is tested.
std::shared_ptr<const DefDatabase> skillsDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

SkillRules skillsRules() { return SkillRules::read(*skillsDefs()); }

SkillRecord skillsRecord(int level, Passion passion = Passion::None, float xp = 0.0f) {
    SkillRecord record;
    record.def = "Cooking";
    record.level = level;
    record.passion = passion;
    record.xp = xp;
    return record;
}

Sim skillsSim() { return Sim(SimSettings{5, 48, 0, 0, true}, skillsDefs()); }

Entity skillsSpawn(Sim& sim, const std::string& def, CellPos cell, int count = 1) {
    sim.submit(SpawnThingCommand{def, cell, count});
    sim.step();
    for (const Event& e : sim.events()) {
        if (e.type == EventType::ThingSpawned && e.cell == cell && sim.defs().get(e.def).defName() == def) {
            return e.thing;
        }
    }
    return kNoEntity;
}

// A colonist with one skill.
Entity skillsColonist(Sim& sim, CellPos cell, const std::string& skill, int level, Passion passion) {
    const Entity pawn = skillsSpawn(sim, "Human", cell);
    Colonist colonist;
    SkillRecord record = skillsRecord(level, passion);
    record.def = skill;
    colonist.skills.push_back(record);
    sim.things().registry().emplace<Colonist>(pawn, std::move(colonist));
    return pawn;
}

const SkillRecord& skillsOf(const Sim& sim, Entity pawn) { return sim.things().registry().get<Colonist>(pawn).skills[0]; }

template <typename F>
int skillsRunUntil(Sim& sim, int limit, F done) {
    for (int i = 0; i < limit; ++i) {
        if (done()) {
            return i;
        }
        sim.step();
    }
    return done() ? limit : -1;
}

bool skillsHasThought(const Sim& sim, Entity pawn, const std::string& def) {
    const std::vector<ThoughtView> thoughts = sim.mood().thoughts(pawn, sim.time().seconds);
    return std::any_of(thoughts.begin(), thoughts.end(), [&](const ThoughtView& t) { return t.def == def; });
}

}  // namespace

TEST_CASE("Skills: the rules come from the data") {
    const SkillRules rules = skillsRules();
    CHECK(rules.present);
    CHECK(rules.maxLevel() == 20);
    CHECK(rules.dailyXpSoftCap == doctest::Approx(4000.0));
    CHECK(rules.xpFactorOverCap == doctest::Approx(0.2));
    CHECK(rules.passionMood[1] == doctest::Approx(8.0));
    CHECK(rules.passionMood[2] == doctest::Approx(14.0));
    CHECK(rules.decayXpPerDay.size() == 21);
}

TEST_CASE("Skills: passion scales what is learned, and a skill rises when its experience fills") {
    const SkillRules rules = skillsRules();
    SkillRecord none = skillsRecord(3);
    SkillRecord minor = skillsRecord(3, Passion::Minor);
    SkillRecord major = skillsRecord(3, Passion::Major);
    CHECK(learnSkillXp(rules, none, 100.0) == doctest::Approx(35.0));
    CHECK(learnSkillXp(rules, minor, 100.0) == doctest::Approx(100.0));
    CHECK(learnSkillXp(rules, major, 100.0) == doctest::Approx(150.0));
    CHECK(major.xp == doctest::Approx(150.0f));
    CHECK(major.xpToday == doctest::Approx(150.0f));

    SkillRecord rising = skillsRecord(3, Passion::Minor, 3990.0f);  // level 3 needs 4,000
    learnSkillXp(rules, rising, 30.0);
    CHECK(rising.level == 4);
    CHECK(rising.xp == doctest::Approx(20.0f));

    SkillRecord disabled = skillsRecord(3, Passion::Major);
    disabled.disabled = true;
    CHECK(learnSkillXp(rules, disabled, 100.0) == 0.0);
    CHECK(disabled.xp == 0.0f);
}

TEST_CASE("Skills: after the daily soft cap a skill learns a fifth as fast, until midnight") {
    const SkillRules rules = skillsRules();
    SkillRecord skill = skillsRecord(5, Passion::Minor);
    learnSkillXp(rules, skill, 3999.0);
    CHECK(skill.xpToday == doctest::Approx(3999.0f));
    learnSkillXp(rules, skill, 100.0);  // still under the cap when it starts: full rate
    CHECK(skill.xpToday == doctest::Approx(4099.0f));
    CHECK(learnSkillXp(rules, skill, 100.0) == doctest::Approx(20.0));  // over it now
    skillDayPassed(rules, skill);
    CHECK(skill.xpToday == 0.0f);
    CHECK(learnSkillXp(rules, skill, 100.0) == doctest::Approx(100.0));
}

TEST_CASE("Skills: at level 20 experience stops just short of a level and holds off the decay") {
    const SkillRules rules = skillsRules();
    SkillRecord skill = skillsRecord(20, Passion::Major, 31000.0f);
    learnSkillXp(rules, skill, 5000.0);
    CHECK(skill.level == 20);
    CHECK(skill.xp == doctest::Approx(31999.0f));
}

TEST_CASE("Skills: from level 10 a skill loses experience each day, and the level when it runs out") {
    const SkillRules rules = skillsRules();
    SkillRecord low = skillsRecord(9, Passion::None, 500.0f);
    skillDayPassed(rules, low);
    CHECK(low.xp == doctest::Approx(500.0f));  // nothing lost below level 10
    SkillRecord ten = skillsRecord(10, Passion::None, 500.0f);
    skillDayPassed(rules, ten);
    CHECK(ten.xp == doctest::Approx(470.0f));  // 30 a day
    SkillRecord twelve = skillsRecord(12, Passion::None, 500.0f);
    skillDayPassed(rules, twelve);
    CHECK(twelve.xp == doctest::Approx(380.0f));  // 120 a day

    // Level 12 at 100 xp, 120 a day: it passes zero on the first day, sinks to -1,000 before the level goes.
    SkillRecord sinking = skillsRecord(12, Passion::None, 100.0f);
    int days = 0;
    while (sinking.level == 12 && days < 50) {
        skillDayPassed(rules, sinking);
        ++days;
    }
    CHECK(days == 10);  // 100 - 10 x 120 = -1,100
    CHECK(sinking.level == 11);
    CHECK(sinking.xp == doctest::Approx(-1100.0f + 14000.0f));  // level 11 needs 14,000

    SkillRecord bottom = skillsRecord(0, Passion::None, 0.0f);
    skillDayPassed(rules, bottom);
    CHECK(bottom.level == 0);
    CHECK(bottom.xp == 0.0f);
}

TEST_CASE("Skills: building a wall teaches Construction, more with passion") {
    const auto xpAfter = [](Passion passion) {
        Sim sim = skillsSim();
        const Entity pawn = skillsColonist(sim, CellPos{12, 11, 0}, "Construction", 8, passion);
        skillsSpawn(sim, "Steel", CellPos{12, 12, 0}, 5);
        sim.submit(PlaceBlueprintCommand{"Wall", "Steel", CellPos{14, 10, 0}, Rot4::North});
        sim.step();
        const int taken = skillsRunUntil(sim, 6000, [&] { return sim.map().passage(CellPos{14, 10, 0}) == Passage::Blocked; });
        CHECK(taken >= 0);
        return skillsOf(sim, pawn).xp;
    };
    const float none = xpAfter(Passion::None);
    const float major = xpAfter(Passion::Major);
    // A steel wall is 150 work at the build rate, a pawn of level 8 does 1.0 of it a second.
    CHECK(none > 5.0f);
    CHECK(major == doctest::Approx(none * (1.5f / 0.35f)).epsilon(0.05));
}

TEST_CASE("Skills: cooking teaches Cooking in proportion to the recipe's experience, whatever the speed") {
    float manip = 1.0f;  // a colonist's hands are not at 100% (the work, and so the experience per run, scales with it)
    const auto xpAfterRun = [&manip](int level) {
        Sim sim = skillsSim();
        const Entity stove = testFueled(sim, skillsSpawn(sim, "FueledStove", CellPos{20, 20, 0}));
        testFueled(sim, skillsSpawn(sim, "TorchLamp", CellPos{19, 22, 0}));  // the sim starts at midnight; cook at full speed
        const Entity cook = skillsColonist(sim, CellPos{10, 10, 0}, "Cooking", level, Passion::Minor);
        skillsSpawn(sim, "RawBerries", CellPos{12, 12, 0}, 25);
        sim.submit(AddBillCommand{stove, "CookMealSimple", BillMode::Count, 1});
        const int taken = skillsRunUntil(sim, 6000, [&] { return sim.jobs().bills().stockOf(sim.defs().idOf("Things", "MealSimple")) == 1; });
        CHECK(taken >= 0);
        manip = sim.health().capacityValue(cook, "Manipulation");
        return skillsOf(sim, cook).xp;
    };
    const Sim recipeSim(SimSettings{5, 48, 0, 0, true}, skillsDefs());  // keeps the recipe alive
    const Recipe* recipe = recipeSim.jobs().bills().recipe(skillsDefs()->idOf("Recipes", "CookMealSimple"));
    SKILLS_REQUIRE(recipe != nullptr);
    // Minor passion is 1.0x. At skill 8 the run is a steady 'skillXp' over 'work' seconds, so a whole run gives skillXp.
    const float xp8 = xpAfterRun(8);
    CHECK(xp8 == doctest::Approx(recipe->skillXp / manip).epsilon(0.03));
    // A slower cook takes longer at the same rate and learns more from the run.
    CHECK(xpAfterRun(0) > xp8 * 1.5f);
}

TEST_CASE("Skills: a passionate colonist is happier while the work goes on") {
    Sim sim = skillsSim();
    const Entity stove = testFueled(sim, skillsSpawn(sim, "FueledStove", CellPos{20, 20, 0}));
    const Entity cook = skillsColonist(sim, CellPos{10, 10, 0}, "Cooking", 8, Passion::Major);
    const Entity plain = skillsColonist(sim, CellPos{30, 30, 0}, "Cooking", 8, Passion::None);
    skillsSpawn(sim, "RawBerries", CellPos{12, 12, 0}, 25);
    sim.submit(AddBillCommand{stove, "CookMealSimple", BillMode::Count, 1});
    CHECK_FALSE(skillsHasThought(sim, cook, "PassionMajorWork"));  // not working yet
    SKILLS_REQUIRE(skillsRunUntil(sim, 3000, [&] { return skillsHasThought(sim, cook, "PassionMajorWork"); }) >= 0);
    CHECK_FALSE(skillsHasThought(sim, cook, "PassionMinorWork"));
    CHECK_FALSE(skillsHasThought(sim, plain, "PassionMajorWork"));
    const std::vector<ThoughtView> thoughts = sim.mood().thoughts(cook, sim.time().seconds);
    const auto it = std::find_if(thoughts.begin(), thoughts.end(), [](const ThoughtView& t) { return t.def == "PassionMajorWork"; });
    SKILLS_REQUIRE(it != thoughts.end());
    CHECK(it->offset == doctest::Approx(14.0f));
    CHECK(it->situation);
    // And it goes when the cooking is done.
    SKILLS_REQUIRE(skillsRunUntil(sim, 6000, [&] { return sim.jobs().bills().billsOf(stove)->bills.empty(); }) >= 0);
    sim.stepMany(5);
    CHECK_FALSE(skillsHasThought(sim, cook, "PassionMajorWork"));
}

TEST_CASE("Skills: midnight starts the day's total again and high skills decay, and a save keeps both") {
    Sim sim = skillsSim();
    const Entity pawn = skillsColonist(sim, CellPos{10, 10, 0}, "Cooking", 12, Passion::None);
    sim.things().registry().get<Colonist>(pawn).skills[0].xp = 1000.0f;
    sim.things().registry().get<Colonist>(pawn).skills[0].xpToday = 3500.0f;

    // A save in the middle of the day keeps today's total.
    SaveLoadResult loaded = loadSim(saveSim(sim), skillsDefs());
    SKILLS_REQUIRE(loaded.sim != nullptr);
    const SkillRecord& kept = loaded.sim->things().registry().get<Colonist>(pawn).skills[0];
    CHECK(kept.xpToday == doctest::Approx(3500.0f));
    CHECK(kept.xp == doctest::Approx(1000.0f));
    CHECK(loaded.sim->stateHash() == sim.stateHash());

    sim.stepMany(static_cast<int>(kSecondsPerDay) - static_cast<int>(sim.time().seconds));
    const SkillRecord& after = skillsOf(sim, pawn);
    CHECK(after.xpToday == doctest::Approx(-120.0f));  // the day starts with the decay already counted
    CHECK(after.xp == doctest::Approx(880.0f));  // level 12 loses 120 a day
    CHECK(after.level == 12);
}

TEST_CASE("Skills: the daily cap counts net experience, so decay lets a master learn a little more") {
    const SkillRules rules = skillsRules();
    SkillRecord master = skillsRecord(20, Passion::Minor, 0.0f);
    skillDayPassed(rules, master);
    CHECK(master.xpToday == doctest::Approx(-3600.0f));  // level 20 loses 3,600 a day
    // 7,600 earns at full rate until the net total reaches the 4,000 cap, then a fifth.
    learnSkillXp(rules, master, 7600.0);
    CHECK(master.xpToday == doctest::Approx(4000.0f));
    CHECK(learnSkillXp(rules, master, 100.0) == doctest::Approx(20.0));
    // Below level 10 nothing decays, so the day starts at 0.
    SkillRecord novice = skillsRecord(5, Passion::None, 0.0f);
    skillDayPassed(rules, novice);
    CHECK(novice.xpToday == 0.0f);
}

TEST_CASE("Skills: the xp, decay and passion numbers are the wiki's") {
    const SkillRules rules = skillsRules();
    SKILLS_REQUIRE(rules.maxLevel() == 20);
    double total = 0.0;
    for (int level = 0; level < 20; ++level) {
        total += rules.xpToNextLevel[static_cast<size_t>(level)];
    }
    CHECK(total == doctest::Approx(265000.0));  // total XP for level 20
    CHECK(rules.xpToNextLevel[9] == 10000.0);
    CHECK(rules.xpToNextLevel[10] == 12000.0);
    CHECK(rules.xpToNextLevel[20] == 32000.0);
    CHECK(rules.decayXpPerDay[10] == 30.0);
    CHECK(rules.decayXpPerDay[15] == 540.0);
    CHECK(rules.decayXpPerDay[20] == 3600.0);
    CHECK(rules.decayXpPerDay[9] == 0.0);
    CHECK(rules.passionLearn[0] == doctest::Approx(0.35));
    CHECK(rules.passionLearn[1] == doctest::Approx(1.0));
    CHECK(rules.passionLearn[2] == doctest::Approx(1.5));
    CHECK(rules.passionMood[1] == 8.0);
    CHECK(rules.passionMood[2] == 14.0);
}
