#include "ColonySim/Colonist.h"
#include "ColonySim/BodyMotion.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Melee.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace colony;

// These tests check docs/design/melee.md (task F8).

#define MELEE_REQUIRE(...)     \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> meleeTestDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        namespace fs = std::filesystem;
        const fs::path root = fs::path(COLONY_DATA_DIR);
        std::vector<DefSource> sources;
        std::vector<fs::path> files;
        for (const auto& entry : fs::recursive_directory_iterator(root / "defs")) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());
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
    }();
    return defs;
}

std::unique_ptr<Sim> meleeTestSim(uint64_t seed = 11, int size = 64) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = size;
    settings.flatMap = true;
    return std::make_unique<Sim>(settings, meleeTestDefs());
}

Entity meleeTestPawn(Sim& sim, float x, float y, const char* weapon = nullptr) {
    const Entity pawn = sim.things().spawn("Human", CellPos{static_cast<int>(x), static_cast<int>(y), 0});
    Position& pos = sim.things().registry().get<Position>(pawn);
    pos.x = x;
    pos.y = y;
    if (weapon != nullptr) {
        sim.combat().equip(pawn, sim.things().make(sim.defs().idOf("Things", weapon)));
    }
    return pawn;
}

// Puts a pawn back on its spot, standing still: blows that stagger throw the body back (docs/design/body-motion.md).
void meleeTestPlace(Sim& sim, Entity pawn, float x, float y) {
    sim.combat().motion().stop(pawn);
    Position& pos = sim.things().registry().get<Position>(pawn);
    pos.x = x;
    pos.y = y;
}

void meleeTestSkill(Sim& sim, Entity pawn, int level, const char* trait = nullptr) {
    Colonist& c = sim.things().registry().get_or_emplace<Colonist>(pawn);
    c.skills.clear();
    c.skills.push_back(SkillRecord{"Melee", level});
    c.traits.clear();
    if (trait != nullptr) {
        c.traits.push_back(TraitRecord{trait, 0});
    }
}

void meleeTestHeal(Sim& sim, Entity pawn) {
    sim.things().registry().replace<Health>(pawn, Health{});
    sim.health().refresh(pawn);
}

void meleeTestFace(Sim& sim, Entity pawn, float fx, float fy) {
    Posture& p = sim.things().registry().get<Posture>(pawn);
    p.facingX = fx;
    p.facingY = fy;
}

int meleeTestCount(const Sim& sim, EventType type, const char* message = nullptr) {
    return static_cast<int>(std::count_if(sim.events().begin(), sim.events().end(), [&](const Event& e) {
        return e.type == type && (message == nullptr || e.message == message);
    }));
}

bool meleeTestFailed(const Sim& sim) {
    return meleeTestCount(sim, EventType::CommandFailed) > 0;
}

float meleeTestDistance(Sim& sim, Entity a, Entity b) {
    const Position& p = sim.things().registry().get<Position>(a);
    const Position& q = sim.things().registry().get<Position>(b);
    return std::sqrt((p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y));
}

// Swings `swings` times at a standing man (no dodging, no blocking) and counts the hits.
double meleeTestHitRate(int skill, int swings, uint64_t seed) {
    auto sim = meleeTestSim(seed);
    const Entity attacker = meleeTestPawn(*sim, 30.5f, 30.5f, "Melee_Knife");
    const Entity target = meleeTestPawn(*sim, 31.5f, 30.5f);
    meleeTestSkill(*sim, attacker, skill);
    meleeTestFace(*sim, target, -1.0f, 0.0f);  // facing the attacker, as the hit chance table assumes
    int hits = 0;
    for (int i = 0; i < swings; ++i) {
        meleeTestPlace(*sim, target, 31.5f, 30.5f);  // a staggering blow throws him back (body-motion.md): he steps up again
        sim->submit(MeleeCommand{attacker, target, MeleeMode::Attack});
        for (int s = 0; s < 80; ++s) {
            sim->step();
            hits += meleeTestCount(*sim, EventType::MeleeHit, "hit") + meleeTestCount(*sim, EventType::MeleeHit, "deflected");
            if (sim->melee().ready(attacker)) {
                break;
            }
        }
        meleeTestHeal(*sim, target);
        meleeTestFace(*sim, target, -1.0f, 0.0f);
    }
    return static_cast<double>(hits) / swings;
}

}  // namespace

