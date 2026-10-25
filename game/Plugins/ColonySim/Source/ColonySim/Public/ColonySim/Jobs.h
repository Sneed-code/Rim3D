#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Designations.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"
#include "ColonySim/Grid.h"
#include "ColonySim/JobType.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Pathfinder.h"
#include "ColonySim/Zones.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace colony {

// The job system, as RimWorld's: each pawn walks a think list to pick a job, and a job is a short
// list of steps with fail checks. See docs/design/jobs.md.
//
// Unreal does not include this header: it reads jobs through the snapshot and events and changes
// work priorities and zones through commands (docs/design/bridge.md).

// What a job, a scanner or a reservation points at: a thing, or a cell when `thing` is kNoEntity.
struct JobTarget {
    Entity thing = kNoEntity;
    CellPos cell;
};

// One job in progress. Which fields a job type uses is in its comment above.
struct Job {
    JobType type = JobType::None;
    Entity thing = kNoEntity;  // HaulToCell, Ingest: the item (the part in the pawn's hands once picked up);
                               // LayDown: the bed, or none for the ground
    CellPos cell;              // HaulToCell: the storage cell; Ingest: where it eats (a chair, else where
                               // the food lay); LayDown: the bed's cell for this pawn, or where it lies
    int count = 0;             // HaulToCell, Ingest: how many to carry
    Entity chair = kNoEntity;  // Ingest: the chair at a table it eats in, or none (it eats where the food lay)
    DefId workType;            // the work type whose scanner gave it; none for orders and idling
    bool forced = false;       // a player order (OrderHaulCommand)
    uint8_t step = 0;          // which of the job type's steps is running
    uint8_t walk = 0;          // inside a go-to step: 0 not set off, 1 waiting for a path, 2 walking
    uint8_t repaths = 0;       // new paths asked for after giving up a walk
    int64_t startedAt = 0;     // game second
    int64_t until = 0;         // Wait: the game second it ends; Ingest: the game second it finishes eating
                               // (0 until it starts)
    float work = 0.0f;         // Mine: work towards the next pick hit; CutPlant, HarvestPlant: work done on the
                               // plant. Both in game seconds at 100% work speed (the miner's speed adds to it
                               // every second). Mine, CutPlant, HarvestPlant: `cell` is the rock or plant
    PathTicket path;           // while `walk` is 1
};

// A target a job failed on lately, which the pawn's scanners skip until `until`.
struct AvoidTarget {
    JobTarget target;
    int64_t until = 0;
};

// Pawns that think and take jobs: colonists (and, later, anyone with a mind).
struct Worker {
    Job job;
    std::vector<AvoidTarget> avoid;  // oldest first, at most JobSystem::kMaxAvoid
};

// A pawn's Work tab row: one entry per work type, in Work tab order. Priority 0 means the pawn
// doesn't do it; 1 comes first and 4 last, as in RimWorld. `disabled` is for work the pawn can
// never do (its backstory, C8), which no command can switch on.
struct WorkPriority {
    DefId workType;
    uint8_t priority = 0;
    bool disabled = false;
};

struct WorkSettings {
    std::vector<WorkPriority> entries;
};

// On a pawn carrying something. The carried thing is off the map (no Position).
struct Carrying {
    Entity thing = kNoEntity;
};

// True for the jobs that work on a designated cell (rock or plant) and hold that cell.
inline bool jobIsGather(JobType type) {
    return type == JobType::Mine || type == JobType::CutPlant || type == JobType::HarvestPlant;
}

// How many steps a job type has (for save checks).
COLONY_API int jobStepCount(JobType type);

// LayDown has two steps, walking to the bed and lying in it; rest rises during the second.
inline constexpr uint8_t kLayDownLieStep = 1;
// Ingest has four: go to the food, pick it up, go to the eating spot, eat. The pawn carries the
// food from the third on.
inline constexpr uint8_t kIngestPickUpStep = 1;
inline bool jobIsLyingDown(const Job& job) {
    return job.type == JobType::LayDown && job.step >= kLayDownLieStep;
}

class DefDatabase;
class HealthSystem;
class Map;
class Mineables;
class Pathfinder;
class Plants;
class Things;
class WorkGiver;
struct GameTime;
struct SimStats;

// A thing or cell a scanner offers, with its straight-line distance from the pawn.
struct WorkCandidate {
    JobTarget target;
    float distSq = 0.0f;
    uint16_t workType = 0;  // index in JobSystem::workTypes()
    uint16_t giver = 0;     // index in that work type's givers
};

