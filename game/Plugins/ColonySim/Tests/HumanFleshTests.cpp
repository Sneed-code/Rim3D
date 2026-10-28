// Moods about butchering people, eating human flesh and wearing human leather (docs/design/hunting.md).
#include "ColonySim/Apparel.h"
#include "ColonySim/Bills.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Mood.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Spoilage.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define FLESH_REQUIRE(...)                  \
    do {                                     \
        const bool fleshOk = (__VA_ARGS__); \
        CHECK(fleshOk);                      \
        if (!fleshOk) {                      \
            return;                          \
        }                                    \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> fleshDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

struct FleshFixture {
    Sim sim;

    FleshFixture() : sim(settings(), fleshDefs()) {
        for (int y = 0; y < sim.map().height(); ++y) {
            for (int x = 0; x < sim.map().width(); ++x) {
                sim.plants().remove(CellPos{x, y, 0});
            }
        }
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
    DefId def(const char* name) const { return sim.defs().idOf("Things", name); }
    Entity colonist(CellPos cell, const char* trait = nullptr) {
        const Entity pawn = sim.things().spawn("Human", cell);
        Colonist c;
        c.skills.push_back(SkillRecord{"Cooking", 8, Passion::None, 0.0f, false});
        if (trait != nullptr) {
            c.traits.push_back(TraitRecord{trait, 0});
        }
        reg().emplace<Colonist>(pawn, c);
        sim.jobs().addWorker(pawn);
        return pawn;
    }
    bool remembers(Entity pawn, const char* thought) {
        const Mood* mood = reg().try_get<Mood>(pawn);
        return mood != nullptr && std::any_of(mood->memories.begin(), mood->memories.end(), [&](const Memory& m) { return m.def == thought; });
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
    // Butchers a dead person at a table with `butcher` standing by; true when the corpse is gone.
    bool butcherPerson(Entity butcher) {
        sim.submit(SpawnThingCommand{"ButcherTable", CellPos{12, 10, 0}, 1});
        sim.step();
        Entity table = kNoEntity;
        for (const Event& e : sim.events()) {
            if (e.type == EventType::ThingSpawned && sim.defs().get(e.def).defName() == "ButcherTable") {
                table = e.thing;
            }
        }
        const Entity victim = sim.things().spawn("Human", CellPos{18, 12, 0});
        if (table == kNoEntity || victim == kNoEntity || butcher == kNoEntity) {
            return false;
        }
        sim.health().kill(victim, DeathCause::BloodLoss);
        sim.submit(AddBillCommand{table, "ButcherCorpse", BillMode::Count, 1});
        // The bill leaves people out by default: let them in.
        sim.submit(SetBillOptionsCommand{table, 1, 0, 20, 0.0f, 0, 0, 0, {}});
        for (int i = 0; i < 6000 && reg().valid(victim); ++i) {
            sim.step();
        }
        sim.stepMany(3);
        return !reg().valid(victim);
    }
    // Hungry, with `item` on the floor beside the pawn: runs until it has eaten.
    bool eat(Entity pawn, Entity item) {
        (void)item;
        reg().get<Needs>(pawn).food = 0.1f;
        for (int i = 0; i < 6000; ++i) {
            sim.step();
            for (const Event& e : sim.events()) {
                if (e.type == EventType::PawnAte && e.thing == pawn) {
                    sim.step();
                    return true;
                }
            }
        }
        return false;
    }
    std::vector<ThoughtView> thoughts_;
};

}  // namespace

TEST_CASE("Human flesh: butchering a person is a bad memory for the butcher, unless a cannibal or psychopath") {
    for (const char* trait : {static_cast<const char*>(nullptr), "Cannibal", "Psychopath"}) {
        FleshFixture f;
        const Entity butcher = f.colonist(CellPos{10, 10, 0}, trait);
        FLESH_REQUIRE(f.butcherPerson(butcher));
        CHECK(f.sim.jobs().bills().stockOf(f.def("Meat_Human")) > 100);
        if (trait == nullptr) {
            CHECK(f.remembers(butcher, "ButcheredHumanlikeCorpse"));
            const ThoughtView* t = f.thought(butcher, "ButcheredHumanlikeCorpse");
            FLESH_REQUIRE(t != nullptr);
            CHECK(t->offset == doctest::Approx(-15.0f));
        } else {
            CHECK_FALSE(f.remembers(butcher, "ButcheredHumanlikeCorpse"));
        }
    }
}

TEST_CASE("Human flesh: cutting up an animal gives no such memory") {
    FleshFixture f;
    const Entity butcher = f.colonist(CellPos{10, 10, 0});
    f.sim.submit(SpawnThingCommand{"ButcherTable", CellPos{12, 10, 0}, 1});
    f.sim.step();
    Entity table = kNoEntity;
    for (const Event& e : f.sim.events()) {
        if (e.type == EventType::ThingSpawned && f.sim.defs().get(e.def).defName() == "ButcherTable") {
            table = e.thing;
        }
    }
    const Entity deer = f.sim.things().spawn("Deer", CellPos{18, 12, 0});
    FLESH_REQUIRE(table != kNoEntity);
    FLESH_REQUIRE(deer != kNoEntity);
    f.sim.stepMany(5);
    f.sim.health().kill(deer, DeathCause::BloodLoss);
    f.sim.submit(AddBillCommand{table, "ButcherCorpse", BillMode::Count, 1});
    for (int i = 0; i < 6000 && f.reg().valid(deer); ++i) {
        f.sim.step();
    }
    f.sim.stepMany(3);
    CHECK_FALSE(f.reg().valid(deer));
    CHECK_FALSE(f.remembers(butcher, "ButcheredHumanlikeCorpse"));
}

TEST_CASE("Human flesh: eating human meat is -20 for most, +20 for a cannibal and nothing for a psychopath") {
    struct Row {
        const char* trait;
        const char* thought;
    };
    for (const Row& row : {Row{nullptr, "AteHumanlikeMeat"}, Row{"Cannibal", "AteHumanlikeMeatCannibal"}, Row{"Psychopath", nullptr}}) {
        FleshFixture f;
        const Entity pawn = f.colonist(CellPos{10, 10, 0}, row.trait);
        const Entity meat = f.sim.things().spawn("Meat_Human", CellPos{11, 10, 0}, 20);
        FLESH_REQUIRE(f.eat(pawn, meat));
        for (const char* name : {"AteHumanlikeMeat", "AteHumanlikeMeatCannibal"}) {
            const bool expected = row.thought != nullptr && std::string(name) == row.thought;
            CHECK(f.remembers(pawn, name) == expected);
        }
        if (row.thought != nullptr) {
            const ThoughtView* t = f.thought(pawn, row.thought);
            FLESH_REQUIRE(t != nullptr);
            CHECK(t->offset == doctest::Approx(row.trait == nullptr ? -20.0f : 20.0f));
        }
    }
}

TEST_CASE("Human flesh: a meal cooked from human meat gives the milder ingredient memory; other meals none") {
    struct Row {
        const char* trait;
        const char* ingredient;
        const char* thought;
    };
    for (const Row& row : {Row{nullptr, "Meat_Human", "AteHumanlikeMeatAsIngredient"},
                           Row{"Cannibal", "Meat_Human", "AteHumanlikeMeatAsIngredientCannibal"},
                           Row{"Psychopath", "Meat_Human", nullptr}, Row{nullptr, "Meat_Deer", nullptr}}) {
        FleshFixture f;
        const Entity pawn = f.colonist(CellPos{10, 10, 0}, row.trait);
        const Entity meal = f.sim.things().spawn("MealSimple", CellPos{11, 10, 0});
        const std::vector<BillUsed> used{BillUsed{f.def(row.ingredient), 5}};
        f.sim.spoilage().madeBy(meal, used, pawn, CellPos{11, 10, 0});
        FLESH_REQUIRE(f.eat(pawn, meal));
        for (const char* name : {"AteHumanlikeMeatAsIngredient", "AteHumanlikeMeatAsIngredientCannibal", "AteHumanlikeMeat"}) {
            const bool expected = row.thought != nullptr && std::string(name) == row.thought;
            CHECK(f.remembers(pawn, name) == expected);
        }
        if (row.thought != nullptr) {
            CHECK(f.thought(pawn, row.thought)->offset == doctest::Approx(row.trait == nullptr ? -10.0f : 10.0f));
        }
    }
}

TEST_CASE("Human flesh: wearing human leather is -4, +4 for a cannibal or psychopath, and other leather is nothing") {
    FleshFixture f;
    const Entity plain = f.colonist(CellPos{10, 10, 0});
    const Entity cannibal = f.colonist(CellPos{10, 12, 0}, "Cannibal");
    const Entity psycho = f.colonist(CellPos{10, 14, 0}, "Psychopath");
    const auto wear = [&](Entity pawn, const char* stuff) {
        const Position& at = f.reg().get<Position>(pawn);
        const Entity item = f.sim.apparel().make(f.def("Apparel_Duster"), f.def(stuff), -1, cellOf(at));
        std::string why;
        CHECK(item != kNoEntity);
        CHECK(f.sim.apparel().putOn(pawn, item, why));
    };
    wear(plain, "Leather_Plain");
    CHECK(f.thought(plain, "HumanLeatherApparelSad") == nullptr);
    CHECK(f.thought(plain, "HumanLeatherApparelHappy") == nullptr);
    wear(plain, "Leather_Human");  // a second coat replaces the first
    wear(cannibal, "Leather_Human");
    wear(psycho, "Leather_Human");
    FLESH_REQUIRE(f.thought(plain, "HumanLeatherApparelSad") != nullptr);
    CHECK(f.thought(plain, "HumanLeatherApparelSad")->offset == doctest::Approx(-4.0f));
    CHECK(f.thought(plain, "HumanLeatherApparelHappy") == nullptr);
    for (const Entity pawn : {cannibal, psycho}) {
        CHECK(f.thought(pawn, "HumanLeatherApparelSad") == nullptr);
        FLESH_REQUIRE(f.thought(pawn, "HumanLeatherApparelHappy") != nullptr);
        CHECK(f.thought(pawn, "HumanLeatherApparelHappy")->offset == doctest::Approx(4.0f));
    }
}

TEST_CASE("Human flesh: the rest of the colony knows about a butchered person, unless cannibal or psychopath") {
    FleshFixture f;
    const Entity butcher = f.colonist(CellPos{10, 10, 0});
    const Entity friendly = f.colonist(CellPos{8, 8, 0});
    const Entity cannibal = f.colonist(CellPos{8, 9, 0}, "Cannibal");
    const Entity cold = f.colonist(CellPos{8, 10, 0}, "Psychopath");
    FLESH_REQUIRE(f.butcherPerson(butcher));
    CHECK(f.remembers(friendly, "KnowButcheredHumanlikeCorpse"));
    CHECK_FALSE(f.remembers(butcher, "KnowButcheredHumanlikeCorpse"));
    CHECK_FALSE(f.remembers(cannibal, "KnowButcheredHumanlikeCorpse"));
    CHECK_FALSE(f.remembers(cold, "KnowButcheredHumanlikeCorpse"));
    const ThoughtView* t = f.thought(friendly, "KnowButcheredHumanlikeCorpse");
    FLESH_REQUIRE(t != nullptr);
    CHECK(t->offset == doctest::Approx(-5.0f));
}

TEST_CASE("Mood: every other living colonist knows when a colonist dies; a dead animal is no one's loss") {
    FleshFixture f;
    const Entity dead = f.colonist(CellPos{10, 10, 0});
    const Entity friendly = f.colonist(CellPos{8, 8, 0});
    const Entity cold = f.colonist(CellPos{8, 10, 0}, "Psychopath");
    f.sim.stepMany(2);
    f.sim.submit(DamagePawnCommand{dead, "Bullet", 500.0f, 0});
    f.sim.stepMany(3);
    CHECK(f.remembers(friendly, "KnowColonistDied"));
    CHECK_FALSE(f.remembers(cold, "KnowColonistDied"));
    CHECK_FALSE(f.remembers(dead, "KnowColonistDied"));
}

TEST_CASE("Mood: an animal's death is not a colonist's death") {
    FleshFixture f;
    const Entity friendly = f.colonist(CellPos{8, 8, 0});
    const Entity deer = f.sim.things().spawn("Deer", CellPos{18, 12, 0});
    f.sim.stepMany(2);
    f.sim.submit(DamagePawnCommand{deer, "Bullet", 500.0f, 0});
    f.sim.stepMany(3);
    CHECK_FALSE(f.remembers(friendly, "KnowColonistDied"));
}
