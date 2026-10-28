#include "doctest.h"

#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Regions.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace colony;

namespace {

std::shared_ptr<const DefDatabase> colonistTestDefs() {
    auto db = std::make_shared<DefDatabase>();
    std::vector<std::string> errors;
    const bool ok = DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
    for (const std::string& e : errors) MESSAGE(e);
    CHECK(ok);
    return db;
}

bool colonistSame(const Colonist& a, const Colonist& b) {
    if (a.firstName != b.firstName || a.nickName != b.nickName || a.lastName != b.lastName || a.female != b.female ||
        a.age != b.age || a.childhood != b.childhood || a.adulthood != b.adulthood || a.traits.size() != b.traits.size() ||
        a.skills.size() != b.skills.size() || a.disabledWorkTags != b.disabledWorkTags) {
        return false;
    }
    for (size_t i = 0; i < a.traits.size(); ++i) {
        if (a.traits[i].def != b.traits[i].def || a.traits[i].degree != b.traits[i].degree) return false;
    }
    for (size_t i = 0; i < a.skills.size(); ++i) {
        const SkillRecord& x = a.skills[i];
        const SkillRecord& y = b.skills[i];
        if (x.def != y.def || x.level != y.level || x.passion != y.passion || x.xp != y.xp || x.disabled != y.disabled) return false;
    }
    return true;
}

std::vector<std::string> colonistTags(const Def& def, const char* path) {
    std::vector<std::string> out;
    for (size_t i = 0; i < def.count(path); ++i) out.push_back(def.textAt(path, i));
    return out;
}

}  // namespace

TEST_CASE("colonists: the same seed makes the same colonist, other seeds make others") {
    const auto defs = colonistTestDefs();
    Colonist a, b, c;
    std::string error;
    CHECK(generateColonist(*defs, 77, false, a, error));
    CHECK(generateColonist(*defs, 77, false, b, error));
    CHECK(generateColonist(*defs, 78, false, c, error));
    CHECK(error.empty());
    CHECK(colonistSame(a, b));
    CHECK_FALSE(colonistSame(a, c));
}

