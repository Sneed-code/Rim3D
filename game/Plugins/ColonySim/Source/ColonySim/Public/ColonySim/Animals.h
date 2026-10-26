#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"
#include "ColonySim/Grid.h"
#include "ColonySim/Random.h"
#include "ColonySim/ThreatMap.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace colony {

struct AnimalCare;
class HealthSystem;
class Husbandry;
class Map;
class MeleeSystem;
class Pathfinder;
class Plants;
class StateHasher;
class Things;

// Animals on the map (task N1, docs/design/animal-life.md): wild herds placed by biome when the map
// is made, walking about in herds, grazing on the plant layer, sleeping, coming in and going out at
// the map edges to keep the wild population at the biome's density, and corpses that clear away.
//
// An animal is a pawn whose def says `race.intelligence: "Animal"`. It has a body and health like
// any pawn (health.md) and no Worker, so the job system leaves it alone; `Things::make` gives it the
// `Animal` component below. Hunting, taming, breeding and fighting come with N2 to N5.
//
// Unreal does not include this header: it reads `food`, `asleep` and the life stage from the snapshot.

// The side of every wild animal (and of every animal until N3 lets the player tame one). Fighters
// never take an animal for an enemy on their own (docs/design/animal-life.md).
inline constexpr FactionId kWildFaction = 100;

enum class AnimalMode : uint8_t {
    Idle,      // standing about until `until`
    Wander,    // walking to `goal` (a wander spot, or the leader's side)
    GoToFood,  // walking to the plant in `goal`
    Eat,       // eating the plant in `goal` until `until`
    Sleep,     // lying down until `until`
    Leave,     // walking to the map edge at `goal`, to go
    // Tame animals (task N3, docs/design/husbandry.md):
    WaitPath,  // waiting for the path to its zone or its food that AnimalCare::path asked for
    GoToItem,  // walking to the food item (hay, kibble, meat) lying in `goal`
    EatItem,   // eating the item in `goal` until `until`
    // Fights (task N5, docs/design/animal-fights.md):
    Flee,      // running from a fight, a shot or a predator to `goal` until `until`
    Chase,     // running at `enemy`
    Fight,     // in reach of `enemy`, swinging (stands still)
    Feast,     // a predator walking to the corpse of its kill
    Gorge,     // a predator eating the corpse of `enemy` until `until`
    Seized     // prey caught by a predator: frozen with fear until `until` (the predator renews it while it fights)
};

// What an animal has against the colony. Anything but Calm is an enemy to the fighter AI.
enum class AnimalTemper : uint8_t {
    Calm,
    Hunting,    // a predator after a pawn
    Revenge,    // fighting back at whoever hurt it, for a short time
    Manhunter   // after every colonist it can see, for hours
};

// On every animal. Every field is saved and hashed.
struct Animal {
    bool started = false;      // set up by the system on its first look (food, side, sleep hours)
    bool wild = true;          // wild animals come, go and keep to their herd; N3 makes tame ones
    bool feeding = false;      // hungry, and eating until it is full
    uint8_t sleepShift = 0;    // hours its sleep is later than the rules' hours (0 to 2)
    uint32_t herd = 0;         // 0: alone; else the herd's id, and the lowest id of the living members leads it
    int64_t bornAt = 0;        // game second; below 0 for an animal older than the game
    int64_t diedAt = -1;       // game second the system saw it dead, or -1
    float food = 0.0f;         // nutrition, up to its body size (as RimWorld's food need)
    int64_t foodAt = 0;        // game second `food` was worked out for
    AnimalMode mode = AnimalMode::Idle;
    int64_t until = 0;         // game second the mode ends (Idle, Eat, Sleep)
    int64_t nextThink = 0;     // game second of its next decision
    int64_t staysUntil = 0;    // a newcomer from the edge won't leave again before this game second
    CellPos goal;              // Wander, GoToFood, Eat, Leave, Flee, Feast
    // N5 (docs/design/animal-fights.md).
    AnimalTemper temper = AnimalTemper::Calm;
    Entity enemy = kNoEntity;  // whom it chases, fights or has killed and eats (Chase, Fight, Feast, Gorge)
    int64_t temperUntil = 0;   // game second a Revenge or Manhunter temper ends
    int64_t huntAfter = 0;     // a predator looks for prey again from this game second
};