TEST_CASE("Melee: weapons and fists come from the data, and only melee fighters charge") {
    auto sim = meleeTestSim();
    const Entity knifer = meleeTestPawn(*sim, 10.5f, 10.5f, "Melee_Knife");
    const Entity spearman = meleeTestPawn(*sim, 12.5f, 10.5f, "Melee_Spear");
    const Entity rifleman = meleeTestPawn(*sim, 14.5f, 10.5f, "Gun_BoltActionRifle");
    const Entity bare = meleeTestPawn(*sim, 16.5f, 10.5f);
    const Entity brawler = meleeTestPawn(*sim, 18.5f, 10.5f);
    meleeTestSkill(*sim, brawler, 6, "Brawler");

    const entt::registry& reg = sim->things().registry();
    MELEE_REQUIRE(reg.get<Gear>(knifer).weapon != kNoEntity);  // a melee weapon goes in the hands like a gun
    CHECK(sim->combat().isMeleeWeapon(reg.get<Thing>(reg.get<Gear>(knifer).weapon).def));
    CHECK(sim->melee().reach(knifer) == doctest::Approx(1.0f));
    CHECK(sim->melee().reach(spearman) == doctest::Approx(1.9f));
    CHECK(sim->melee().reach(rifleman) == doctest::Approx(0.85f));  // a gun: fists
    CHECK(sim->melee().reach(bare) == doctest::Approx(0.85f));

    CHECK(sim->melee().charges(knifer));
    CHECK(sim->melee().charges(spearman));
    CHECK_FALSE(sim->melee().charges(rifleman));
    CHECK_FALSE(sim->melee().charges(bare));
    CHECK(sim->melee().charges(brawler));  // the trait says so

    // Only a melee weapon blocks.
    sim->submit(MeleeCommand{bare, kNoEntity, MeleeMode::Block});
    sim->step();
    CHECK(meleeTestFailed(*sim));
}

TEST_CASE("Melee: skill sets swing speed, damage, the hand's wobble and reaction time") {
    // sneed (docs/design/real-time-combat.md): skill 0 swings 20% slower and weaker, skill 20 20% faster and harder.
    auto sim = meleeTestSim();
    const Entity pawn = meleeTestPawn(*sim, 10.5f, 10.5f);
    const auto at = [&](int skill, const char* trait, float speed, float damage, float wobble, float reaction) {
        meleeTestSkill(*sim, pawn, skill, trait);
        CHECK(sim->melee().swingSpeed(pawn) == doctest::Approx(speed));
        CHECK(sim->melee().swingDamage(pawn) == doctest::Approx(damage));
        CHECK(sim->melee().aimWobble(pawn) == doctest::Approx(wobble));
        CHECK(sim->melee().reactionSteps(pawn) == doctest::Approx(reaction));
    };
    at(0, nullptr, 0.8f, 0.8f, 0.12f, 14.0f);
    at(10, nullptr, 1.0f, 1.0f, 0.075f, 9.5f);
    at(20, nullptr, 1.2f, 1.2f, 0.03f, 5.0f);
    at(20, "Nimble", 1.2f, 1.2f, 0.03f, 2.0f);  // Nimble reads swings quicker
    // A pawn with no skills counts as level 4.
    const Entity raider = meleeTestPawn(*sim, 12.5f, 10.5f);
    CHECK(sim->melee().swingSpeed(raider) == doctest::Approx(0.88f));

    // The swing itself: fists' 60-step cooldown, 30% wind-up.
    const auto windupAt = [&](int skill) {
        auto s2 = meleeTestSim();
        const Entity a = meleeTestPawn(*s2, 30.5f, 30.5f);
        const Entity t = meleeTestPawn(*s2, 31.3f, 30.5f);
        meleeTestSkill(*s2, a, skill);
        s2->submit(MeleeCommand{a, t, MeleeMode::Attack});
        s2->step();
        for (const Event& e : s2->events()) {
            if (e.type == EventType::MeleeSwing) {
                return static_cast<int>(e.amount);
            }
        }
        return -1;
    };
    CHECK(windupAt(0) == 23);   // 18 / 0.8
    CHECK(windupAt(10) == 18);
    CHECK(windupAt(20) == 15);  // 18 / 1.2
}

