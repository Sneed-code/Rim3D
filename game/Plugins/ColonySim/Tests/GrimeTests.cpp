#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Filth.h"
#include "ColonySim/Grime.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Dirt and blood on bodies, clothes and weapons (docs/design/grime.md).

#define GRIME_REQUIRE(...)    \
    do {                      \
        CHECK(__VA_ARGS__);   \
        if (!(__VA_ARGS__)) { \
            return;           \
        }                     \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> grimeTestDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

std::unique_ptr<Sim> grimeTestSim() {
    SimSettings settings;
    settings.seed = 7;
    settings.mapSize = 48;
    settings.flatMap = true;
    return std::make_unique<Sim>(settings, grimeTestDefs());
}

Entity grimeTestPawn(Sim& sim, const char* def, CellPos cell) {
    const Entity pawn = sim.things().spawn(def, cell);
    return pawn;
}

// Walks the pawn `metres` east, a tenth of a metre a step, by moving it by hand (no job, no path).
void grimeTestWalk(Sim& sim, Entity pawn, float metres) {
    auto& reg = sim.things().registry();
    for (float done = 0.0f; done < metres; done += 0.1f) {
        reg.get<Position>(pawn).x += 0.1f;
        sim.grime().step(sim.time().seconds, {}, 0.0f);
    }
}

Event grimeInjury(Entity pawn, const Sim& sim, const char* hediff, float damage) {
    Event e;
    e.type = EventType::PawnInjured;
    e.thing = pawn;
    e.def = sim.defs().idOf("Hediffs", hediff);
    e.amount = damage;
    return e;
}

}  // namespace

TEST_CASE("Grime: walking on bare soil dusts the legs, mud cakes them, a floor keeps them clean") {
    auto sim = grimeTestSim();
    const GrimeNumbers& n = sim->grime().numbers();
    CHECK(n.soilMax == doctest::Approx(0.5f));  // from GrimeRules
    const Entity a = grimeTestPawn(*sim, "Human", CellPos{2, 10, 0});
    sim->grime().step(0, {}, 0.0f);
    GRIME_REQUIRE(sim->grime().of(a) != nullptr);
    CHECK(sim->grime().of(a)->clean());

    // 36 m of soil: 36 / 7200 * 0.5 = 0.0025 of the way.
    grimeTestWalk(*sim, a, 36.0f);
    const float dusted = sim->grime().of(a)->dirtLegs;
    CHECK(dusted == doctest::Approx(36.0f / n.soilFullSeconds * n.soilMax).epsilon(0.1));
    CHECK(sim->grime().of(a)->dirtBody == 0.0f);

    // Mud: 30 m is 5% of the way to caked.
    for (int x = 0; x < 48; ++x) {
        sim->editMap().setTerrain(CellPos{x, 12, 0}, Terrain::Mud);
    }
    const Entity b = grimeTestPawn(*sim, "Human", CellPos{2, 12, 0});
    sim->grime().step(0, {}, 0.0f);
    grimeTestWalk(*sim, b, 30.0f);
    CHECK(sim->grime().of(b)->dirtLegs == doctest::Approx(30.0f / n.mudFullSeconds).epsilon(0.1));
    CHECK(sim->grime().of(b)->dirtLegs > dusted * 10.0f);

    // Soil never takes legs past soilMax, however far.
    sim->grime().set(a, 0.49f, 0.0f, 0.0f, -1, 0);
    grimeTestWalk(*sim, a, 5.0f);
    CHECK(sim->grime().of(a)->dirtLegs <= n.soilMax + 1e-6f);
}

