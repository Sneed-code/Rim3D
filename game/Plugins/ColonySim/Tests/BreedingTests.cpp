// Breeding, young animals, eggs, milk and wool (task N4, docs/design/breeding.md).
#include "ColonySim/Animals.h"
#include "ColonySim/Breeding.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Health.h"
#include "ColonySim/Husbandry.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define BREED_REQUIRE(...)     \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> breedDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

SimSettings breedSettings(int size) {
    SimSettings s;
    s.seed = 5;
    s.mapSize = size;
    s.wildAnimals = 0;
    s.flatMap = true;
    s.checkpointEvery = 0;
    return s;
}

struct BreedFixture {
    Sim sim;
    Entity colonist = kNoEntity;

    explicit BreedFixture(int size = 64) : sim(breedSettings(size), breedDefs()) {
        for (int y = 0; y < sim.map().height(); ++y) {
            for (int x = 0; x < sim.map().width(); ++x) {
                sim.plants().remove(CellPos{x, y, 0});
            }
        }
        sim.stepMany(9 * kSecondsPerHour);  // daytime: animals are awake
    }

    // Every breeding event so far (the sim clears its own list each step).
    std::vector<Event> seen;

    // Steps, keeping the events this test is about.
    void run(int64_t steps) {
        for (int64_t i = 0; i < steps; ++i) {
            sim.step();
            for (const Event& e : sim.events()) {
                if (e.type == EventType::AnimalMated || e.type == EventType::AnimalBorn || e.type == EventType::EggLaid ||
                    e.type == EventType::EggHatched || e.type == EventType::AnimalGathered) {
                    seen.push_back(e);
                }
            }
        }
    }
    // Steps `steps` seconds, feeding every animal each hour so none starves.
    void runFed(int64_t steps) {
        while (steps > 0) {
            feedAll();
            const int64_t n = std::min<int64_t>(steps, kSecondsPerHour);
            run(n);
            steps -= n;
        }
    }
    Breeding& br() { return sim.breeding(); }
    entt::registry& reg() { return sim.things().registry(); }
    DefId def(const char* name) const { return sim.defs().idOf("Things", name); }

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

    // A tame grown-up animal (its def's last life stage), full of food, at a cell. `wantFemale`: 1 female, 0 male, -1 either;
    // sexes are fixed by the entity, so animals are made until one comes out right, and the others are removed again.
    Entity tame(const char* defName, CellPos cell, int wantFemale = -1) {
        for (int tries = 0; tries < 60; ++tries) {
            const Entity e = sim.things().spawn(defName, cell);
            if (e == kNoEntity) {
                return kNoEntity;
            }
            if (wantFemale >= 0 && (br().sexOf(e) == AnimalSex::Female) != (wantFemale == 1)) {
                sim.things().destroy(e);
                continue;
            }
            Animal& a = reg().get<Animal>(e);
            a.wild = false;
            a.bornAt = -400 * kSecondsPerDay;
            reg().emplace_or_replace<Faction>(e, Faction{kPlayerFaction});
            AnimalCare* care = sim.husbandry().careFor(e);
            care->hasHome = true;
            care->home = cell;
            care->tamedAt = sim.time().seconds;
            sim.stepMany(5);  // its first look sets it up
            feed(e);
            return e;
        }
        return kNoEntity;
    }
    void feed(Entity e) { sim.animals().addFood(e, 50.0f); }
    void feedAll() {
        std::vector<Entity> all;
        for (const Entity e : reg().view<const Animal>()) {
            all.push_back(e);
        }
        for (const Entity e : all) {
            feed(e);
        }
    }
    int itemsOnMap(const char* name) {
        const DefId id = def(name);
        int total = 0;
        reg().view<const Thing, const StackCount, const Position>().each(
            [&](const Thing& thing, const StackCount& stack, const Position&) { total += thing.def == id ? stack.count : 0; });
        return total;
    }
    int livingAnimals(const char* name) {
        const DefId id = def(name);
        int total = 0;
        for (const auto [e, t] : reg().view<const Thing>().each()) {
            if (t.def == id && reg().all_of<Animal>(e) && !reg().get<Health>(e).dead) {
                ++total;
            }
        }
        return total;
    }
    int countEvents(EventType type) const {
        return static_cast<int>(std::count_if(seen.begin(), seen.end(), [&](const Event& e) { return e.type == type; }));
    }
    template <typename F>
    bool runUntil(int limit, F done) {
        for (int i = 0; i < limit; ++i) {
            if (done()) {
                return true;
            }
            run(1);
        }
        return done();
    }
};

}  // namespace

TEST_CASE("Breeding: every animal is male or female for good, about half and half") {
    BreedFixture f;
    int females = 0;
    std::vector<Entity> animals;
    for (int i = 0; i < 200; ++i) {
        const Entity e = f.sim.things().spawn("Cow", CellPos{10 + i % 40, 10 + i / 40, 0});
        BREED_REQUIRE(e != kNoEntity);
        animals.push_back(e);
        females += f.br().female(e) ? 1 : 0;
    }
    CHECK(females > 70);
    CHECK(females < 130);
    f.run(100);
    for (const Entity e : animals) {
        CHECK((f.br().sexOf(e) == AnimalSex::Female) == f.br().female(e));
    }
    Snapshot snap;
    f.sim.writeSnapshot(snap);
    const ThingView* view = snap.find(animals[0]);
    BREED_REQUIRE(view != nullptr);
    CHECK(view->sex == (f.br().female(animals[0]) ? 1 : 0));
}

