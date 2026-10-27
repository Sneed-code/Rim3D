#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace colony {

class HealthSystem;
class MoodSystem;
class StateHasher;
class Things;
enum class ScheduleKind : uint8_t;

// Drugs, tolerance, addiction, withdrawal, overdose and drug policies (Stage 2). See docs/design/drugs.md.
//
// A drug is a Things def with a `drug` block. A pawn takes one by eating it (the Ingest job). The high, a
// hangover, a withdrawal and an overdose are Environment hediffs (Health.h), so their effects reach the
// capacities with no new code; this system moves their severities each second. Unreal does not include this
// header: it reads the hediffs through the health report and the thoughts through the mood system.

enum class DrugCategory : uint8_t { Social, Hard, Medical };

COLONY_API const char* drugCategoryName(DrugCategory category);

// One chemical (a Chemicals def): what several drugs share (alcohol; psychite for tea, flake and yayo).
struct DrugChemical {
    DefId def;
    std::string defName;
    DefId withdrawalHediff;
    DefId hangoverHediff;
    float needFallPerDay = 0.5f;
    float needPerDose = 0.9f;
    float toleranceFallPerDay = 0.0f;
    float progressPerDay = 0.0f;     // how fast an addiction heals
    float doseProgressOffset = 0.0f;  // a dose sets the healing back by this
    float withdrawalRisePerDay = 2.0f;
    float withdrawalRecoverPerDay = 2.0f;
    bool permanent = false;          // luciferium: never cured
};

// One drug item def.
struct DrugKind {
    DefId def;
    std::string defName;
    int chemical = -1;               // index in DrugSystem::chemicals()
    DrugCategory category = DrugCategory::Social;
    float addictiveness = 0.0f;
    float minToleranceToAddict = 0.0f;
    float toleranceGain = 0.0f;
    DefId high;                      // the high hediff
    float highSeverity = 0.5f;
    float joy = 0.0f;                // recreation gained
    float restOffset = 0.0f;         // rest gained (negative: lost)
    bool overdose = false;
};

// What a pawn's policy says about one drug (DrugPolicies defs and per-pawn changes). The "for addiction", "for
// recreation" and "scheduled" boxes combine with OR; the sliders AND with the schedule.
struct DrugPolicyEntry {
    std::string drug;                // Things defName
    bool forAddiction = true;
    bool forJoy = false;
    bool scheduled = false;
    float daysFrequency = 1.0f;      // scheduled: at least this long since the last dose of the drug
    float moodBelow = 1.01f;         // scheduled: only if mood (0 to 1) is under this; 1.01: always
    float joyBelow = 1.01f;          // scheduled: only if recreation is under this
};

struct DrugPolicy {
    std::string defName;
    std::string label;
    std::vector<DrugPolicyEntry> entries;
};

// A pawn's standing with one chemical. Sorted by chemical name in DrugState.
struct ChemicalState {
    std::string chemical;
    float tolerance = 0.0f;   // 0 to 1
    bool addicted = false;
    float need = 0.0f;        // 0 (empty: withdrawal) to 1
    float progress = 0.0f;    // 0 to 1; an addiction that reaches 1 is over
    bool armed = false;       // alcohol: it got drunk, so a hangover follows
};

struct DrugDose {
    std::string drug;         // Things defName
    int64_t at = 0;           // game second of the last dose
};

// On a pawn that has ever taken a drug or been given a policy. Every field is saved.
struct DrugState {
    std::string policy;                 // a DrugPolicies defName; empty: the default policy
    std::vector<DrugPolicyEntry> edits;  // this pawn's changes, over the policy
    std::vector<ChemicalState> chemicals;
    std::vector<DrugDose> last;          // sorted by drug
    int64_t searchAfter = 0;             // no drug was found; do not look again before this
};

struct COLONY_API DrugRules {
    bool present = false;
    std::string defaultPolicy = "Social";
    float toleranceDurationFactor = 0.75f;
    std::vector<std::pair<float, float>> addictionChanceByTolerance = {{0.0f, 1.0f}, {0.5f, 5.0f}, {0.8f, 15.0f}, {1.0f, 15.0f}};
    float overdoseSeverityMin = 0.18f;
    float overdoseSeverityMax = 0.35f;
    float overdoseFallPerDay = 1.0f;
    float majorOverdoseChance = 0.005f;
    float majorOverdoseSeverity = 0.75f;
    float newAddictionProgress = 0.5f;
    float seekNeedBelow = 0.3f;
    float joyBelow = 0.5f;
    float redoseBelow = 0.25f;
    float overdoseHoldOff = 0.4f;
    int64_t searchSeconds = 60;
    float hangoverFromSeverity = 0.4f;
    float hangoverSeverity = 1.0f;

    static DrugRules read(const DefDatabase& defs);
    float addictionMultiplier(float tolerance) const;
};

// A drug a pawn would like to take now, and how badly: 3 an addiction, 2 the policy's schedule, 1 recreation.
struct DrugWant {
    DefId drug;
    int priority = 0;
};

