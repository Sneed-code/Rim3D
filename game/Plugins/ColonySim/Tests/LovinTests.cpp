#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Mood.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Social.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <memory>
#include <string>
#include <vector>

using namespace colony;

// These tests check docs/design/lovin.md: double beds, partners sharing them, and lovin (abstract, adults only).

#define LOVIN_REQUIRE(...)                     \
    do {                                       \
        const bool lovinChecked = (__VA_ARGS__); \
        CHECK(lovinChecked);                   \
        if (!lovinChecked) {                   \
            return;                            \
        }                                      \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> lovinDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

Sim lovinSim() { return Sim(SimSettings{5, 48, 0, 0, true}, lovinDefs()); }

// Everybody bisexual: no asexual pawn by chance.
void lovinAllBi(Sim& sim) {
    float* weights = sim.social().rules().orientationWeights;  // everybody bisexual: no asexual pawn by chance
    weights[0] = 0.0f;
    weights[1] = 0.0f;
    weights[2] = 1.0f;
    weights[3] = 0.0f;
}

// A colonist of the given age who sleeps every hour and is tired, so it goes to bed at once.
Entity lovinPawn(Sim& sim, CellPos cell, int age = 25) {
    const Entity pawn = sim.things().spawn("Human", cell);
    sim.jobs().addWorker(pawn);
    Colonist colonist;
    colonist.age = age;
    sim.things().registry().emplace<Colonist>(pawn, std::move(colonist));
    for (int h = 0; h < 24; ++h) {
        sim.submit(SetScheduleCommand{pawn, h, ScheduleKind::Sleep});
    }
    return pawn;
}

// Makes the components (first step), keeps everybody from chatting, and makes the two lovers.
void lovinCouple(Sim& sim, Entity a, Entity b, BondKind kind = BondKind::Lover) {
    sim.step();
    entt::registry& reg = sim.things().registry();
    for (const Entity pawn : reg.view<Social>()) {
        reg.get<Social>(pawn).nextTalk = sim.time().seconds + 100 * kSecondsPerDay;
    }
    reg.get<Social>(a).bonds.push_back(Bond{kind, b, 0});
    reg.get<Social>(b).bonds.push_back(Bond{kind, a, 0});
}

void lovinTired(Sim& sim, std::initializer_list<Entity> pawns) {
    for (const Entity pawn : pawns) {
        sim.things().registry().get<Needs>(pawn).rest = 0.3f;
    }
}

struct LovinLog {
    std::vector<Event> events;
    int count(const std::string& message) const {
        int n = 0;
        for (const Event& e : events) {
            n += e.type == EventType::Lovin && e.message == message ? 1 : 0;
        }
        return n;
    }
};

template <typename F>
bool lovinRunUntil(Sim& sim, LovinLog& log, int steps, F done) {
    for (int i = 0; i < steps; ++i) {
        sim.step();
        log.events.insert(log.events.end(), sim.events().begin(), sim.events().end());
        if (done()) {
            return true;
        }
    }
    return false;
}

bool lovinThought(Sim& sim, Entity pawn, const std::string& def) {
    for (const ThoughtView& t : sim.mood().thoughts(pawn, sim.time().seconds)) {
        if (t.def == def) {
            return true;
        }
    }
    return false;
}

bool lovinMemory(Sim& sim, Entity pawn, Entity other) {
    for (const OpinionMemory& m : sim.things().registry().get<Social>(pawn).memories) {
        if (m.other == other && m.label == sim.social().rules().lovinLabel) {
            return true;
        }
    }
    return false;
}

const ThingView* lovinView(const Snapshot& snap, Entity pawn) { return snap.find(pawn); }

}  // namespace

TEST_CASE("Lovin: the double bed is real data with two slots, unlocked by complex furniture") {
    const Def* bed = lovinDefs()->find("Things", "DoubleBed");
    LOVIN_REQUIRE(bed != nullptr);
    CHECK(bed->flag("building.bed"));
    CHECK(bed->integer("building.sleepingSlots") == 2);
    CHECK(bed->text("researchPrerequisite") == "ComplexFurniture");
}

