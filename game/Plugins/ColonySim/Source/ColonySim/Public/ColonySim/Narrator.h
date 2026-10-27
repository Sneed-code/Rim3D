#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Grid.h"
#include "ColonySim/Random.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace colony {

class RaidSystem;
class StateHasher;
struct GameTime;

// The narrator, its incidents and threat points (task T1). See docs/design/narrator.md.
//
// A narrator (Orrin, Maren or Jinx: RimWorld's Cassandra, Phoebe and Randy) decides when something
// happens to the colony. Every incident check (RimWorld's 1,000 ticks, 24 game minutes) it rolls each
// incident category against the narrator's mean days between (`mtbDays`); Orrin and Maren also send
// their big threats on an on/off cycle. Within a category an incident is picked by weight among those
// that can happen now (earliest day passed, cooldown over, enough colonists, and something that can
// carry it out). A threat gets threat points: the colony's raid points (wealth and colonists by
// RimWorld's curves, RaidSystem::threatPoints) times the difficulty's threat scale and the narrator's
// points factor. Each incident that happens says so in a Letter event.
//
// An incident happens through its **worker**, registered by name. The raid (F6) is the first; the
// event tasks (T2, T3, C23) add theirs with `setWorker`. Incidents without one never come.
//
// The narrator is off until a SetNarratorCommand picks one (U20 does at game start; `simrun
// --narrator`), so tests and old saves don't get raids they never asked for.
//
// Unreal does not include this header: it reads the snapshot's `narrator` and the Letter events,
// and sends SetNarratorCommand and FireIncidentCommand.

// How a letter looks (RimWorld's letter colours): NarratorRules.letterKinds, in this order.
enum class LetterKind : uint8_t {
    ThreatBig,      // red: a raid, a manhunter pack
    NegativeEvent,  // orange: a disease, a blight
    NeutralEvent,   // grey-blue: a trader, an eclipse
    PositiveEvent,  // blue: a wanderer, a cargo pod
};

// Incident categories (D10's `category`), each rolled on its own.
enum class IncidentCategory : uint8_t {
    ThreatBig,
    ThreatSmall,
    Misc,
    FactionArrival,
    OrbitalVisitor,
    Disease,
};
inline constexpr int kIncidentCategoryCount = 6;

// One incident about to happen, handed to its worker.
struct IncidentFiring {
    std::string incident;  // Incidents defName
    float points = 0.0f;   // threat points for it; 0 for incidents without `points`
    bool forced = false;   // a FireIncidentCommand: timing rules were skipped
    // Filled in by the worker.
    std::string text;       // the letter's text, in plain words
    bool hasCell = false;   // where it happened, for the letter's jump-to button
    CellPos cell;
    uint32_t squad = 0;     // a raid's squad
    std::string error;      // why it could not happen (the worker returned false)
};

// Carries an incident out. False, with `error`, if it can't happen now (no hostile faction, no way
// in); the narrator then tries another of the category. It must not fail half-done.
using IncidentWorker = std::function<bool(IncidentFiring&)>;

// The last time an incident happened, for its cooldown.
struct IncidentFired {
    std::string incident;  // Incidents defName
    int64_t at = 0;        // game second
};

// Everything the narrator keeps between steps; saved as a whole (section "narrator").
struct NarratorState {
    std::string narrator;    // Narrators defName; empty: no narrator, nothing comes by itself
    std::string difficulty;  // Difficulties defName
    uint64_t rngState = 0;
    uint64_t rngIncrement = 0;
    int64_t cycleStart = -1;          // game second the current threat cycle began (-1: none yet)
    std::vector<int64_t> threats;     // big threats still to come in this cycle, game seconds, in order
    std::vector<IncidentFired> fired; // by incident name
    uint32_t incidents = 0;           // incidents that happened so far
    float wealth = 0.0f;              // the colony's wealth and threat points at the last check
    float points = 0.0f;
};

// The numbers (NarratorRules, the Narrators and Difficulties defs).
struct NarratorRules {
    struct Point {
        float x, y;
    };
    std::string defaultNarrator = "Orrin";
    std::string defaultDifficulty = "StriveToSurvive";
    std::vector<Point> populationIntent{{0, 8.0f}, {1, 2.0f}, {5, 1.0f}, {9, 0.4f}, {12, 0.0f}, {20, -1.0f}};
    std::vector<Point> populationIntentByDay{{0, 0.0f}, {10, 1.0f}};
    float minIncidentChanceFactor = 0.05f;
    int checkSeconds = 1440;  // RimWorld's storyteller tick: every 1,000 ticks
};