class COLONY_API DrugSystem {
public:
    DrugSystem(Things& things, HealthSystem& health, const DefDatabase& defs);

    void setEventLog(std::vector<Event>* events) { events_ = events; }
    // Registers the thoughts whose `situation.condition` is "drug:<Hediffs defName>" (the high, withdrawal and
    // hangover moods): a thought stage follows the stage of that hediff.
    void registerThoughts(MoodSystem& mood);

    const DrugRules& rules() const { return rules_; }
    const std::vector<DrugChemical>& chemicals() const { return chemicals_; }
    const std::vector<DrugKind>& kinds() const { return kinds_; }
    const std::vector<DrugPolicy>& policies() const { return policies_; }
    const DrugKind* kindOf(DefId thingDef) const;
    bool isDrug(DefId thingDef) const { return kindOf(thingDef) != nullptr; }
    const DrugPolicy* policyNamed(const std::string& defName) const;
    int chemicalIndex(const std::string& defName) const;

    // The pawn's record, or null if it has none.
    const DrugState* state(Entity pawn) const;
    const std::map<Entity, DrugState>& states() const { return states_; }
    std::map<Entity, DrugState>& states() { return states_; }
    // Standing with a chemical (by defName): zeros for a pawn that has none.
    float tolerance(Entity pawn, const std::string& chemical) const;
    bool addicted(Entity pawn, const std::string& chemical) const;
    float need(Entity pawn, const std::string& chemical) const;
    float progress(Entity pawn, const std::string& chemical) const;

    // Whether a pawn can use drugs at all: a living humanlike with needs and a body, not a prisoner.
    bool eligible(Entity pawn) const;

    // ---- Policies -----------------------------------------------------------------------------------
    // Gives the pawn a policy (a DrugPolicies defName) and clears its own changes. False, with `error`, for a
    // name that is none or a pawn that is not eligible.
    bool setPolicy(Entity pawn, const std::string& policy, std::string& error);
    // Changes one drug's entry in the pawn's policy. False for a drug that is none or a pawn not eligible.
    bool setEntry(Entity pawn, const DrugPolicyEntry& entry, std::string& error);
    // What the pawn's policy says about a drug (the pawn's change, else its policy's entry, else addiction only).
    DrugPolicyEntry entryFor(Entity pawn, const std::string& drug) const;

    // ---- Taking -------------------------------------------------------------------------------------
    // The drugs the pawn would take now, best first (by priority, then drug def order). `slot` is the hour's
    // schedule. Empty during the pawn's search wait. The job system finds an item of one of them.
    std::vector<DrugWant> wants(Entity pawn, int64_t now, ScheduleKind slot);
    // Marks a failed search: do not ask again for a while.
    void searchFailed(Entity pawn, int64_t now);
    // The pawn takes one dose of the drug (the item is not touched). Applies the high, tolerance, the addiction
    // roll, the need, rest, recreation and overdose; sends the events. False if the pawn cannot, or the def is
    // no drug.
    bool takeDose(Entity pawn, DefId drug, int64_t now);

    // ---- Stepping -----------------------------------------------------------------------------------
    // One game second: highs fade, needs fall, tolerance wears off, withdrawals rise and fall, addictions heal.
    void step(int64_t now);
    // What the pawn's sleep falls by (the product of its highs' and withdrawals' `restFallFactor`); 1 for none.
    float restFallFactor(Entity pawn) const;

    void prune();
    void hashState(StateHasher& h) const;

private:
    ChemicalState& chemicalState(DrugState& state, const std::string& chemical);
    const ChemicalState* chemicalState(const DrugState& state, const std::string& chemical) const;
    DrugState& ensure(Entity pawn);
    double unit(Entity pawn, int64_t now, uint64_t salt) const;
    void emit(EventType type, Entity pawn, DefId def, float amount, const char* message);
    float severity(Entity pawn, DefId hediff) const;
    void setSeverity(Entity pawn, DefId hediff, float value);

    Things& things_;
    HealthSystem& health_;
    const DefDatabase& defs_;
    std::vector<Event>* events_ = nullptr;
    DrugRules rules_;
    std::vector<DrugChemical> chemicals_;
    std::vector<DrugKind> kinds_;
    std::vector<int32_t> kindIndex_;  // by DefId value, -1 for none
    std::vector<DrugPolicy> policies_;
    DefId overdoseHediff_;
    // Per hediff def (by DefId value): the fall per day and the sleep factor, for the hediffs the system moves.
    struct HediffInfo {
        bool known = false;
        float fallPerDay = 0.0f;
        float restFallFactor = 1.0f;
        std::vector<float> stages;  // each stage's minSeverity
    };
    std::vector<HediffInfo> hediffs_;
    std::vector<DefId> restHediffs_;  // hediffs with a restFallFactor other than 1
    std::map<Entity, DrugState> states_;
};

}  // namespace colony