TEST_CASE("Lovin: partners in one double bed have lovin, stay in bed for it, and are glad afterwards") {
    Sim sim = lovinSim();
    lovinAllBi(sim);
    const Entity a = lovinPawn(sim, CellPos{20, 20, 0});
    const Entity b = lovinPawn(sim, CellPos{24, 20, 0});
    const Entity bed = sim.things().spawn("DoubleBed", CellPos{22, 24, 0});
    lovinCouple(sim, a, b);
    lovinTired(sim, {a, b});
    const float before = sim.social().opinion(a, b);

    LovinLog log;
    LOVIN_REQUIRE(lovinRunUntil(sim, log, 600, [&] { return sim.social().inLovin(a); }));
    entt::registry& reg = sim.things().registry();
    CHECK(sim.social().inLovin(b));
    CHECK(log.count("started") == 1);
    CHECK(sim.jobs().bedOf(a) == bed);
    CHECK(sim.jobs().bedOf(b) == bed);
    CHECK(reg.get<Bed>(bed).owners.size() == 2);
    // Both lie in it, the snapshot says so (Unreal: the hearts icon and a cuddling pose under the covers).
    CHECK(jobIsLyingDown(reg.get<Worker>(a).job));
    CHECK(jobIsLyingDown(reg.get<Worker>(b).job));
    Snapshot snap;
    sim.writeSnapshot(snap);
    const ThingView* view = lovinView(snap, a);
    LOVIN_REQUIRE(view != nullptr);
    CHECK(view->lovin);
    CHECK(view->lovinPose >= 0);
    CHECK(view->lovinPose <= 2);
    CHECK(reg.get<Lovin>(a).pose == reg.get<Lovin>(b).pose);
    const int64_t until = reg.get<Lovin>(a).until;
    CHECK(until - sim.time().seconds <= static_cast<int64_t>(sim.social().rules().lovinMaxMinutes * 60.0f) + 1);

    // Fully rested, a pawn still stays until it is over.
    reg.get<Needs>(a).rest = 1.0f;
    sim.step();
    CHECK(sim.social().inLovin(a));
    CHECK(jobIsLyingDown(reg.get<Worker>(a).job));

    LOVIN_REQUIRE(lovinRunUntil(sim, log, 5000, [&] { return log.count("done") == 1; }));
    CHECK_FALSE(sim.social().inLovin(a));
    CHECK_FALSE(sim.social().inLovin(b));
    CHECK(sim.social().stats().lovins == 1);
    CHECK(lovinThought(sim, a, "GotSomeLovin"));
    CHECK(lovinThought(sim, b, "GotSomeLovin"));
    CHECK(lovinMemory(sim, a, b));
    CHECK(lovinMemory(sim, b, a));
    CHECK(sim.social().opinion(a, b) == doctest::Approx(before + sim.social().rules().lovinOpinion));
    // The pause before the next is about the curve's hours at 25 (2.44), never under the minimum.
    const int64_t gap = reg.get<Social>(a).nextLovin - sim.time().seconds;
    CHECK(gap >= static_cast<int64_t>(sim.social().rules().lovinIntervalMin * 3600.0f) - 1);
    CHECK(gap <= 6 * 3600);
    CHECK(sim.social().lovinIntervalHours(a) == doctest::Approx(2.4375f));
    // No second lovin straight away: they sleep.
    sim.stepMany(600);
    CHECK(log.count("started") == 1);
}

TEST_CASE("Lovin: a pawn moves into its partner's double bed, and a stranger never takes the free slot") {
    Sim sim = lovinSim();
    lovinAllBi(sim);
    const Entity a = lovinPawn(sim, CellPos{20, 20, 0});
    const Entity b = lovinPawn(sim, CellPos{30, 20, 0});
    const Entity stranger = lovinPawn(sim, CellPos{21, 21, 0});
    const Entity doubleBed = sim.things().spawn("DoubleBed", CellPos{22, 24, 0});
    const Entity single = sim.things().spawn("Bed", CellPos{30, 24, 0});
    lovinCouple(sim, a, b);
    entt::registry& reg = sim.things().registry();
    std::string error;
    LOVIN_REQUIRE(sim.jobs().assignBed(doubleBed, a, error));
    LOVIN_REQUIRE(sim.jobs().assignBed(single, b, error));
    // Apart they want to sleep together.
    sim.step();
    CHECK(lovinThought(sim, a, "WantToSleepWithPartner"));
    CHECK(lovinThought(sim, b, "WantToSleepWithPartner"));
    CHECK_FALSE(lovinThought(sim, stranger, "WantToSleepWithPartner"));

    // The stranger goes to bed first: it does not lie down next to a, it sleeps on the ground (no bed else).
    lovinTired(sim, {stranger});
    LovinLog log;
    LOVIN_REQUIRE(lovinRunUntil(sim, log, 300, [&] { return jobIsLyingDown(reg.get<Worker>(stranger).job); }));
    CHECK(sim.jobs().bedOf(stranger) == kNoEntity);
    CHECK(reg.get<Worker>(stranger).job.thing == kNoEntity);
    CHECK(reg.get<Bed>(doubleBed).owners.size() == 1);

    // b goes to bed: it leaves its own bed for a's double bed.
    lovinTired(sim, {b});
    LOVIN_REQUIRE(lovinRunUntil(sim, log, 600, [&] { return jobIsLyingDown(reg.get<Worker>(b).job); }));
    CHECK(sim.jobs().bedOf(b) == doubleBed);
    CHECK(reg.get<Bed>(single).owners.empty());
    sim.step();
    CHECK_FALSE(lovinThought(sim, a, "WantToSleepWithPartner"));
    CHECK_FALSE(lovinThought(sim, b, "WantToSleepWithPartner"));
}