TEST_CASE("Melee: a swing winds up, hits at its moment and recovers") {
    auto sim = meleeTestSim();
    const Entity attacker = meleeTestPawn(*sim, 30.5f, 30.5f);  // fists: 60 steps a swing
    const Entity target = meleeTestPawn(*sim, 31.3f, 30.5f);
    const Entity far = meleeTestPawn(*sim, 35.5f, 30.5f);
    meleeTestSkill(*sim, attacker, 10);  // RimWorld's pace
    meleeTestFace(*sim, target, -1.0f, 0.0f);

    sim->submit(MeleeCommand{attacker, far, MeleeMode::Attack});
    sim->step();
    CHECK(meleeTestFailed(*sim));  // out of reach

    sim->submit(MeleeCommand{attacker, target, MeleeMode::Attack});
    sim->step();
    MELEE_REQUIRE(meleeTestCount(*sim, EventType::MeleeSwing) == 1);
    const auto swing = std::find_if(sim->events().begin(), sim->events().end(),
                                    [](const Event& e) { return e.type == EventType::MeleeSwing; });
    const int windup = static_cast<int>(swing->amount);
    CHECK(windup == 18);  // 30% of the fists' 60-step cooldown
    CHECK(swing->target == target);
    CHECK((swing->message == "left fist" || swing->message == "right fist" || swing->message == "head"));
    CHECK(sim->melee().winding(attacker));
    Snapshot snap;
    sim->writeSnapshot(snap);
    CHECK(snap.find(attacker)->meleePhase == MeleePhase::Windup);
    CHECK(snap.find(attacker)->meleeTarget == target);

    sim->submit(MeleeCommand{attacker, target, MeleeMode::Attack});  // still swinging: refused
    sim->step();
    CHECK(meleeTestFailed(*sim));

    int hitAt = -1;
    for (int s = 3; s <= windup + 2; ++s) {
        sim->step();
        if (meleeTestCount(*sim, EventType::MeleeHit) > 0) {
            hitAt = s;
            break;
        }
    }
    CHECK(hitAt == windup);  // the hit moment: the swing's first step counts as 1
    CHECK_FALSE(sim->melee().winding(attacker));
    CHECK_FALSE(sim->melee().ready(attacker));  // recovering
    int steps = 0;
    while (!sim->melee().ready(attacker) && steps < 100) {
        sim->step();
        ++steps;
    }
    CHECK(steps + windup == 60);  // the whole swing is the tool's cooldown: RimWorld's damage per second
}

TEST_CASE("Melee: reach is real: a target that steps back once the strike is fixed is missed") {
    auto sim = meleeTestSim();
    const Entity attacker = meleeTestPawn(*sim, 30.5f, 30.5f, "Melee_Knife");
    const Entity target = meleeTestPawn(*sim, 31.4f, 30.5f);
    meleeTestSkill(*sim, attacker, 20);
    sim->submit(MeleeCommand{attacker, target, MeleeMode::Attack});
    sim->step();
    MELEE_REQUIRE(sim->melee().winding(attacker));
    // Once the line is fixed, the target moves 1.5 m away.
    while (!sim->things().registry().get<Melee>(attacker).committed) {
        sim->step();
    }
    sim->things().registry().get<Position>(target).x += 1.5f;
    bool missed = false;
    for (int s = 0; s < 10; ++s) {
        sim->step();
        missed = missed || meleeTestCount(*sim, EventType::MeleeHit, "missed") > 0;
    }
    CHECK(missed);
    HealthReport report;
    sim->health().report(target, report);
    CHECK(report.injuries.empty());
}

TEST_CASE("Melee: no dice: a swing at a man who stands still lands at any skill") {
    // docs/decisions/0010-real-time-combat.md: RimWorld's melee hit chance is gone. A swing misses because the target
    // moved, dodged, blocked or was out of reach, not because of a roll.
    const double novice = meleeTestHitRate(0, 100, 21);
    const double master = meleeTestHitRate(20, 100, 22);
    MESSAGE("skill 0: " << novice << ", skill 20: " << master);
    CHECK(novice >= 0.97);
    CHECK(master >= 0.97);
}

