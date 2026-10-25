#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"
#include "ColonySim/Grid.h"
#include "ColonySim/JobType.h"
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
    Entity thing = kNoEntity;  // HaulToCell: the item (the part in the pawn's hands once picked up)
    CellPos cell;              // HaulToCell: the storage cell
    int count = 0;             // HaulToCell: how many to carry
    DefId workType;            // the work type whose scanner gave it; none for orders and idling
    bool forced = false;       // a player order (OrderHaulCommand)
    uint8_t step = 0;          // which of the job type's steps is running
    uint8_t walk = 0;          // inside a go-to step: 0 not set off, 1 waiting for a path, 2 walking
    uint8_t repaths = 0;       // new paths asked for after giving up a walk
    int64_t startedAt = 0;     // game second
    int64_t until = 0;         // Wait: the game second it ends
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

// How many steps a job type has (for save checks).
COLONY_API int jobStepCount(JobType type);

class DefDatabase;
class HealthSystem;
class Map;
class Pathfinder;
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

    JobSystem(Map& map, Things& things, Pathfinder& pathfinder, Zones& zones, const DefDatabase& defs,
              const HealthSystem& health, const GameTime& time, SimStats& stats, std::vector<Event>& events);
    ~JobSystem();
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    // Every work type, in Work tab order (`naturalPriority`, highest first, as RimWorld).
    const std::vector<WorkType>& workTypes() const { return workTypes_; }
    // Index in workTypes(), or -1.
    int workTypeIndex(DefId def) const;

    // Makes a pawn a worker: no job, and priority 3 on every work type it can do.
    // Work types with a tag in `disabledWorkTags` (a colonist's backstories and traits) are off for
    // good, as RimWorld's "incapable of".
    void addWorker(Entity pawn, const std::vector<std::string>& disabledWorkTags = {});
    // A Work tab row with the default priorities.
    WorkSettings defaultWorkSettings(const std::vector<std::string>& disabledWorkTags = {}) const;

    // One step for every worker.
    void step();

    // Commands. Return false, with a reason in plain words, when they can't be done.
    bool setPriority(Entity pawn, DefId workType, int priority, std::string& error);
    // A player order: haul this item to the best stockpile now, ending the pawn's current job.
    bool orderHaul(Entity pawn, Entity thing, std::string& error);

    // Call before a thing is destroyed: ends the jobs that carry it or are done by it.
    void forget(Entity thing);
    // After loading a save: rebuilds the reservations from the jobs. Returns false, with
    // `error`, if two jobs hold the same target or a job waits for a path nobody looks for.
    bool afterLoad(std::string& error);

    // ---- For scanners and tests -----------------------------------------------------------
    Things& things() { return things_; }
    Map& map() { return map_; }
    Zones& zones() { return zones_; }
    const DefDatabase& defs() const { return defs_; }
    int64_t now() const;

    // The pawn holding a target, or kNoEntity.
    Entity reservedBy(const JobTarget& target) const;
    bool reservedByOther(const JobTarget& target, Entity pawn) const;
    bool avoiding(Entity pawn, const JobTarget& target) const;
    bool canReach(CellPos from, CellPos to) const;

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
    bool thinkWork(Entity pawn, Job& out);
    bool thinkIdle(Entity pawn, Job& out);

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
    const DefDatabase& defs_;
    const HealthSystem& health_;
    const GameTime& time_;
    SimStats& stats_;
    std::vector<Event>& events_;

    std::vector<std::unique_ptr<WorkGiver>> givers_;
    std::vector<WorkType> workTypes_;
    std::vector<uint8_t> standable_;  // per def id: a building pawns can stand on (storage allowed)
    std::vector<int> stackLimits_;    // per def id
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