TEST_CASE("Lovin: never for anyone under the adult age, nor for an asexual pawn") {
    for (int round = 0; round < 2; ++round) {
        Sim sim = lovinSim();
        lovinAllBi(sim);
        const Entity a = lovinPawn(sim, CellPos{20, 20, 0});
        const Entity b = lovinPawn(sim, CellPos{24, 20, 0}, round == 0 ? 17 : 25);
        sim.things().spawn("DoubleBed", CellPos{22, 24, 0});
        if (round == 1) {
            float* weights = sim.social().rules().orientationWeights;  // everybody asexual
            weights[2] = 0.0f;
            weights[3] = 1.0f;
        }
        lovinCouple(sim, a, b);  // forced in by hand: no romance would ever make this bond
        lovinTired(sim, {a, b});
        LovinLog log;
        sim.stepMany(5);
        lovinRunUntil(sim, log, 3600, [] { return false; });
        CHECK(log.count("started") == 0);
        CHECK(sim.social().stats().lovins == 0);
        CHECK_FALSE(sim.social().inLovin(a));
    }
}

TEST_CASE("Lovin: one partner getting up ends it early, with no thought") {
    Sim sim = lovinSim();
    lovinAllBi(sim);
    const Entity a = lovinPawn(sim, CellPos{20, 20, 0});
    const Entity b = lovinPawn(sim, CellPos{24, 20, 0});
    sim.things().spawn("DoubleBed", CellPos{22, 24, 0});
    lovinCouple(sim, a, b, BondKind::Spouse);
    lovinTired(sim, {a, b});
    LovinLog log;
    LOVIN_REQUIRE(lovinRunUntil(sim, log, 600, [&] { return sim.social().inLovin(a); }));
    sim.submit(DraftCommand{b, true});
    LOVIN_REQUIRE(lovinRunUntil(sim, log, 10, [&] { return log.count("interrupted") == 1; }));
    CHECK_FALSE(sim.social().inLovin(a));
    CHECK_FALSE(sim.social().inLovin(b));
    CHECK_FALSE(lovinThought(sim, a, "GotSomeLovin"));
    CHECK(sim.social().stats().lovins == 0);
}

TEST_CASE("Lovin: saved in the middle, a loaded game goes on the same") {
    Sim original = lovinSim();
    lovinAllBi(original);
    const Entity a = lovinPawn(original, CellPos{20, 20, 0});
    const Entity b = lovinPawn(original, CellPos{24, 20, 0});
    original.things().spawn("DoubleBed", CellPos{22, 24, 0});
    lovinCouple(original, a, b);
    lovinTired(original, {a, b});
    LovinLog log;
    LOVIN_REQUIRE(lovinRunUntil(original, log, 600, [&] { return original.social().inLovin(a); }));
    original.stepMany(30);

    const std::vector<uint8_t> bytes = saveSim(original);
    const SaveLoadResult loaded = loadSim(bytes, lovinDefs());
    LOVIN_REQUIRE(loaded.sim != nullptr);
    Sim& copy = *loaded.sim;
    float* weights = copy.social().rules().orientationWeights;
    weights[0] = 0.0f;
    weights[1] = 0.0f;
    weights[2] = 1.0f;
    weights[3] = 0.0f;
    CHECK(copy.stateHash() == original.stateHash());
    CHECK(copy.social().inLovin(a));
    CHECK(copy.things().registry().get<Lovin>(a).until == original.things().registry().get<Lovin>(a).until);
    for (int i = 0; i < 2 * 3600; ++i) {
        original.step();
        copy.step();
    }
    CHECK(original.social().stats().lovins == 1);
    CHECK(copy.stateHash() == original.stateHash());
    CHECK(saveSim(copy) == saveSim(original));
}