TEST_CASE("Grime: a bleeding wound wets the body in its own fluid; a bruise does not") {
    auto sim = grimeTestSim();
    const Entity man = grimeTestPawn(*sim, "Human", CellPos{10, 10, 0});
    const Entity bug = grimeTestPawn(*sim, "Megaspider", CellPos{14, 10, 0});
    GRIME_REQUIRE(man != kNoEntity);
    GRIME_REQUIRE(bug != kNoEntity);
    const int blood = sim->filth().kindIndex("Blood");
    const int insect = sim->filth().kindIndex("InsectBlood");

    sim->grime().step(100, {grimeInjury(man, *sim, "Bruise", 10.0f)}, 0.0f);
    CHECK(sim->grime().of(man)->blood == 0.0f);

    sim->grime().step(101, {grimeInjury(man, *sim, "Gunshot", 10.0f), grimeInjury(bug, *sim, "Cut", 10.0f)}, 0.0f);
    const PawnGrime* g = sim->grime().of(man);
    CHECK(g->blood == doctest::Approx(sim->grime().numbers().bloodPerDamage * 10.0f).epsilon(0.01));
    CHECK(g->bloodKind == blood);
    CHECK(g->bloodAt == 101);
    CHECK(sim->grime().of(bug)->bloodKind == insect);

    // The snapshot says which fluid and how old.
    sim->grime().set(man, 0.2f, 0.1f, g->blood, blood, 101);
    Snapshot snap;
    sim->writeSnapshot(snap);
    const ThingView* view = snap.find(man);
    GRIME_REQUIRE(view != nullptr);
    CHECK(view->blood > 0.0f);
    CHECK(view->dirtLegs == doctest::Approx(0.2f));
    CHECK(view->bloodFilth == sim->defs().idOf("Filth", "Blood"));
    CHECK(view->bloodAge == doctest::Approx(static_cast<float>(sim->time().seconds - 101)));
}

TEST_CASE("Grime: a melee blow that draws blood spatters the attacker and bloodies the weapon, not fists") {
    auto sim = grimeTestSim();
    const Entity attacker = grimeTestPawn(*sim, "Human", CellPos{10, 10, 0});
    const Entity boxer = grimeTestPawn(*sim, "Human", CellPos{12, 10, 0});
    const Entity victim = grimeTestPawn(*sim, "Megaspider", CellPos{11, 10, 0});
    const DefId sword = sim->defs().idOf("Things", "Melee_Knife");
    GRIME_REQUIRE(sword.valid());
    Event hit;
    hit.type = EventType::MeleeHit;
    hit.thing = attacker;
    hit.target = victim;
    hit.def = sword;
    hit.hit = ProjectileHitKind::Pawn;
    hit.amount = 20.0f;
    Event punch = hit;
    punch.thing = boxer;
    punch.def = sim->things().registry().get<Thing>(boxer).def;
    Event blocked = hit;
    blocked.hit = ProjectileHitKind::Armor;
    sim->grime().step(50, {hit, punch, blocked}, 0.0f);
    const GrimeNumbers& n = sim->grime().numbers();
    const int insect = sim->filth().kindIndex("InsectBlood");
    const PawnGrime* a = sim->grime().of(attacker);
    GRIME_REQUIRE(a != nullptr);
    CHECK(a->blood == doctest::Approx(n.spatterPerDamage * 20.0f));
    CHECK(a->weaponBlood == doctest::Approx(n.weaponPerDamage * 20.0f));
    CHECK(a->weaponKind == insect);
    const PawnGrime* b = sim->grime().of(boxer);
    GRIME_REQUIRE(b != nullptr);
    CHECK(b->blood > 0.0f);
    CHECK(b->weaponBlood == 0.0f);
}