TEST_CASE("colonists: every colonist follows the rules in the defs") {
    const auto defs = colonistTestDefs();
    std::set<std::string> traitsSeen, childhoods, adulthoods;
    std::map<int, int> levels;
    int majors = 0;
    for (uint64_t seed = 1; seed <= 400; ++seed) {
        Colonist c;
        std::string error;
        if (!generateColonist(*defs, seed, seed % 7 == 0, c, error)) {
            CHECK(error.empty());
            return;
        }
        CHECK(c.protagonist == (seed % 7 == 0));
        CHECK(c.age >= 18);
        CHECK(c.age <= 100);
        CHECK_FALSE(c.firstName.empty());
        CHECK_FALSE(c.lastName.empty());
        childhoods.insert(c.childhood);
        adulthoods.insert(c.adulthood);

        // Backstories: the right slots, and their work stays disabled.
        const Def* child = defs->find("Backstories", c.childhood);
        const Def* adult = defs->find("Backstories", c.adulthood);
        if (child == nullptr || adult == nullptr) {
            CHECK(false);
            return;
        }
        CHECK(child->text("slot") == "Childhood");
        CHECK(adult->text("slot") == "Adulthood");
        for (const Def* b : {child, adult}) {
            for (const std::string& tag : colonistTags(*b, "workDisables")) {
                CHECK(std::binary_search(c.disabledWorkTags.begin(), c.disabledWorkTags.end(), tag));
            }
        }

        // Traits: one to three, known, no repeats, no conflicts, none needing disabled work.
        CHECK(c.traits.size() >= 1);
        CHECK(c.traits.size() <= 3);
        std::set<std::string> names;
        for (const TraitRecord& t : c.traits) {
            const Def* def = defs->find("Traits", t.def);
            if (def == nullptr) {
                CHECK(false);
                return;
            }
            CHECK(names.insert(t.def).second);
            traitsSeen.insert(t.def);
            for (const std::string& other : colonistTags(*def, "conflictingTraits")) {
                CHECK(std::find_if(c.traits.begin(), c.traits.end(), [&](const TraitRecord& x) { return x.def == other; }) == c.traits.end());
            }
            for (const std::string& tag : colonistTags(*def, "requiredWorkTags")) {
                CHECK_FALSE(std::binary_search(c.disabledWorkTags.begin(), c.disabledWorkTags.end(), tag));
            }
        }

        // Skills: all twelve, in range, passions only on skills that can be used.
        CHECK(c.skills.size() == defs->idsOfKind("Skills").size());
        int passions = 0;
        for (const SkillRecord& s : c.skills) {
            CHECK(s.level >= 0);
            CHECK(s.level <= 20);
            CHECK(s.xp >= 0.0f);
            levels[s.level] += 1;
            if (s.passion != Passion::None) {
                ++passions;
                majors += s.passion == Passion::Major ? 1 : 0;
                CHECK_FALSE(s.disabled);
                if (s.def == "Shooting") {
                    CHECK(std::find_if(c.traits.begin(), c.traits.end(), [](const TraitRecord& t) { return t.def == "Brawler"; }) == c.traits.end());
                }
            }
            const Def* skillDef = defs->find("Skills", s.def);
            bool shouldBeDisabled = false;
            for (const std::string& tag : colonistTags(*skillDef, "disablingWorkTags")) {
                shouldBeDisabled = shouldBeDisabled || std::binary_search(c.disabledWorkTags.begin(), c.disabledWorkTags.end(), tag);
            }
            CHECK(s.disabled == shouldBeDisabled);
        }
        CHECK(passions <= 4);
    }
    // Over 400 colonists the seed reaches wide: most traits and backstories, a spread of levels.
    CHECK(traitsSeen.size() >= 20);
    CHECK(childhoods.size() >= 10);
    CHECK(adulthoods.size() >= 12);
    CHECK(levels.size() >= 15);
    CHECK(majors > 0);
}

TEST_CASE("colonists: colonists need the defs") {
    DefDatabase empty;
    Colonist c;
    std::string error;
    CHECK_FALSE(generateColonist(empty, 1, false, c, error));
    CHECK_FALSE(error.empty());
}

namespace {

SimSettings colonistStartSettings(uint64_t seed) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = 256;
    settings.scenario = "Crashlanded";
    return settings;
}

std::vector<Colonist> colonistsOf(const Sim& sim) {
    std::vector<Colonist> out;
    sim.things().registry().view<const Colonist>().each([&](const Colonist& c) { out.push_back(c); });
    return out;
}

}  // namespace

TEST_CASE("Crashlanded: the protagonist, five colonists and the starting items") {
    const auto defs = colonistTestDefs();
    Sim sim(colonistStartSettings(42), defs);
    for (const std::string& e : sim.setupErrors()) MESSAGE(e);
    CHECK(sim.setupErrors().empty());

    const std::vector<Colonist> colonists = colonistsOf(sim);
    CHECK(colonists.size() == 6);
    CHECK(std::count_if(colonists.begin(), colonists.end(), [](const Colonist& c) { return c.protagonist; }) == 1);
    std::set<std::string> names;
    for (const Colonist& c : colonists) names.insert(c.firstName + " " + c.lastName);
    CHECK(names.size() == 6);

    // The colonists stand on walkable ground in the same area, all able to walk to each other.
    std::vector<CellPos> cells;
    sim.things().registry().view<const Colonist, const Position>().each([&](const Colonist&, const Position& p) { cells.push_back(cellOf(p)); });
    CHECK(cells.size() == 6);
    for (const CellPos& cell : cells) {
        CHECK(sim.map().walkable(cell));
        CHECK(sim.regions().canReach(cells[0], cell));
    }

    // Starting, nearby and scattered things add up to the scenario's counts.
    std::map<std::string, int> items;
    sim.things().registry().view<const Thing, const StackCount>().each([&](const Thing& t, const StackCount& s) {
        items[sim.defs().get(t.def).defName()] += s.count;
    });
    CHECK(items["Silver"] == 800);
    CHECK(items["MealSurvivalPack"] == 57);
    CHECK(items["ComponentIndustrial"] == 30);
    CHECK(items["MedicineIndustrial"] == 30);
    CHECK(items["Steel"] == 1170);
    CHECK(items["WoodLog"] == 300);
}