TEST_CASE("Breeding: a pregnant mother gives birth to a litter of tame young that grow up") {
    BreedFixture f;
    const Entity boar = f.tame("WildBoar", CellPos{20, 20, 0}, 1);
    BREED_REQUIRE(boar != kNoEntity);
    Husbandry& hu = f.sim.husbandry();
    const AnimalZoneId zone = hu.createZone(CellRect{15, 15, 12, 12});
    std::string error;
    BREED_REQUIRE(hu.assignZone(boar, zone, error));

    CHECK_FALSE(f.br().makePregnant(f.sim.things().spawn("WildBoar", CellPos{30, 30, 0}), 5, 1000));  // a wild one can't
    BREED_REQUIRE(f.br().makePregnant(boar, 5, f.sim.time().seconds + 200));
    CHECK(f.br().pregnant(boar));
    f.run(100);
    CHECK(f.livingAnimals("WildBoar") == 2);  // (the wild one is still there; no young yet)
    f.run(200);
    BREED_REQUIRE(f.livingAnimals("WildBoar") == 2 + 5);
    CHECK_FALSE(f.br().pregnant(boar));
    CHECK(f.br().stats().born == 5);
    CHECK(f.countEvents(EventType::AnimalBorn) == 5);

    // The young are babies, tame, in the mother's zone and her colony.
    int babies = 0;
    for (const auto [e, a] : f.reg().view<Animal>().each()) {
        if (e == boar || a.wild) {
            continue;
        }
        ++babies;
        CHECK(f.sim.animals().lifeStage(e) == 0);
        CHECK_FALSE(f.sim.animals().isAdult(e));
        CHECK(f.reg().get<Faction>(e).id == kPlayerFaction);
        CHECK(hu.care(e)->zone == zone);
        CHECK(f.sim.animals().bodySize(e) < f.sim.animals().bodySize(boar));
        CHECK_FALSE(f.br().matureTame(e));
        CHECK(f.br().fullness(e, AnimalProduct::Wool) < 0.0f);
    }
    CHECK(babies == 5);

    // A day's wait is not enough for a boar to grow up (0.4 years); growing takes the life stages' ages.
    Entity baby = kNoEntity;
    for (const auto [e, a] : f.reg().view<Animal>().each()) {
        if (e != boar && !a.wild) {
            baby = e;
            break;
        }
    }
    BREED_REQUIRE(baby != kNoEntity);
    f.run(kSecondsPerDay);
    CHECK(f.sim.animals().lifeStage(baby) < 2);
    Animal& a = f.reg().get<Animal>(baby);
    a.bornAt -= 6 * kSecondsPerDay;  // 0.1 years: a juvenile
    CHECK(f.sim.animals().lifeStage(baby) == 1);
    a.bornAt -= 30 * kSecondsPerDay;  // 0.4 years and more: grown
    CHECK(f.sim.animals().lifeStage(baby) == 2);
    CHECK(f.sim.animals().isAdult(baby));
    CHECK(f.br().matureTame(baby));
}

TEST_CASE("Breeding: tame animals of a kind mate, and a mating can leave the female pregnant") {
    BreedFixture f;
    std::vector<Entity> cows;
    for (int i = 0; i < 6; ++i) {
        cows.push_back(f.tame("Cow", CellPos{20 + i, 20, 0}, 0));
        cows.push_back(f.tame("Cow", CellPos{20 + i, 22, 0}, 1));
    }
    f.tame("Husky", CellPos{23, 21, 0}, 0);  // a male of another kind never mates with a cow
    for (const Entity e : cows) {
        BREED_REQUIRE(e != kNoEntity);
    }
    bool mated = false;
    bool pregnantEvent = false;
    bool sawBadPair = false;
    for (int hour = 0; hour < 24 * 3 && f.br().stats().pregnancies < 2; ++hour) {
        f.feedAll();
        f.run(kSecondsPerHour);
        for (const Event& e : f.seen) {
            if (e.type == EventType::AnimalMated) {
                mated = true;
                pregnantEvent = pregnantEvent || e.message == "pregnant";
                sawBadPair = sawBadPair || f.reg().get<Thing>(e.thing).def != f.reg().get<Thing>(e.target).def ||
                             f.br().female(e.thing) || !f.br().female(e.target);
            }
        }
    }
    CHECK(mated);
    CHECK(pregnantEvent);
    CHECK_FALSE(sawBadPair);
    CHECK(f.br().stats().matings >= 1);
    CHECK(f.br().stats().pregnancies >= 1);
    int pregnant = 0;
    for (const Entity e : cows) {
        pregnant += f.br().pregnant(e) ? 1 : 0;
        if (f.br().pregnant(e)) {
            CHECK(f.br().female(e));
            // A cow's gestation is 12 days and she carries one calf.
            CHECK(f.br().breed(e)->litter == 1);
            CHECK(f.br().breed(e)->pregnantUntil > f.sim.time().seconds + 8 * kSecondsPerDay);
        }
    }
    CHECK(pregnant >= 1);
}

