#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fire.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Mood.h"
#include "ColonySim/Random.h"
#include "ColonySim/RoomStats.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Skills.h"
#include "ColonySim/Things.h"
#include "ColonySim/Traits.h"

#include <doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

namespace {

// The game's real data: trait numbers, thoughts and the health model are part of what is tested.
std::shared_ptr<const DefDatabase> traitFxDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

std::unique_ptr<Sim> traitFxSim() {
    return std::make_unique<Sim>(SimSettings{5, 64, 0, 0, true}, traitFxDefs());
}

Entity traitFxPawn(Sim& sim, CellPos cell, const std::string& trait = std::string(), int degree = 0) {
    const Entity pawn = sim.things().spawn("Human", cell);
    sim.jobs().addWorker(pawn);
    Colonist colonist;
    if (!trait.empty()) {
        colonist.traits.push_back(TraitRecord{trait, degree});
    }
    for (const char* skill : {"Shooting", "Melee", "Construction"}) {
        colonist.skills.push_back(SkillRecord{skill, 5, Passion::None, 0.0f, false, 0.0f});
    }
    sim.things().registry().emplace_or_replace<Colonist>(pawn, colonist);
    return pawn;
}

bool traitFxHas(Sim& sim, Entity pawn, const std::string& def) {
    const std::vector<ThoughtView> thoughts = sim.mood().thoughts(pawn, sim.time().seconds);
    return std::any_of(thoughts.begin(), thoughts.end(), [&](const ThoughtView& t) { return t.def == def; });
}

float traitFxThought(Sim& sim, Entity pawn, const std::string& def) {
    for (const ThoughtView& t : sim.mood().thoughts(pawn, sim.time().seconds)) {
        if (t.def == def) {
            return t.offset;
        }
    }
    return 0.0f;
}

float traitFxInjury(Sim& sim, Entity pawn) {
    HealthReport report;
    sim.healthReport(pawn, report);
    float total = 0.0f;
    for (const HealthReport::InjuryLine& line : report.injuries) {
        total += line.severity;
    }
    return total;
}

}  // namespace

TEST_CASE("Trait effects: the stats of each trait degree come from the defs") {
    auto sim = traitFxSim();
    const TraitBook book(sim->things().registry(), *traitFxDefs());
    const Entity plain = traitFxPawn(*sim, CellPos{10, 10, 0});
    const Entity industrious = traitFxPawn(*sim, CellPos{12, 10, 0}, "Industriousness", 2);
    const Entity lazy = traitFxPawn(*sim, CellPos{14, 10, 0}, "Industriousness", -1);
    const Entity tough = traitFxPawn(*sim, CellPos{16, 10, 0}, "Tough");
    const Entity jogger = traitFxPawn(*sim, CellPos{18, 10, 0}, "SpeedOffset", 2);
    const Entity careful = traitFxPawn(*sim, CellPos{20, 10, 0}, "ShootingAccuracy", 1);
    const Entity trigger = traitFxPawn(*sim, CellPos{22, 10, 0}, "ShootingAccuracy", -1);
    const Entity slow = traitFxPawn(*sim, CellPos{24, 10, 0}, "SlowLearner");
    const Entity memory = traitFxPawn(*sim, CellPos{26, 10, 0}, "GreatMemory");
    const Entity wimp = traitFxPawn(*sim, CellPos{28, 10, 0}, "Wimp");

    CHECK(book.of(plain).workSpeed == doctest::Approx(1.0f));
    CHECK(book.of(industrious).workSpeed == doctest::Approx(1.35f));
    CHECK(book.of(lazy).workSpeed == doctest::Approx(0.8f));
    CHECK(book.of(tough).damageFactor == doctest::Approx(0.5f));
    CHECK(book.of(jogger).moveSpeedStep == doctest::Approx(0.013333f));
    CHECK(book.of(careful).shootingSkill == doctest::Approx(5.0f));
    CHECK(book.of(careful).aimDelayFactor == doctest::Approx(1.25f));
    CHECK(book.of(trigger).shootingSkill == doctest::Approx(-5.0f));
    CHECK(book.of(trigger).aimDelayFactor == doctest::Approx(0.5f));
    CHECK(book.of(slow).learningFactor == doctest::Approx(0.25f));
    CHECK(book.of(memory).skillLossFactor == doctest::Approx(0.5f));
    CHECK(book.of(wimp).painShockOffset == doctest::Approx(-0.5f));
    CHECK(book.has(tough, "Tough"));
    CHECK(!book.has(plain, "Tough"));
}

