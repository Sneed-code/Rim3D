#pragma once

#include "ColonySim/Animals.h"
#include "ColonySim/Api.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"
#include "ColonySim/Grid.h"
#include "ColonySim/Pathfinder.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace colony {

class HealthSystem;
class Map;
class StateHasher;
class Things;

// Taming, training, animal zones and pens (task N3, docs/design/husbandry.md): the player marks a wild
// animal to tame or a tame one to slaughter, picks what a tame animal should learn, and paints animal
// zones that keep animals in. Colonists with the Handling work type do the rest through the job system
// (HusbandryJobs.cpp): feed and talk an animal round (tame), practise a trick with it (train), kill it
// (slaughter), or carry hay and kibble to a hungry one (feed). The animals' own behaviour once tame
// (eating hay and kibble, staying in the zone, starving) is in AnimalSystem (AnimalCare.cpp).
//
// Unreal does not include this header: it reads the marks, the zone and what an animal has learned from
// the snapshot, and the results from events (AnimalTamed, AnimalTrained, ...).

// What a tame animal can be trained to do, in the order of the rules' `trainables` list. Saves store
// the number, so new ones go at the end.
enum class Trainable : uint8_t { Obedience, Release, Rescue, Haul };
inline constexpr int kTrainableCount = 4;

COLONY_API const char* trainableName(Trainable t);
// False for a name that is none of them.
COLONY_API bool trainableFromName(const std::string& name, Trainable& out);

// An animal zone: cells (level 0) the player painted for animals to stay in. A zone whose cells are
// walled in (`Husbandry::isPen`) is a pen.
using AnimalZoneId = uint32_t;
inline constexpr AnimalZoneId kNoAnimalZone = 0;

struct AnimalZone {
    AnimalZoneId id = kNoAnimalZone;
    std::vector<CellPos> cells;  // in row order: by y, then x
};

struct TrainingState {
    uint16_t steps = 0;    // successful attempts so far
    bool wanted = false;   // the player wants it learned
};

// On an animal once anything is set on it (made when first needed). Every field is saved and hashed.
struct AnimalCare {
    bool markTame = false;       // a wild animal marked to be tamed
    bool markSlaughter = false;  // a tame animal marked to be slaughtered
    AnimalZoneId zone = kNoAnimalZone;  // the zone a tame animal stays in; none: anywhere near home
    bool hasHome = false;
    CellPos home;                // where it was tamed; an animal with no zone stays within `homeRadius` of it
    int64_t tamedAt = -1;        // game second, or -1 while wild
    int64_t heldUntil = 0;       // a handler is with it: it stands still until this game second
    int64_t starvingSince = -1;  // game second its food ran out, or -1
    int64_t nextTrainAt = 0;     // earliest game second of the next training attempt
    int64_t lastTrainAt = 0;     // the last attempt; unfinished training fades from here
    int64_t noPathUntil = 0;     // a walk to its zone found no path: it doesn't ask again before this
    TrainingState training[kTrainableCount];
    PathTicket path;             // while its mode is AnimalMode::WaitPath
    CellPos pathGoal;            // ... and where the path goes
    AnimalMode pathMode = AnimalMode::Wander;  // ... and what it does on arriving
};

struct TrainableRule {
    std::string name;
    int trainability = 1;   // the animal's `race.trainability` must be at least this (None 0, Simple 1, Intermediate 2, Advanced 3)
    int steps = 10;         // successful attempts to learn it
    int prerequisite = -1;  // index of a trainable that must be learned first, or -1
};