// The numbers (AnimalRules in data/defs/Rules/AnimalRules.json, all ours: RimWorld's animals have
// no herd walking to copy).
struct AnimalRules {
    int idleMin = 8;               // seconds an animal stands before its next walk
    int idleMax = 45;
    float wanderChance = 0.6f;     // of leaving the standing spot at each decision
    float wanderMin = 4.0f;        // metres of a wander walk
    float wanderMax = 14.0f;
    float wanderSpeed = 0.6f;      // share of its top speed it walks at
    float herdRadius = 4.5f;       // a follower stands within this of its leader
    float herdPull = 9.0f;         // and walks back at once when further than this
    float grazeRadius = 10.0f;     // metres it looks for a plant to eat
    float hungryBelow = 0.5f;      // share of its food capacity under which it eats
    float fullAt = 0.9f;           // and eats on until it is this full
    float minPlantGrowth = 0.1f;   // it won't eat a sprout
    int eatSeconds = 8;
    int sleepStartHour = 21;       // diurnal animals sleep from here ...
    int sleepEndHour = 5;          // ... to here
    int nocturnalStartHour = 7;
    int nocturnalEndHour = 17;
    std::vector<std::string> nocturnal;  // def names that sleep by day
    float edgeReach = 8.0f;        // metres from the map edge within which an animal may leave
    float leaveChance = 0.001f;     // per decision there; four times that while the wild herd is over its density
    float entryChance = 0.35f;     // per game hour that a group walks in, while the wild herd is under its density
    float underTarget = 0.9f;      // share of the biome's density under which animals come in
    float overTarget = 1.15f;      // and over which they leave faster
    float areaPerDensity = 10000;  // square metres one unit of the biome's animalDensity covers
    float groupSpread = 3.5f;      // metres round its spot a new group is placed
    float startClearRadius = 25;   // no animal is placed this close to a colonist when the map is made
    int corpseDays = 15;           // game days a corpse lies there
    float startAgeMax = 0.5f;      // placed animals are as old as this share of their life expectancy at most
    // N5 (animal-fights.md, all ours except meatNutrition).
    float scareRadius = 22.0f;     // calm animals this close to a shot or a blow in a fight run from it
    float fleeDistance = 20.0f;    // how far they run
    int fleeSteps = 240;           // and for how long, at most
    float herdAlert = 20.0f;       // herd mates this close to a hurt animal run too
    float manhunterChanceFlagged = 0.5f;   // chance a hurt animal that `race.manhunterOnDamage` turns manhunter
    float manhunterChancePredator = 0.2f;  // a hurt predator
    float manhunterChanceOther = 0.02f;    // any other
    int manhunterSteps = 7200;     // a manhunter stays one for this long (two game hours)
    int revengeSteps = 300;        // a hurt predator or tusker that fights back stays at it this long
    float manhunterSight = 45.0f;  // metres a manhunter spots a colonist at
    float chaseGiveUp = 60.0f;     // a chase is dropped when the enemy gets this far away
    int huntChaseSteps = 1200;     // a hunt that has not caught its prey by then is given up
    float preyFleeSpeed = 0.8f;    // share of its top speed prey runs at from a predator (the predator runs flat out)
    int huntCooldown = 900;        // and the predator does not look for prey again for this long
    int huntLook = 300;            // nor when it found none
    float predatorHungryBelow = 0.4f;  // share of its food capacity under which a predator hunts
    float huntRadius = 35.0f;      // metres a predator looks for prey
    float preyMaxRatio = 1.5f;     // prey up to this times the predator's body size
    float loneRadius = 10.0f;      // a pawn with no other colonist this close is alone
    int gorgeSteps = 240;          // eating a kill
    int seizedSteps = 90;          // prey in a predator's jaws stands frozen this long, renewed while the predator keeps at it
    float meatNutrition = 0.05f;   // nutrition per unit of meat (RimWorld's raw meat)
};

