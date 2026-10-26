#pragma once

#include "ColonySim/Animals.h"
#include "ColonySim/Api.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"
#include "ColonySim/Grid.h"

#include <cstdint>
#include <string>
#include <vector>

namespace colony {

class Husbandry;
class Map;
class StateHasher;
class TemperatureSystem;
class Things;

// Breeding and animal products (task N4, docs/design/breeding.md): tame animals mate, females carry
// their young and give birth, hens lay eggs (fertilized ones hatch), young animals grow up through
// the life stages of N1, and colonists with the Handling work type milk and shear. Everything runs
// from one pass every `passSeconds` game seconds over the tame animals, so a farm of hundreds costs
// a few animals' work per step. Wild animals do none of it (the wild herd is kept up by the edges, N1).
//
// Unreal does not include this header: it reads sex, pregnancy and fullness from the snapshot, and
// the events AnimalMated, AnimalBorn, EggLaid, EggHatched and AnimalGathered.

enum class AnimalSex : uint8_t { Male, Female };

// The value of a product timer the pass has not started yet.
inline constexpr int64_t kBreedNotStarted = INT64_MIN;

// On a tame animal once the pass has seen it grown up. Every field is saved and hashed.
struct AnimalBreed {
    int64_t pregnantUntil = -1;  // game second the young are born, or -1 while not carrying any
    uint8_t litter = 0;          // how many it carries
    uint8_t fertilized = 0;      // eggs it will lay fertilized (a hen that mated)
    int64_t nextMateAt = 0;      // game second it may mate again
    int64_t milkSince = kBreedNotStarted;  // game second the milk (wool, eggs) began to fill; kBreedNotStarted until the pass starts it
    int64_t woolSince = kBreedNotStarted;
    int64_t eggSince = kBreedNotStarted;
    Entity courting = kNoEntity;  // a male on his way to a female he chose
    int64_t courtUntil = 0;       // and the game second he gives up
    bool breedingOff = false;     // the player switched breeding off for this animal (saved from section version 2)
};

// On a fertilized egg lying on the map: the game second it hatches.
struct Hatching {
    int64_t hatchAt = 0;
};

// The numbers (BreedingRules in data/defs/Rules/BreedingRules.json; the file marks each as RimWorld's
// from memory or ours).
struct BreedingRules {
    int passSeconds = 60;             // seconds between two passes over the tame animals
    double mateMeanHours = 12.0;      // a mature male looks for a mate this often, on average
    double mateChance = 0.5;          // that a mating of a gestating kind leaves the female pregnant
    double mateRadius = 12.0;         // metres between a male and the female he mates with
    double mateSearchRadius = 40.0;   // metres a male looks for a female; further than mateRadius he walks to her first
    double courtSeconds = 900.0;      // and gives up this long after setting out
    double mateHoldSeconds = 6.0;     // the two stand still this long
    double mateCooldownHours = 12.0;  // a male or an egg layer waits this long before the next mating
    double birthCooldownDays = 2.0;   // a female waits this long after giving birth
    double minFood = 0.3;             // share of its food an animal needs to mate
    double milkWorkSeconds = 6.7;     // a colonist milks this long (400 RimWorld ticks)
    double woolWorkSeconds = 28.3;    // and shears this long (1,700 ticks)
    double gatherXp = 75.0;           // Animals experience for one milking or shearing
    double birthSpread = 1.5;         // metres round the mother the young are placed
    double hatchMinTemp = 0.0;        // a fertilized egg only warms towards hatching between these degrees
    double hatchMaxTemp = 50.0;
};

struct BreedingStats {
    uint64_t matings = 0;
    uint64_t pregnancies = 0;
    uint64_t born = 0;
    uint64_t eggsLaid = 0;
    uint64_t hatched = 0;
    uint64_t milked = 0;
    uint64_t sheared = 0;
    uint64_t total() const { return matings + pregnancies + born + eggsLaid + hatched + milked + sheared; }
};

// What an animal gives to a colonist's hands.
enum class AnimalProduct : uint8_t { Milk, Wool };

class COLONY_API Breeding {
public:
    Breeding(Things& things, const Map& map, AnimalSystem& animals, Husbandry& husbandry, const DefDatabase& defs,
             const GameTime& time, uint64_t seed);
    ~Breeding();
    Breeding(const Breeding&) = delete;
    Breeding& operator=(const Breeding&) = delete;

    void setEventLog(std::vector<Event>* events) { events_ = events; }
    // Where eggs get the temperature that decides whether they hatch. Not saved; without it every egg is warm enough.
    void setClimate(TemperatureSystem* temperature) { temperature_ = temperature; }
    const BreedingRules& rules() const { return rules_; }

    // Once a game second; the work is done every `passSeconds`.
    void step();
    // Does the pass now. With `everyMaleSeeks`, every mature male looks for a mate at once instead of rolling for it (for
    // tests and the dev console).
    void runPass(bool everyMaleSeeks) { pass(time_.seconds, everyMaleSeeks); }

