// Taming, training, animal zones and pens (task N3, docs/design/husbandry.md).
#include "ColonySim/Animals.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Health.h"
#include "ColonySim/Husbandry.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define HUSB_REQUIRE(...)      \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// The game's real data.
std::shared_ptr<const DefDatabase> husbDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

SimSettings husbSettings(int size = 64) {
    SimSettings s;
    s.seed = 5;
    s.mapSize = size;
    s.wildAnimals = 0;
    s.flatMap = true;
    s.checkpointEvery = 0;
    return s;
}

struct HusbFixture {
    Sim sim;
    Entity colonist = kNoEntity;

    explicit HusbFixture(int size = 64) : sim(husbSettings(size), husbDefs()) {
        for (int y = 0; y < sim.map().height(); ++y) {
            for (int x = 0; x < sim.map().width(); ++x) {
                sim.plants().remove(CellPos{x, y, 0});
            }
        }
        sim.stepMany(9 * kSecondsPerHour);  // daytime: animals are awake
    }

    Husbandry& hu() { return sim.husbandry(); }
    entt::registry& reg() { return sim.things().registry(); }
    DefId def(const char* name) const { return sim.defs().idOf("Things", name); }

    // A colonist with the given Animals level.
    Entity addColonist(CellPos cell, int animalsLevel) {
        const Entity pawn = sim.things().spawn("Human", cell);
        Colonist c;
        SkillRecord s;
        s.def = "Animals";
        s.level = animalsLevel;
        c.skills.push_back(s);
        reg().emplace<Colonist>(pawn, c);
        sim.jobs().addWorker(pawn, {}, false);
        colonist = pawn;
        return pawn;
    }
    Entity spawn(const char* def, CellPos cell, int count = 1) {
        const Entity e = sim.things().spawn(def, cell, count);
        sim.step();
        return e;
    }
    Entity animal(const char* def, CellPos cell) {
        const Entity e = sim.things().spawn(def, cell);
        sim.stepMany(5);  // its first look sets it up
        return e;
    }
    // Makes the animal tame at once, the way finishTame does (a high skill, repeated until it works).
    bool tameNow(Entity animal) {
        std::string error;
        if (!hu().markTame(animal, true, error)) {
            return false;
        }
        for (int i = 0; i < 200 && !hu().isTame(animal); ++i) {
            hu().finishTame(colonist, animal, 20);
        }
        return hu().isTame(animal);
    }
    int itemsOnMap(const char* name) {
        const DefId id = def(name);
        int total = 0;
        reg().view<const Thing, const StackCount, const Position>().each(
            [&](const Thing& thing, const StackCount& stack, const Position&) { total += thing.def == id ? stack.count : 0; });
        return total;
    }
    CellPos cellOfThing(Entity e) { return cellOf(reg().get<Position>(e)); }
    bool dead(Entity e) { return reg().get<Health>(e).dead; }
    template <typename F>
    bool runUntil(int limit, F done) {
        for (int i = 0; i < limit; ++i) {
            if (done()) {
                return true;
            }
            sim.step();
        }
        return done();
    }
    bool sawEvent(EventType type) const {
        return std::any_of(sim.events().begin(), sim.events().end(), [&](const Event& e) { return e.type == type; });
    }
};

}  // namespace