TEST_CASE("Melee: a skilled blow hurts more than an unskilled one") {
    const auto damage = [](int skill) {
        auto sim = meleeTestSim(23);
        const Entity attacker = meleeTestPawn(*sim, 30.5f, 30.5f, "Melee_Mace");
        const Entity target = meleeTestPawn(*sim, 31.5f, 30.5f);
        meleeTestSkill(*sim, attacker, skill);
        meleeTestFace(*sim, target, -1.0f, 0.0f);
        float total = 0.0f;
        for (int i = 0; i < 20; ++i) {
            sim->submit(MeleeCommand{attacker, target, MeleeMode::Attack});
            for (int s = 0; s < 100; ++s) {
                sim->step();
                for (const Event& e : sim->events()) {
                    if (e.type == EventType::MeleeHit && e.message == "hit") {
                        total += e.amount;
                    }
                }
                if (sim->melee().ready(attacker)) {
                    break;
                }
            }
            meleeTestHeal(*sim, target);
            meleeTestFace(*sim, target, -1.0f, 0.0f);
        }
        return total;
    };
    const float novice = damage(0);
    const float master = damage(20);
    MESSAGE("20 swings: skill 0 did " << novice << ", skill 20 did " << master);
    CHECK(novice > 0.0f);
    CHECK(master == doctest::Approx(novice * 1.5f).epsilon(0.15));  // 1.2 / 0.8
}

TEST_CASE("Melee: an unarmed fighter sidesteps the swings it reads in time; a slow reader gets hit") {
  const auto run = [](int defenderSkill, int& outSwings, int& outDodged, int& outHits) {
    auto sim = meleeTestSim(31);
    const Entity attacker = meleeTestPawn(*sim, 30.5f, 30.5f, "Melee_Club");
    const Entity defender = meleeTestPawn(*sim, 31.5f, 30.5f);
    meleeTestSkill(*sim, attacker, 20);
    meleeTestSkill(*sim, defender, defenderSkill);
    sim->submit(DraftCommand{defender, true});
    sim->step();
    int swings = 0;
    int dodges = 0;
    int dodged = 0;
    int hits = 0;
    for (int i = 0; i < 120; ++i) {
        // Put them back side by side, unhurt.
        entt::registry& reg = sim->things().registry();
        Position& a = reg.get<Position>(attacker);
        Position& d = reg.get<Position>(defender);
        a.x = 30.5f;
        a.y = 30.5f;
        d.x = 31.5f;
        d.y = 30.5f;
        reg.get<Mover>(defender).path.clear();
        reg.get<Mover>(defender).next = 0;
        meleeTestHeal(*sim, attacker);
        meleeTestHeal(*sim, defender);
        sim->submit(MeleeCommand{attacker, defender, MeleeMode::Attack});
        for (int s = 0; s < 120; ++s) {
            sim->step();
            swings += meleeTestCount(*sim, EventType::MeleeSwing) > 0 &&
                              std::any_of(sim->events().begin(), sim->events().end(),
                                          [&](const Event& e) { return e.type == EventType::MeleeSwing && e.thing == attacker; })
                          ? 1
                          : 0;
            for (const Event& e : sim->events()) {
                if (e.type == EventType::PawnAction && e.action == PawnAction::Dodge && e.thing == defender) {
                    ++dodges;
                    CHECK(e.target == attacker);
                }
                if (e.type == EventType::MeleeHit && e.thing == attacker) {
                    dodged += e.message == "dodged" ? 1 : 0;
                    hits += e.message == "hit" ? 1 : 0;
                }
            }
            if (sim->melee().ready(attacker)) {
                break;
            }
        }
    }
    MESSAGE("defender skill " << defenderSkill << ": swings " << swings << ", sidesteps " << dodges << ", dodged " << dodged
                              << ", hits " << hits);
    outSwings = swings;
    outDodged = dodged;
    outHits = hits;
  };
    int swings = 0;
    int dodged = 0;
    int hits = 0;
    run(20, swings, dodged, hits);
    CHECK(swings >= 100);
    CHECK(dodged > swings * 3 / 4);  // a skilled fighter reads nearly every swing in time and gets clear
    int slowSwings = 0;
    int slowDodged = 0;
    int slowHits = 0;
    run(0, slowSwings, slowDodged, slowHits);
    CHECK(slowDodged < dodged / 2);  // a skill 20 swing is too quick for a skill 0 reader most of the time
    CHECK(slowHits > hits);
}