TEST_CASE("Trait effects: a tough colonist takes half the damage") {
    auto sim = traitFxSim();
    const Entity plain = traitFxPawn(*sim, CellPos{10, 10, 0});
    const Entity tough = traitFxPawn(*sim, CellPos{14, 10, 0}, "Tough");
    Rng rng(7);
    const DefId blunt = sim->health().damageTypeId("Blunt");
    for (const Entity pawn : {plain, tough}) {
        CHECK(sim->health().applyDamage(pawn, blunt, 4.0f, 0, rng));
    }
    sim->stepMany(2);
    const float plainHurt = traitFxInjury(*sim, plain);
    CHECK(plainHurt > 0.0f);
    CHECK(traitFxInjury(*sim, tough) == doctest::Approx(plainHurt * 0.5f).epsilon(0.05));
}

TEST_CASE("Trait effects: a wimp is downed by less pain") {
    auto sim = traitFxSim();
    const Entity plain = traitFxPawn(*sim, CellPos{10, 10, 0});
    const Entity wimp = traitFxPawn(*sim, CellPos{14, 10, 0}, "Wimp");
    const DefId blunt = sim->health().damageTypeId("Blunt");
    // The same hits on both, until the plain colonist is in pain above 0.4 (a wimp is downed above 0.3, a plain one above 0.8).
    Rng plainRng(7);
    int hits = 0;
    while (sim->things().registry().get<Health>(plain).pain < 0.4f && hits < 200) {
        CHECK(sim->health().applyDamage(plain, blunt, 2.0f, -1, plainRng));
        sim->step();
        ++hits;
    }
    Rng wimpRng(7);
    for (int i = 0; i < hits; ++i) {
        CHECK(sim->health().applyDamage(wimp, blunt, 2.0f, -1, wimpRng));
    }
    sim->stepMany(2);
    CHECK(hits > 0);
    CHECK(!sim->health().incapacitated(plain));
    CHECK(sim->health().incapacitated(wimp));
}

TEST_CASE("Trait effects: fast learners learn faster and slow ones slower") {
    auto sim = traitFxSim();
    const Entity plain = traitFxPawn(*sim, CellPos{10, 10, 0});
    const Entity fast = traitFxPawn(*sim, CellPos{12, 10, 0}, "FastLearner");
    const Entity slow = traitFxPawn(*sim, CellPos{14, 10, 0}, "SlowLearner");
    for (const Entity pawn : {plain, fast, slow}) {
        sim->jobs().addSkillXp(pawn, "Construction", 100.0);
    }
    const auto xp = [&](Entity pawn) { return sim->things().registry().get<Colonist>(pawn).skill("Construction")->xp; };
    CHECK(xp(plain) > 0.0f);
    CHECK(xp(fast) == doctest::Approx(xp(plain) * 1.75f).epsilon(0.01));
    CHECK(xp(slow) == doctest::Approx(xp(plain) * 0.25f).epsilon(0.01));
}