// Runs every Worker once per step: thinks for the ones without a job, runs the jobs of the rest.
// Pawns go in entity id order, so a pawn with a lower id gets first pick of a target.
class COLONY_API JobSystem {
public:
    // Numbers from JobRules in data/defs/Rules (with these values when the def is missing).
    static constexpr int kDefaultPriority = 3;  // RimWorld: work a new colonist can do starts at 3
    static constexpr int kMaxPriority = 4;
    static constexpr size_t kMaxAvoid = 8;

    // A work type from the WorkTypes defs, with the scanners its `tasks` name that exist so far.
    struct WorkType {
        DefId def;
        std::string defName;
        int naturalPriority = 0;
        std::vector<std::string> workTags;  // a colonist with one of these disabled can't do it
        std::vector<WorkGiver*> givers;  // in `tasks` order; owned by the job system
    };

    JobSystem(Map& map, Things& things, Pathfinder& pathfinder, Zones& zones, Plants& plants, Mineables& mineables,
              Designations& designations, const DefDatabase& defs, const HealthSystem& health,
              const NeedsSystem& needs, const GameTime& time, SimStats& stats, std::vector<Event>& events);
    ~JobSystem();
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    // Every work type, in Work tab order (`naturalPriority`, highest first, as RimWorld).
    const std::vector<WorkType>& workTypes() const { return workTypes_; }
    // Index in workTypes(), or -1.
    int workTypeIndex(DefId def) const;

    // Makes a pawn a worker: no job, and priority 3 on every work type it can do.
    // Work types with a tag in `disabledWorkTags` (a colonist's backstories and traits) are off for
    // good, as RimWorld's "incapable of". With `withNeeds` (and a NeedRules def) it also gets food,
    // rest and the standard schedule (Needs.h).
    void addWorker(Entity pawn, const std::vector<std::string>& disabledWorkTags = {}, bool withNeeds = true);
    // A Work tab row with the default priorities.
    WorkSettings defaultWorkSettings(const std::vector<std::string>& disabledWorkTags = {}) const;

    // One step for every worker.
    void step();

    // Commands. Return false, with a reason in plain words, when they can't be done.
    bool setPriority(Entity pawn, DefId workType, int priority, std::string& error);
    // A player order: haul this item to the best stockpile now, ending the pawn's current job.
    // Refused for the pawn the player controls directly (PlayerControl).
    bool orderHaul(Entity pawn, Entity thing, std::string& error);
    // Ends the job of any pawn mining, cutting or picking at this cell, as interrupted: the mark was
    // taken off (CancelDesignationsCommand).
    void cancelGatherAt(CellPos cell);
    // Ends the pawn's job, if it has one, as interrupted for `why`: reservations let go, what it
    // carried put down. Used when the player takes the pawn over (ControlPawnCommand).
    void interrupt(Entity pawn, const std::string& why);
    // Schedule: what a pawn does in an hour of the day (0 to 23).
    bool setSchedule(Entity pawn, int hour, ScheduleKind kind, std::string& error);
    // Bed ownership. `pawn` becomes an owner of `bed` (and stops owning any other bed); kNoEntity
    // takes every owner off the bed. Fails if the bed is full or either is not what it should be.
    bool assignBed(Entity bed, Entity pawn, std::string& error);
    // The bed a pawn owns, or kNoEntity.
    Entity bedOf(Entity pawn) const;
    const NeedRules& needRules() const { return rules_; }

    // Call before a thing is destroyed: ends the jobs that carry it or are done by it.
    void forget(Entity thing);
    // After loading a save: rebuilds the reservations from the jobs. Returns false, with
    // `error`, if two jobs hold the same target or a job waits for a path nobody looks for.
    bool afterLoad(std::string& error);

    // ---- For scanners and tests -----------------------------------------------------------
    Things& things() { return things_; }
    Map& map() { return map_; }
    Zones& zones() { return zones_; }
    Plants& plants() { return plants_; }
    Mineables& mineables() { return mineables_; }
    Designations& designations() { return designations_; }
    const DefDatabase& defs() const { return defs_; }
    int64_t now() const;

    // The pawn holding a target, or kNoEntity.
    Entity reservedBy(const JobTarget& target) const;
    bool reservedByOther(const JobTarget& target, Entity pawn) const;
    bool avoiding(Entity pawn, const JobTarget& target) const;
    bool canReach(CellPos from, CellPos to) const;