TEST_CASE("Melee: a pawn with a melee weapon parries swings from the front until its guard breaks") {
    auto sim = meleeTestSim(41);
    const Entity attacker = meleeTestPawn(*sim, 30.5f, 30.5f, "Melee_Mace");
    const Entity blocker = meleeTestPawn(*sim, 31.5f, 30.5f, "Melee_Gladius");
    meleeTestSkill(*sim, attacker, 20);
    meleeTestSkill(*sim, blocker, 20);  // reads every swing in time; not a fighter, so it never sidesteps
    meleeTestFace(*sim, blocker, -1.0f, 0.0f);
    int blocked = 0;
    int raised = 0;
    int swings = 0;
    bool staggered = true;
    for (int i = 0; i < 80; ++i) {
        meleeTestHeal(*sim, blocker);
        meleeTestPlace(*sim, blocker, 31.5f, 30.5f);  // a blow that got through threw it back (body-motion.md)
        meleeTestFace(*sim, blocker, -1.0f, 0.0f);
        sim->submit(MeleeCommand{attacker, blocker, MeleeMode::Attack});
        for (int s = 0; s < 120; ++s) {
            sim->step();
            for (const Event& e : sim->events()) {
                raised += e.type == EventType::PawnAction && e.action == PawnAction::Block && e.thing == blocker ? 1 : 0;
                swings += e.type == EventType::MeleeSwing && e.thing == attacker ? 1 : 0;
                if (e.type == EventType::MeleeHit && e.message == "blocked") {
                    ++blocked;
                    staggered = staggered && !sim->melee().ready(blocker);
                    HealthReport report;
                    sim->health().report(blocker, report);
                    CHECK(report.injuries.empty());  // the weapon took it
                }
            }
            if (sim->melee().ready(attacker)) {
                break;
            }
        }
    }
    MESSAGE("swings " << swings << ", raised " << raised << ", blocked " << blocked << ", guard breaks "
                       << sim->melee().guardBreaks());
    CHECK(swings == 80);
    CHECK(raised > 50);                  // it parries whenever its guard holds...
    CHECK(blocked > raised * 3 / 4);     // ...and the raised weapon catches the strike
    CHECK(sim->melee().guardBreaks() > 10);  // every third parry in a row breaks the guard...
    CHECK(blocked < 70);                 // ...and the next blow gets through
    CHECK(staggered);

    // From behind there is nothing to block with.
    sim->things().registry().get<Melee>(blocker) = Melee{};
    meleeTestFace(*sim, blocker, 1.0f, 0.0f);
    int blockedBehind = 0;
    for (int i = 0; i < 20; ++i) {
        meleeTestHeal(*sim, blocker);
        meleeTestFace(*sim, blocker, 1.0f, 0.0f);
        sim->submit(MeleeCommand{attacker, blocker, MeleeMode::Attack});
        for (int s = 0; s < 120; ++s) {
            sim->step();
            meleeTestFace(*sim, blocker, 1.0f, 0.0f);
            blockedBehind += meleeTestCount(*sim, EventType::MeleeHit, "blocked");
            if (sim->melee().ready(attacker)) {
                break;
            }
        }
    }
    CHECK(blockedBehind == 0);
}