TEST_CASE("Grime: rain washes it off under the open sky, not under a roof; it wears off slowly in the dry") {
    auto sim = grimeTestSim();
    const Entity out = grimeTestPawn(*sim, "Human", CellPos{10, 10, 0});
    const Entity in = grimeTestPawn(*sim, "Human", CellPos{20, 20, 0});
    sim->editMap().setThickRoof(CellPos{20, 20, 0}, true);
    const int blood = sim->filth().kindIndex("Blood");
    sim->grime().set(out, 1.0f, 1.0f, 1.0f, blood, 0);
    sim->grime().set(in, 1.0f, 1.0f, 1.0f, blood, 0);
    const GrimeNumbers& n = sim->grime().numbers();
    const int half = static_cast<int>(n.rainWashSeconds / 2.0f);
    for (int s = 0; s < half; ++s) {
        sim->grime().step(s, {}, 1.0f);
    }
    const float wear = static_cast<float>(half) / n.wearOffSeconds;
    CHECK(sim->grime().of(out)->blood == doctest::Approx(1.0f - 0.5f - wear).epsilon(0.02));
    CHECK(sim->grime().of(in)->blood == doctest::Approx(1.0f - wear).epsilon(0.01));
    for (int s = 0; s < half + 10; ++s) {
        sim->grime().step(half + s, {}, 1.0f);
    }
    CHECK(sim->grime().of(out)->clean());
    CHECK(sim->grime().of(out)->bloodKind == -1);
    CHECK(sim->grime().of(in)->blood > 0.9f);

    // A corpse in the dirt does not wear clean.
    sim->things().registry().get<Health>(in).dead = true;
    const float before = sim->grime().of(in)->blood;
    for (int s = 0; s < 100; ++s) {
        sim->grime().step(20000 + s, {}, 0.0f);
    }
    CHECK(sim->grime().of(in)->blood == before);
}

TEST_CASE("Grime: wading washes the legs, swimming washes everything") {
    auto sim = grimeTestSim();
    for (int y = 10; y < 20; ++y) {
        for (int x = 10; x < 20; ++x) {
            sim->editMap().setTerrain(CellPos{x, y, 0}, Terrain::Water);
        }
    }
    for (int x = 0; x < 48; ++x) {
        sim->editMap().setTerrain(CellPos{x, 5, 0}, Terrain::ShallowWater);
    }
    const Entity wader = grimeTestPawn(*sim, "Human", CellPos{3, 5, 0});
    const Entity swimmer = grimeTestPawn(*sim, "Human", CellPos{8, 15, 0});
    GRIME_REQUIRE(wader != kNoEntity);
    GRIME_REQUIRE(swimmer != kNoEntity);
    auto& pos = sim->things().registry().get<Position>(swimmer);
    pos.x = 15.5f;
    pos.y = 15.5f;
    const int blood = sim->filth().kindIndex("Blood");
    sim->grime().set(wader, 1.0f, 1.0f, 1.0f, blood, 0);
    sim->grime().set(swimmer, 1.0f, 1.0f, 1.0f, blood, 0);
    const GrimeNumbers& n = sim->grime().numbers();
    for (int s = 0; s < static_cast<int>(n.swimWashSeconds) + 5; ++s) {
        sim->grime().step(s, {}, 0.0f);
    }
    CHECK(sim->grime().of(swimmer)->clean());
    const PawnGrime* w = sim->grime().of(wader);
    CHECK(w->dirtBody > 0.9f);
    CHECK(w->blood > 0.9f);
    CHECK(w->dirtLegs < 1.0f);
}

TEST_CASE("Grime: it lives in the sim's step and survives a save") {
    auto sim = grimeTestSim();
    const Entity pawn = grimeTestPawn(*sim, "Human", CellPos{10, 10, 0});
    sim->step();
    GRIME_REQUIRE(sim->grime().of(pawn) != nullptr);
    const int blood = sim->filth().kindIndex("Blood");
    sim->grime().set(pawn, 0.3f, 0.4f, 0.5f, blood, 1);
    const uint64_t hash = sim->stateHash();
    const std::vector<uint8_t> bytes = saveSim(*sim);
    SaveLoadResult loaded = loadSim(bytes, grimeTestDefs());
    GRIME_REQUIRE(loaded.sim != nullptr);
    const PawnGrime* g = loaded.sim->grime().of(pawn);
    GRIME_REQUIRE(g != nullptr);
    CHECK(g->dirtLegs == doctest::Approx(0.3f));
    CHECK(g->dirtBody == doctest::Approx(0.4f));
    CHECK(g->blood == doctest::Approx(0.5f));
    CHECK(g->bloodKind == blood);
    CHECK(loaded.sim->stateHash() == hash);
}