TEST_CASE("Breeding: wild animals and hungry ones do not breed, fed tame ones do") {
    BreedFixture f;
    // A wild pair.
    f.sim.things().spawn("Cow", CellPos{20, 20, 0});
    f.sim.things().spawn("Cow", CellPos{21, 20, 0});
    // A tame pair, the cow kept hungry.
    const Entity bull = f.tame("Cow", CellPos{40, 40, 0}, 0);
    const Entity cow = f.tame("Cow", CellPos{41, 40, 0}, 1);
    BREED_REQUIRE(bull != kNoEntity);
    BREED_REQUIRE(cow != kNoEntity);
    for (int hour = 0; hour < 24 * 3; ++hour) {
        f.feed(bull);
        Animal& a = f.reg().get<Animal>(cow);
        a.food = 0.0f;  // starving takes five days; mating needs 30% fed
        a.foodAt = f.sim.time().seconds;
        f.run(kSecondsPerHour);
    }
    CHECK(f.br().stats().matings == 0);
    CHECK_FALSE(f.br().pregnant(cow));
    // Fed, the same pair mates within days.
    for (int hour = 0; hour < 24 * 6 && f.br().stats().matings == 0; ++hour) {
        f.feedAll();
        f.run(kSecondsPerHour);
    }
    CHECK(f.br().stats().matings >= 1);
    CHECK(f.countEvents(EventType::AnimalMated) >= 1);
}

TEST_CASE("Breeding: milk and wool fill over days, a colonist gathers them, and only grown females give milk") {
    BreedFixture f;
    const Entity cow = f.tame("Cow", CellPos{20, 20, 0}, 1);
    const Entity bull = f.tame("Cow", CellPos{22, 20, 0}, 0);
    const Entity muffalo = f.tame("Yak", CellPos{24, 20, 0}, 1);
    const Entity sheep = f.tame("Sheep", CellPos{26, 20, 0}, 1);
    const Entity wildCow = f.sim.things().spawn("Cow", CellPos{40, 40, 0});
    BREED_REQUIRE(cow != kNoEntity);
    BREED_REQUIRE(bull != kNoEntity);
    BREED_REQUIRE(muffalo != kNoEntity);
    f.run(120);  // the pass starts the fill
    CHECK(f.br().fullness(cow, AnimalProduct::Milk) >= 0.0f);
    CHECK(f.br().fullness(cow, AnimalProduct::Milk) < 0.1f);
    CHECK(f.br().fullness(bull, AnimalProduct::Milk) < 0.0f);     // a bull gives none
    CHECK(f.br().fullness(cow, AnimalProduct::Wool) < 0.0f);      // a cow has no wool
    CHECK(f.br().fullness(muffalo, AnimalProduct::Wool) < 0.0f);  // a yak is milked, not shorn
    CHECK(f.br().fullness(sheep, AnimalProduct::Wool) >= 0.0f);
    CHECK(f.br().fullness(sheep, AnimalProduct::Milk) < 0.0f);    // a sheep is shorn, not milked
    CHECK(f.br().fullness(wildCow, AnimalProduct::Milk) < 0.0f);  // wild ones give none
    CHECK_FALSE(f.br().readyToGather(cow, AnimalProduct::Milk));

    // A cow's milk is full after a day and gives 14.
    f.feed(cow);
    f.run(kSecondsPerDay);
    CHECK(f.br().fullness(cow, AnimalProduct::Milk) == doctest::Approx(1.0f));
    CHECK(f.br().readyToGather(cow, AnimalProduct::Milk));
    CHECK(f.br().productAmount(cow, AnimalProduct::Milk) == 14);
    CHECK(f.br().productAmount(sheep, AnimalProduct::Wool) == 45);
    CHECK_FALSE(f.br().readyToGather(sheep, AnimalProduct::Wool));  // ten days to go

    // The yak's milk (11) is full after the same day: the colonist milks both.
    CHECK(f.br().productAmount(muffalo, AnimalProduct::Milk) == 11);
    const Entity pawn = f.addColonist(CellPos{10, 20, 0}, 8);
    f.feedAll();
    const int milkBefore = f.itemsOnMap("MilkRaw");
    bool cowEvent = false;
    bool muffaloEvent = false;
    const bool done = f.runUntil(8000, [&] {
        for (const Event& e : f.seen) {
            if (e.type == EventType::AnimalGathered && e.target == pawn && e.message == "milk") {
                cowEvent = cowEvent || (e.thing == cow && e.amount == doctest::Approx(14.0f));
                muffaloEvent = muffaloEvent || (e.thing == muffalo && e.amount == doctest::Approx(11.0f));
            }
        }
        return f.br().stats().milked >= 2;
    });
    BREED_REQUIRE(done);
    CHECK(cowEvent);
    CHECK(muffaloEvent);
    CHECK(f.itemsOnMap("MilkRaw") - milkBefore == 25);
    CHECK(f.br().fullness(cow, AnimalProduct::Milk) < 0.2f);  // it starts to fill again
    CHECK_FALSE(f.br().readyToGather(cow, AnimalProduct::Milk));
    const SkillRecord* skill = f.reg().get<Colonist>(pawn).skill("Animals");
    BREED_REQUIRE(skill != nullptr);
    CHECK((skill->level > 8 || skill->xp > 0.0f));

    // Shearing: the wool is full after 10 days (shifted back here to save the waiting).
    f.reg().get<AnimalBreed>(sheep).woolSince -= 10 * kSecondsPerDay;
    CHECK(f.br().readyToGather(sheep, AnimalProduct::Wool));
    const int woolBefore = f.itemsOnMap("WoolSheep");
    const bool sheared = f.runUntil(5000, [&] {
        f.feedAll();
        return f.br().stats().sheared >= 1;
    });
    BREED_REQUIRE(sheared);
    CHECK(f.itemsOnMap("WoolSheep") - woolBefore == 45);
    CHECK(f.br().fullness(sheep, AnimalProduct::Wool) < 0.2f);
}