TEST_CASE("Melee: knife raiders charge, a drafted brawler charges back, an unarmed colonist holds and strikes back") {
    auto sim = meleeTestSim(51);
    const Entity raider = meleeTestPawn(*sim, 40.5f, 30.5f, "Melee_Knife");
    const Entity colonist = meleeTestPawn(*sim, 30.5f, 30.5f);
    meleeTestSkill(*sim, colonist, 8);
    sim->submit(SetFactionCommand{raider, 2});
    sim->submit(DraftCommand{colonist, true});
    sim->step();
    float closest = 100.0f;
    int colonistSwings = 0;
    int raiderSwings = 0;
    const CellPos colonistStart = cellOf(sim->things().registry().get<Position>(colonist));
    for (int s = 0; s < 400; ++s) {
        sim->step();
        meleeTestHeal(*sim, colonist);
        meleeTestHeal(*sim, raider);
        closest = std::min(closest, meleeTestDistance(*sim, raider, colonist));
        for (const Event& e : sim->events()) {
            colonistSwings += e.type == EventType::MeleeSwing && e.thing == colonist ? 1 : 0;
            raiderSwings += e.type == EventType::MeleeSwing && e.thing == raider ? 1 : 0;
        }
    }
    MESSAGE("closest " << closest << ", raider swings " << raiderSwings << ", colonist swings " << colonistSwings);
    CHECK(closest <= sim->melee().reach(raider) + kPawnRadius);
    CHECK(raiderSwings >= 2);  // fewer than before: the colonist sidesteps the swings it reads (real-time-combat.md)
    CHECK(colonistSwings >= 2);  // unarmed, but it hits back at an enemy in reach
    // It didn't charge: the raider came to it. (Its own position wanders since phase 2: it rolls 4 m clear of heavy stabs
    // and the raider follows, real-time-combat.md.)
    (void)colonistStart;

    // A drafted Brawler runs at an enemy 5 m away (inside its leash and the colonists' 6 m ambush range: further off it
    // holds with the others, real-time-combat.md); a plain unarmed one doesn't.
    auto sim2 = meleeTestSim(52);
    const Entity brawler = meleeTestPawn(*sim2, 20.5f, 20.5f);
    const Entity plain = meleeTestPawn(*sim2, 20.5f, 40.5f);
    const Entity enemyA = meleeTestPawn(*sim2, 25.5f, 20.5f);
    const Entity enemyB = meleeTestPawn(*sim2, 25.5f, 40.5f);
    meleeTestSkill(*sim2, brawler, 10, "Brawler");
    meleeTestSkill(*sim2, plain, 10);
    for (const Entity e : {enemyA, enemyB}) {
        sim2->submit(SetFactionCommand{e, 2});  // unarmed and no trait: they hold too
    }
    sim2->submit(DraftCommand{brawler, true});
    sim2->submit(DraftCommand{plain, true});
    sim2->step();
    int brawlerSwings = 0;
    for (int s = 0; s < 300; ++s) {
        sim2->step();
        for (const Entity e : {brawler, plain, enemyA, enemyB}) {
            meleeTestHeal(*sim2, e);
        }
        for (const Event& e : sim2->events()) {
            brawlerSwings += e.type == EventType::MeleeSwing && e.thing == brawler ? 1 : 0;
        }
    }
    CHECK(brawlerSwings >= 1);  // its enemy sidesteps (real-time-combat.md)
    CHECK(meleeTestDistance(*sim2, plain, enemyB) > 4.0f);
}

TEST_CASE("Melee: the player's swing goes straight ahead and needs no target") {
    auto sim = meleeTestSim(61);
    const Entity hero = meleeTestPawn(*sim, 30.5f, 30.5f, "Melee_Longsword");
    const Entity dummy = meleeTestPawn(*sim, 31.6f, 30.5f);
    sim->things().registry().emplace<Colonist>(hero);
    sim->jobs().addWorker(hero);
    sim->submit(ControlPawnCommand{hero});
    sim->step();
    meleeTestFace(*sim, hero, 1.0f, 0.0f);
    sim->submit(MeleeCommand{hero, kNoEntity, MeleeMode::Attack});
    int hits = 0;
    for (int s = 0; s < 40; ++s) {
        sim->step();
        for (const Event& e : sim->events()) {
            if (e.type == EventType::MeleeHit && e.message == "hit") {
                ++hits;
                CHECK(e.target == dummy);
            }
        }
    }
    CHECK(hits == 1);
    // Turned away, the swing meets nothing.
    meleeTestFace(*sim, hero, -1.0f, 0.0f);
    while (!sim->melee().ready(hero)) {
        sim->step();
    }
    meleeTestFace(*sim, hero, -1.0f, 0.0f);
    sim->submit(MeleeCommand{hero, kNoEntity, MeleeMode::Attack});
    int missed = 0;
    for (int s = 0; s < 40; ++s) {
        sim->step();
        missed += meleeTestCount(*sim, EventType::MeleeHit, "missed");
    }
    CHECK(missed == 1);
    // The player blocks by hand.
    while (!sim->melee().ready(hero)) {
        sim->step();
    }
    sim->submit(MeleeCommand{hero, kNoEntity, MeleeMode::Block});
    sim->step();
    CHECK_FALSE(meleeTestFailed(*sim));
    sim->stepMany(sim->melee().rules().blockSteps);
    CHECK(sim->melee().blocking(hero));
}

