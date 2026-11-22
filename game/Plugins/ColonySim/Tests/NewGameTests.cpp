#include "doctest.h"

#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Looks.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Skills.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Weather.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

#define NG_REQUIRE(...)        \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

// Tests for docs/design/new-game.md: the Crash Landed scenario and the character screen's edits, on the real data.
namespace {

std::shared_ptr<const DefDatabase> newGameDefs() {
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

SimSettings newGameSettings(uint64_t seed) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = 128;
    settings.checkpointEvery = 0;
    settings.scenario = "CrashLanded";
    return settings;
}

std::vector<Entity> newGameColonists(const Sim& sim) {
    std::vector<Entity> out;
    for (const auto [e, c] : sim.things().registry().view<const Colonist>().each()) {
        (void)c;
        out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](Entity a, Entity b) { return entt::to_entity(a) < entt::to_entity(b); });
    return out;
}

// Items of `def` lying within `radius` metres of `at` (the landing spot), stacks added up.
int newGameItemsNear(const Sim& sim, const char* def, Position at, float radius) {
    int total = 0;
    const DefId id = sim.defs().idOf("Things", def);
    for (const auto [e, thing, stack, pos] : sim.things().registry().view<const Thing, const StackCount, const Position>().each()) {
        (void)e;
        const float dx = pos.x - at.x;
        const float dy = pos.y - at.y;
        if (thing.def == id && std::sqrt(dx * dx + dy * dy) <= radius) {
            total += stack.count;
        }
    }
    return total;
}

// Pieces of `def` lying within `radius` metres of `at` (apparel has no stack count).
int newGamePiecesNear(const Sim& sim, const char* def, Position at, float radius) {
    int total = 0;
    const DefId id = sim.defs().idOf("Things", def);
    for (const auto [e, thing, pos] : sim.things().registry().view<const Thing, const Position>().each()) {
        const float dx = pos.x - at.x;
        const float dy = pos.y - at.y;
        if (thing.def == id && sim.things().spawned(e) && std::sqrt(dx * dx + dy * dy) <= radius) {
            ++total;
        }
    }
    return total;
}

std::map<uint64_t, std::string> newGameFailures(const Sim& sim) {
    std::map<uint64_t, std::string> failed;
    for (const Event& e : sim.events()) {
        if (e.type == EventType::CommandFailed) {
            failed[e.command] = e.message;
        }
    }
    return failed;
}

}  // namespace

TEST_CASE("Crash Landed: the amnesiac player and two 18-year-old strangers, naked, with sneed's starting kit") {
    const auto defs = newGameDefs();
    const Def* scenario = defs->find("Scenarios", "CrashLanded");
    NG_REQUIRE(scenario != nullptr);
    CHECK(scenario->flag("menu"));
    CHECK(scenario->text("biome") == "TropicalRainforest");

    Sim sim(newGameSettings(7), defs);
    CHECK(sim.setupErrors().empty());
    for (const std::string& e : sim.setupErrors()) {
        MESSAGE(e);
    }
    const std::vector<Entity> people = newGameColonists(sim);
    NG_REQUIRE(people.size() == 3);
    int protagonists = 0;
    for (const Entity e : people) {
        const Colonist& c = sim.things().registry().get<Colonist>(e);
        protagonists += c.protagonist ? 1 : 0;
        const PawnLooks* looks = sim.things().registry().try_get<PawnLooks>(e);
        NG_REQUIRE(looks != nullptr);
        CHECK(looks->lineage == "Baseline");  // humans only for now
        CHECK(looks->creator.empty());
        // No backstories: traits from the 10-point pool, all spent; who YOU are has none.
        CHECK(c.childhood.empty());
        CHECK(c.adulthood.empty());
        if (c.protagonist) {
            CHECK(c.traits.empty());
        } else {
            CHECK(c.age == 18);
            CHECK_FALSE(c.traits.empty());
            CHECK(traitPointsSpent(*defs, c.traits) == 10);
            std::string why;
            CHECK(traitSetFits(*defs, c.traits, 10, why));
        }
        // Everyone lands naked.
        const Gear* worn = sim.things().registry().try_get<Gear>(e);
        CHECK((worn == nullptr || worn->apparel.empty()));
    }
    CHECK(protagonists == 1);
    CHECK(sim.things().registry().get<Colonist>(people[0]).protagonist);

    // The kit lies beside them: nothing more, nothing less (only one gun, carried by nobody).
    const Position at = sim.things().registry().get<Position>(people[0]);
    CHECK(newGameItemsNear(sim, "MealSurvivalPack", at, 12.0f) == 50);
    CHECK(newGameItemsNear(sim, "Silver", at, 12.0f) == 200);
    CHECK(newGameItemsNear(sim, "Steel", at, 12.0f) == 50);
    CHECK(newGameItemsNear(sim, "ComponentIndustrial", at, 12.0f) == 30);
    CHECK(newGameItemsNear(sim, "Gun_BoltActionRifle", at, 12.0f) == 1);
    CHECK(newGameItemsNear(sim, "WoodLog", at, 12.0f) == 0);
    // The cargo pack's cotton shirt and trousers for each of the three.
    CHECK(newGamePiecesNear(sim, "Apparel_TShirt", at, 12.0f) == 3);
    CHECK(newGamePiecesNear(sim, "Apparel_Pants", at, 12.0f) == 3);
    for (const Entity e : people) {
        const Gear* gear = sim.things().registry().try_get<Gear>(e);
        CHECK((gear == nullptr || gear->weapon == kNoEntity));
    }
}