struct AnimalStats {
    uint64_t arrived = 0;   // animals that walked in from the edge
    uint64_t left = 0;      // animals that walked off the edge
    uint64_t grazed = 0;    // plants eaten
    uint64_t cleared = 0;   // corpses that rotted away
    uint64_t manhunters = 0;  // animals that turned manhunter
    uint64_t revenges = 0;    // animals that fought back at whoever hurt them
    uint64_t hunts = 0;       // hunts begun
    uint64_t kills = 0;       // hunted prey that died
    uint64_t meals = 0;       // kills eaten
    uint64_t scares = 0;      // animals that ran from a fight
};

// What the save keeps of the system itself; the animals are components.
struct AnimalSaved {
    bool populated = false;
    int percent = 0;
    uint64_t rngState = 0;
    uint64_t rngIncrement = 0;
    uint32_t nextHerd = 1;
    bool over = false;     // the wild herd was over its density at the last hourly count
    AnimalStats stats;
};

// The component a freshly made animal gets: an adult, full of the defaults. Used by Things::make.
COLONY_API Animal makeAnimalFor(const Def& def);

class COLONY_API AnimalSystem {
public:
    // `biome` names the Biomes def that says which animals live here and how many.
    AnimalSystem(Things& things, const Map& map, Plants& plants, HealthSystem& health, const DefDatabase& defs,
                 const GameTime& time, uint64_t seed, const std::string& biome);
    ~AnimalSystem();
    AnimalSystem(const AnimalSystem&) = delete;
    AnimalSystem& operator=(const AnimalSystem&) = delete;

    void setEventLog(std::vector<Event>* events) { events_ = events; }
    const AnimalRules& rules() const { return rules_; }

    // Fills the map with wild herds up to the biome's animal density times `percent` / 100, away
    // from colonists. Does nothing a second time, or with no biome or no animal for it. After it the
    // system keeps the wild herd at that density: animals come in from the edges when it falls
    // short and go out when it is over (`SimSettings::wildAnimals` calls this when the map is made).
    void populate(int percent);
    bool populated() const { return saved_.populated; }

    // One game second: every animal that is due decides what to do next (about every 5 to 45
    // seconds each); once an hour the wild herd is counted against the biome's density.
    void step();

    // Wakes a sleeping animal (something hurt it, or scared it).
    void wake(Entity animal);
    // Keeps an animal where it is until the game second: a handler is with it (Husbandry::hold).
    void hold(Entity animal, int64_t until);

    // ---- Fights (AnimalFights.cpp, docs/design/animal-fights.md)
    // Melee is how animals bite and claw. Without it they only run.
    void setMelee(MeleeSystem* melee) { melee_ = melee; }
    // Something hurt `victim`: a hurt animal runs, fights back or turns manhunter; its herd runs.
    // Called by the combat code the moment the hit lands.
    void hurt(Entity victim, Entity attacker);
    // Calm animals near the shots and blows among `events[from..]` run from them.
    void scare(const std::vector<Event>& events, size_t from);
    // Makes the animal an enemy of `target` (and of every colonist if `temper` is Manhunter) at once;
    // the incident tasks use it. False if it is no living animal that could.
    bool enrage(Entity animal, AnimalTemper temper, Entity target);
    // Whether the fighter AI takes the animal for an enemy.
    bool hostile(Entity animal) const;

