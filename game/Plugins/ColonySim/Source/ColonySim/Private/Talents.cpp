#include "ColonySim/Talents.h"

#include "ColonySim/Colonist.h"
#include "ColonySim/Events.h"
#include "ColonySim/Hash.h"
#include "ColonySim/JobType.h"
#include "ColonySim/Things.h"

#include <algorithm>

namespace colony {

TalentRules TalentRules::read(const DefDatabase& defs) {
    TalentRules r;
    const Def* rules = defs.find("Rules", "TalentRules");
    if (rules == nullptr) {
        return r;
    }
    r.present = true;
    r.xpFirstPoint = std::max<int64_t>(1, rules->integer("xpFirstPoint", r.xpFirstPoint));
    r.xpGrowthPerPoint = std::max<int64_t>(0, rules->integer("xpGrowthPerPoint", r.xpGrowthPerPoint));
    r.maxPoints = std::max(0, static_cast<int>(rules->integer("maxPoints", r.maxPoints)));
    r.minFactor = rules->number("minFactor", r.minFactor);
    return r;
}

TalentSystem::TalentSystem(Things& things, const DefDatabase& defs) : things_(things), defs_(defs) {
    rules_ = TalentRules::read(defs_);
    for (const DefId id : defs_.idsOfKind("Talents")) {
        const Def& def = defs_.get(id);
        TalentDef t;
        t.def = id;
        t.defName = def.defName();
        t.stat = def.text("stat");
        t.perRank = def.number("perRank");
        t.maxRank = std::max(1, static_cast<int>(def.integer("maxRank", 1)));
        t.cost = std::max(1, static_cast<int>(def.integer("cost", 1)));
        for (const std::string& key : def.keys("requires")) {
            t.needs.emplace_back(key, static_cast<int>(def.integer(("requires." + key).c_str(), 1)));
        }
        talents_.push_back(std::move(t));
    }
}

const TalentDef* TalentSystem::find(const std::string& defName) const {
    for (const TalentDef& t : talents_) {
        if (t.defName == defName) {
            return &t;
        }
    }
    return nullptr;
}

bool TalentSystem::eligible(Entity pawn) const {
    if (!things_.exists(pawn)) {
        return false;
    }
    const Colonist* colonist = things_.registry().try_get<Colonist>(pawn);
    return colonist != nullptr && colonist->protagonist;
}

const TalentState* TalentSystem::state(Entity pawn) const {
    const auto it = states_.find(pawn);
    return it == states_.end() ? nullptr : &it->second;
}

int64_t TalentSystem::xpForPoints(int n) const {
    if (n <= 0) {
        return 0;
    }
    // n points: first + (first + g) + ... = n x first + g x n (n - 1) / 2.
    return static_cast<int64_t>(n) * rules_.xpFirstPoint + rules_.xpGrowthPerPoint * (static_cast<int64_t>(n) * (n - 1) / 2);
}

int TalentSystem::pointsEarned(Entity pawn) const {
    const TalentState* s = state(pawn);
    if (s == nullptr) {
        return 0;
    }
    int n = 0;
    while (n < rules_.maxPoints && xpForPoints(n + 1) <= s->xp) {
        ++n;
    }
    return n;
}

int TalentSystem::pointsSpent(Entity pawn) const {
    const TalentState* s = state(pawn);
    int spent = 0;
    if (s != nullptr) {
        for (const auto& [name, rank] : s->ranks) {
            if (const TalentDef* t = find(name)) {
                spent += t->cost * rank;
            }
        }
    }
    return spent;
}

int TalentSystem::rank(Entity pawn, const std::string& talent) const {
    const TalentState* s = state(pawn);
    if (s == nullptr) {
        return 0;
    }
    const auto it = s->ranks.find(talent);
    return it == s->ranks.end() ? 0 : it->second;
}

bool TalentSystem::addXp(Entity pawn, int64_t amount, std::vector<Event>& events, std::string& error) {
    if (!eligible(pawn)) {
        error = "only the protagonist has talents";
        return false;
    }
    if (amount <= 0) {
        error = "talent XP must be more than zero";
        return false;
    }
    const int before = pointsEarned(pawn);
    TalentState& s = states_[pawn];
    s.xp += amount;
    const int after = pointsEarned(pawn);
    for (int p = before; p < after; ++p) {
        Event event;
        event.type = EventType::TalentPoint;
        event.thing = pawn;
        event.def = things_.registry().get<Thing>(pawn).def;
        event.amount = static_cast<float>(pointsFree(pawn));
        events.push_back(std::move(event));
    }
    return true;
}

bool TalentSystem::spend(Entity pawn, const std::string& talent, std::string& error) {
    if (!eligible(pawn)) {
        error = "only the protagonist has talents";
        return false;
    }
    const TalentDef* t = find(talent);
    if (t == nullptr) {
        error = "no talent '" + talent + "'";
        return false;
    }
    if (rank(pawn, talent) >= t->maxRank) {
        error = t->defName + " is at its top rank";
        return false;
    }
    for (const auto& [need, rankNeeded] : t->needs) {
        if (rank(pawn, need) < rankNeeded) {
            error = t->defName + " needs " + need + " at rank " + std::to_string(rankNeeded);
            return false;
        }
    }
    if (pointsFree(pawn) < t->cost) {
        error = "not enough talent points for " + t->defName;
        return false;
    }
    states_[pawn].ranks[talent] += 1;
    return true;
}

void TalentSystem::awardJobXp(const std::vector<Event>& events, size_t from, std::vector<Event>& out) {
    if (!rules_.present) {
        return;
    }
    const Def* rules = defs_.find("Rules", "TalentRules");
    for (size_t i = from; i < events.size(); ++i) {
        const Event& e = events[i];
        if (!eligible(e.thing)) {
            continue;
        }
        int64_t xp = 0;
        if (e.type == EventType::JobEnded && e.message == "done") {
            xp = rules->integer((std::string("jobXp.") + jobTypeName(e.job)).c_str(), 0);
        } else if (e.type == EventType::ShotFired) {
            xp = rules->integer("combatXp.shot", 0);
        } else if (e.type == EventType::MeleeHit && (e.hit == ProjectileHitKind::Pawn || e.hit == ProjectileHitKind::Armor)) {
            xp = rules->integer("combatXp.meleeHit", 0);
        }
        std::string error;
        if (xp > 0) {
            addXp(e.thing, xp, out, error);
        }
    }
}

double TalentSystem::statFactor(Entity pawn, const char* stat) const {
    const TalentState* s = state(pawn);
    if (s == nullptr) {
        return 1.0;
    }
    double factor = 1.0;
    for (const auto& [name, rank] : s->ranks) {
        const TalentDef* t = find(name);
        if (t != nullptr && t->stat == stat) {
            factor += t->perRank * rank;
        }
    }
    return std::max(rules_.minFactor, factor);
}

void TalentSystem::prune() {
    for (auto it = states_.begin(); it != states_.end();) {
        it = things_.exists(it->first) ? std::next(it) : states_.erase(it);
    }
}

void TalentSystem::hashState(StateHasher& h) const {
    h.add(static_cast<uint64_t>(states_.size()));
    for (const auto& [pawn, s] : states_) {
        h.add(static_cast<uint64_t>(pawn));
        h.add(s.xp);
        h.add(static_cast<uint64_t>(s.ranks.size()));
        for (const auto& [name, rank] : s.ranks) {
            h.add(name.data(), name.size());
            h.add(rank);
        }
    }
}

}  // namespace colony