TEST_CASE("Crashlanded: same seed and same game, other seeds other colonists") {
    const auto defs = colonistTestDefs();
    Sim a(colonistStartSettings(5), defs);
    Sim b(colonistStartSettings(5), defs);
    Sim c(colonistStartSettings(6), defs);
    CHECK(a.stateHash() == b.stateHash());
    CHECK(a.stateHash() != c.stateHash());
    const std::vector<Colonist> ca = colonistsOf(a);
    const std::vector<Colonist> cb = colonistsOf(b);
    if (!(ca.size() == 6 && cb.size() == 6)) {
        CHECK(false);
        return;
    }
    for (size_t i = 0; i < 6; ++i) CHECK(colonistSame(ca[i], cb[i]));
    a.stepMany(200);
    b.stepMany(200);
    CHECK(a.stateHash() == b.stateHash());
}

TEST_CASE("Crashlanded: colonists work, except the work their backstories and traits rule out") {
    const auto defs = colonistTestDefs();
    int incapable = 0;  // colonists with some work off, over a few games
    for (uint64_t seed = 1; seed <= 6; ++seed) {
        Sim sim(colonistStartSettings(seed), defs);
        const JobSystem& jobs = sim.jobs();
        sim.things().registry().view<const Colonist>().each([&](const Entity pawn, const Colonist& c) {
            const WorkSettings* work = sim.things().registry().try_get<WorkSettings>(pawn);
            CHECK(sim.things().registry().all_of<Worker>(pawn));
            if (work == nullptr) {
                CHECK(false);
                return;
            }
            CHECK(work->entries.size() == jobs.workTypes().size());
            incapable += c.disabledWorkTags.empty() ? 0 : 1;
            for (size_t i = 0; i < work->entries.size() && i < jobs.workTypes().size(); ++i) {
                const JobSystem::WorkType& type = jobs.workTypes()[i];
                const bool ruledOut = std::any_of(type.workTags.begin(), type.workTags.end(), [&](const std::string& tag) {
                    return std::binary_search(c.disabledWorkTags.begin(), c.disabledWorkTags.end(), tag);
                });
                CHECK(work->entries[i].disabled == ruledOut);
                CHECK(work->entries[i].priority == (ruledOut ? 0 : JobSystem::kDefaultPriority));
            }
        });
    }
    CHECK(incapable > 0);
}

TEST_CASE("Crashlanded: colonists survive a save and load") {
    const auto defs = colonistTestDefs();
    Sim original(colonistStartSettings(9), defs);
    original.stepMany(100);
    const std::vector<uint8_t> bytes = saveSim(original);
    SaveLoadResult loaded = loadSim(bytes, defs);
    if (!(loaded.sim != nullptr)) {
        MESSAGE(loaded.error);
        CHECK(false);
        return;
    }
    CHECK(loaded.sim->stateHash() == original.stateHash());
    CHECK(loaded.sim->settings().scenario == "Crashlanded");
    CHECK(saveSim(*loaded.sim) == bytes);
    const std::vector<Colonist> before = colonistsOf(original);
    const std::vector<Colonist> after = colonistsOf(*loaded.sim);
    if (!(before.size() == after.size() && before.size() == 6)) {
        CHECK(false);
        return;
    }
    for (size_t i = 0; i < before.size(); ++i) CHECK(colonistSame(before[i], after[i]));
}