TEST_CASE("Breeding: nobody gathers unless the animal is full, and a young animal gives nothing") {
    BreedFixture f;
    const Entity cow = f.tame("Cow", CellPos{20, 20, 0}, 1);
    BREED_REQUIRE(cow != kNoEntity);
    f.addColonist(CellPos{10, 20, 0}, 8);
    f.feed(cow);
    f.run(6 * kSecondsPerHour);
    CHECK(f.br().stats().milked == 0);  // not full yet

    // A calf: grown cows give milk, calves do not, whatever time has passed since it was born.
    const Entity calf = f.tame("Cow", CellPos{25, 20, 0}, 1);
    BREED_REQUIRE(calf != kNoEntity);
    f.reg().get<Animal>(calf).bornAt = f.sim.time().seconds;
    f.run(3 * kSecondsPerDay / 2);
    CHECK(f.br().fullness(calf, AnimalProduct::Milk) < 0.0f);
    CHECK_FALSE(f.sim.animals().mayWork(calf));
}

TEST_CASE("Breeding: a hen lays eggs, a fertilized egg hatches into a tame chick, and an unfertilized one never does") {
    BreedFixture f;
    const Entity hen = f.tame("Chicken", CellPos{20, 20, 0}, 1);
    BREED_REQUIRE(hen != kNoEntity);
    f.run(120);
    CHECK(f.br().laysEggs(hen));

    // One egg a day.
    f.runFed(kSecondsPerDay / 2);
    CHECK(f.itemsOnMap("EggChickenUnfertilized") == 0);
    f.runFed(kSecondsPerDay / 2 + 200);
    CHECK(f.itemsOnMap("EggChickenUnfertilized") == 1);
    CHECK(f.br().stats().eggsLaid == 1);
    CHECK(f.countEvents(EventType::EggLaid) == 1);
    f.runFed(5 * kSecondsPerDay);
    CHECK(f.br().stats().eggsLaid >= 4);  // (eggs rot away after about five days)
    CHECK(f.livingAnimals("Chicken") == 1);  // unfertilized eggs stay eggs
    CHECK(f.itemsOnMap("EggChickenFertilized") == 0);

    // A fertilized one carries a hatch time and turns into a chick on the spot.
    const Entity egg = f.br().layEgg(hen, true);
    BREED_REQUIRE(egg != kNoEntity);
    BREED_REQUIRE(f.reg().all_of<Hatching>(egg));
    const CellPos at = cellOf(f.reg().get<Position>(egg));
    CHECK(f.reg().get<Hatching>(egg).hatchAt - f.sim.time().seconds > 3 * kSecondsPerDay);
    CHECK(f.reg().get<StackCount>(egg).count == 1);
    f.runFed(3 * kSecondsPerDay);
    CHECK(f.livingAnimals("Chicken") == 1);
    f.runFed(kSecondsPerDay);
    CHECK(f.livingAnimals("Chicken") == 2);
    CHECK_FALSE(f.sim.things().exists(egg));
    CHECK(f.br().stats().hatched == 1);
    CHECK(f.countEvents(EventType::EggHatched) == 1);
    Entity chick = kNoEntity;
    for (const auto [e, a] : f.reg().view<Animal>().each()) {
        if (e != hen && !a.wild) {
            chick = e;
        }
    }
    BREED_REQUIRE(chick != kNoEntity);
    CHECK(f.sim.animals().lifeStage(chick) == 0);
    CHECK(f.reg().get<Faction>(chick).id == kPlayerFaction);
    CHECK(f.sim.husbandry().care(chick)->hasHome);
    CHECK(std::abs(f.sim.husbandry().care(chick)->home.x - at.x) <= 1);
    CHECK(f.br().fullness(chick, AnimalProduct::Wool) < 0.0f);
}

TEST_CASE("Breeding: a rooster walks to a hen far off and fertilizes her next egg") {
    BreedFixture f;
    const Entity hen = f.tame("Chicken", CellPos{20, 45, 0}, 1);
    const Entity rooster = f.tame("Chicken", CellPos{20, 20, 0}, 0);
    BREED_REQUIRE(hen != kNoEntity);
    BREED_REQUIRE(rooster != kNoEntity);
    f.feedAll();
    f.run(130);
    const auto apart = [&] {
        const Position& a = f.reg().get<Position>(hen);
        const Position& b = f.reg().get<Position>(rooster);
        return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
    };
    BREED_REQUIRE(apart() > f.br().rules().mateRadius + 1.0);
    BREED_REQUIRE(apart() < f.br().rules().mateSearchRadius);

    // He picks her, and sets out.
    f.br().runPass(true);
    CHECK(f.br().breed(rooster)->courting == hen);
    CHECK(f.br().stats().matings == 0);
    CHECK_FALSE(f.reg().get<Mover>(rooster).arrived());
    // A few minutes on, he is by her side and they have mated.
    bool done = false;
    for (int minute = 0; minute < 30 && !done; ++minute) {
        f.feedAll();
        f.run(60);
        done = f.br().stats().matings == 1;
    }
    BREED_REQUIRE(done);
    bool fertilizedMating = false;
    for (const Event& e : f.seen) {
        fertilizedMating = fertilizedMating || (e.type == EventType::AnimalMated && e.message == "fertilized" && e.thing == rooster &&
                                                e.target == hen);
    }
    CHECK(fertilizedMating);
    CHECK(f.br().breed(rooster)->courting == kNoEntity);
    CHECK(f.br().breed(hen)->fertilized == 1);

    // Her next egg is a fertilized one, and only that one.
    f.reg().get<AnimalBreed>(hen).eggSince -= 2 * kSecondsPerDay;
    f.runFed(130);
    bool fertilizedEgg = false;
    for (const Event& e : f.seen) {
        fertilizedEgg = fertilizedEgg || (e.type == EventType::EggLaid && e.message == "fertilized" && e.target == hen);
    }
    CHECK(fertilizedEgg);
    CHECK(f.itemsOnMap("EggChickenFertilized") == 1);
    CHECK(f.br().breed(hen)->fertilized == 0);
    f.reg().get<AnimalBreed>(hen).eggSince -= 2 * kSecondsPerDay;
    f.runFed(130);
    CHECK(f.itemsOnMap("EggChickenFertilized") == 1);  // the next one is not
    CHECK(f.itemsOnMap("EggChickenUnfertilized") == 1);
}