TEST_CASE("Melee: a brawl is the same after a save and load, and through a replay") {
    auto sim = meleeTestSim(71);
    std::vector<Entity> side1;
    std::vector<Entity> side2;
    const char* weapons[] = {"Melee_Knife", "Melee_Club", "Melee_Spear"};
    for (int i = 0; i < 3; ++i) {
        const Entity a = meleeTestPawn(*sim, 25.5f, 25.5f + 2.0f * static_cast<float>(i));
        meleeTestSkill(*sim, a, 4 + 4 * i, "Brawler");
        side1.push_back(a);
        const Entity b = meleeTestPawn(*sim, 33.5f, 25.5f + 2.0f * static_cast<float>(i), weapons[i]);
        side2.push_back(b);
    }
    for (const Entity a : side1) {
        sim->submit(DraftCommand{a, true});
    }
    for (const Entity b : side2) {
        sim->submit(SetFactionCommand{b, 2});
    }
    sim->stepMany(150);  // into the fight
    CHECK(sim->melee().swings() > 0);
    SaveLoadResult loaded = loadSim(saveSim(*sim), meleeTestDefs());
    MELEE_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->stateHash() == sim->stateHash());
    for (int s = 0; s < 600; ++s) {
        sim->step();
        loaded.sim->step();
        if (sim->stateHash() != loaded.sim->stateHash()) {
            FAIL("different after " << s << " steps");
            break;
        }
    }
    CHECK(sim->melee().hits() > 0);

    // The same fight from a replay file, with a player swing and block in it.
    auto rec = meleeTestSim(72);
    const Entity a = meleeTestPawn(*rec, 30.5f, 30.5f, "Melee_Knife");
    const Entity b = meleeTestPawn(*rec, 31.4f, 30.5f, "Melee_Club");
    rec->submit(MeleeCommand{a, b, MeleeMode::Attack});
    rec->submit(MeleeCommand{b, kNoEntity, MeleeMode::Block});  // b isn't controlled: still a block
    rec->stepMany(200);
    const Replay replay = recordReplay(*rec);
    std::string error;
    Replay back;
    MELEE_REQUIRE(readReplay(writeReplay(replay), back, error));
    CHECK(back.commands.size() == replay.commands.size());
    const auto* first = std::get_if<MeleeCommand>(&back.commands[0].command);
    MELEE_REQUIRE(first != nullptr);
    CHECK(first->target == b);
    const auto* second = std::get_if<MeleeCommand>(&back.commands[1].command);
    MELEE_REQUIRE(second != nullptr);
    CHECK(second->mode == MeleeMode::Block);
}