// The numbers (HusbandryRules in data/defs/Rules/HusbandryRules.json; each is RimWorld's, from the wiki,
// or ours, as the file says).
struct HusbandryRules {
    double interactSeconds = 389.0;   // one taming or training attempt
    double slaughterSeconds = 259.0;
    double feedSeconds = 20.0;        // hand-feeding a hungry animal
    double skillChanceBase = 0.5;     // an attempt's chance is (base + perLevel x Animals level) x ...
    double skillChancePerLevel = 0.05;
    double feedNutritionPerBodySize = 0.15;  // food a taming or training attempt feeds the animal
    double handReach = 1.6;           // metres from the animal a handler works
    double holdRadius = 6.0;          // an animal a handler is walking to stands still within this
    double attemptIntervalSeconds = 21600.0;  // between two training attempts on one animal
    double decayDays = 6.0;           // unfinished training loses a step this often
    double starveDays = 5.0;          // a tame animal with no food dies after this long
    double handFeedBelow = 0.25;      // a tame animal under this share of its food is hand-fed
    double workMinFood = 0.3;         // a hauling animal stops working under this share
    double homeRadius = 40.0;         // metres a tame animal with no zone strays from where it was tamed
    double itemRadius = 12.0;         // metres a tame animal looks for hay, kibble or meat
    double tameXp = 200.0;
    double trainXp = 100.0;
    double slaughterXp = 50.0;
    double feedXp = 20.0;
    double repathSeconds = 600.0;
    std::vector<std::string> dietFoods[3];  // food types (`ingestible.foodType`) a Herbivore, Carnivore, Omnivore eats
    std::vector<TrainableRule> trainables;  // indexed by Trainable
};

struct HusbandryStats {
    uint64_t tamed = 0;
    uint64_t tameFailed = 0;
    uint64_t trainingSteps = 0;
    uint64_t trainingFailed = 0;
    uint64_t slaughtered = 0;
    uint64_t fed = 0;
    uint64_t starved = 0;
};

struct HusbandrySaved {
    AnimalZoneId nextZone = 1;
    std::vector<AnimalZone> zones;
    HusbandryStats stats;
};

class COLONY_API Husbandry {
public:
    Husbandry(Things& things, const Map& map, AnimalSystem& animals, HealthSystem& health, const DefDatabase& defs,
              const GameTime& time, uint64_t seed);
    ~Husbandry();
    Husbandry(const Husbandry&) = delete;
    Husbandry& operator=(const Husbandry&) = delete;

    void setEventLog(std::vector<Event>* events) { events_ = events; }
    // Called when an animal learns Haul (true) or stops being a hauler (false: released, or dead):
    // the sim makes it a worker of the job system and takes it off again.
    void setHaulerHook(std::function<void(Entity, bool)> hook) { haulerHook_ = std::move(hook); }
    const HusbandryRules& rules() const { return rules_; }

    // ---- Commands. Each returns false, with a reason in plain words, when it can't be done.
    // Marks a wild animal to be tamed, or takes the mark off.
    bool markTame(Entity animal, bool on, std::string& error);
    // Marks a tame animal to be slaughtered, or takes the mark off.
    bool markSlaughter(Entity animal, bool on, std::string& error);
    // A tame animal goes wild again: it loses its training, zone and marks, and leaves for the map edge in its own time.
    bool release(Entity animal, std::string& error);
    // Wants (or no longer wants) the animal to learn a trick. Wanting one also wants what it needs first.
    bool setTraining(Entity animal, Trainable what, bool wanted, std::string& error);
    // Zones. A new zone covers the cells of `area` that are walkable and in no zone; returns its id, or none.
    AnimalZoneId createZone(const CellRect& area);
    int removeZoneCells(const CellRect& area);
    // A tame animal stays in a zone (0: anywhere near home).
    bool assignZone(Entity animal, AnimalZoneId zone, std::string& error);

    // ---- Questions
    const AnimalCare* care(Entity animal) const;
    // The care record, made if the animal has none yet; null for a thing that isn't an animal.
    AnimalCare* careFor(Entity animal);
    bool isTame(Entity animal) const;  // a living or dead animal that is not wild
    // Trainability of the animal's def: 0 None, 1 Simple, 2 Intermediate, 3 Advanced.
    int trainability(Entity animal) const;
    double wildness(Entity animal) const;
    // The animal can be trained for this at all: tame, trainable enough.
    bool canLearn(Entity animal, Trainable what) const;
    int steps(Entity animal, Trainable what) const;
    int stepsNeeded(Trainable what) const;
    bool learned(Entity animal, Trainable what) const;
    // The first wanted trick the animal can practise now (learnable, not learned, prerequisite learned).
    bool nextToTrain(Entity animal, Trainable& out) const;
    // Past the wait since the last attempt.
    bool trainingDue(Entity animal) const;
    // Bit i set: Trainable i is learned / wanted.
    uint32_t learnedMask(Entity animal) const;
    uint32_t wantedMask(Entity animal) const;
    // An attempt's chance for a handler of that Animals level: 0 to 1.
    double tameChance(int skill, Entity animal) const;
    double trainChance(int skill, Entity animal) const;
    // Nutrition a taming or training attempt feeds the animal.
    double feedNutrition(Entity animal) const;
    // Whether an animal of this def eats this item def (by its diet and the item's food type).
    bool eats(DefId animalDef, DefId item) const;
    bool animalEats(Entity animal, DefId item) const;
    // Nutrition one unit of the item gives (0 for a thing that isn't food).
    float nutritionOf(DefId item) const;