TEST_CASE("Breeding: a male does not court a female in another zone or of another kind") {
    BreedFixture f;
    Husbandry& hu = f.sim.husbandry();
    const AnimalZoneId zoneA = hu.createZone(CellRect{10, 10, 8, 8});
    const AnimalZoneId zoneB = hu.createZone(CellRect{30, 10, 8, 8});
    const Entity rooster = f.tame("Chicken", CellPos{12, 12, 0}, 0);
    const Entity hen = f.tame("Chicken", CellPos{32, 12, 0}, 1);
    const Entity cow = f.tame("Cow", CellPos{14, 14, 0}, 1);
    BREED_REQUIRE(rooster != kNoEntity);
    BREED_REQUIRE(hen != kNoEntity);
    BREED_REQUIRE(cow != kNoEntity);
    std::string error;
    BREED_REQUIRE(hu.assignZone(rooster, zoneA, error));
    BREED_REQUIRE(hu.assignZone(hen, zoneB, error));
    f.feedAll();
    f.run(130);
    f.br().runPass(true);
    CHECK(f.br().breed(rooster)->courting == kNoEntity);
    CHECK(f.br().stats().matings == 0);
    // Put the hen in his zone and he courts her (and not the cow).
    BREED_REQUIRE(hu.assignZone(hen, zoneA, error));
    f.run(300);
    f.feedAll();
    f.br().runPass(true);
    const AnimalBreed* b = f.br().breed(rooster);
    CHECK((b->courting == hen || f.br().stats().matings == 1));
}

TEST_CASE("Breeding: saving and loading keeps pregnancy, eggs, fill levels and a milking in progress") {
    BreedFixture f;
    f.sim.setOutdoorOverride(4.0f);  // mild enough for the egg to hatch (the override is not saved: the copy gets it too)
    const Entity cow = f.tame("Cow", CellPos{20, 20, 0}, 1);
    const Entity boar = f.tame("WildBoar", CellPos{30, 30, 0}, 1);
    const Entity hen = f.tame("Chicken", CellPos{40, 40, 0}, 1);
    BREED_REQUIRE(cow != kNoEntity);
    BREED_REQUIRE(boar != kNoEntity);
    BREED_REQUIRE(hen != kNoEntity);
    BREED_REQUIRE(f.br().makePregnant(boar, 6, f.sim.time().seconds + 4 * kSecondsPerDay));
    f.run(120);
    const Entity egg = f.br().layEgg(hen, true);
    BREED_REQUIRE(egg != kNoEntity);
    f.reg().get<AnimalBreed>(cow).milkSince -= kSecondsPerDay;  // full
    f.addColonist(CellPos{15, 20, 0}, 8);
    BREED_REQUIRE(f.runUntil(2000, [&] {
        const Worker* worker = f.reg().try_get<Worker>(f.colonist);
        return worker != nullptr && worker->job.type == JobType::GatherAnimal && worker->job.step >= 1;
    }));

    const std::vector<uint8_t> bytes = saveSim(f.sim);
    SaveLoadResult loaded = loadSim(bytes, breedDefs());
    BREED_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.error.empty());
    CHECK(loaded.warnings.empty());
    Sim& copy = *loaded.sim;
    copy.setOutdoorOverride(4.0f);
    CHECK(copy.stateHash() == f.sim.stateHash());
    CHECK(saveSim(copy) == bytes);
    CHECK(saveToJson(f.sim).find("\"breeding\"") != std::string::npos);
    CHECK(copy.breeding().pregnant(boar));
    CHECK(copy.breeding().breed(boar)->litter == 6);
    CHECK(copy.things().registry().all_of<Hatching>(egg));
    CHECK(copy.breeding().sexOf(cow) == f.br().sexOf(cow));

    // On from the save the two games stay the same: the milking ends, the boar gives birth, the egg hatches.
    for (int i = 0; i < 6; ++i) {
        f.run(kSecondsPerDay);
        copy.stepMany(kSecondsPerDay);
        CHECK(copy.stateHash() == f.sim.stateHash());
        for (Sim* s : {&f.sim, &copy}) {
            for (const Entity e : {cow, boar, hen}) {
                s->animals().addFood(e, 50.0f);
            }
        }
    }
    CHECK(copy.breeding().stats().born == f.br().stats().born);
    CHECK(f.br().stats().born == 6);
    CHECK(f.br().stats().hatched == 1);
    CHECK(f.br().stats().milked >= 1);
    CHECK(saveSim(copy) == saveSim(f.sim));
}

TEST_CASE("Breeding: a game without tame animals hashes as it always did") {
    BreedFixture f;
    f.sim.things().spawn("Deer", CellPos{20, 20, 0});
    f.run(5000);
    CHECK(f.reg().view<AnimalBreed>().empty());
    CHECK(f.br().stats().total() == 0);
}