TEST_CASE("Crashlanded: a scenario that does not exist is a setup error") {
    SimSettings settings = colonistStartSettings(1);
    settings.scenario = "Nowhere";
    Sim sim(settings, colonistTestDefs());
    CHECK_FALSE(sim.setupErrors().empty());
}

// ---- Backgrounds: categories, forced and banned traits, age, gear, rerolls ----

namespace {

std::vector<Colonist> colonistBgMany(const DefDatabase& defs, const std::string& category, uint64_t first, int count) {
    std::vector<Colonist> out;
    for (int i = 0; i < count; ++i) {
        Colonist c;
        std::string error;
        if (generateColonist(defs, first + static_cast<uint64_t>(i), false, c, error, category)) out.push_back(c);
    }
    return out;
}

bool colonistBgHas(const Colonist& c, const std::string& trait) {
    return std::any_of(c.traits.begin(), c.traits.end(), [&](const TraitRecord& t) { return t.def == trait; });
}

}  // namespace

TEST_CASE("colonists: raiders and settlers draw from their own backstories") {
    const auto defs = colonistTestDefs();
    std::set<std::string> settlerStories, raiderStories;
    for (const Colonist& c : colonistBgMany(*defs, "Colony", 1000, 600)) {
        settlerStories.insert(c.childhood);
        settlerStories.insert(c.adulthood);
    }
    for (const Colonist& c : colonistBgMany(*defs, "Raider", 1000, 600)) {
        raiderStories.insert(c.childhood);
        raiderStories.insert(c.adulthood);
    }
    for (const std::string& name : settlerStories) {
        const std::vector<std::string> cats = colonistTags(*defs->find("Backstories", name), "categories");
        CHECK((cats.empty() || std::find(cats.begin(), cats.end(), "Colony") != cats.end()));
    }
    for (const std::string& name : raiderStories) {
        const std::vector<std::string> cats = colonistTags(*defs->find("Backstories", name), "categories");
        CHECK((cats.empty() || std::find(cats.begin(), cats.end(), "Raider") != cats.end()));
    }
    CHECK(settlerStories.count("Adult_Warlord") == 0);
    CHECK(raiderStories.count("Adult_Warlord") == 1);
    CHECK(raiderStories.count("Adult_Teacher") == 0);
    CHECK(settlerStories.count("Adult_Teacher") == 1);
    // Plenty of variety: the data has dozens of backstories in each slot.
    CHECK(settlerStories.size() >= 50);
    CHECK(raiderStories.size() >= 35);
    // A category nobody fits still gets the backstories that fit all.
    CHECK_FALSE(colonistBgMany(*defs, "Nobody", 1, 5).empty());
}

TEST_CASE("colonists: backstories force and forbid traits") {
    const auto defs = colonistTestDefs();
    int gladiators = 0, gladiatorBrawlers = 0, soldiers = 0, doctors = 0;
    for (const Colonist& c : colonistBgMany(*defs, "Colony", 5000, 1500)) {
        if (c.adulthood == "Adult_Soldier") {
            ++soldiers;
            CHECK_FALSE(colonistBgHas(c, "Wimp"));
        }
        if (c.adulthood == "Adult_Doctor") {
            ++doctors;
            CHECK_FALSE(colonistBgHas(c, "Psychopath"));
            CHECK_FALSE(colonistBgHas(c, "Bloodlust"));
            CHECK_FALSE(colonistBgHas(c, "Abrasive"));
        }
        if (c.adulthood == "Adult_Gladiator") {
            ++gladiators;
            gladiatorBrawlers += colonistBgHas(c, "Brawler") ? 1 : 0;
        }
        if (c.childhood == "Child_Pampered") CHECK_FALSE(colonistBgHas(c, "Tough"));
    }
    CHECK(soldiers > 10);
    CHECK(doctors > 10);
    CHECK(gladiators > 10);
    // Forced at 50%, plus the ordinary chance of drawing Brawler.
    CHECK(gladiatorBrawlers > gladiators / 3);
    // A forced trait never breaks the trait rules: no conflicts, no repeats, at most three.
    for (const Colonist& c : colonistBgMany(*defs, "Raider", 9000, 800)) {
        std::set<std::string> names;
        for (const TraitRecord& t : c.traits) CHECK(names.insert(t.def).second);
        CHECK(c.traits.size() <= 3);
        if (colonistBgHas(c, "Brawler")) CHECK_FALSE(colonistBgHas(c, "ShootingAccuracy"));
        if (colonistBgHas(c, "Kind")) CHECK_FALSE(colonistBgHas(c, "Psychopath"));
        if (c.adulthood == "Adult_Warlord" || c.adulthood == "Adult_Slaver") CHECK_FALSE(colonistBgHas(c, "Kind"));
    }
}