class COLONY_API NarratorSystem {
public:
    NarratorSystem(RaidSystem& raids, const DefDatabase& defs, const GameTime& time, uint64_t seed);
    ~NarratorSystem();
    NarratorSystem(const NarratorSystem&) = delete;
    NarratorSystem& operator=(const NarratorSystem&) = delete;

    void setEventLog(std::vector<Event>* events) { events_ = events; }
    const NarratorRules& rules() const { return rules_; }

    // The worker that carries out an incident (replaces any before it). The raid's is built in.
    void setWorker(const std::string& incident, IncidentWorker worker);
    bool hasWorker(const std::string& incident) const;

    // Commands. False, with a reason in plain words, when they can't be done.
    // Picks the narrator ("" turns it off) and the difficulty ("" keeps it, or the default).
    bool setNarrator(const std::string& narrator, const std::string& difficulty, std::string& error);
    // Makes an incident happen now, whatever its timing (`points` 0: the colony's threat points).
    bool fire(const std::string& incident, float points, std::string& error);

    // The clock has just advanced: on every incident check, rolls the categories and sends the
    // cycle's big threats that are due.
    void step();

    // The colony's threat points now for the difficulty and narrator (Jinx's random factor not
    // included), between RaidRules' minimum and maximum.
    float threatPoints() const;
    // How much the population pushes incidents marked `populationScaled` (wanderers, survivors).
    float populationFactor() const;
    // The chance weight of an incident now, 0 if it can't happen (timing, colonists, no worker).
    float weight(const std::string& incident) const;

    NarratorState state() const;
    DefId narratorDef() const;
    DefId difficultyDef() const;
    // Game second of the next big threat the cycle has planned, -1 if none.
    int64_t nextThreatAt() const { return state_.threats.empty() ? -1 : state_.threats.front(); }

    // After loading a save. False if it names a narrator, difficulty or incident the defs don't have.
    bool restore(NarratorState state, std::string& error);
    void hashState(StateHasher& h) const;

private:
    struct Incident {
        DefId def;
        std::string name;
        std::string label;
        IncidentCategory category = IncidentCategory::Misc;
        LetterKind letter = LetterKind::NeutralEvent;
        float baseChance = 1.0f;
        float earliestDay = 0.0f;
        float cooldownDays = 0.0f;
        int minColonists = 0;
        bool hasPoints = false;
        float pointsFactor = 1.0f;
        float pointsMax = 0.0f;  // 0: no cap of its own
        bool populationScaled = false;
    };
    struct Narrator {
        DefId def;
        std::string name;
        bool cycle = false;
        float onDays = 0.0f;
        float offDays = 0.0f;
        int perCycleMin = 1;
        int perCycleMax = 1;
        float firstThreatDay = 0.0f;
        float mtbDays[kIncidentCategoryCount] = {0, 0, 0, 0, 0, 0};  // 0: never by chance
        float pointsFactor = 1.0f;
        bool randomPoints = false;
        float randomPointsMin = 1.0f;
        float randomPointsMax = 1.0f;
        float minChanceFactor = 0.05f;
    };

    const Narrator* narrator() const;
    float threatScale() const;
    float diseaseFrequency() const;
    const Incident* incident(const std::string& name) const;
    bool canHappen(const Incident& incident) const;
    int64_t lastFired(const std::string& name) const;
    // Picks an incident of the category by weight and makes it happen; false if none could.
    bool fireCategory(IncidentCategory category);
    bool happen(const Incident& incident, float points, bool forced, std::string& error);
    float pointsFor(const Incident& incident, float points, bool roll);
    void applyAdaptation();
    void planCycle(int64_t start);
    void check();
    bool raidWorker(IncidentFiring& firing);
    int64_t now() const;
    float day() const;

    RaidSystem& raids_;
    const DefDatabase& defs_;
    const GameTime& time_;
    NarratorRules rules_;
    std::vector<Incident> incidents_;  // in def order
    std::vector<Narrator> narrators_;  // in def order
    std::vector<std::pair<std::string, IncidentWorker>> workers_;
    float minPoints_ = 35.0f;
    float maxPoints_ = 10000.0f;
    Rng rng_;
    NarratorState state_;  // the random stream in it is only filled in by state()
    std::vector<Event>* events_ = nullptr;
};

COLONY_API const char* letterKindName(LetterKind kind);
COLONY_API const char* incidentCategoryName(IncidentCategory category);

}  // namespace colony