    // Gathering (GatherJobs.cpp, docs/design/mining.md). Whether a pawn can use its hands for
    // mining and cutting: its Manipulation capacity is above 0.
    bool canUseHands(Entity pawn) const;
    // Whether a designated plant can be harvested now: its def gives something and it has grown
    // to `plant.harvestMinGrowth`.
    bool harvestableNow(CellPos cell) const;
    // Where `pawn` would stand to work on `target`: a walkable cell next to it that `from` can
    // reach (the nearest, `from` itself if it is one), else the target itself if it is walkable and
    // reachable. False if there is none.
    bool touchCell(CellPos from, CellPos target, CellPos& out) const;
    // How fast `pawn` mines (1 is 100%): 4% plus 12% per Mining level, times its Manipulation.
    double miningSpeed(Entity pawn) const;
    // How fast it cuts and picks: 8% plus 11.5% per Plants level, times Manipulation, at least 10%.
    double plantWorkSpeed(Entity pawn) const;
    // A skill level of a pawn: from its Colonist record, `unskilledLevel` for a pawn with none.
    int skillLevel(Entity pawn, const char* skill) const;

    // How well an item is stored where it lies: its stockpile's priority if that stockpile takes
    // it, else Unstored. Unstored for an item off the map.
    StoragePriority storedPriority(Entity item) const;
    // How many more of `def` a cell can take: a full stack on an empty storage cell, the room
    // left on a stack of the same def, 0 for anything else (another def, a building that isn't
    // standable, a cell that can't be walked).
    int roomFor(DefId def, CellPos cell) const;
    // The best cell to store `def` brought from `from` by `pawn`: the highest priority above
    // `better`, then the nearest, that takes the def, has room, isn't held by another pawn and
    // can be reached from `from`. False if there is none.
    bool findStorageCell(DefId def, CellPos from, StoragePriority better, Entity pawn, CellPos& out) const;
    // How many of an item def a pawn can carry at once: RimWorld's `carryingCapacity` (75 when the
    // pawn def has none), never more than a stack.
    int carryLimit(Entity pawn, DefId def) const;
    int stackLimit(DefId def) const;

private:
    enum class StepResult : uint8_t { Running, Next, Failed };
    enum class JobEnd : uint8_t { Done, Failed, Interrupted };

    struct TargetKey {
        uint32_t thing = 0xFFFFFFFFu;
        int x = 0;
        int y = 0;
        int level = 0;
        friend bool operator<(const TargetKey& a, const TargetKey& b) {
            if (a.thing != b.thing) return a.thing < b.thing;
            if (a.level != b.level) return a.level < b.level;
            if (a.y != b.y) return a.y < b.y;
            return a.x < b.x;
        }
    };
    static TargetKey keyOf(const JobTarget& target);

    // The think list: each node may give a job; the first that does wins (docs/design/jobs.md).
    using ThinkNode = bool (JobSystem::*)(Entity pawn, Job& out);
    bool thinkNeeds(Entity pawn, Job& out);
    bool thinkSchedule(Entity pawn, Job& out);
    bool thinkWork(Entity pawn, Job& out);
    bool thinkIdle(Entity pawn, Job& out);

    // Needs (NeedJobs.cpp). A pawn on a job it may drop (hauling, waiting, mining, cutting) is checked every step:
    // when it is hungry enough to eat or tired enough to sleep, the need job replaces its own.
    void interruptForNeeds(Entity pawn);
    ScheduleKind scheduleNow(Entity pawn) const;
    float sleepBelow(ScheduleKind slot) const;
    bool findFood(Entity pawn, Job& out);
    // The chair, next to a table, that `pawn` would eat at when the food lies at `from`: the nearest
    // within NeedRules.eatSpotRadius that it can reach and nobody holds. kNoEntity if there is none.
    Entity findChair(Entity pawn, CellPos from, CellPos& cell) const;
    bool makeSleepJob(Entity pawn, Job& out);
    bool findBed(Entity pawn, Entity& bed, CellPos& cell);
    bool bedSlotCell(Entity bed, Entity pawn, CellPos& out) const;
    StepResult eat(Entity pawn, Job& job, std::string& why);
    StepResult lie(Entity pawn, Job& job, std::string& why);
    bool failCheckIngest(Entity pawn, Job& job, std::string& why);
    bool failCheckLayDown(Entity pawn, Job& job, std::string& why);
    void wakeUp(Entity pawn, const Job& job);
    // The targets a job holds, which no other pawn's scanner takes; up to two.
    int heldTargets(const Job& job, JobTarget out[2]) const;

    // Gathering jobs (GatherJobs.cpp).
    bool failCheckGather(Entity pawn, Job& job, std::string& why);
    StepResult goToTouch(Entity pawn, Job& job, std::string& why);
    StepResult mineRock(Entity pawn, Job& job, std::string& why);
    StepResult plantWork(Entity pawn, Job& job, std::string& why);
    void learnSkill(Entity pawn, const char* skill, double xp);
    double yieldFactor(int level) const;
    int plantYield(Entity pawn, CellPos cell) const;
    void dropItems(DefId def, int count, CellPos cell);
    void gatherEvent(Entity pawn, PawnAction action, CellPos cell);
    void loadGatherRules();