TEST_CASE("Crash Landed: the character screen renames, changes gender and stores the creator's look, before the start only") {
    const auto defs = newGameDefs();
    Sim sim(newGameSettings(11), defs);
    NG_REQUIRE(newGameColonists(sim).size() == 3);
    const Entity player = newGameColonists(sim)[0];
    const Colonist before = sim.things().registry().get<Colonist>(player);
    const std::string childhood = before.childhood;

    EditColonistCommand rename;
    rename.slot = 0;
    rename.firstName = "Elara";
    rename.nickName = "Ell";
    rename.lastName = "Rivera";
    rename.female = before.female ? 0 : 1;
    rename.setCreator = true;
    rename.creator = "v=1;height=1.04;weight=-0.2;hair=3;";
    const uint64_t good = sim.submit(rename);
    EditColonistCommand badSlot;
    badSlot.slot = 3;
    const uint64_t bad = sim.submit(badSlot);
    EditColonistCommand longName;
    longName.slot = 1;
    longName.firstName = std::string(25, 'a');
    const uint64_t tooLong = sim.submit(longName);
    sim.step();
    const auto failed = newGameFailures(sim);
    CHECK(failed.count(good) == 0);
    CHECK(failed.count(bad) == 1);
    CHECK(failed.count(tooLong) == 1);

    const std::vector<Entity> people = newGameColonists(sim);
    NG_REQUIRE(people.size() == 3);
    CHECK(people[0] == player);  // edited in place, not remade
    const Colonist& after = sim.things().registry().get<Colonist>(player);
    CHECK(after.firstName == "Elara");
    CHECK(after.nickName == "Ell");
    CHECK(after.lastName == "Rivera");
    CHECK(after.female != before.female);
    CHECK(after.childhood == childhood);  // the rest stays
    CHECK(after.protagonist);
    const PawnLooks& looks = sim.things().registry().get<PawnLooks>(player);
    CHECK(looks.female == after.female);
    CHECK(looks.creator == rename.creator);
    // The snapshot shows it to Unreal.
    Snapshot snap;
    sim.writeSnapshot(snap);
    bool seen = false;
    for (const ThingView& view : snap.things) {
        if (view.id == player && view.looks >= 0) {
            seen = snap.looks[static_cast<size_t>(view.looks)].creator == rename.creator;
        }
    }
    CHECK(seen);

    // Same seed and same edits give the same game; a save and a replay keep them.
    Sim twin(newGameSettings(11), defs);
    twin.submit(rename);
    twin.submit(badSlot);
    twin.submit(longName);
    twin.step();
    CHECK(twin.stateHash() == sim.stateHash());
    SaveLoadResult loaded = loadSim(saveSim(sim), defs);
    CHECK(loaded.error.empty());
    NG_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->stateHash() == sim.stateHash());
    CHECK(loaded.sim->things().registry().get<PawnLooks>(player).creator == rename.creator);
    Replay replay;
    std::string error;
    CHECK(readReplay(writeReplay(recordReplay(sim)), replay, error));
    int edits = 0;
    for (const LoggedCommand& logged : replay.commands) {
        if (const auto* edit = std::get_if<EditColonistCommand>(&logged.command)) {
            edits += 1;
            if (edit->slot == 0) {
                CHECK(edit->creator == rename.creator);
                CHECK(edit->firstName == "Elara");
            }
        }
    }
    CHECK(edits == 3);

    // Once the game runs it is too late.
    sim.stepMany(3);
    EditColonistCommand late;
    late.slot = 0;
    late.firstName = "Late";
    const uint64_t lateId = sim.submit(late);
    sim.step();
    CHECK(newGameFailures(sim).count(lateId) == 1);
    CHECK(sim.things().registry().get<Colonist>(player).firstName == "Elara");
}