TEST_CASE("Melee: three brawlers against three armed raiders fight it out") {
    // A scenario over many steps: both sides close in, swing, block and dodge until one side is down.
    const auto fight = [](uint64_t seed, int& downed, uint64_t& swings, uint64_t& hits, uint64_t& hash) {
        auto sim = meleeTestSim(seed);
        std::vector<Entity> all;
        const char* weapons[] = {"Melee_Knife", "Melee_Mace", "Melee_Gladius"};
        for (int i = 0; i < 3; ++i) {
            const Entity a = meleeTestPawn(*sim, 20.5f, 26.5f + 2.0f * static_cast<float>(i));
            meleeTestSkill(*sim, a, 8, "Brawler");
            sim->submit(DraftCommand{a, true});
            all.push_back(a);
            const Entity b = meleeTestPawn(*sim, 26.5f, 26.5f + 2.0f * static_cast<float>(i), weapons[i]);
            sim->submit(SetFactionCommand{b, 2});
            all.push_back(b);
        }
        int s = 0;
        for (; s < 6000; ++s) {
            sim->step();
            int side1 = 0;
            int side2 = 0;
            for (const Entity e : all) {
                if (!sim->health().incapacitated(e)) {
                    (sim->fighters().factionOf(e) == kPlayerFaction ? side1 : side2) += 1;
                }
            }
            if (side1 == 0 || side2 == 0) {
                break;
            }
        }
        downed = 0;
        for (const Entity e : all) {
            downed += sim->health().incapacitated(e) ? 1 : 0;
        }
        swings = sim->melee().swings();
        hits = sim->melee().hits();
        hash = sim->stateHash();
        MESSAGE("seed " << seed << ": over after " << s << " steps, " << downed << " down, " << swings << " swings, "
                        << hits << " hits, " << sim->melee().blocks() << " blocks, " << sim->melee().dodges() << " dodges");
    };
    int downed = 0;
    uint64_t swings = 0;
    uint64_t hits = 0;
    uint64_t hash = 0;
    for (const uint64_t seed : {82, 83, 84}) {
        fight(seed, downed, swings, hits, hash);
        CHECK(downed >= 3);  // one side is down: nobody runs round in circles for ever
        CHECK(swings > 20);
        CHECK(hits > 5);
    }
    fight(81, downed, swings, hits, hash);
    int downed2 = 0;
    uint64_t swings2 = 0;
    uint64_t hits2 = 0;
    uint64_t hash2 = 0;
    fight(81, downed2, swings2, hits2, hash2);
    CHECK(hash2 == hash);  // deterministic
}

TEST_CASE("Melee: an AI fighter rolls clear of a heavy blow and sidesteps a light one") {
    // docs/design/real-time-combat.md phase 2: the player's dodge roll, used by the AI against strong swings.
    auto sim = meleeTestSim(61);
    const Entity attacker = meleeTestPawn(*sim, 30.5f, 30.5f, "Melee_Longsword");
    const Entity defender = meleeTestPawn(*sim, 31.5f, 30.5f);
    meleeTestSkill(*sim, attacker, 10);
    meleeTestSkill(*sim, defender, 20);
    sim->submit(DraftCommand{defender, true});
    sim->step();
    int strong = 0;
    int rolled = 0;
    int dodged = 0;
    for (int i = 0; i < 40; ++i) {
        entt::registry& reg = sim->things().registry();
        // Wait out any roll, then put them back side by side, unhurt.
        for (int s = 0; s < 80 && sim->combat().rollLocked(defender); ++s) {
            sim->step();
        }
        Position& a = reg.get<Position>(attacker);
        Position& d = reg.get<Position>(defender);
        a.x = 30.5f;
        a.y = 30.5f;
        d.x = 31.5f;
        d.y = 30.5f;
        reg.get<Mover>(defender).path.clear();
        reg.get<Mover>(defender).next = 0;
        meleeTestHeal(*sim, attacker);
        meleeTestHeal(*sim, defender);
        sim->submit(MeleeCommand{attacker, defender, MeleeMode::Attack});
        for (int s = 0; s < 120; ++s) {
            sim->step();
            for (const Event& e : sim->events()) {
                strong += e.type == EventType::MeleeSwing && e.thing == attacker && e.count == 1 ? 1 : 0;
                rolled += e.type == EventType::PawnAction && e.action == PawnAction::Roll && e.thing == defender ? 1 : 0;
                dodged += e.type == EventType::MeleeHit && e.thing == attacker && e.message == "dodged" ? 1 : 0;
            }
            if (sim->melee().ready(attacker)) {
                break;
            }
        }
    }
    MESSAGE("40 swings, " << strong << " strong; " << rolled << " rolls, " << dodged << " dodged");
    CHECK(strong >= 5);
    CHECK(rolled >= 8);       // rolls from heavy blows, at most one every aiRollEverySteps...
    CHECK(rolled <= strong);  // ...and none from the light ones (sidesteps)
    CHECK(dodged >= 30);
}