TEST_CASE("Breeding: eggs laid in one cell join one stack and fertilized ones never do") {
    BreedFixture f;
    const Entity hen = f.tame("Chicken", CellPos{30, 30, 0}, 1);
    BREED_REQUIRE(hen != kNoEntity);
    const CellPos cell = cellOf(f.reg().get<Position>(hen));
    for (int i = 0; i < 3; ++i) {
        const Entity egg = f.br().layEgg(hen, false);
        BREED_REQUIRE(egg != kNoEntity);
    }
    BREED_REQUIRE(f.sim.things().itemsAt(cell).size() == 1);
    CHECK(f.reg().get<StackCount>(f.sim.things().itemsAt(cell)[0]).count == 3);
    const Entity first = f.br().layEgg(hen, true);
    const Entity second = f.br().layEgg(hen, true);
    BREED_REQUIRE(first != kNoEntity);
    BREED_REQUIRE(second != kNoEntity);
    CHECK(f.sim.things().itemsAt(cell).size() == 3);  // the stack and two single fertilized eggs
    CHECK(f.itemsOnMap("EggChickenFertilized") == 2);
    CHECK(f.itemsOnMap("EggChickenUnfertilized") == 3);
}

TEST_CASE("Breeding: a farm of 500 chickens lays every hen's egg and loses none") {
    BreedFixture f(128);
    std::vector<Entity> chickens;
    for (int i = 0; i < 500; ++i) {
        const CellPos cell{20 + i % 50, 20 + i / 50, 0};
        chickens.push_back(f.tame("Chicken", cell, -1));
        BREED_REQUIRE(chickens.back() != kNoEntity);
    }
    f.feedAll();
    f.run(130);  // the pass starts every hen's egg timer
    int hens = 0;
    for (const Entity e : chickens) {
        hens += f.br().female(e) ? 1 : 0;
    }
    for (const auto [e, b] : f.reg().view<AnimalBreed>().each()) {
        (void)e;
        b.eggSince -= 2 * kSecondsPerDay;  // a day and a half has gone by
    }
    const size_t thingsBefore = f.sim.things().count();
    f.run(130);
    CHECK(f.br().stats().eggsLaid == static_cast<uint64_t>(hens));
    CHECK(f.itemsOnMap("EggChickenUnfertilized") + f.itemsOnMap("EggChickenFertilized") == hens);
    CHECK(f.sim.things().count() <= thingsBefore + static_cast<size_t>(hens));

    // Another day and a half on, each hen lays again and nothing is left over from the first round.
    for (const auto [e, b] : f.reg().view<AnimalBreed>().each()) {
        (void)e;
        b.eggSince -= 2 * kSecondsPerDay;
    }
    f.feedAll();
    f.run(130);
    CHECK(f.br().stats().eggsLaid == static_cast<uint64_t>(2 * hens));
    CHECK(f.itemsOnMap("EggChickenUnfertilized") + f.itemsOnMap("EggChickenFertilized") == 2 * hens);
}



TEST_CASE("Breeding: a young animal has a share of its adult hit points that grows with its life stage") {
    BreedFixture f;
    const Entity adult = f.tame("Cow", CellPos{20, 20, 0}, 1);
    const Entity calf = f.tame("Cow", CellPos{24, 20, 0}, 1);
    BREED_REQUIRE(adult != kNoEntity);
    BREED_REQUIRE(calf != kNoEntity);
    f.reg().get<Animal>(calf).bornAt = f.sim.time().seconds;  // a newborn
    f.run(kSecondsPerHour);
    HealthSystem& health = f.sim.health();
    const float full = health.partMaxHitPoints(adult, 0);
    BREED_REQUIRE(full > 0.0f);
    CHECK(health.partMaxHitPoints(calf, 0) == doctest::Approx(full * 0.3f));  // baby: healthScaleFactor 0.3
    f.reg().get<Animal>(calf).bornAt -= 14 * kSecondsPerDay;  // 0.2 years and more: a juvenile
    f.run(kSecondsPerHour);
    CHECK(health.partMaxHitPoints(calf, 0) == doctest::Approx(full * 0.6f));
    f.reg().get<Animal>(calf).bornAt -= 60 * kSecondsPerDay;  // grown
    f.run(kSecondsPerHour);
    CHECK(health.partMaxHitPoints(calf, 0) == doctest::Approx(full));
}

