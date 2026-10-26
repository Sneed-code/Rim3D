#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace colony {

class StateHasher;
class Things;
struct Event;

// The protagonist's talent tree (task P3). See docs/design/talents.md.
//
// The protagonist (a Colonist with `protagonist` set) earns talent XP from finished jobs; every so
// much XP is one talent point; a point is spent on a rank of a talent (the Talents defs). A talent
// changes one stat by `perRank` per rank, and the systems that use the stat ask `statFactor`.
// Unreal does not include this header: it reads `Snapshot::talents`.

// One talent, from the Talents defs.
struct TalentDef {
    DefId def;
    std::string defName;
    std::string stat;    // "buildSpeed", "aimSpread", "carryCapacity", "tradePrice"
    double perRank = 0.0;  // change to the stat's factor per rank, e.g. 0.10 is +10%
    int maxRank = 1;
    int cost = 1;        // points per rank
    std::vector<std::pair<std::string, int>> needs;  // talent and the rank it needs, sorted by name
};

// The numbers from TalentRules (data/defs/Rules). `present` is false without that def.
struct COLONY_API TalentRules {
    bool present = false;
    int64_t xpFirstPoint = 100;     // XP for the first point; the n-th point costs this + growth x (n - 1)
    int64_t xpGrowthPerPoint = 25;
    int maxPoints = 40;
    double minFactor = 0.1;         // a stat factor never goes below this

    static TalentRules read(const DefDatabase& defs);
};

// One pawn's talents: the saved state.
struct TalentState {
    int64_t xp = 0;
    std::map<std::string, int> ranks;  // talent defName -> rank, only ranks above 0
};

class COLONY_API TalentSystem {
public:
    TalentSystem(Things& things, const DefDatabase& defs);

    const TalentRules& rules() const { return rules_; }
    const std::vector<TalentDef>& talents() const { return talents_; }
    const TalentDef* find(const std::string& defName) const;

    // A living protagonist: the only pawn that has talents.
    bool eligible(Entity pawn) const;

    const std::map<Entity, TalentState>& states() const { return states_; }
    std::map<Entity, TalentState>& states() { return states_; }
    const TalentState* state(Entity pawn) const;

    // Total XP at which the n-th point is earned (0 for n = 0).
    int64_t xpForPoints(int n) const;
    int pointsEarned(Entity pawn) const;
    int pointsSpent(Entity pawn) const;
    int pointsFree(Entity pawn) const { return pointsEarned(pawn) - pointsSpent(pawn); }
    int rank(Entity pawn, const std::string& talent) const;

    // ---- Commands ----------------------------------------------------------------------------
    // Adds XP to the protagonist; a TalentPoint event comes for each point it earns. Fails for a pawn
    // that has no talents or an amount that is not positive.
    bool addXp(Entity pawn, int64_t amount, std::vector<Event>& events, std::string& error);
    // Spends points on one more rank. Fails for a talent that doesn't exist, is at its top rank,
    // has a requirement missing or costs more than the free points.
    bool spend(Entity pawn, const std::string& talent, std::string& error);

    // ---- Working -----------------------------------------------------------------------------
    // Gives XP for the jobs that ended "done" among events[from..]. Called once a step.
    void awardJobXp(const std::vector<Event>& events, size_t from, std::vector<Event>& out);
    // What a stat is multiplied by for this pawn: 1 plus the ranks' changes, never below the rules'
    // `minFactor`; exactly 1 for a pawn with no talents.
    double statFactor(Entity pawn, const char* stat) const;

    // Drops the states of pawns that no longer exist. Hash of the state, for Sim::stateHash.
    void prune();
    void hashState(StateHasher& h) const;

private:
    Things& things_;
    const DefDatabase& defs_;
    TalentRules rules_;
    std::vector<TalentDef> talents_;
    std::map<Entity, TalentState> states_;
};

}  // namespace colony