    void startJob(Entity pawn, Job job);
    void tickJob(Entity pawn);
    void endJob(Entity pawn, JobEnd end, const std::string& why);
    bool failCheck(Entity pawn, Job& job, std::string& why);
    StepResult runStep(Entity pawn, Job& job, std::string& why);
    StepResult walkTo(Entity pawn, Job& job, CellPos goal, std::string& why);
    StepResult pickUp(Entity pawn, Job& job, std::string& why);
    StepResult place(Entity pawn, Job& job, std::string& why);
    bool retargetStorage(Entity pawn, Job& job);
    void dropCarried(Entity pawn);
    void reserve(Entity pawn, const Job& job);
    void release(Entity pawn, const Job& job);
    void jobEvent(Entity pawn, const Job& job, bool started, const std::string& message);
    void pawnDid(Entity pawn, PawnAction action, Entity target);

    Map& map_;
    Things& things_;
    Pathfinder& pathfinder_;
    Zones& zones_;
    Plants& plants_;
    Mineables& mineables_;
    Designations& designations_;
    const DefDatabase& defs_;
    const HealthSystem& health_;
    const NeedsSystem& needs_;
    const NeedRules& rules_;
    const GameTime& time_;
    SimStats& stats_;
    std::vector<Event>& events_;

    std::vector<std::unique_ptr<WorkGiver>> givers_;
    std::vector<WorkType> workTypes_;
    std::vector<uint8_t> standable_;  // per def id: a building pawns can stand on (storage allowed)
    std::vector<int> stackLimits_;    // per def id
    // Per def id, from the Things defs (NeedJobs.cpp): food tiers (-1 not food), nutrition and how
    // long a sitting takes; the kinds of building.
    std::vector<int8_t> foodTier_;
    std::vector<float> nutrition_;
    std::vector<uint16_t> maxIngest_;
    std::vector<float> ingestSeconds_;
    std::vector<uint8_t> isBed_;
    std::vector<uint8_t> isTable_;
    std::vector<uint8_t> isChair_;
    // WorkRules and SkillRules (GatherJobs.cpp), with RimWorld's numbers when the defs are missing.
    struct GatherRules {
        double miningSpeedBase = 0.04;
        double miningSpeedPerLevel = 0.12;
        double hitDamage = 80.0;
        double hitSeconds = 144.0;
        double miningXpPerSecond = 0.048611;
        double plantSpeedBase = 0.08;
        double plantSpeedPerLevel = 0.115;
        double plantXpPerSecond = 0.059028;
        double growthYieldFloor = 0.5;
        int unskilledLevel = 8;
        std::vector<double> yieldByLevel;
        std::vector<double> xpToNextLevel;
        double passionLearn[3] = {0.35, 1.0, 1.5};
    } gather_;
    int manipulationIndex_ = -1;  // position of the Manipulation capacity in Health::capacities, or -1
    std::map<TargetKey, Entity> reservations_;  // follows from the jobs; rebuilt on load
    int64_t idleSeconds_ = 0;
    int64_t avoidSeconds_ = 0;
    int maxRepaths_ = 0;
    uint8_t defaultPriority_ = kDefaultPriority;
    std::vector<Entity> order_;              // scratch: workers this step
    std::vector<WorkCandidate> candidates_;  // scratch: one think
};

// A work scanner, as RimWorld's WorkGiver_Scanner: offers targets, then makes a job on one.
// Named by the `tasks` of the WorkTypes defs; a task with no scanner yet gives no work.
class COLONY_API WorkGiver {
public:
    virtual ~WorkGiver() = default;
    // The task name in the WorkTypes defs, e.g. "HaulToStockpile".
    virtual const char* task() const = 0;
    // Appends every target that might have work for `pawn`, with its distance. Cheap checks only;
    // the job system then tries them nearest first and skips reserved, avoided and unreachable ones.
    virtual void scan(JobSystem& jobs, Entity pawn, std::vector<WorkCandidate>& out) = 0;
    // Makes the job on one target, or returns false if there is none after all.
    virtual bool makeJob(JobSystem& jobs, Entity pawn, const WorkCandidate& candidate, Job& out) = 0;
};

// The scanners that exist so far, one per task name. Later tasks add theirs here.
std::vector<std::unique_ptr<WorkGiver>> makeWorkGivers();
// Haul jobs, shared by the hauling scanner and the haul order: the job to store `item`, or false.
bool makeHaulJob(JobSystem& jobs, Entity pawn, Entity item, Job& out);

}  // namespace colony