    const std::vector<AnimalZone>& zones() const { return saved_.zones; }
    const AnimalZone* findZone(AnimalZoneId id) const;
    AnimalZoneId zoneAt(CellPos cell) const;
    // A zone whose cells are all walled in: the walkable cells joined to it (not through doors) are all in the zone.
    bool isPen(AnimalZoneId zone) const;
    uint64_t zoneChanges() const { return zoneChanges_; }

    // ---- For the jobs and the animals
    // Keeps the animal still until the game second.
    void hold(Entity animal, int64_t until);
    // The result of a taming attempt by a handler of that Animals level: the roll is made, the animal is tamed
    // (if it came up) and the events sent. True if it is tame now.
    bool finishTame(Entity handler, Entity animal, int skill);
    // The same for one training attempt at `what`; true if the attempt worked.
    bool finishTrain(Entity handler, Entity animal, Trainable what, int skill);
    // Kills the animal for the colony's table. False if it is already dead.
    bool finishSlaughter(Entity handler, Entity animal);
    // A handler gives the animal food: it gains the nutrition (up to what it holds).
    void feed(Entity animal, float nutrition);
    const AnimalSystem& animals() const { return animals_; }
    // A tame animal died of hunger (AnimalSystem).
    void noteStarved(Entity animal);

    // Once a game second: unfinished training fades, a few times a day.
    void step();

    const HusbandryStats& stats() const { return saved_.stats; }

    // Saving, loading and the state hash (docs/design/saves.md).
    const HusbandrySaved& saved() const { return saved_; }
    // Rebuilds the cell index. Returns false, with `error`, if the zones don't add up.
    bool restore(HusbandrySaved state, std::string& error);
    void hashState(StateHasher& h) const;
    void hashThing(Entity e, StateHasher& h) const;

private:
    struct Kind {
        int trainability = 0;
        double wildness = 0.0;
        int diet = 2;  // index into HusbandryRules::dietFoods
        float bodySize = 1.0f;
    };
    const Kind* kindOf(Entity animal) const;
    bool valid(Entity animal) const;
    void emit(EventType type, Entity animal, Entity handler, const char* message = nullptr, float amount = 0.0f, int part = -1);
    double skillFactor(int skill) const;
    bool prerequisiteLearned(const AnimalCare& care, Trainable what) const;
    void becomeTame(Entity handler, Entity animal);
    void setHauler(Entity animal, bool on);
    size_t slot(CellPos c) const;

    Things& things_;
    const Map& map_;
    AnimalSystem& animals_;
    HealthSystem& health_;
    const DefDatabase& defs_;
    const GameTime& time_;
    uint64_t seed_;
    HusbandryRules rules_;
    HusbandrySaved saved_;
    std::vector<Kind> kinds_;           // by DefId.value; trainability -1 for a def that is no animal
    std::vector<int> kindOfDef_;
    std::vector<float> nutrition_;       // by DefId.value
    std::vector<uint8_t> foodType_;       // by DefId.value: bit mask of rules_.dietFoods the item belongs to (1 << diet), 0 none
    std::vector<AnimalZoneId> zoneOf_;   // per cell of level 0
    uint64_t zoneChanges_ = 0;
    mutable uint64_t penStamp_[2] = {~0ULL, ~0ULL};  // zone changes and region changes the cache is for
    mutable std::vector<std::pair<AnimalZoneId, bool>> penCache_;
    std::vector<Event>* events_ = nullptr;
    std::function<void(Entity, bool)> haulerHook_;
};

}  // namespace colony
