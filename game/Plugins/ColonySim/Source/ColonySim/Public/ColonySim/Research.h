#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace colony {

class StateHasher;
class Things;
class TraitBook;
struct Event;

// Research (task C21). See docs/design/research.md.
//
// The colony has one current project at a time. A colonist with the Research work type sits at a
// research bench and adds points to it each game second; at the project's cost it is done, and the
// things, recipes and ground it unlocks (a def's `researchPrerequisite`) can then be planned and
// made. Unreal does not include this header: it reads the projects from `Snapshot::research`.

// One project, from the Research defs.
struct ResearchProject {
    DefId def;
    std::string defName;
    std::string techLevel;
    double baseCost = 0.0;
    int benchTier = 1;
    std::vector<std::string> prerequisites;  // visible and hidden ones together: all must be done
};

// The numbers from ResearchRules (data/defs/Rules). `present` is false without that def.
struct COLONY_API ResearchRules {
    bool present = false;
    double pointsPerTick = 0.00825;     // one pawn at 100% speed, per RimWorld tick
    double secondsPerTick = 1.44;       // one RimWorld tick is this many sim seconds
    double skillSpeedBase = 0.2;        // research speed = base + perLevel x Intellectual level
    double skillSpeedPerLevel = 0.1;
    int defaultSkillLevel = 8;          // for a worker with no skills (a test pawn)

    static ResearchRules read(const DefDatabase& defs);
    double pointsPerSecond() const { return pointsPerTick / secondsPerTick; }
};

// What the benches of one Things def do (the def's `researchBench`).
struct ResearchBenchInfo {
    int tier = 1;
    double speedFactor = 1.0;
};

// The saved state of the research system.
struct ResearchState {
    std::string current;                       // the project being worked on, or empty
    std::vector<std::string> done;             // sorted
    std::map<std::string, double> progress;    // started, unfinished projects, by defName
    bool neolithicStart = false;               // projects cost more (see TechLevels)
};

class COLONY_API ResearchSystem {
public:
    ResearchSystem(Things& things, const DefDatabase& defs);

    const ResearchRules& rules() const { return rules_; }
    const std::vector<ResearchProject>& projects() const { return projects_; }
    const ResearchProject* find(const std::string& defName) const;

    const ResearchState& state() const { return state_; }
    ResearchState& state() { return state_; }
    const std::string& current() const { return state_.current; }
    bool isDone(const std::string& project) const;
    // Points put into a project so far (its cost once it is done).
    double progress(const std::string& project) const;
    // Points a project needs: its base cost, times the tech level's factor for a neolithic start.
    double cost(const ResearchProject& project) const;
    // Every prerequisite is done and the project is not.
    bool available(const ResearchProject& project) const;

    // ---- Commands ----------------------------------------------------------------------------
    // Picks the project the colony works on; empty puts it down. Fails for a project that doesn't exist,
    // is done or has prerequisites missing.
    bool setCurrent(const std::string& project, std::string& error);
    // Dev: a project is done at once, whatever it needs. Fails for a project that doesn't exist.
    bool unlock(const std::string& project, std::string& error);
    void setNeolithicStart(bool neolithic) { state_.neolithicStart = neolithic; }

    // ---- Gating ------------------------------------------------------------------------------
    // Whether everything a def's `researchPrerequisite` names is done (true for an ungated def). Otherwise
    // `error` says which project is missing, in plain words.
    bool unlocked(const Def& def, std::string& error) const;
    bool unlocked(const Def& def) const;

    // ---- Working -----------------------------------------------------------------------------
    // The tier and speed of a Things def that is a research bench, or null.
    const ResearchBenchInfo* benchInfo(DefId thingDef) const;
    // Whether a bench can work on the current project (a project is chosen, available, and the bench's tier is enough).
    bool benchCanWork(const ResearchBenchInfo& bench) const;
    // Industrious researchers work faster (docs/design/traits.md); null: no trait does.
    void setTraits(const TraitBook* traits) { traits_ = traits; }
    // Points per game second for this pawn at this bench.
    double speedAt(Entity pawn, const ResearchBenchInfo& bench) const;
    // Adds points to the current project. Returns true when that finished it (the event is appended to `events`).
    bool addProgress(double points, Entity pawn, std::vector<Event>& events);

    // Hash of the state, for Sim::stateHash.
    void hashState(StateHasher& h) const;

private:
    Things& things_;
    const DefDatabase& defs_;
    const TraitBook* traits_ = nullptr;
    ResearchRules rules_;
    std::vector<ResearchProject> projects_;
    std::map<uint32_t, ResearchBenchInfo> benches_;  // by Things def (DefId::value)
    ResearchState state_;
};

}  // namespace colony