TEST_CASE("colonists: skills come from the backstories and rise with age") {
    const auto defs = colonistTestDefs();
    double youngSum = 0, oldSum = 0;
    int youngCount = 0, oldCount = 0;
    int soldierShooting = 0, soldiers = 0, otherShooting = 0, others = 0;
    for (const Colonist& c : colonistBgMany(*defs, "Colony", 20000, 2500)) {
        double total = 0;
        for (const SkillRecord& s : c.skills) total += s.level;
        if (c.age <= 24) {
            youngSum += total;
            ++youngCount;
        } else if (c.age >= 50) {
            oldSum += total;
            ++oldCount;
        }
        const int shooting = c.skill("Shooting")->level;
        if (c.adulthood == "Adult_Soldier") {
            soldierShooting += shooting;
            ++soldiers;
        } else if (c.adulthood != "Adult_Sniper" && c.adulthood != "Adult_Hunter") {
            otherShooting += shooting;
            ++others;
        }
    }
    CHECK(youngCount > 50);
    if (!(youngCount > 50)) return;
    CHECK(oldCount > 50);
    if (!(oldCount > 50)) return;
    CHECK(oldSum / oldCount > youngSum / youngCount + 3.0);
    CHECK(soldiers > 10);
    if (!(soldiers > 10)) return;
    CHECK(static_cast<double>(soldierShooting) / soldiers > static_cast<double>(otherShooting) / others + 3.0);
}

TEST_CASE("Crashlanded: the kind of colonist sets what they land wearing") {
    const auto defs = colonistTestDefs();
    int dressed = 0, nudists = 0;
    for (uint64_t seed = 1; seed <= 8; ++seed) {
        Sim sim(colonistStartSettings(seed), defs);
        CHECK(sim.setupErrors().empty());
        sim.things().registry().view<const Colonist>().each([&](const Entity pawn, const Colonist& c) {
            const Gear* gear = sim.things().registry().try_get<Gear>(pawn);
            const size_t worn = gear == nullptr ? 0 : gear->apparel.size();
            if (colonistBgHas(c, "Nudist")) {
                ++nudists;
                CHECK(worn == 0);
            } else {
                ++dressed;
                CHECK(worn == 2);
                CHECK(gear->weapon == kNoEntity);
            }
        });
    }
    CHECK(dressed > 20);
    CHECK(nudists > 0);
}

namespace {

std::vector<Entity> colonistBgSlots(const Sim& sim) {
    std::vector<Entity> out;
    sim.things().registry().view<const Colonist>().each([&](const Entity pawn, const Colonist&) { out.push_back(pawn); });
    std::sort(out.begin(), out.end(), [](Entity a, Entity b) { return entt::to_entity(a) < entt::to_entity(b); });
    return out;
}

}  // namespace