    // ---- Questions
    // Every animal is male or female for good, from the game's seed and its id.
    AnimalSex sexOf(Entity animal) const;
    bool female(Entity animal) const { return sexOf(animal) == AnimalSex::Female; }
    // Tame, grown up, and (for a female that carries young) not too far gone to mate.
    bool matureTame(Entity animal) const;
    const AnimalBreed* breed(Entity animal) const;
    bool pregnant(Entity animal) const;
    // Whether this animal may mate: its own switch and its zone's switch are both on.
    bool breedingAllowed(Entity animal) const;
    // Switches breeding on or off for one tame animal. Fails, with `error`, for a wild one or a thing that is no animal.
    bool setBreeding(Entity animal, bool allow, std::string& error);
    // How full the animal's milk or wool is, 0 to 1; -1 for an animal that gives none (wild, young, or the wrong sex).
    float fullness(Entity animal, AnimalProduct what) const;
    // True when a colonist can milk or shear it now (full, alive, up and about).
    bool readyToGather(Entity animal, AnimalProduct what) const;
    // Whether the animal's def gives the product at all, and what and how much (the def's `milk` or `wool`).
    bool gives(Entity animal, AnimalProduct what) const;
    DefId productDef(Entity animal, AnimalProduct what) const;
    int productAmount(Entity animal, AnimalProduct what) const;
    // Seconds of work one gathering takes.
    double gatherSeconds(AnimalProduct what) const;
    // Whether the animal's def lays eggs.
    bool laysEggs(Entity animal) const;

    // ---- For the jobs
    // A colonist gathered the product: the items are put at the animal, it starts to fill again, the event goes out.
    bool finishGather(Entity handler, Entity animal, AnimalProduct what);

    // ---- Shortcuts for tests and the dev console: they do at once what the pass does in its own time.
    // The female is made to carry young that are born at `bornAt` (a game second); false if she can't.
    bool makePregnant(Entity female, int litter, int64_t bornAt);
    // A hen lays now (fertilized or not). Returns the egg item.
    Entity layEgg(Entity hen, bool fertilized);

    const BreedingStats& stats() const { return stats_; }

    // Saving, loading and the state hash (docs/design/saves.md).
    const BreedingStats& saved() const { return stats_; }
    void restore(const BreedingStats& stats) { stats_ = stats; }
    void hashState(StateHasher& h) const;
    void hashThing(Entity e, StateHasher& h) const;

private:
    struct Kind {
        bool gestates = false;      // has `gestationDays`: its females carry young
        double gestationSeconds = 0.0;
        int litterMin = 1;
        int litterMax = 1;
        bool eggs = false;
        double eggIntervalSeconds = 0.0;
        int eggCount = 1;
        DefId eggUnfertilized;
        DefId eggFertilized;
        double milkIntervalSeconds = 0.0;
        int milkAmount = 0;
        DefId milkThing;
        double woolIntervalSeconds = 0.0;
        int woolAmount = 0;
        DefId woolThing;
    };
    struct Hatch {
        DefId into;
        double seconds = 0.0;
    };
    struct Candidate {
        Entity animal = kNoEntity;
        DefId def;
        uint32_t zone = 0;
        float x = 0.0f;
        float y = 0.0f;
        int level = 0;
    };

    const Kind* kindOf(Entity animal) const;
    bool upAndAbout(Entity animal) const;
    bool fertileNow(Entity animal, const AnimalBreed& breed, int64_t now) const;
    // A female that a male could mate with now: tame, grown, fed, not carrying young, not mated lately.
    bool willing(Entity female, const AnimalBreed& breed, const Kind& kind, int64_t now) const;
    void pass(int64_t now, bool everyMaleSeeks);
    void giveBirth(Entity mother, AnimalBreed& breed, const Kind& kind, int64_t now);
    Entity spawnYoung(DefId def, Entity mother, CellPos around, int64_t now);
    Entity placeEgg(DefId def, CellPos cell, int64_t now, bool fertilized);
    void hatch(Entity egg, int64_t now);
    void mate(Entity male, Entity female, const Kind& kind, int64_t now);
    void emit(EventType type, Entity thing, Entity target, DefId def, CellPos cell, const char* message = nullptr,
              float amount = 0.0f);

    Things& things_;
    const Map& map_;
    AnimalSystem& animals_;
    Husbandry& husbandry_;
    const DefDatabase& defs_;
    const GameTime& time_;
    TemperatureSystem* temperature_ = nullptr;
    uint64_t seed_;
    BreedingRules rules_;
    BreedingStats stats_;
    std::vector<Kind> kinds_;       // by DefId.value; the default Kind for a def that is no animal
    std::vector<uint8_t> isAnimal_;  // by DefId.value
    std::vector<Hatch> hatches_;    // by DefId.value: what an egg def hatches into (into invalid if it doesn't hatch)
    std::vector<int> stackLimits_;  // by DefId.value
    std::vector<Event>* events_ = nullptr;
    // scratch for the pass
    std::vector<Entity> tame_;
    std::vector<Candidate> males_;
    std::vector<Candidate> females_;
    std::vector<Entity> eggs_;
};

}  // namespace colony