TEST_CASE("Crash Landed: the character screen's edits apply at once before the start and replay the same") {
    const auto defs = newGameDefs();
    Sim sim(newGameSettings(5), defs);
    sim.submit(RerollColonistCommand{1});
    CHECK(sim.applyPregameCommands());
    CHECK(sim.time().steps == 0);
    CHECK(sim.time().seconds == 7 * kSecondsPerHour);  // the scenario lands at 7:00, by daylight
    CHECK(sim.time().hourOfDay() == 7);
    // It lands in clear weather that began at the landing, so the HUD doesn't name a storm the sky hasn't shown yet.
    CHECK(sim.weather().current() == sim.defs().idOf("Weathers", "Clear"));
    CHECK(sim.weather().weatherStartedAt() == 7 * kSecondsPerHour);
    CHECK(sim.weather().weatherEndsAt() > 7 * kSecondsPerHour + 2 * kSecondsPerHour);
    EditColonistCommand edit;
    edit.slot = 1;
    edit.firstName = "Mina";
    edit.setCreator = true;
    edit.creator = "seed=4;female=1;";
    sim.submit(edit);
    sim.submit(RerollColonistCommand{2});
    CHECK(sim.applyPregameCommands());
    CHECK(sim.things().registry().get<Colonist>(newGameColonists(sim)[1]).firstName == "Mina");
    sim.step();
    CHECK_FALSE(sim.applyPregameCommands());

    // A replay (all of them in its first step) comes out the same.
    Sim twin(newGameSettings(5), defs);
    twin.submit(RerollColonistCommand{1});
    twin.submit(edit);
    twin.submit(RerollColonistCommand{2});
    twin.step();
    CHECK(twin.stateHash() == sim.stateHash());
}

TEST_CASE("Trait points: colonists made this way spend the whole pool, and skill traits pass 20") {
    const auto defs = newGameDefs();
    int seenSkillTrait = 0;
    int seenNegative = 0;
    for (uint64_t seed = 1; seed <= 300; ++seed) {
        Colonist c;
        std::string error;
        ColonistGenOptions options;
        options.traitPoints = 10;
        NG_REQUIRE(generateColonist(*defs, seed, false, c, error, options));
        CHECK(c.childhood.empty());
        CHECK(traitPointsSpent(*defs, c.traits) == 10);
        CHECK(traitSetFits(*defs, c.traits, 10, error));
        for (const TraitRecord& t : c.traits) {
            seenNegative += traitPointCost(*defs, t) < 0 ? 1 : 0;
            if (t.def == "NaturalMarksman") {
                ++seenSkillTrait;
                const SkillRecord* shooting = c.skill("Shooting");
                NG_REQUIRE(shooting != nullptr);
                CHECK(shooting->bonus == 3);
                CHECK(shooting->level >= 3);
            }
        }
        for (const SkillRecord& s : c.skills) {
            CHECK(s.level >= 0);
            CHECK(s.level <= 20 + std::max(0, s.bonus));
        }
    }
    CHECK(seenSkillTrait > 0);
    CHECK(seenNegative > 0);

    // The backstory way is unchanged: no point-only traits drawn there.
    for (uint64_t seed = 1; seed <= 200; ++seed) {
        Colonist c;
        std::string error;
        NG_REQUIRE(generateColonist(*defs, seed, false, c, error));
        CHECK_FALSE(c.childhood.empty());
        for (const TraitRecord& t : c.traits) {
            const Def* trait = defs->find("Traits", t.def);
            CHECK((trait != nullptr && !trait->flag("pointsOnly")));
        }
    }

    // A +3 skill learns up to 23, and no further.
    SkillRules rules = SkillRules::read(*defs);
    SkillRecord skill;
    skill.def = "Shooting";
    skill.level = 20;
    skill.bonus = 3;
    skill.passion = Passion::Major;
    for (int i = 0; i < 400; ++i) {
        learnSkillXp(rules, skill, 5000.0);
        skill.xpToday = 0.0f;
    }
    CHECK(skill.level == 23);
    skillDayPassed(rules, skill);
    CHECK(skill.level >= 22);
    SkillRecord plain;
    plain.level = 20;
    for (int i = 0; i < 100; ++i) {
        learnSkillXp(rules, plain, 5000.0);
    }
    CHECK(plain.level == 20);

    // Tough is -30% damage taken (sneed), not RimWorld's -50%.
    const Def* tough = defs->find("Traits", "Tough");
    NG_REQUIRE(tough != nullptr);
    CHECK(tough->number("degreeDatas.0.statFactors.incomingDamageFactor", 0.0) == doctest::Approx(0.7));
    CHECK(traitPointCost(*defs, TraitRecord{"Tough", 0}) == 5);
    CHECK(traitPointCost(*defs, TraitRecord{"SpeedOffset", 1}) == 1);
    CHECK(traitPointCost(*defs, TraitRecord{"SpeedOffset", 2}) == 3);
    CHECK(traitPointCost(*defs, TraitRecord{"Wimp", 0}) < 0);
}