TEST_CASE("Crashlanded: a starting colonist can be rerolled before the game starts") {
    const auto defs = colonistTestDefs();
    Sim sim(colonistStartSettings(42), defs);
    const std::vector<Entity> before = colonistBgSlots(sim);
    CHECK(before.size() == 6);
    if (!(before.size() == 6)) return;
    std::vector<Colonist> old;
    for (const Entity e : before) old.push_back(sim.things().registry().get<Colonist>(e));
    const auto wornBy = [&](const std::vector<Entity>& pawns) {
        size_t n = 0;
        for (const Entity e : pawns) {
            const Gear* gear = sim.things().registry().try_get<Gear>(e);
            n += gear == nullptr ? 0 : gear->apparel.size();
        }
        return n;
    };
    const size_t thingsBefore = sim.things().count() - wornBy(before);
    std::vector<CellPos> cellsBefore;
    for (const Entity e : before) cellsBefore.push_back(cellOf(sim.things().registry().get<Position>(e)));

    const uint64_t good = sim.submit(RerollColonistCommand{2});
    const uint64_t protagonist = sim.submit(RerollColonistCommand{0});
    const uint64_t outOfRange = sim.submit(RerollColonistCommand{6});
    sim.step();
    std::map<uint64_t, std::string> failed;
    for (const Event& e : sim.events()) {
        if (e.type == EventType::CommandFailed) failed[e.command] = e.message;
    }
    CHECK(failed.count(good) == 0);
    CHECK(failed.count(protagonist) == 0);
    CHECK(failed.count(outOfRange) == 1);

    // Two new colonists in the same places; the others are as they were. No clothes are left lying about.
    const std::vector<Entity> after = colonistBgSlots(sim);
    CHECK(after.size() == 6);
    if (!(after.size() == 6)) return;
    CHECK(sim.things().count() - wornBy(after) == thingsBefore);
    int changed = 0;
    for (size_t i = 0; i < after.size(); ++i) {
        const Colonist& c = sim.things().registry().get<Colonist>(after[i]);
        const bool same = colonistSame(c, old[i]);
        changed += same ? 0 : 1;
        CHECK(sim.things().registry().all_of<Worker>(after[i]));
        const Gear* gear = sim.things().registry().try_get<Gear>(after[i]);
        CHECK((gear == nullptr ? 0u : gear->apparel.size()) == (colonistBgHas(c, "Nudist") ? 0u : 2u));
    }
    CHECK(changed >= 2);
    int protagonists = 0;
    for (const Entity e : after) protagonists += sim.things().registry().get<Colonist>(e).protagonist ? 1 : 0;
    CHECK(protagonists == 1);
    CHECK(sim.things().registry().get<Colonist>(after[0]).protagonist);  // slot 0 is still the protagonist
    for (size_t i = 0; i < after.size(); ++i) {
        if (i != 0 && i != 2) CHECK(after[i] == before[i]);  // the others were not touched
    }

    // Same seed and same rerolls give the same colonists; a save and a replay keep the command.
    Sim twin(colonistStartSettings(42), defs);
    twin.submit(RerollColonistCommand{2});
    twin.submit(RerollColonistCommand{0});
    twin.submit(RerollColonistCommand{6});
    twin.step();
    CHECK(twin.stateHash() == sim.stateHash());
    const std::vector<uint8_t> bytes = saveSim(sim);
    SaveLoadResult loaded = loadSim(bytes, defs);
    CHECK(loaded.error.empty());
    CHECK(loaded.sim != nullptr);
    if (!(loaded.sim != nullptr)) return;
    CHECK(loaded.sim->stateHash() == sim.stateHash());
    const std::string json = writeReplay(recordReplay(sim));
    Replay replay;
    std::string error;
    CHECK(readReplay(json, replay, error));
    int rerolls = 0;
    for (const LoggedCommand& logged : replay.commands) rerolls += std::holds_alternative<RerollColonistCommand>(logged.command) ? 1 : 0;
    CHECK(rerolls == 3);

    // Once the game is running it is too late.
    sim.stepMany(5);
    const uint64_t late = sim.submit(RerollColonistCommand{1});
    sim.step();
    bool lateFailed = false;
    for (const Event& e : sim.events()) lateFailed = lateFailed || (e.type == EventType::CommandFailed && e.command == late);
    CHECK(lateFailed);
}