TEST_CASE("Husbandry: chances follow skill and wildness, and what can be learned follows trainability") {
    HusbFixture f;
    const Entity muffalo = f.animal("Muffalo", CellPos{20, 20, 0});
    const Entity husky = f.animal("Husky", CellPos{30, 20, 0});
    const Entity deer = f.animal("Deer", CellPos{40, 20, 0});
    HUSB_REQUIRE(muffalo != kNoEntity);
    HUSB_REQUIRE(husky != kNoEntity);
    HUSB_REQUIRE(deer != kNoEntity);
    const Husbandry& hu = f.hu();

    CHECK(hu.wildness(muffalo) == doctest::Approx(0.5));
    CHECK(hu.trainability(muffalo) == 1);
    CHECK(hu.trainability(husky) == 3);
    CHECK(hu.trainability(deer) == 0);
    // (0.5 + 0.05 x skill) x (1 - wildness)
    CHECK(hu.tameChance(0, muffalo) == doctest::Approx(0.25));
    CHECK(hu.tameChance(10, muffalo) == doctest::Approx(0.5));
    CHECK(hu.tameChance(20, muffalo) == doctest::Approx(0.75));
    CHECK(hu.tameChance(10, deer) < hu.tameChance(10, muffalo));  // wilder is harder
    CHECK(hu.tameChance(20, f.animal("Thrumbo", CellPos{50, 50, 0})) < 0.05);
    CHECK(hu.trainChance(10, muffalo) == doctest::Approx(1.0));
    CHECK(hu.trainChance(0, muffalo) == doctest::Approx(0.5));
    CHECK(hu.stepsNeeded(Trainable::Obedience) == 10);
    CHECK(hu.stepsNeeded(Trainable::Haul) == 30);

    // Wild animals can't be trained, and a tame one only learns what its kind can.
    CHECK_FALSE(hu.canLearn(muffalo, Trainable::Obedience));
    f.addColonist(CellPos{10, 10, 0}, 20);
    HUSB_REQUIRE(f.tameNow(muffalo));
    HUSB_REQUIRE(f.tameNow(husky));
    CHECK(hu.canLearn(muffalo, Trainable::Obedience));
    CHECK_FALSE(hu.canLearn(muffalo, Trainable::Haul));  // Simple trainability, Haul needs Advanced
    CHECK(hu.canLearn(husky, Trainable::Haul));
    std::string error;
    CHECK_FALSE(f.hu().setTraining(muffalo, Trainable::Haul, true, error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("Husbandry: marking only works on the right animals, and a colonist tames a marked one with hay") {
    HusbFixture f;
    const Entity pawn = f.addColonist(CellPos{10, 10, 0}, 10);
    const Entity muffalo = f.animal("Muffalo", CellPos{22, 10, 0});
    f.spawn("Hay", CellPos{12, 10, 0}, 100);
    HUSB_REQUIRE(muffalo != kNoEntity);
    const int hayBefore = f.itemsOnMap("Hay");

    std::string error;
    CHECK_FALSE(f.hu().markSlaughter(muffalo, true, error));  // not tame
    CHECK_FALSE(f.hu().markTame(pawn, true, error));          // not an animal
    HUSB_REQUIRE(f.hu().markTame(muffalo, true, error));
    CHECK(f.hu().care(muffalo)->markTame);

    // Without the mark nothing happens; with it the colonist fetches hay, goes to the animal and keeps at it until it works.
    bool tamedEvent = false;
    const bool done = f.runUntil(30000, [&] {
        for (const Event& e : f.sim.events()) {
            tamedEvent = tamedEvent || (e.type == EventType::AnimalTamed && e.thing == muffalo && e.target == pawn);
        }
        return f.hu().isTame(muffalo);
    });
    HUSB_REQUIRE(done);
    CHECK(tamedEvent);
    CHECK(f.reg().get<Animal>(muffalo).wild == false);
    CHECK(f.reg().get<Faction>(muffalo).id == kPlayerFaction);
    CHECK(f.hu().stats().tamed == 1);
    CHECK(f.itemsOnMap("Hay") < hayBefore);  // the animal ate what was brought
    CHECK(f.hu().care(muffalo)->hasHome);
    CHECK_FALSE(f.hu().care(muffalo)->markTame);
    // The Animals skill grew from the work.
    const SkillRecord* skill = f.reg().get<Colonist>(pawn).skill("Animals");
    HUSB_REQUIRE(skill != nullptr);
    CHECK((skill->level > 10 || skill->xp > 0.0f));
    Snapshot snap;
    f.sim.writeSnapshot(snap);
    const ThingView* view = snap.find(muffalo);
    HUSB_REQUIRE(view != nullptr);
    CHECK(view->faction == kPlayerFaction);
}

TEST_CASE("Husbandry: a failed attempt keeps the mark and a later one can still work") {
    HusbFixture f;
    f.addColonist(CellPos{10, 10, 0}, 0);
    const Entity boar = f.animal("WildBoar", CellPos{20, 20, 0});
    HUSB_REQUIRE(boar != kNoEntity);
    std::string error;
    HUSB_REQUIRE(f.hu().markTame(boar, true, error));
    int tries = 0;
    while (!f.hu().isTame(boar) && tries < 400) {
        f.hu().finishTame(f.colonist, boar, 0);  // 0.5 x 0.4 = 20% a try
        ++tries;
        f.sim.step();  // the roll comes from the animal and the second
    }
    HUSB_REQUIRE(f.hu().isTame(boar));
    CHECK(tries > 1);  // it took more than one go (with this seed; the chance is a fifth)
    CHECK(f.hu().stats().tameFailed == static_cast<uint64_t>(tries - 1));
    CHECK(f.hu().stats().tamed == 1);

    // A failed try leaves the mark on.
    const Entity cat = f.animal("Cat", CellPos{30, 30, 0});
    HUSB_REQUIRE(f.hu().markTame(cat, true, error));
    for (int i = 0; i < 100 && f.hu().stats().tameFailed == static_cast<uint64_t>(tries - 1); ++i) {
        f.hu().finishTame(f.colonist, cat, 0);
        f.sim.step();
    }
    if (!f.hu().isTame(cat)) {
        CHECK(f.hu().care(cat)->markTame);
        CHECK(f.hu().stats().tameFailed == static_cast<uint64_t>(tries));
    }
    // Taking the mark off works.
    const Entity deer = f.animal("Deer", CellPos{40, 40, 0});
    HUSB_REQUIRE(f.hu().markTame(deer, true, error));
    HUSB_REQUIRE(f.hu().markTame(deer, false, error));
    CHECK_FALSE(f.hu().care(deer)->markTame);
}

TEST_CASE("Husbandry: training takes steps in order, waits between attempts, fades, and Haul makes a worker") {
    HusbFixture f;
    f.addColonist(CellPos{10, 10, 0}, 20);
    const Entity husky = f.animal("Husky", CellPos{20, 20, 0});
    HUSB_REQUIRE(f.tameNow(husky));
    Husbandry& hu = f.hu();
    std::string error;

    // Wanting Haul also wants what it needs first.
    HUSB_REQUIRE(hu.setTraining(husky, Trainable::Haul, true, error));
    CHECK((hu.wantedMask(husky) & (1u << static_cast<int>(Trainable::Obedience))) != 0);
    CHECK((hu.wantedMask(husky) & (1u << static_cast<int>(Trainable::Haul))) != 0);
    Trainable next = Trainable::Haul;
    HUSB_REQUIRE(hu.nextToTrain(husky, next));
    CHECK(next == Trainable::Obedience);
    CHECK(hu.trainingDue(husky));

    HUSB_REQUIRE(hu.finishTrain(f.colonist, husky, Trainable::Obedience, 20));
    CHECK(hu.steps(husky, Trainable::Obedience) == 1);
    CHECK_FALSE(hu.trainingDue(husky));  // the next attempt waits
    f.sim.stepMany(6 * kSecondsPerHour + 10);
    CHECK(hu.trainingDue(husky));

    for (int i = 0; i < 9; ++i) {
        hu.finishTrain(f.colonist, husky, Trainable::Obedience, 20);
    }
    CHECK(hu.learned(husky, Trainable::Obedience));
    HUSB_REQUIRE(hu.nextToTrain(husky, next));
    CHECK(next == Trainable::Haul);
    CHECK_FALSE(f.reg().all_of<Worker>(husky));
    for (int i = 0; i < 30; ++i) {
        hu.finishTrain(f.colonist, husky, Trainable::Haul, 20);
    }
    CHECK(hu.learned(husky, Trainable::Haul));
    CHECK_FALSE(hu.nextToTrain(husky, next));
    CHECK(f.reg().all_of<Worker>(husky));  // a hauler of the job system
    CHECK(f.hu().stats().trainingSteps == 40);

    // Taking the training away stops the want, but not what is learned.
    HUSB_REQUIRE(hu.setTraining(husky, Trainable::Haul, false, error));
    CHECK(hu.learned(husky, Trainable::Haul));
}

TEST_CASE("Husbandry: unfinished training fades over days, learned training stays") {
    HusbFixture f;
    f.addColonist(CellPos{10, 10, 0}, 20);
    const Entity muffalo = f.animal("Muffalo", CellPos{20, 20, 0});
    HUSB_REQUIRE(f.tameNow(muffalo));
    std::string error;
    HUSB_REQUIRE(f.hu().setTraining(muffalo, Trainable::Obedience, true, error));
    for (int i = 0; i < 4; ++i) {
        f.hu().finishTrain(f.colonist, muffalo, Trainable::Obedience, 20);
    }
    CHECK(f.hu().steps(muffalo, Trainable::Obedience) == 4);
    // Keep it fed through a week; training then loses a step every `decayDays`.
    for (int hour = 0; hour < 24 * 7; ++hour) {
        f.sim.animals().addFood(muffalo, 5.0f);
        f.sim.stepMany(kSecondsPerHour);
    }
    CHECK(f.hu().steps(muffalo, Trainable::Obedience) < 4);
    CHECK(f.hu().steps(muffalo, Trainable::Obedience) >= 0);
}

TEST_CASE("Husbandry: a colonist slaughters a marked tame animal") {
    HusbFixture f;
    const Entity pawn = f.addColonist(CellPos{10, 10, 0}, 8);
    const Entity muffalo = f.animal("Muffalo", CellPos{18, 10, 0});
    HUSB_REQUIRE(f.tameNow(muffalo));
    std::string error;
    HUSB_REQUIRE(f.hu().markSlaughter(muffalo, true, error));
    bool event = false;
    const bool done = f.runUntil(5000, [&] {
        for (const Event& e : f.sim.events()) {
            event = event || (e.type == EventType::AnimalSlaughtered && e.thing == muffalo && e.target == pawn);
        }
        return f.dead(muffalo);
    });
    HUSB_REQUIRE(done);
    CHECK(event);
    CHECK(f.hu().stats().slaughtered == 1);
    CHECK_FALSE(f.hu().finishSlaughter(pawn, muffalo));  // already dead
}

TEST_CASE("Husbandry: a tame animal eats hay, a hungry one far from food is fed by hand, and one with none starves") {
    HusbFixture f;
    f.addColonist(CellPos{10, 10, 0}, 10);
    const Entity cow = f.animal("Cow", CellPos{20, 20, 0});
    HUSB_REQUIRE(f.tameNow(cow));
    f.reg().get<Animal>(cow).food = 0.0f;
    f.spawn("Hay", CellPos{24, 20, 0}, 40);
    const int hay = f.itemsOnMap("Hay");
    CHECK(f.runUntil(3000, [&] { return f.sim.animals().foodShare(cow) > 0.2f; }));
    CHECK(f.itemsOnMap("Hay") < hay);

    // Out of the animal's own reach, the Handling colonist carries it food.
    const Entity cow2 = f.animal("Cow", CellPos{50, 50, 0});
    HUSB_REQUIRE(f.tameNow(cow2));
    f.reg().get<Animal>(cow2).food = 0.0f;
    CHECK(f.runUntil(6000, [&] { return f.sim.animals().foodShare(cow2) > 0.2f; }));
}

TEST_CASE("Husbandry: a tame animal with no food at all starves after the grace period") {
    HusbFixture f;
    const Entity pawn = f.addColonist(CellPos{10, 10, 0}, 10);
    const Entity cow = f.animal("Cow", CellPos{20, 20, 0});
    HUSB_REQUIRE(f.tameNow(cow));
    f.sim.things().destroy(pawn);  // nobody to hand-feed it
    f.reg().get<Animal>(cow).food = 0.0f;
    bool event = false;
    const bool done = f.runUntil(7 * 86400, [&] {
        for (const Event& e : f.sim.events()) {
            event = event || (e.type == EventType::AnimalStarved && e.thing == cow);
        }
        return f.dead(cow);
    });
    HUSB_REQUIRE(done);
    CHECK(event);
    CHECK(f.hu().stats().starved == 1);
    CHECK(f.sim.time().seconds > 4 * 86400);  // not at once
}

TEST_CASE("Husbandry: zones are painted on walkable cells, never overlap, and shrink") {
    HusbFixture f;
    const AnimalZoneId a = f.hu().createZone(CellRect{10, 10, 5, 5});
    HUSB_REQUIRE(a != kNoAnimalZone);
    CHECK(f.hu().findZone(a)->cells.size() == 25);
    // A second zone over the same cells gets only the new ones.
    const AnimalZoneId b = f.hu().createZone(CellRect{12, 12, 5, 5});
    HUSB_REQUIRE(b != kNoAnimalZone);
    CHECK(f.hu().findZone(b)->cells.size() == 25 - 9);
    CHECK(f.hu().zoneAt(CellPos{12, 12, 0}) == a);
    CHECK(f.hu().zoneAt(CellPos{16, 16, 0}) == b);
    CHECK(f.hu().createZone(CellRect{10, 10, 5, 5}) == kNoAnimalZone);  // nothing free
    CHECK(f.hu().removeZoneCells(CellRect{10, 10, 5, 5}) == 25);
    CHECK(f.hu().findZone(a) == nullptr);  // emptied zones go
    CHECK(f.hu().zoneAt(CellPos{12, 12, 0}) == kNoAnimalZone);
}

TEST_CASE("Husbandry: an animal assigned to a zone stays in it, and walks round walls to reach it") {
    HusbFixture f;
    f.addColonist(CellPos{5, 5, 0}, 10);
    // A pen: walls around cells 30..36, with a gap at (33, 29).
    for (int i = 29; i <= 37; ++i) {
        for (int j = 29; j <= 37; ++j) {
            const bool edge = i == 29 || i == 37 || j == 29 || j == 37;
            if (edge && !(i == 33 && j == 29)) {
                f.sim.things().spawn("Wall", CellPos{i, j, 0});
            }
        }
    }
    f.sim.stepMany(5);
    const AnimalZoneId zone = f.hu().createZone(CellRect{30, 30, 7, 7});
    HUSB_REQUIRE(zone != kNoAnimalZone);
    CHECK_FALSE(f.hu().isPen(zone));  // the gap

    const Entity cow = f.animal("Cow", CellPos{33, 22, 0});
    HUSB_REQUIRE(f.tameNow(cow));
    std::string error;
    HUSB_REQUIRE(f.hu().assignZone(cow, zone, error));
    f.reg().get<Animal>(cow).food = 1.0f;
    const bool in = f.runUntil(4000, [&] {
        f.sim.animals().addFood(cow, 0.5f);
        return f.hu().zoneAt(f.cellOfThing(cow)) == zone;
    });
    CHECK(in);

    // Shut the gap: the zone is a pen now, and the cow stays inside it.
    f.sim.things().spawn("Wall", CellPos{33, 29, 0});
    f.sim.stepMany(5);
    CHECK(f.hu().isPen(zone));
    int outside = 0;
    for (int i = 0; i < 3000; ++i) {
        f.sim.animals().addFood(cow, 0.01f);
        f.sim.step();
        outside += f.hu().zoneAt(f.cellOfThing(cow)) == zone ? 0 : 1;
    }
    CHECK(outside == 0);
    CHECK_FALSE(f.hu().assignZone(cow, 9999, error));  // no such zone
}

TEST_CASE("Husbandry: releasing sends a tame animal back to the wild") {
    HusbFixture f;
    f.addColonist(CellPos{10, 10, 0}, 20);
    const Entity husky = f.animal("Husky", CellPos{20, 20, 0});
    HUSB_REQUIRE(f.tameNow(husky));
    std::string error;
    HUSB_REQUIRE(f.hu().setTraining(husky, Trainable::Haul, true, error));
    for (int i = 0; i < 40; ++i) {
        f.hu().finishTrain(f.colonist, husky, i < 10 ? Trainable::Obedience : Trainable::Haul, 20);
    }
    HUSB_REQUIRE(f.reg().all_of<Worker>(husky));
    const AnimalZoneId zone = f.hu().createZone(CellRect{18, 18, 5, 5});
    HUSB_REQUIRE(f.hu().assignZone(husky, zone, error));

    HUSB_REQUIRE(f.hu().release(husky, error));
    CHECK(f.reg().get<Animal>(husky).wild);
    CHECK(f.reg().get<Faction>(husky).id == kWildFaction);
    CHECK_FALSE(f.reg().all_of<Worker>(husky));
    CHECK_FALSE(f.hu().isTame(husky));
    CHECK(f.hu().care(husky)->zone == kNoAnimalZone);
    CHECK_FALSE(f.hu().learned(husky, Trainable::Obedience));
    CHECK_FALSE(f.hu().release(husky, error));  // already wild
}

TEST_CASE("Husbandry: the commands work, fail with a reason, and replay to the same game") {
    HusbFixture f;
    f.addColonist(CellPos{10, 10, 0}, 20);
    const Entity muffalo = f.animal("Muffalo", CellPos{20, 20, 0});
    const Entity husky = f.animal("Husky", CellPos{30, 20, 0});
    HUSB_REQUIRE(f.tameNow(husky));
    f.sim.submit(MarkAnimalCommand{muffalo, "Tame", true});
    f.sim.submit(MarkAnimalCommand{husky, "Slaughter", true});
    f.sim.submit(MarkAnimalCommand{husky, "Slaughter", false});
    f.sim.submit(SetTrainingCommand{husky, "Haul", true});
    f.sim.submit(CreateAnimalZoneCommand{CellRect{40, 40, 5, 5}});
    f.sim.step();
    CHECK_FALSE(f.sawEvent(EventType::CommandFailed));
    CHECK(f.hu().care(muffalo)->markTame);
    CHECK(f.hu().wantedMask(husky) != 0);
    const AnimalZoneId zone = f.hu().zoneAt(CellPos{40, 40, 0});
    HUSB_REQUIRE(zone != kNoAnimalZone);
    f.sim.submit(AssignAnimalZoneCommand{husky, zone});
    f.sim.submit(MarkAnimalCommand{muffalo, "Slaughter", true});  // wild: fails
    f.sim.submit(SetTrainingCommand{muffalo, "Haul", true});      // wild: fails
    f.sim.submit(SetTrainingCommand{husky, "Fly", true});         // no such trick: fails
    f.sim.submit(MarkAnimalCommand{husky, "Dance", true});        // no such mark: fails
    f.sim.step();
    int failed = 0;
    for (const Event& e : f.sim.events()) {
        if (e.type == EventType::CommandFailed) {
            ++failed;
            CHECK_FALSE(e.message.empty());
        }
    }
    CHECK(failed == 4);
    CHECK(f.hu().care(husky)->zone == zone);
    f.sim.submit(RemoveAnimalZoneCellsCommand{CellRect{40, 40, 5, 5}});
    f.sim.step();
    CHECK(f.hu().findZone(zone) == nullptr);
    CHECK(f.hu().care(husky)->zone == kNoAnimalZone);
    f.sim.submit(MarkAnimalCommand{husky, "Release", true});
    f.sim.stepMany(2000);

    // Every command writes to the replay file and reads back.
    const Replay recorded = recordReplay(f.sim);
    Replay loaded;
    std::string error;
    HUSB_REQUIRE(readReplay(writeReplay(recorded), loaded, error));
    CHECK(loaded.commands.size() == recorded.commands.size());
    for (const char* name : {"MarkAnimal", "SetTraining", "CreateAnimalZone", "RemoveAnimalZoneCells", "AssignAnimalZone"}) {
        const bool seen = std::any_of(loaded.commands.begin(), loaded.commands.end(),
                                      [&](const auto& c) { return std::string(commandName(c.command)) == name; });
        CHECK_MESSAGE(seen, name);
    }
}

TEST_CASE("Husbandry: saving and loading keeps zones, marks, training and the game going the same") {
    HusbFixture f;
    f.addColonist(CellPos{10, 10, 0}, 20);
    const Entity husky = f.animal("Husky", CellPos{20, 20, 0});
    const Entity muffalo = f.animal("Muffalo", CellPos{30, 20, 0});
    HUSB_REQUIRE(f.tameNow(husky));
    std::string error;
    HUSB_REQUIRE(f.hu().setTraining(husky, Trainable::Haul, true, error));
    for (int i = 0; i < 5; ++i) {
        f.hu().finishTrain(f.colonist, husky, Trainable::Obedience, 20);
    }
    const AnimalZoneId zone = f.hu().createZone(CellRect{40, 40, 7, 7});
    HUSB_REQUIRE(f.hu().assignZone(husky, zone, error));
    HUSB_REQUIRE(f.hu().markTame(muffalo, true, error));
    f.spawn("Hay", CellPos{14, 10, 0}, 60);
    f.sim.stepMany(1500);

    const std::vector<uint8_t> bytes = saveSim(f.sim);
    SaveLoadResult loaded = loadSim(bytes, husbDefs());
    HUSB_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.error.empty());
    CHECK(loaded.warnings.empty());
    Sim& copy = *loaded.sim;
    CHECK(copy.stateHash() == f.sim.stateHash());
    CHECK(saveSim(copy) == bytes);
    CHECK(saveToJson(f.sim).find("\"husbandry\"") != std::string::npos);
    CHECK(copy.husbandry().zones().size() == 1);
    CHECK(copy.husbandry().steps(husky, Trainable::Obedience) == f.hu().steps(husky, Trainable::Obedience));
    CHECK(copy.husbandry().care(husky)->zone == zone);
    CHECK(copy.husbandry().care(muffalo)->markTame);
    CHECK(copy.husbandry().isPen(zone) == f.hu().isPen(zone));

    // On from the save the two games stay the same, through taming, walking to the zone and feeding.
    f.sim.stepMany(8000);
    copy.stepMany(8000);
    CHECK(copy.stateHash() == f.sim.stateHash());
    CHECK(copy.husbandry().stats().tamed == f.hu().stats().tamed);
    CHECK(saveSim(copy) == saveSim(f.sim));
}

TEST_CASE("Husbandry: a game that never touches animal care hashes the same with or without the feature") {
    // No animal care record is made for an animal nobody marked, so the husbandry state of an untouched game is empty.
    HusbFixture f;
    f.animal("Deer", CellPos{20, 20, 0});
    f.sim.stepMany(2000);
    CHECK(f.reg().view<AnimalCare>().empty());
    CHECK(f.hu().zones().empty());
    CHECK(f.hu().stats().tamed == 0);
}