TEST_CASE("Trait effects: a great memory halves the loss of a high skill") {
    SkillRules rules;
    rules.xpToNextLevel.assign(21, 1000.0);
    rules.decayXpPerDay.assign(21, 0.0);
    rules.decayXpPerDay[12] = 200.0;
    SkillRecord a{"Shooting", 12, Passion::None, 500.0f, false, 0.0f};
    SkillRecord b = a;
    skillDayPassed(rules, a);
    skillDayPassed(rules, b, 0.5f);
    CHECK(a.xp == doctest::Approx(300.0f));
    CHECK(b.xp == doctest::Approx(400.0f));
}

TEST_CASE("Trait effects: shooting traits move the aim skill") {
    auto sim = traitFxSim();
    const Entity plain = traitFxPawn(*sim, CellPos{10, 10, 0});
    const Entity careful = traitFxPawn(*sim, CellPos{12, 10, 0}, "ShootingAccuracy", 1);
    const Entity trigger = traitFxPawn(*sim, CellPos{14, 10, 0}, "ShootingAccuracy", -1);
    CHECK(sim->combat().shootingSkill(plain) == 5);
    CHECK(sim->combat().shootingSkill(careful) == 10);
    CHECK(sim->combat().shootingSkill(trigger) == 0);
}

TEST_CASE("Trait effects: a brawler minds holding a gun") {
    auto sim = traitFxSim();
    const Entity brawler = traitFxPawn(*sim, CellPos{10, 10, 0}, "Brawler");
    const Entity plain = traitFxPawn(*sim, CellPos{14, 10, 0});
    CHECK(!traitFxHas(*sim, brawler, "BrawlerRangedWeapon"));
    for (const Entity pawn : {brawler, plain}) {
        sim->combat().equip(pawn, sim->things().make(sim->defs().idOf("Things", "Gun_Revolver")));
    }
    CHECK(traitFxThought(*sim, brawler, "BrawlerRangedWeapon") == doctest::Approx(-10.0f));
    CHECK(!traitFxHas(*sim, plain, "BrawlerRangedWeapon"));
}

TEST_CASE("Trait effects: a pyromaniac enjoys fires nearby, a plain colonist does not") {
    auto sim = traitFxSim();
    const Entity pyro = traitFxPawn(*sim, CellPos{10, 10, 0}, "Pyromaniac");
    const Entity plain = traitFxPawn(*sim, CellPos{12, 10, 0});
    CHECK(!traitFxHas(*sim, pyro, "PyromaniacBeautifulFire"));
    CHECK(sim->fire().ignite(CellPos{14, 10, 0}, 0.0f, true));
    CHECK(traitFxThought(*sim, pyro, "PyromaniacBeautifulFire") == doctest::Approx(4.0f));
    CHECK(sim->fire().ignite(CellPos{14, 11, 0}, 0.0f, true));
    CHECK(traitFxThought(*sim, pyro, "PyromaniacBeautifulFire") == doctest::Approx(8.0f));
    CHECK(!traitFxHas(*sim, plain, "PyromaniacBeautifulFire"));
}

TEST_CASE("Trait effects: an ascetic does not care about a table or the quality of a meal") {
    auto sim = traitFxSim();
    const Entity ascetic = traitFxPawn(*sim, CellPos{10, 10, 0}, "Ascetic");
    const Entity plain = traitFxPawn(*sim, CellPos{14, 10, 0});
    sim->step();  // the moods start
    for (const Entity pawn : {ascetic, plain}) {
        sim->mood().remember(pawn, "AteWithoutTable", sim->time().seconds);
        sim->mood().remember(pawn, "AteLavishMeal", sim->time().seconds);
    }
    CHECK(!traitFxHas(*sim, ascetic, "AteWithoutTable"));
    CHECK(!traitFxHas(*sim, ascetic, "AteLavishMeal"));
    CHECK(traitFxHas(*sim, plain, "AteWithoutTable"));
    CHECK(traitFxHas(*sim, plain, "AteLavishMeal"));
}