TEST_CASE("Crash Landed: the companions screen buys and randomizes traits, before the start only") {
    const auto defs = newGameDefs();
    Sim sim(newGameSettings(13), defs);
    NG_REQUIRE(newGameColonists(sim).size() == 3);
    const Entity stranger = newGameColonists(sim)[1];

    SetColonistTraitsCommand buy;
    buy.slot = 1;
    buy.traits = "Tough;NaturalMarksman;Wimp;SpeedOffset:2;Pacifist";  // 5 + 3 - 3 + 3 - 4 = 4, but Pacifist clashes with the marksman
    const uint64_t clash = sim.submit(buy);
    SetColonistTraitsCommand tooMuch;
    tooMuch.slot = 1;
    tooMuch.traits = "Tough;Industriousness:2;NaturalMarksman";  // 13 points
    const uint64_t over = sim.submit(tooMuch);
    SetColonistTraitsCommand you;
    you.slot = 0;
    you.traits = "Kind";
    const uint64_t protagonist = sim.submit(you);
    SetColonistTraitsCommand good;
    good.slot = 1;
    good.traits = "Tough;NaturalMarksman;Wimp;SpeedOffset:2;Pampered";  // 5 + 3 - 3 + 3 - 3 = 5
    const uint64_t ok = sim.submit(good);
    CHECK(sim.applyPregameCommands());
    const auto failed = newGameFailures(sim);
    CHECK(failed.count(clash) == 1);
    CHECK(failed.count(over) == 1);
    CHECK(failed.count(protagonist) == 1);
    CHECK(failed.count(ok) == 0);

    const Colonist& c = sim.things().registry().get<Colonist>(stranger);
    CHECK(c.traits.size() == 5);
    CHECK(traitPointsSpent(*defs, c.traits) == 5);
    const SkillRecord* shooting = c.skill("Shooting");
    NG_REQUIRE(shooting != nullptr);
    CHECK(shooting->bonus == 3);
    CHECK(std::binary_search(c.disabledWorkTags.begin(), c.disabledWorkTags.end(), std::string("ManualDumb")));
    const int shootingWith = shooting->level;

    // Taking the marksman away gives back the base level.
    SetColonistTraitsCommand fewer;
    fewer.slot = 1;
    fewer.traits = "Tough";
    sim.submit(fewer);
    CHECK(sim.applyPregameCommands());
    CHECK(sim.things().registry().get<Colonist>(stranger).skill("Shooting")->level == shootingWith - 3);
    CHECK(sim.things().registry().get<Colonist>(stranger).disabledWorkTags.empty());

    // Random traits spend the whole pool; the same roll gives the same traits.
    SetColonistTraitsCommand dice;
    dice.slot = 2;
    dice.randomize = true;
    dice.roll = 7;
    sim.submit(dice);
    CHECK(sim.applyPregameCommands());
    const std::vector<TraitRecord> rolled = sim.things().registry().get<Colonist>(newGameColonists(sim)[2]).traits;
    CHECK(traitPointsSpent(*defs, rolled) == 10);

    Sim twin(newGameSettings(13), defs);
    for (const SetColonistTraitsCommand& command : {buy, tooMuch, you, good, fewer, dice}) {
        twin.submit(command);
    }
    twin.step();
    sim.step();
    CHECK(twin.stateHash() == sim.stateHash());
    SaveLoadResult loaded = loadSim(saveSim(sim), defs);
    NG_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->stateHash() == sim.stateHash());
    Replay replay;
    std::string error;
    CHECK(readReplay(writeReplay(recordReplay(sim)), replay, error));

    SetColonistTraitsCommand late;
    late.slot = 1;
    late.traits = "Kind";
    const uint64_t lateId = sim.submit(late);
    sim.step();
    CHECK(newGameFailures(sim).count(lateId) == 1);
}

TEST_CASE("Title backdrop: a wild rainforest tile in the morning, with no people and no cargo, off the menu") {
    const auto defs = newGameDefs();
    const Def* scenario = defs->find("Scenarios", "TitleBackdrop");
    NG_REQUIRE(scenario != nullptr);
    CHECK_FALSE(scenario->flag("menu"));
    SimSettings settings = newGameSettings(42);
    settings.scenario = "TitleBackdrop";
    settings.world.enabled = true;
    settings.wildAnimals = 100;
    Sim sim(settings, defs);
    for (const std::string& e : sim.setupErrors()) {
        MESSAGE(e);
    }
    CHECK(sim.setupErrors().empty());
    CHECK(sim.biome() == "TropicalRainforest");
    CHECK(newGameColonists(sim).empty());
    CHECK(sim.time().hourOfDay() == 7);
    CHECK(sim.time().steps == 0);
    CHECK(sim.plants().count() > 0);
}