    // ---- Tame animals (AnimalCare.cpp, docs/design/husbandry.md)
    // Husbandry says where a tame animal may be and what it eats; the pathfinder lets it walk round walls
    // to its zone and food (without one it only walks straight). Both are set by the sim after construction.
    void setHusbandry(Husbandry* husbandry) { husbandry_ = husbandry; }
    void setPathfinder(Pathfinder* pathfinder) { pathfinder_ = pathfinder; }
    // The food an animal holds now, in nutrition, and the most it can hold; 0 for a thing that is no animal.
    float foodLevel(Entity animal) const;
    float foodCapacity(Entity animal) const;
    // Adds nutrition (up to what it can hold).
    void addFood(Entity animal, float nutrition);
    // Sends a tame animal walking to a cell, round walls if need be (a male setting out for a female, Breeding.h). False if it
    // can't set off now: it is wild, asleep, not set up yet, or waiting for another path.
    bool goTo(Entity animal, CellPos goal);
    // A tame animal that has learned to haul works while it is awake, fed and unhurt (the job system asks).
    bool mayWork(Entity animal) const;

    // ---- Questions
    // Living animals on the map, wild ones, and their weight in body sizes against what the biome wants.
    size_t livingCount() const;
    size_t wildCount() const;
    float wildWeight() const;
    float targetWeight() const;
    // 0 baby, 1 juvenile, 2 adult (the order of the def's `race.lifeStages`); -1 for a thing that is no animal.
    int lifeStage(Entity animal) const;
    // The def's body size times the stage's factor; 0 for a thing that is no animal.
    float bodySize(Entity animal) const;
    // In its last life stage: old enough to mate, to give milk and wool, to work. False for a thing that is no animal.
    bool isAdult(Entity animal) const;
    // Food as a share of the animal's capacity, 0 to 1; -1 for a thing that is no animal.
    float foodShare(Entity animal) const;
    bool asleep(Entity animal) const;
    // The leader of the animal's herd: the living member with the lowest id; the animal itself if alone.
    Entity leaderOf(Entity animal) const;

    const AnimalStats& stats() const { return saved_.stats; }

    // Saving, loading and the state hash (docs/design/saves.md).
    AnimalSaved saved() const;
    // `rngState` and `rngIncrement` are put back too.
    void restore(const AnimalSaved& state);
    // Adds the system's own state to the hash; nothing at all before `populate`, so games without
    // animals hash as they always did.
    void hashState(StateHasher& h) const;
    // Adds one thing's animal component to the hash, if it has one.
    void hashThing(Entity e, StateHasher& h) const;

private:
    struct Stage {
        int64_t minAgeSeconds = 0;
        float bodySizeFactor = 1.0f;
        float foodFactor = 1.0f;
    };
    struct Kind {
        DefId def;
        float topSpeed = 0.1f;     // metres per step
        float bodySize = 1.0f;
        float foodPerDay = 1.0f;
        bool grazer = false;       // eats plants: herbivores and omnivores
        bool nocturnal = false;
        bool domestic = false;
        int herdMin = 1;
        int herdMax = 1;
        double commonality = 0.0;  // weight in this biome's wild animals, 0 if it has none
        int64_t adultAgeSeconds = 0;
        double lifeExpectancyDays = 0.0;
        bool predator = false;       // N5: hunts when hungry
        bool tusker = false;         // `race.manhunterOnDamage`
        std::vector<Stage> stages;
    };

    const Kind* kindOf(Entity animal) const;
    int stageAt(const Kind& kind, int64_t ageSeconds) const;
    float capacity(const Kind& kind, int64_t ageSeconds) const;
    float foodFallPerSecond(const Kind& kind, int64_t ageSeconds) const;