TEST_CASE("Breeding: breeding can be switched off for an animal or a whole zone") {
    BreedFixture f;
    const Entity bull = f.tame("Cow", CellPos{20, 20, 0}, 0);
    const Entity cow = f.tame("Cow", CellPos{22, 20, 0}, 1);
    BREED_REQUIRE(bull != kNoEntity);
    BREED_REQUIRE(cow != kNoEntity);
    const Entity wild = f.sim.things().spawn("WildBoar", CellPos{40, 40, 0});
    CHECK(f.br().breedingAllowed(bull));

    // The command: a wild animal can't be told, a tame one can.
    std::string error;
    CHECK_FALSE(f.br().setBreeding(wild, false, error));
    CHECK_FALSE(error.empty());
    f.sim.submit(SetBreedingCommand{cow, 0, false});
    f.sim.step();
    CHECK_FALSE(f.br().breedingAllowed(cow));
    for (int i = 0; i < 10; ++i) {
        f.feedAll();
        f.br().runPass(true);
        f.run(61);
    }
    CHECK(f.br().stats().matings == 0);

    // Back on: they mate.
    f.sim.submit(SetBreedingCommand{cow, 0, true});
    f.sim.step();
    CHECK(f.br().breedingAllowed(cow));
    for (int i = 0; i < 10 && f.br().stats().matings == 0; ++i) {
        f.feedAll();
        f.br().runPass(true);
        f.run(61);
    }
    CHECK(f.br().stats().matings >= 1);

    // A zone's switch stops every animal in it, and survives saving.
    BreedFixture g;
    const Entity bull2 = g.tame("Cow", CellPos{20, 20, 0}, 0);
    const Entity cow2 = g.tame("Cow", CellPos{22, 20, 0}, 1);
    BREED_REQUIRE(bull2 != kNoEntity);
    BREED_REQUIRE(cow2 != kNoEntity);
    const AnimalZoneId zone = g.sim.husbandry().createZone(CellRect{15, 15, 12, 12});
    BREED_REQUIRE(g.sim.husbandry().assignZone(bull2, zone, error));
    BREED_REQUIRE(g.sim.husbandry().assignZone(cow2, zone, error));
    g.sim.submit(SetBreedingCommand{kNoEntity, zone, false});
    g.sim.step();
    CHECK_FALSE(g.br().breedingAllowed(bull2));
    g.sim.submit(SetBreedingCommand{cow2, 0, false});
    g.sim.step();

    const std::vector<uint8_t> bytes = saveSim(g.sim);
    SaveLoadResult loaded = loadSim(bytes, breedDefs());
    BREED_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.error.empty());
    CHECK(loaded.sim->stateHash() == g.sim.stateHash());
    CHECK(saveSim(*loaded.sim) == bytes);
    CHECK_FALSE(loaded.sim->breeding().breedingAllowed(bull2));
    CHECK_FALSE(loaded.sim->breeding().breedingAllowed(cow2));
    for (int i = 0; i < 10; ++i) {
        g.feedAll();
        g.br().runPass(true);
        g.run(61);
    }
    CHECK(g.br().stats().matings == 0);
}

TEST_CASE("Breeding: a fertilized egg that is too cold or too hot does not hatch until it is warm again") {
    BreedFixture f;
    const Entity hen = f.tame("Chicken", CellPos{20, 20, 0}, 1);
    BREED_REQUIRE(hen != kNoEntity);
    f.sim.setOutdoorOverride(-15.0f);
    const Entity egg = f.br().layEgg(hen, true);
    BREED_REQUIRE(egg != kNoEntity);
    // The weather wears an egg in the open away in a few days: keep it whole, this test is about the clock.
    const int hitPoints = f.reg().get<HitPoints>(egg).current;
    auto runWhole = [&](int64_t seconds) {
        for (int64_t t = 0; t < seconds && f.sim.things().exists(egg); t += 600) {
            f.runFed(std::min<int64_t>(600, seconds - t));
            if (f.sim.things().exists(egg)) {
                f.reg().get<HitPoints>(egg).current = hitPoints;
            }
        }
    };
    // Too cold: the time left to hatch stays the same however long it lies there.
    const int64_t left = f.reg().get<Hatching>(egg).hatchAt - f.sim.time().seconds;
    runWhole(kSecondsPerDay);
    BREED_REQUIRE(f.sim.things().exists(egg));
    CHECK(std::abs(f.reg().get<Hatching>(egg).hatchAt - f.sim.time().seconds - left) <= 61);
    f.sim.setOutdoorOverride(60.0f);  // too hot is the same
    runWhole(2 * kSecondsPerHour);  // (a short while: an egg rots in the heat)
    BREED_REQUIRE(f.sim.things().exists(egg));
    CHECK(std::abs(f.reg().get<Hatching>(egg).hatchAt - f.sim.time().seconds - left) <= 61);
    CHECK(f.br().stats().hatched == 0);
    // Mild again: it hatches when the time left has gone.
    f.sim.setOutdoorOverride(4.0f);
    runWhole(left - 2 * kSecondsPerHour);
    BREED_REQUIRE(f.sim.things().exists(egg));
    CHECK(f.br().stats().hatched == 0);
    runWhole(3 * kSecondsPerHour);
    CHECK(f.br().stats().hatched == 1);
}

TEST_CASE("Breeding: a fertilized egg beside a hatchery hatches in the cold, one far from it does not") {
    BreedFixture f;
    const Entity hen = f.tame("Chicken", CellPos{20, 20, 0}, 1);
    BREED_REQUIRE(hen != kNoEntity);
    f.sim.setOutdoorOverride(-15.0f);
    BREED_REQUIRE(f.sim.things().spawn("Hatchery", CellPos{30, 30, 0}) != kNoEntity);
    const Entity cold = f.br().layEgg(hen, true);
    BREED_REQUIRE(cold != kNoEntity);
    // A second egg is laid next to the hatchery.
    const Entity warm = f.sim.things().spawn("EggChickenFertilized", CellPos{31, 30, 0});
    BREED_REQUIRE(warm != kNoEntity);
    f.reg().emplace_or_replace<Hatching>(warm, Hatching{f.sim.time().seconds + 4 * kSecondsPerHour});
    const int hitPoints = f.reg().get<HitPoints>(cold).current;
    for (int64_t t = 0; t < 5 * kSecondsPerHour; t += 600) {
        f.runFed(600);
        if (f.sim.things().exists(cold)) {
            f.reg().get<HitPoints>(cold).current = hitPoints;
        }
    }
    CHECK(f.br().stats().hatched == 1);
    CHECK_FALSE(f.sim.things().exists(warm));
    CHECK(f.sim.things().exists(cold));
}