TEST_CASE("Trait effects: the killer is named in the death, and a bloodlusty one is pleased") {
    auto sim = traitFxSim();
    const Entity killer = traitFxPawn(*sim, CellPos{10, 10, 0}, "Bloodlust");
    const Entity calm = traitFxPawn(*sim, CellPos{12, 10, 0});
    sim->step();  // the moods start
    Rng rng(3);
    const DefId bullet = sim->health().damageTypeId("Bullet");
    std::vector<Entity> victims;
    for (int i = 0; i < 4; ++i) {
        victims.push_back(traitFxPawn(*sim, CellPos{20 + 3 * i, 20, 0}));
    }
    // Hunger kills nobody's killer.
    // (Damage dealt outside a step puts its events in the list the next step clears, so the mood system is handed the list here.)
    CHECK(sim->health().kill(victims[3], DeathCause::Starvation));
    int deaths = 0;
    for (const Event& e : sim->events()) {
        if (e.type == EventType::PawnDied) {
            ++deaths;
            CHECK(e.thing == victims[3]);
            CHECK(e.target == kNoEntity);
        }
    }
    CHECK(deaths == 1);
    CHECK(!traitFxHas(*sim, killer, "KilledBloodlust1"));

    const float expected[] = {12.0f, 24.0f, 37.0f};
    for (int i = 0; i < 2; ++i) {
        // A bystander's shot does not count for the killer, who finishes the victim off.
        CHECK(sim->health().applyDamage(victims[static_cast<size_t>(i)], bullet, 500.0f, 0, rng, i == 0 ? calm : killer));
        bool died = false;
        for (const Event& e : sim->events()) {
            if (e.type == EventType::PawnDied && e.thing == victims[static_cast<size_t>(i)]) {
                died = true;
                CHECK(e.target == (i == 0 ? calm : killer));
            }
        }
        CHECK(died);
        sim->mood().noteKills(sim->time().seconds, sim->events());
        sim->step();  // clears the list
    }
    CHECK(traitFxThought(*sim, killer, "KilledBloodlust1") == doctest::Approx(expected[0]));
    CHECK(!traitFxHas(*sim, calm, "KilledBloodlust1"));
    CHECK(sim->health().applyDamage(victims[2], bullet, 500.0f, 0, rng, killer));
    sim->mood().noteKills(sim->time().seconds, sim->events());
    sim->step();
    CHECK(!traitFxHas(*sim, killer, "KilledBloodlust1"));
    CHECK(traitFxThought(*sim, killer, "KilledBloodlust2") == doctest::Approx(expected[1]));
}

TEST_CASE("Trait effects: a jealous colonist minds another's better bedroom") {
    auto sim = traitFxSim();
    const Entity jealous = traitFxPawn(*sim, CellPos{10, 10, 0}, "Jealous");
    const Entity plain = traitFxPawn(*sim, CellPos{14, 10, 0});
    sim->step();
    CHECK(!traitFxHas(*sim, jealous, "JealousBedroom"));  // nobody has a bedroom
    const auto room = [&](int x0) {
        for (int y = 30; y <= 36; ++y) {
            for (int x = x0; x <= x0 + 6; ++x) {
                if (x == x0 || x == x0 + 6 || y == 30 || y == 36) {
                    sim->things().spawn("Wall", CellPos{x, y, 0});
                }
            }
        }
        return sim->things().spawn("Bed", CellPos{x0 + 3, 33, 0});
    };
    std::string error;
    const Entity first = room(30);
    CHECK(sim->jobs().assignBed(first, plain, error));
    sim->rooms().update();
    sim->roomStats().refresh();
    CHECK(traitFxThought(*sim, jealous, "JealousBedroom") == doctest::Approx(-8.0f));
    CHECK(!traitFxHas(*sim, plain, "JealousBedroom"));
    // An equal room of its own is enough.
    const Entity second = room(40);
    CHECK(sim->jobs().assignBed(second, jealous, error));
    sim->rooms().update();
    sim->roomStats().refresh();
    CHECK(!traitFxHas(*sim, jealous, "JealousBedroom"));
}