    void think(Entity e);
    // Fights (AnimalFights.cpp).
    bool thinkFight(Entity e, Animal& a, const Kind& kind, int64_t now);
    bool startHunt(Entity e, Animal& a, const Kind& kind, Rng& rng);
    void startFlee(Entity e, Animal& a, const Kind& kind, float fromX, float fromY, Rng& rng, float speedShare = 1.0f);
    void beginChase(Entity e, Animal& a, const Kind& kind, Entity enemy, AnimalTemper temper, int64_t chaseUntil);
    bool chaseStep(Entity e, Animal& a, const Kind& kind, int64_t now);
    bool feastStep(Entity e, Animal& a, const Kind& kind, int64_t now);
    Entity nearestColonist(Entity e, float range) const;
    bool walkAt(Entity e, Animal& a, const Kind& kind, float x, float y, float speedShare, AnimalMode mode);
    float meatNutritionOf(Entity corpse) const;
    // Tame animals (AnimalCare.cpp).
    void decideTamed(Entity e, Animal& a, const Kind& kind, Rng& rng);
    bool tamedMayStand(const AnimalCare* care, CellPos cell) const;
    bool findTamedFood(Entity e, Animal& a, const Kind& kind, const AnimalCare& care, Rng& rng, CellPos& out,
                       bool& isItem, bool& blocked) const;
    Entity edibleItemAt(Entity animal, CellPos cell) const;
    bool walkOrPath(Entity e, Animal& a, const Kind& kind, AnimalCare& care, CellPos goal, AnimalMode mode);
    bool pollPath(Entity e, Animal& a, const Kind& kind, AnimalCare& care);
    void cancelPath(AnimalCare& care);
    void finishEatingItem(Entity e, Animal& a, const Kind& kind);
    void starveCheck(Entity e, Animal& a, const Kind& kind, AnimalCare& care);
    void begin(Entity e, Animal& a, const Kind& kind);
    void decide(Entity e, Animal& a, const Kind& kind, Rng& rng);
    bool walkTo(Entity e, Animal& a, const Kind& kind, float x, float y, AnimalMode mode);
    bool findFood(Entity e, const Kind& kind, Rng& rng, CellPos& out) const;
    bool tryLeave(Entity e, Animal& a, const Kind& kind, Rng& rng);
    void finishEating(Entity e, Animal& a, const Kind& kind);
    bool sleepNow(const Kind& kind, const Animal& a, int64_t now) const;
    int64_t sleepEnd(const Kind& kind, const Animal& a, int64_t now) const;
    void hourly(int64_t now);
    int pickKind(Rng& rng) const;
    // Places a group of `kind` round `centre`; returns the weight placed (body sizes).
    float placeGroup(int kind, CellPos centre, Rng& rng, bool adultsOnly, bool entering);
    bool pickGroundCell(Rng& rng, bool clearOfColonists, CellPos& out) const;
    bool pickEdgeCell(Rng& rng, CellPos& out) const;
    void rebuildLeaders();

    Things& things_;
    const Map& map_;
    Plants& plants_;
    HealthSystem& health_;
    const DefDatabase& defs_;
    const GameTime& time_;
    uint64_t seed_;
    AnimalRules rules_;
    Rng rng_;                       // populate and the hourly count; each animal's decisions use their own stream
    AnimalSaved saved_;
    std::vector<Kind> kinds_;
    std::vector<int> kindOfDef_;    // by DefId.value: index into kinds_, or -1
    std::vector<float> nutrition_;  // by DefId.value: what a plant gives when eaten, 0 if animals leave it alone
    double totalCommonality_ = 0.0;
    float density_ = 0.0f;          // the biome's animalDensity
    bool hasBiome_ = false;
    std::vector<Event>* events_ = nullptr;
    Husbandry* husbandry_ = nullptr;
    Pathfinder* pathfinder_ = nullptr;
    MeleeSystem* melee_ = nullptr;
    std::vector<Entity> due_;       // scratch: animals to decide this step, in id order
    std::unordered_map<uint32_t, Entity> leaders_;  // scratch: herd id to its leader, rebuilt when needed
};

}  // namespace colony