TEST_CASE("Breeding: a mother in the cold carries her young for longer") {
    BreedFixture f;
    const Entity cow = f.tame("Cow", CellPos{20, 20, 0}, 1);
    BREED_REQUIRE(cow != kNoEntity);
    f.sim.setOutdoorOverride(-20.0f);
    BREED_REQUIRE(f.br().makePregnant(cow, 1, f.sim.time().seconds + 2 * kSecondsPerHour));
    f.runFed(4 * kSecondsPerHour);
    CHECK(f.br().pregnant(cow));
    CHECK(f.br().stats().born == 0);
    f.sim.setOutdoorOverride(10.0f);
    f.runFed(4 * kSecondsPerHour);
    CHECK(f.br().stats().born == 1);
}

TEST_CASE("Breeding: a calf keeps near its mother, and the link survives a save") {
    BreedFixture f;
    f.sim.setOutdoorOverride(10.0f);
    const Entity cow = f.tame("Cow", CellPos{20, 20, 0}, 1);
    BREED_REQUIRE(cow != kNoEntity);
    BREED_REQUIRE(f.br().makePregnant(cow, 1, f.sim.time().seconds + 60));
    f.runFed(180);
    BREED_REQUIRE(f.br().stats().born == 1);
    Entity calf = kNoEntity;
    for (const auto [e, y] : f.reg().view<Young>().each()) {
        calf = e;
        CHECK(y.mother == cow);
    }
    BREED_REQUIRE(calf != kNoEntity);
    // The mother is carried far off; the calf walks after her and ends up within a few metres.
    f.reg().get<Position>(cow).x = 45.5f;
    f.reg().get<Position>(cow).y = 20.5f;
    f.runFed(3 * kSecondsPerHour);
    const Position& a = f.reg().get<Position>(calf);
    const Position& b = f.reg().get<Position>(cow);
    CHECK(std::hypot(a.x - b.x, a.y - b.y) < 12.0);

    const std::vector<uint8_t> bytes = saveSim(f.sim);
    SaveLoadResult loaded = loadSim(bytes, breedDefs());
    BREED_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->stateHash() == f.sim.stateHash());
    const Young* y = loaded.sim->things().registry().try_get<Young>(calf);
    BREED_REQUIRE(y != nullptr);
    CHECK(y->mother == cow);

TEST_CASE("Breeding: goats give 12 milk every 3 days, camels 18 every 2, caribou 10 every 2, and a muffalo none") {
    BreedFixture f;
    const Entity goat = f.tame("Goat", CellPos{20, 20, 0}, 1);
    const Entity billy = f.tame("Goat", CellPos{22, 20, 0}, 0);
    const Entity dromedary = f.tame("Dromedary", CellPos{24, 20, 0}, 1);
    const Entity caribou = f.tame("Caribou", CellPos{26, 20, 0}, 1);
    const Entity muffalo = f.tame("Muffalo", CellPos{28, 20, 0}, 1);
    BREED_REQUIRE(goat != kNoEntity);
    BREED_REQUIRE(billy != kNoEntity);
    BREED_REQUIRE(dromedary != kNoEntity);
    BREED_REQUIRE(caribou != kNoEntity);
    BREED_REQUIRE(muffalo != kNoEntity);
    f.run(120);
    CHECK(f.br().productAmount(goat, AnimalProduct::Milk) == 12);
    CHECK(f.br().productAmount(dromedary, AnimalProduct::Milk) == 18);
    CHECK(f.br().productAmount(caribou, AnimalProduct::Milk) == 10);
    CHECK(f.br().fullness(billy, AnimalProduct::Milk) < 0.0f);
    CHECK(f.br().fullness(muffalo, AnimalProduct::Milk) < 0.0f);
    CHECK(f.br().fullness(dromedary, AnimalProduct::Wool) < 0.0f);
    CHECK(f.br().productAmount(muffalo, AnimalProduct::Wool) == 120);
    f.feedAll();
    f.run(2 * kSecondsPerDay);
    CHECK(f.br().readyToGather(dromedary, AnimalProduct::Milk));
    CHECK(f.br().readyToGather(caribou, AnimalProduct::Milk));
    CHECK_FALSE(f.br().readyToGather(goat, AnimalProduct::Milk));  // a third day to go
    f.feedAll();
    f.run(kSecondsPerDay);
    CHECK(f.br().readyToGather(goat, AnimalProduct::Milk));
}

TEST_CASE("Breeding: a hen of a fertilized-only kind holds her egg at half time until a male has mated her") {
    BreedFixture f;
    const Entity hen = f.tame("Ostrich", CellPos{20, 20, 0}, 1);
    BREED_REQUIRE(hen != kNoEntity);
    f.run(120);
    CHECK(f.br().laysEggs(hen));
    f.runFed(8 * kSecondsPerDay);  // well past her 3.33 days
    CHECK(f.itemsOnMap("EggOstrichUnfertilized") == 0);
    CHECK(f.itemsOnMap("EggOstrichFertilized") == 0);
    CHECK(f.br().stats().eggsLaid == 0);

    // Once she has mated, the egg comes after the rest of the interval, and it is the ostrich's own egg.
    f.reg().get<AnimalBreed>(hen).fertilized = 1;
    f.runFed(kSecondsPerDay);
    CHECK(f.br().stats().eggsLaid == 0);
    f.runFed(kSecondsPerDay);
    CHECK(f.itemsOnMap("EggOstrichFertilized") == 1);
    CHECK(f.sim.defs().find("Things", "EggOstrichFertilized")->text("hatches.into") == "Ostrich");
    CHECK(f.br().breed(hen)->fertilized == 0);
}
