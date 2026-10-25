#include "ColonySim/Fighters.h"

#include "ColonySim/Animals.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Genes.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Melee.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Regions.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

constexpr int kFightMaxRepaths = 3;
// Weights of the nearest threats a spot is scored against: the nearest counts most.
constexpr float kFightThreatWeights[] = {1.0f, 0.7f, 0.5f, 0.35f, 0.25f, 0.2f, 0.15f, 0.1f};
constexpr int kFightMaxThreats = static_cast<int>(sizeof kFightThreatWeights / sizeof kFightThreatWeights[0]);
constexpr float kFightFallBackCap = 40.0f;  // metres from the nearest enemy beyond which falling back gains nothing

float fightDist(float ax, float ay, float bx, float by) {
    const float dx = ax - bx;
    const float dy = ay - by;
    return std::sqrt(dx * dx + dy * dy);
}

// Steps to wait before a new fighter's first plan, so a group drafted at once doesn't plan in the
// same step: spread over the plan interval by pawn id.
int64_t fightStagger(Entity pawn, int planEvery) {
    return static_cast<int64_t>(entt::to_integral(pawn) % static_cast<uint32_t>(planEvery));
}

float fightCentre(int v) {
    return static_cast<float>(v) + 0.5f;
}

bool fightAdjacent(CellPos a, CellPos b) {
    return a.level == b.level && a != b && std::abs(a.x - b.x) <= 1 && std::abs(a.y - b.y) <= 1;
}

// Better candidates first; ties to the lower cell (y, then x) so the order never depends on floats alone.
bool fightBetter(const auto& a, const auto& b) {
    if (a.score != b.score) return a.score > b.score;
    if (a.cell.y != b.cell.y) return a.cell.y < b.cell.y;
    return a.cell.x < b.cell.x;
}

void fightReadRules(const DefDatabase& defs, FighterRules& r) {
    const Def* def = defs.find("Rules", "FighterRules");
    if (def == nullptr) {
        return;
    }
    const auto num = [&](const char* key, float& out) { out = static_cast<float>(def->number(key, out)); };
    const auto whole = [&](const char* key, int& out) { out = static_cast<int>(def->integer(key, out)); };
    whole("planEvery", r.planEvery);
    num("searchRadius", r.searchRadius);
    num("draftLeash", r.draftLeash);
    num("holdRadius", r.holdRadius);
    num("sightRange", r.sightRange);
    whole("memorySeconds", r.memorySeconds);
    whole("threats", r.threats);
    whole("refine", r.refine);
    num("coverWeight", r.coverWeight);
    num("exposedWeight", r.exposedWeight);
    num("noShotWeight", r.noShotWeight);
    num("travelWeight", r.travelWeight);
    num("rangeWeight", r.rangeWeight);
    num("tooClose", r.tooClose);
    num("crowdRadius", r.crowdRadius);
    num("crowdWeight", r.crowdWeight);
    num("stayBonus", r.stayBonus);
    num("advanceWeight", r.advanceWeight);
    num("fallBackWeight", r.fallBackWeight);
    num("standPenalty", r.standPenalty);
    num("stepOutPenalty", r.stepOutPenalty);
    num("outnumberedRatio", r.outnumberedRatio);
    num("countRadius", r.countRadius);
    num("hurtPain", r.hurtPain);
    num("hurtBloodLoss", r.hurtBloodLoss);
    num("hurtMoving", r.hurtMoving);
    whole("stepBackBelow", r.stepBackBelow);
    num("reloadBelow", r.reloadBelow);
    num("dodgeRadius", r.dodgeRadius);
    num("dodgeClearance", r.dodgeClearance);
    whole("dodgeHoldSteps", r.dodgeHoldSteps);
    r.planEvery = std::max(1, r.planEvery);
    r.threats = std::clamp(r.threats, 1, kFightMaxThreats);
    r.refine = std::max(1, r.refine);
}

}  // namespace

FighterSystem::FighterSystem(Things& things, Map& map, CombatSystem& combat, HealthSystem& health, JobSystem& jobs,
                             Pathfinder& pathfinder, const DefDatabase& defs, const GameTime& time)
    : things_(things),
      map_(map),
      combat_(combat),
      health_(health),
      jobs_(jobs),
      pathfinder_(pathfinder),
      defs_(defs),
      time_(time) {
    fightReadRules(defs_, rules_);
    range_.assign(defs_.size(), 0.0f);
    for (const DefId id : defs_.idsOfKind("Things")) {
        const Def& def = defs_.get(id);
        if (def.has("verbs.0.defaultProjectile")) {
            range_[id.value] = static_cast<float>(def.number("verbs.0.range", 0.0));
        }
    }
}

FighterSystem::~FighterSystem() = default;

int64_t FighterSystem::now() const {
    return time_.seconds;
}

FactionId FighterSystem::factionOf(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Faction* f = reg.valid(pawn) ? reg.try_get<Faction>(pawn) : nullptr;
    return f != nullptr ? f->id : kPlayerFaction;
}

float FighterSystem::weaponRange(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Gear* gear = reg.try_get<Gear>(pawn);
    if (gear == nullptr || gear->weapon == kNoEntity || !reg.valid(gear->weapon)) {
        return 0.0f;
    }
    const DefId def = reg.get<Thing>(gear->weapon).def;
    return def.value < range_.size() ? range_[def.value] : 0.0f;
}

bool FighterSystem::badlyHurt(Entity pawn) const {
    const Health* h = things_.registry().try_get<Health>(pawn);
    return h != nullptr &&
           (h->pain >= rules_.hurtPain || h->bloodLoss >= rules_.hurtBloodLoss || h->movingFactor <= rules_.hurtMoving);
}

const FighterSystem::PawnInfo* FighterSystem::info(Entity pawn) const {
    const auto it = std::lower_bound(pawns_.begin(), pawns_.end(), pawn,
                                     [](const PawnInfo& p, Entity e) { return p.pawn < e; });
    return it != pawns_.end() && it->pawn == pawn ? &*it : nullptr;
}

// ---- Commands -------------------------------------------------------------------------------

bool FighterSystem::draft(Entity pawn, bool drafted, std::string& error) {
    entt::registry& reg = things_.registry();
    if (!things_.spawned(pawn) || !reg.all_of<Thing, Mover, Posture>(pawn)) {
        error = "that isn't a pawn on the map";
        return false;
    }
    if (factionOf(pawn) != kPlayerFaction) {
        error = "only the player's own pawns can be drafted";
        return false;
    }
    Fighter* existing = reg.try_get<Fighter>(pawn);
    if (drafted) {
        if (existing != nullptr && !existing->drafted && existing->defending) {
            // Called up by the alarm: the player takes it over where it stands.
            stop(pawn, *existing);
            existing->drafted = true;
            existing->defending = false;
            existing->mode = FightMode::Fight;
            existing->hasGoal = false;
            existing->moveOrdered = false;
            existing->retreat = false;
            existing->hasSpot = false;
            existing->anchor = cellOf(reg.get<Position>(pawn));
            existing->leash = rules_.draftLeash;
            existing->nextPlan = now();
            return true;
        }
        if (existing != nullptr) {
            error = "that pawn is drafted already";
            return false;
        }
        if (health_.incapacitated(pawn)) {
            error = "that pawn is downed or dead";
            return false;
        }
        jobs_.interrupt(pawn, "drafted");
        Fighter f;
        f.drafted = true;
        f.anchor = cellOf(reg.get<Position>(pawn));
        f.leash = rules_.draftLeash;
        f.nextPlan = now() + fightStagger(pawn, rules_.planEvery);
        reg.emplace<Fighter>(pawn, std::move(f));
        return true;
    }
    if (existing == nullptr || !existing->drafted) {
        error = "that pawn isn't drafted";
        return false;
    }
    stop(pawn, *existing);
    reg.remove<Fighter>(pawn);
    combat_.setStance(pawn, Stance::Standing);
    return true;
}

bool FighterSystem::orderMove(Entity pawn, CellPos cell, std::string& error) {
    entt::registry& reg = things_.registry();
    Fighter* f = things_.spawned(pawn) ? reg.try_get<Fighter>(pawn) : nullptr;
    if (f == nullptr) {
        error = "that pawn isn't drafted";
        return false;
    }
    if (!map_.walkable(cell)) {
        error = "nobody can stand there";
        return false;
    }
    if (!map_.regions().canReach(cellOf(reg.get<Position>(pawn)), cell)) {
        error = "the pawn can't get there";
        return false;
    }
    f->anchor = cell;
    f->leash = rules_.holdRadius;
    f->moveOrdered = true;
    f->nextPlan = now();
    return true;
}

bool FighterSystem::orderAttack(Entity pawn, Entity target, std::string& error) {
    entt::registry& reg = things_.registry();
    Fighter* f = things_.spawned(pawn) ? reg.try_get<Fighter>(pawn) : nullptr;
    if (f == nullptr) {
        error = "that pawn isn't drafted";
        return false;
    }
    if (target != kNoEntity) {
        const Health* h = reg.valid(target) ? reg.try_get<Health>(target) : nullptr;
        if (target == pawn || !things_.spawned(target) || !reg.all_of<Posture>(target) || (h != nullptr && h->dead)) {
            error = "the target isn't a living pawn on the map";
            return false;
        }
    }
    f->forcedTarget = target;
    f->nextPlan = now();
    return true;
}

bool FighterSystem::setFaction(Entity pawn, FactionId faction, std::string& error) {
    entt::registry& reg = things_.registry();
    if (faction == 0) {
        error = "faction 0 is no faction";
        return false;
    }
    if (!reg.valid(pawn) || !reg.all_of<Thing, Mover, Posture>(pawn)) {
        error = "that isn't a pawn";
        return false;
    }
    Fighter* f = reg.try_get<Fighter>(pawn);
    if (faction == kPlayerFaction) {
        reg.remove<Faction>(pawn);
        if (f != nullptr && !f->drafted) {
            stop(pawn, *f);
            reg.remove<Fighter>(pawn);
            combat_.setStance(pawn, Stance::Standing);
        }
        return true;
    }
    reg.emplace_or_replace<Faction>(pawn, Faction{faction});
    if (f != nullptr) {
        f->drafted = false;
        f->advances = true;
        f->leash = 0.0f;
        f->moveOrdered = false;
        return true;
    }
    jobs_.interrupt(pawn, "joined another faction");
    Fighter fighter;
    fighter.advances = true;
    fighter.anchor = things_.spawned(pawn) ? cellOf(reg.get<Position>(pawn)) : CellPos{};
    fighter.nextPlan = now() + fightStagger(pawn, rules_.planEvery);
    reg.emplace<Fighter>(pawn, std::move(fighter));
    return true;
}

Fighter& FighterSystem::enlist(Entity pawn, const char* why) {
    entt::registry& reg = things_.registry();
    if (Fighter* f = reg.try_get<Fighter>(pawn)) {
        return *f;
    }
    jobs_.interrupt(pawn, why);
    Fighter f;
    f.anchor = things_.spawned(pawn) ? cellOf(reg.get<Position>(pawn)) : CellPos{};
    f.nextPlan = now() + fightStagger(pawn, rules_.planEvery);
    return reg.emplace<Fighter>(pawn, std::move(f));
}

void FighterSystem::standDown(Entity pawn) {
    entt::registry& reg = things_.registry();
    if (Fighter* f = reg.valid(pawn) ? reg.try_get<Fighter>(pawn) : nullptr) {
        stop(pawn, *f);
        reg.remove<Fighter>(pawn);
        if (things_.spawned(pawn)) {
            combat_.setStance(pawn, Stance::Standing);
        }
    }
}

void FighterSystem::replan(Entity pawn) {
    entt::registry& reg = things_.registry();
    if (Fighter* f = reg.valid(pawn) ? reg.try_get<Fighter>(pawn) : nullptr) {
        stop(pawn, *f);
        f->hasSpot = false;
        f->nextPlan = std::min(f->nextPlan, now());
    }
}

// ---- Each step ------------------------------------------------------------------------------

void FighterSystem::step() {
    entt::registry& reg = things_.registry();
    if (reg.view<Fighter>().empty()) {
        return;
    }
    pawns_.clear();
    for (const auto [e, thing, pos, posture] : reg.view<const Thing, const Position, const Posture>().each()) {
        if (thing.category != ThingCategory::Pawn) {
            continue;
        }
        const Health* h = reg.try_get<Health>(e);
        if (h != nullptr && h->dead) {
            continue;  // the dead are nobody's concern
        }
        PawnInfo p;
        p.pawn = e;
        p.faction = factionOf(e);
        p.x = pos.x;
        p.y = pos.y;
        p.level = pos.level;
        p.able = h == nullptr || !h->downed;
        p.fighter = reg.all_of<Fighter>(e);
        p.animal = reg.all_of<Animal>(e);
        p.stance = p.able ? posture.stance : Stance::Prone;
        pawns_.push_back(p);
    }
    std::sort(pawns_.begin(), pawns_.end(), [](const PawnInfo& a, const PawnInfo& b) { return a.pawn < b.pawn; });

    incoming_.clear();
    for (const auto [shot, p] : reg.view<const Projectile>().each()) {
        incoming_.push_back(Incoming{shot, p.launcher, p.x, p.y, p.z, p.vx, p.vy, p.vz, p.level, p.age});
    }
    std::sort(incoming_.begin(), incoming_.end(), [](const Incoming& a, const Incoming& b) { return a.shot < b.shot; });

    order_.clear();
    for (const Entity pawn : reg.view<Fighter>()) {
        order_.push_back(pawn);
    }
    std::sort(order_.begin(), order_.end());
    for (const Entity pawn : order_) {
        Fighter& f = reg.get<Fighter>(pawn);
        // The player walks this one (U9), or it can't act: it stops where it is.
        if (!things_.spawned(pawn) || reg.all_of<PlayerControl>(pawn) || health_.incapacitated(pawn)) {
            if (f.walk != 0 || f.hasSpot) {
                stop(pawn, f);
                f.hasSpot = false;
            }
            continue;
        }
        if (now() < f.dodgeUntil) {
            continue;  // out of the way of a shot: the plan waits until it has passed
        }
        if (dodge(pawn, f)) {
            continue;
        }
        if (now() >= f.nextPlan) {
            f.nextPlan = now() + rules_.planEvery;
            plan(pawn, f);
        }
        act(pawn, f);
    }
}

// What the fighter sees now, and what its friends know: they share what they spot.
void FighterSystem::see(Entity pawn, Fighter& f, const PawnInfo& self) {
    const int64_t t = now();
    f.known.erase(std::remove_if(f.known.begin(), f.known.end(),
                                 [&](const KnownEnemy& k) {
                                     return info(k.pawn) == nullptr || t - k.seenAt > rules_.memorySeconds;
                                 }),
                  f.known.end());
    // Friends' lists and this one are both in pawn id order: merge, keeping the newer sighting.
    const auto merge = [&](const std::vector<KnownEnemy>& theirs) {
        merged_.clear();
        size_t a = 0;
        size_t b = 0;
        while (a < f.known.size() || b < theirs.size()) {
            if (b < theirs.size() && t - theirs[b].seenAt > rules_.memorySeconds) {
                ++b;
            } else if (b == theirs.size() || (a < f.known.size() && f.known[a].pawn < theirs[b].pawn)) {
                merged_.push_back(f.known[a++]);
            } else if (a == f.known.size() || theirs[b].pawn < f.known[a].pawn) {
                merged_.push_back(theirs[b++]);
            } else {
                merged_.push_back(theirs[b].seenAt > f.known[a].seenAt ? theirs[b] : f.known[a]);
                ++a;
                ++b;
            }
        }
        f.known.swap(merged_);
    };
    const auto learn = [&](const KnownEnemy& k) {
        const auto it = std::lower_bound(f.known.begin(), f.known.end(), k.pawn,
                                         [](const KnownEnemy& a, Entity e) { return a.pawn < e; });
        if (it != f.known.end() && it->pawn == k.pawn) {
            if (k.seenAt > it->seenAt) {
                *it = k;
            }
        } else {
            f.known.insert(it, k);
        }
    };
    // What friends know first: an enemy one of them saw this very step needs no look of its own.
    const entt::registry& reg = things_.registry();
    for (const PawnInfo& p : pawns_) {
        if (!p.fighter || p.pawn == pawn || p.faction != self.faction) {
            continue;
        }
        merge(reg.get<Fighter>(p.pawn).known);
    }
    const float range2 = rules_.sightRange * rules_.sightRange;
    const Viewpoint eye{self.x, self.y, self.level, Stance::Standing};
    size_t next = 0;  // walks f.known alongside pawns_, both in id order
    for (const PawnInfo& p : pawns_) {
        if (p.faction == self.faction || p.animal || p.level != self.level) {
            continue;
        }
        const float dx = p.x - self.x;
        const float dy = p.y - self.y;
        if (dx * dx + dy * dy > range2) {
            continue;
        }
        while (next < f.known.size() && f.known[next].pawn < p.pawn) {
            ++next;
        }
        if (next < f.known.size() && f.known[next].pawn == p.pawn && f.known[next].seenAt == t) {
            continue;
        }
        ++sightChecks_;
        if (map_.cover().lineOfSight(eye, Viewpoint{p.x, p.y, p.level, p.stance})) {
            learn(KnownEnemy{p.pawn, p.x, p.y, p.level, t});
        }
    }
}

// The closest real threat; the downed only when nothing else is left (game design doc).
Entity FighterSystem::chooseTarget(Entity pawn, const Fighter& f, const PawnInfo& self) const {
    if (f.forcedTarget != kNoEntity && info(f.forcedTarget) != nullptr && f.forcedTarget != pawn) {
        return f.forcedTarget;
    }
    Entity best = kNoEntity;
    int bestRank = 0;
    float bestDist = 0.0f;
    for (const KnownEnemy& k : f.known) {
        const PawnInfo* p = info(k.pawn);
        if (p == nullptr) {
            continue;
        }
        // Able before downed, seen just now before remembered, then the nearest.
        const int rank = (p->able ? 0 : 2) + (k.seenAt == now() ? 0 : 1);
        const float d = fightDist(k.x, k.y, self.x, self.y);
        if (best == kNoEntity || rank < bestRank || (rank == bestRank && d < bestDist)) {
            best = k.pawn;
            bestRank = rank;
            bestDist = d;
        }
    }
    return best;
}

bool FighterSystem::seesFrom(CellPos cell, Stance stance, const PawnInfo& target) {
    ++sightChecks_;
    return map_.cover().lineOfSight(Viewpoint::at(cell, stance), Viewpoint{target.x, target.y, target.level, target.stance});
}

// How the fighter can shoot `target` (at its last known place tx, ty) from `cell`: crouched there,
// standing up there (over low cover), or from the cell beside it on either side (leaning out
// round a corner), crouched first. False if none of these sees it within range.
bool FighterSystem::fireOption(CellPos cell, const PawnInfo& target, float range, float tx, float ty,
                               Candidate& c) {
    if (range <= 0.0f) {
        return false;
    }
    const float dx = tx - fightCentre(cell.x);
    const float dy = ty - fightCentre(cell.y);
    if (dx == 0.0f && dy == 0.0f) {
        return false;
    }
    CellPos options[3] = {cell, cell, cell};
    int count = 1;
    const Dir8Step side = dir8Step(dir8Toward(-dy, dx));  // to the left of the line to the target
    for (const int sign : {1, -1}) {
        const CellPos n{cell.x + sign * side.dx, cell.y + sign * side.dy, cell.level};
        if (!map_.walkable(n) || map_.obstacleDm(n) > 0 || map_.isDoor(n) ||
            !straightWalkIsClear(map_, fightCentre(cell.x), fightCentre(cell.y), fightCentre(n.x), fightCentre(n.y),
                                 kPawnRadius, cell.level)) {
            continue;
        }
        if (std::find(friendSpots_.begin(), friendSpots_.end(), n) == friendSpots_.end()) {
            options[count++] = n;
        }
    }
    for (int i = 0; i < count; ++i) {
        if (fightDist(fightCentre(options[i].x), fightCentre(options[i].y), tx, ty) > range) {
            continue;
        }
        for (const Stance stance : {Stance::Crouching, Stance::Standing}) {
            // Where the target shows itself: standing up to shoot or move. A target crouched behind
            // sandbags is shot at all the same; the sandbags take the hits until it stands.
            PawnInfo seen = target;
            seen.x = tx;
            seen.y = ty;
            seen.stance = target.able ? Stance::Standing : target.stance;
            if (seesFrom(options[i], stance, seen)) {
                c.fireCell = options[i];
                c.fireStance = stance;
                c.canFire = true;
                return true;
            }
        }
    }
    return false;
}

void FighterSystem::plan(Entity pawn, Fighter& f) {
    ++plans_;
    entt::registry& reg = things_.registry();
    const PawnInfo* selfInfo = info(pawn);
    if (selfInfo == nullptr) {
        return;
    }
    const PawnInfo self = *selfInfo;
    if (f.mode != FightMode::Fight) {
        planGoal(pawn, f, self);
        return;
    }
    see(pawn, f, self);
    const Entity previous = f.target;
    f.target = chooseTarget(pawn, f, self);
    if (f.target != previous && f.target != kNoEntity) {
        f.reactUntil = now() + combat_.reactionSteps(pawn);  // a new target: it takes a moment to react
    }
    const PawnInfo* targetInfo = info(f.target);
    float tx = 0.0f;
    float ty = 0.0f;
    if (targetInfo != nullptr) {
        tx = targetInfo->x;
        ty = targetInfo->y;
        for (const KnownEnemy& k : f.known) {
            if (k.pawn == f.target) {
                tx = k.x;  // where it was last seen, not where it is
                ty = k.y;
            }
        }
    }
    const float range = weaponRange(pawn);

    // The nearest able enemies it knows of are the threats a spot is scored against.
    struct Threat {
        float x, y, d;
        Entity pawn;
    };
    Threat threats[kFightMaxThreats];
    int threatCount = 0;
    int enemiesNear = 0;
    for (const KnownEnemy& k : f.known) {
        const PawnInfo* p = info(k.pawn);
        if (p == nullptr || !p->able || k.level != self.level) {
            continue;
        }
        const Threat t{k.x, k.y, fightDist(k.x, k.y, self.x, self.y), k.pawn};
        enemiesNear += t.d <= rules_.countRadius ? 1 : 0;
        int slot = threatCount < rules_.threats ? threatCount++ : rules_.threats;
        while (slot > 0 && (threats[slot - 1].d > t.d || (threats[slot - 1].d == t.d && threats[slot - 1].pawn > t.pawn))) {
            if (slot < rules_.threats) {
                threats[slot] = threats[slot - 1];
            }
            --slot;
        }
        if (slot < rules_.threats) {
            threats[slot] = t;
        }
    }
    float threatWeight = 0.0f;
    for (int i = 0; i < threatCount; ++i) {
        threatWeight += kFightThreatWeights[i];
    }

    // Outnumbered or badly hurt: fall back (game design doc).
    int friendsNear = 0;
    for (const PawnInfo& p : pawns_) {
        if (p.faction == self.faction && p.able && p.fighter && p.level == self.level &&
            fightDist(p.x, p.y, self.x, self.y) <= rules_.countRadius) {
            ++friendsNear;
        }
    }
    f.fallingBack = f.retreat ||
                    (enemiesNear > 0 && (static_cast<float>(enemiesNear) >= rules_.outnumberedRatio * static_cast<float>(friendsNear) ||
                                         badlyHurt(pawn)));
    // With an enemy already at arm's length there is no getting away on foot: it stands and fights (F8).
    if (f.fallingBack && melee_ != nullptr && threatCount > 0 && threats[0].d <= melee_->reach(pawn) + melee_->rules().standGround) {
        f.fallingBack = false;
    }

    // Armed for melee, or a Brawler: it runs at its target instead of taking cover (F8). A drafted
    // one only as far as its leash allows, unless the player ordered the attack.
    if (melee_ != nullptr && targetInfo != nullptr && !f.fallingBack && melee_->charges(pawn)) {
        const CellPos goal = cellOf(Position{tx, ty, targetInfo->level});
        if (f.leash <= 0.0f || f.forcedTarget == f.target ||
            fightDist(fightCentre(goal.x), fightCentre(goal.y), fightCentre(f.anchor.x), fightCentre(f.anchor.y)) <=
                f.leash + melee_->reach(pawn) + 1.0f) {
            planCharge(pawn, f, goal);
            return;
        }
    }

    // Nothing to fear and nobody to shoot: stay put.
    const CellPos at = cellOf(Position{self.x, self.y, self.level});
    if (threatCount == 0 && targetInfo == nullptr) {
        // A raider heads for its squad's target (F6); everyone else stays put or keeps to its order.
        const bool heads = f.moveOrdered || f.hasGoal;
        if (!f.hasSpot || heads) {
            const CellPos goal = f.moveOrdered ? f.anchor : f.hasGoal ? f.goal : at;
            if (f.hasSpot && f.spot == goal && (f.walk != 0 || at == goal)) {
                return;  // on its way there already, or there
            }
            f.hasSpot = true;
            f.spot = goal;
            f.fireCell = goal;
            f.canFire = false;
            if (f.walk != 0) {
                stop(pawn, f);
            }
            if (goal != at) {
                walkToSpot(pawn, f);
            }
        }
        return;
    }

    // Where to look: around the pawn, or around the anchor when it is held there and far from it.
    const bool leashed = f.leash > 0.0f;
    CellPos centre = at;
    if (leashed && fightDist(fightCentre(at.x), fightCentre(at.y), fightCentre(f.anchor.x), fightCentre(f.anchor.y)) > f.leash) {
        centre = f.anchor;
    }
    const int radius = static_cast<int>(rules_.searchRadius);
    const float radius2 = rules_.searchRadius * rules_.searchRadius;
    Cover& cover = map_.cover();
    Regions& regions = map_.regions();

    // Friends' spots near enough to matter: one fighter to a cell, and some room between them.
    friendSpots_.clear();
    const int reach = radius + static_cast<int>(rules_.crowdRadius) + 2;
    for (const PawnInfo& p : pawns_) {
        if (p.fighter && p.pawn != pawn && p.faction == self.faction) {
            const Fighter& other = reg.get<Fighter>(p.pawn);
            const CellPos spot = other.hasSpot ? other.spot : cellOf(Position{p.x, p.y, p.level});
            if (std::abs(spot.x - centre.x) <= reach && std::abs(spot.y - centre.y) <= reach) {
                friendSpots_.push_back(spot);
            }
        }
    }

    candidates_.clear();
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            if (static_cast<float>(dx * dx + dy * dy) > radius2) {
                continue;
            }
            const CellPos c{centre.x + dx, centre.y + dy, centre.level};
            if (!map_.walkable(c) || map_.obstacleDm(c) > 0 || map_.isDoor(c)) {
                continue;
            }
            const float cx = fightCentre(c.x);
            const float cy = fightCentre(c.y);
            if (leashed && fightDist(cx, cy, fightCentre(f.anchor.x), fightCentre(f.anchor.y)) > f.leash) {
                continue;
            }
            bool taken = false;
            float crowd = 0.0f;
            for (const CellPos& fr : friendSpots_) {
                if (fr == c) {
                    taken = true;
                    break;
                }
                const float d = fightDist(cx, cy, fightCentre(fr.x), fightCentre(fr.y));
                if (d < rules_.crowdRadius) {
                    crowd += (rules_.crowdRadius - d) / rules_.crowdRadius;
                }
            }
            if (taken || !regions.canReach(at, c)) {
                continue;
            }
            Candidate cand;
            cand.cell = c;
            float s = 0.0f;
            if (threatCount > 0) {
                float covered = 0.0f;
                for (int i = 0; i < threatCount; ++i) {
                    if (threats[i].x != cx || threats[i].y != cy) {
                        covered += kFightThreatWeights[i] * cover.coverAgainst(c, Stance::Crouching, threats[i].x, threats[i].y);
                    }
                }
                s += rules_.coverWeight * covered / threatWeight;
            }
            s -= rules_.travelWeight * fightDist(cx, cy, self.x, self.y);
            s -= rules_.crowdWeight * crowd;
            if (targetInfo != nullptr && range > 0.0f) {  // without a gun there is no range to get into (F8)
                const float dt = fightDist(cx, cy, tx, ty);
                if (dt > range) {
                    s -= rules_.rangeWeight * (dt - range);
                }
                if (dt < rules_.tooClose) {
                    s -= rules_.rangeWeight * (rules_.tooClose - dt);
                }
                if (f.advances && !f.fallingBack && dt > range * 0.7f) {
                    s -= rules_.advanceWeight * (dt - range * 0.7f);
                }
            }
            if (f.fallingBack) {
                float nearest = kFightFallBackCap;
                for (int i = 0; i < threatCount; ++i) {
                    nearest = std::min(nearest, fightDist(cx, cy, threats[i].x, threats[i].y));
                }
                s += rules_.fallBackWeight * nearest;
            }
            if (f.hasSpot && c == f.spot) {
                s += rules_.stayBonus;
            }
            cand.score = s;
            cand.fireCell = c;
            candidates_.push_back(cand);
        }
    }
    if (candidates_.empty()) {
        return;  // nowhere better to be: keep the old plan
    }

    // The best few by that quick score get the line of sight checks: can the threats see it
    // crouched, and can it shoot the target from there.
    const size_t refine = std::min(candidates_.size(), static_cast<size_t>(rules_.refine));
    std::partial_sort(candidates_.begin(), candidates_.begin() + static_cast<std::ptrdiff_t>(refine), candidates_.end(),
                      [](const Candidate& a, const Candidate& b) { return fightBetter(a, b); });
    for (size_t i = 0; i < refine; ++i) {
        Candidate& cand = candidates_[i];
        if (threatCount > 0) {
            float exposed = 0.0f;
            const Viewpoint crouched = Viewpoint::at(cand.cell, Stance::Crouching);
            for (int t = 0; t < threatCount; ++t) {
                ++sightChecks_;
                if (cover.lineOfSight(Viewpoint{threats[t].x, threats[t].y, self.level, Stance::Standing}, crouched)) {
                    exposed += kFightThreatWeights[t];
                }
            }
            cand.score -= rules_.exposedWeight * exposed / threatWeight;
        }
        if (targetInfo != nullptr) {
            if (fireOption(cand.cell, *targetInfo, range, tx, ty, cand)) {
                cand.score -= (cand.fireStance == Stance::Standing ? rules_.standPenalty : 0.0f) +
                              (cand.fireCell != cand.cell ? rules_.stepOutPenalty : 0.0f);
            } else {
                cand.score -= f.fallingBack ? 1.0f : rules_.noShotWeight;
            }
        }
    }
    const Candidate& best = *std::min_element(candidates_.begin(), candidates_.begin() + static_cast<std::ptrdiff_t>(refine),
                                              [](const Candidate& a, const Candidate& b) { return fightBetter(a, b); });
    const bool moved = !f.hasSpot || best.cell != f.spot;
    f.hasSpot = true;
    f.spot = best.cell;
    f.fireCell = best.fireCell;
    f.fireStance = best.fireStance;
    f.canFire = best.canFire;
    if (moved) {
        if (f.walk != 0) {
            stop(pawn, f);
        }
        const CellPos here = cellOf(reg.get<Position>(pawn));
        if (here != f.spot && !(fightAdjacent(here, f.spot) && here == f.fireCell)) {
            walkToSpot(pawn, f);
        }
    }
}

// Shelter and Flee (F6): no enemies, no shots, just the nearest free cell to the goal (a shelter
// room, the way off the map).
void FighterSystem::planGoal(Entity pawn, Fighter& f, const PawnInfo& self) {
    f.known.clear();
    f.target = kNoEntity;
    f.forcedTarget = kNoEntity;
    f.canFire = false;
    f.fallingBack = false;
    const CellPos at = cellOf(Position{self.x, self.y, self.level});
    const CellPos goal = f.hasGoal ? f.goal : at;
    const auto nearGoal = [&](CellPos c) { return c.level == goal.level && std::abs(c.x - goal.x) <= 3 && std::abs(c.y - goal.y) <= 3; };
    if (f.hasSpot && nearGoal(f.spot) && (f.walk != 0 || at == f.spot)) {
        return;
    }
    entt::registry& reg = things_.registry();
    friendSpots_.clear();
    for (const PawnInfo& p : pawns_) {
        if (p.fighter && p.pawn != pawn && p.faction == self.faction) {
            const Fighter& other = reg.get<Fighter>(p.pawn);
            if (other.hasSpot && nearGoal(other.spot)) {
                friendSpots_.push_back(other.spot);
            }
        }
    }
    // Rings out from the goal: the first free cell it can reach.
    Regions& regions = map_.regions();
    bool found = false;
    CellPos best = goal;
    for (int r = 0; r <= 3 && !found; ++r) {
        for (int dy = -r; dy <= r && !found; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                if (std::max(std::abs(dx), std::abs(dy)) != r) {
                    continue;
                }
                const CellPos c{goal.x + dx, goal.y + dy, goal.level};
                if (!map_.walkable(c) || map_.obstacleDm(c) > 0 || map_.isDoor(c) ||
                    std::find(friendSpots_.begin(), friendSpots_.end(), c) != friendSpots_.end() || !regions.canReach(at, c)) {
                    continue;
                }
                best = c;
                found = true;
                break;
            }
        }
    }
    if (!found) {
        return;  // nowhere free: keep the old plan, try again next time
    }
    if (f.walk != 0) {
        stop(pawn, f);
    }
    f.hasSpot = true;
    f.spot = best;
    f.fireCell = best;
    if (best != at) {
        walkToSpot(pawn, f);
    }
}

void FighterSystem::act(Entity pawn, Fighter& f) {
    entt::registry& reg = things_.registry();
    Mover& mover = reg.get<Mover>(pawn);
    const Position& pos = reg.get<Position>(pawn);
    const CellPos here = cellOf(pos);
    const Shooter* shooter = reg.try_get<Shooter>(pawn);
    const ShotPhase phase = shooter != nullptr ? shooter->phase : ShotPhase::Ready;
    if (phase == ShotPhase::Aiming || phase == ShotPhase::Firing) {
        return;  // holds still and exposed until the burst is out, even if the plan has moved on
    }
    if (meleeAct(pawn, f)) {
        return;  // swinging, or an enemy in reach to swing at (F8)
    }

    // Walking to the spot: upright, not shooting.
    if (f.walk == 1) {
        const PathStatus status = pathfinder_.status(f.path);
        if (status == PathStatus::Queued) {
            return;
        }
        std::vector<CellPos> route;
        const bool found = pathfinder_.takePath(f.path, route);
        f.path = PathTicket{};
        if (!found) {
            f.walk = 0;
            f.hasSpot = false;
            f.nextPlan = std::min(f.nextPlan, now() + 1);
            return;
        }
        smoothPath(map_, pos.x, pos.y, route);
        mover.walkTo(pos.x, pos.y, std::move(route));
        f.walk = 2;
        combat_.setStance(pawn, Stance::Standing);
        return;
    }
    if (f.walk == 2) {
        if (mover.gaveUp) {
            mover.gaveUp = false;
            if (f.repaths >= kFightMaxRepaths) {
                f.walk = 0;
                f.hasSpot = false;
                f.nextPlan = std::min(f.nextPlan, now() + 1);
                return;
            }
            ++f.repaths;
            f.path = pathfinder_.request(here, f.spot);
            f.walk = 1;
            return;
        }
        if (!mover.arrived()) {
            combat_.setStance(pawn, Stance::Standing);
            return;
        }
        f.walk = 0;
    }
    if (!f.hasSpot || !mover.arrived()) {
        return;  // a step out or back is under way
    }
    if (here != f.spot && here != f.fireCell) {
        walkToSpot(pawn, f);  // pushed off its place
        return;
    }

    const PawnInfo* target = info(f.target);
    const bool canShoot = target != nullptr && f.canFire;
    if (canShoot && phase == ShotPhase::Ready && now() >= f.reactUntil) {
        if (here != f.fireCell) {
            stepTo(pawn, f.fireCell);  // lean out round the corner
            return;
        }
        combat_.setStance(pawn, f.fireStance);
        ++sightChecks_;
        const bool sees = map_.cover().lineOfSight(Viewpoint{pos.x, pos.y, pos.level, f.fireStance},
                                                   Viewpoint{target->x, target->y, target->level,
                                                             target->able ? Stance::Standing : target->stance});
        if (sees && fightDist(pos.x, pos.y, target->x, target->y) <= weaponRange(pawn) &&
            combat_.shoot(pawn, f.target, 0.0f, 0.0f, 0.0f, false)) {
            return;
        }
        f.nextPlan = std::min(f.nextPlan, now() + 1);  // the shot isn't there any more: think again
    }
    // Between shots: back into cover, crouched. Stays out if the next shot is close.
    if (here != f.spot) {
        if (!canShoot || combat_.shotReadyIn(pawn) > rules_.stepBackBelow) {
            stepTo(pawn, f.spot);
        }
        return;
    }
    combat_.setStance(pawn, f.known.empty() ? Stance::Standing : Stance::Crouching);
    if (phase == ShotPhase::Ready) {
        tacticalReload(pawn, f, canShoot);
    }
}

// A charge (F8): the spot is the cell the target was last seen in, walked to by the usual path. Once
// the target is in reach meleeAct takes over.
void FighterSystem::planCharge(Entity pawn, Fighter& f, CellPos goal) {
    f.canFire = false;
    f.fireStance = Stance::Standing;
    const bool moved = !f.hasSpot || f.spot != goal;
    f.hasSpot = true;
    f.spot = goal;
    f.fireCell = goal;
    if (!moved) {
        return;
    }
    if (f.walk != 0) {
        // Keeps running along the old route while the pathfinder works out the new one, so a chase
        // doesn't stop each time the target moves.
        if (f.walk == 1) {
            pathfinder_.cancel(f.path);
        }
        f.path = pathfinder_.request(cellOf(things_.registry().get<Position>(pawn)), goal);
        f.walk = 1;
        f.repaths = 0;
        return;
    }
    walkToSpot(pawn, f);
}

// Melee (F8). Winding up a swing it holds still. Without a gun, with its target or another able enemy
// in reach, it stops, stands and swings whenever it is ready. A pawn with a gun shoots instead, even
// point blank.
bool FighterSystem::meleeAct(Entity pawn, Fighter& f) {
    if (melee_ == nullptr) {
        return false;
    }
    if (melee_->winding(pawn)) {
        return true;
    }
    if (weaponRange(pawn) > 0.0f) {
        return false;
    }
    Entity victim = kNoEntity;
    if (info(f.target) != nullptr && melee_->inReach(pawn, f.target)) {
        victim = f.target;
    } else {
        const PawnInfo* self = info(pawn);
        float best = 0.0f;
        for (const PawnInfo& p : pawns_) {
            if (self == nullptr || p.faction == self->faction || !p.able || p.level != self->level ||
                !melee_->inReach(pawn, p.pawn)) {
                continue;
            }
            const float d = fightDist(p.x, p.y, self->x, self->y);
            if (victim == kNoEntity || d < best) {
                victim = p.pawn;
                best = d;
            }
        }
    }
    if (victim == kNoEntity) {
        return false;
    }
    if (victim == f.target && f.walk != 0 && melee_->charges(pawn)) {
        // A charge walks on until the target is well inside its reach, not just at the edge of it.
        const Position& a = things_.registry().get<Position>(pawn);
        const Position& b = things_.registry().get<Position>(victim);
        const float close = std::max(0.7f, melee_->reach(pawn) - melee_->rules().closeIn);
        if (fightDist(a.x, a.y, b.x, b.y) > close) {
            return false;
        }
    }
    if (f.walk != 0) {
        stop(pawn, f);
    }
    combat_.setStance(pawn, Stance::Standing);
    if (melee_->ready(pawn)) {
        melee_->attack(pawn, victim, false);
    }
    return true;
}

// In cover with nothing to shoot just now: top the magazine up rather than run dry mid-fight.
void FighterSystem::tacticalReload(Entity pawn, const Fighter& f, bool canShoot) {
    const int size = combat_.magazineSize(pawn);
    if (size <= 0 || canShoot) {
        return;
    }
    const int rounds = combat_.roundsLeft(pawn);
    const float below = f.known.empty() ? 1.0f : rules_.reloadBelow;
    if (static_cast<float>(rounds) < below * static_cast<float>(size)) {
        combat_.reload(pawn);
    }
}

// A slow projectile coming at the pawn, noticed in time (its reaction time after it was fired) and
// far enough away to get clear: a one-cell sidestep away from its line. Bullets are far too fast
// for this at any range a gun reaches; arrows, grenades and rockets will not be.
bool FighterSystem::dodge(Entity pawn, Fighter& f) {
    if (incoming_.empty()) {
        return false;
    }
    entt::registry& reg = things_.registry();
    const Shooter* shooter = reg.try_get<Shooter>(pawn);
    if (shooter != nullptr && shooter->phase == ShotPhase::Firing) {
        return false;  // mid-burst
    }
    const Position pos = reg.get<Position>(pawn);
    Mover& mover = reg.get<Mover>(pawn);
    const Health* health = reg.try_get<Health>(pawn);
    const PawnGenes* genes = reg.try_get<PawnGenes>(pawn);
    const float geneSpeed = genes != nullptr ? genes->effects.moveSpeed / static_cast<float>(kStepsPerRealSecondAt1x) : 0.0f;
    const float speed = std::max(0.0f, mover.metresPerStep + geneSpeed) * (health != nullptr ? health->movingFactor : 1.0f);
    if (speed <= 0.0f) {
        return false;
    }
    const int reaction = combat_.reactionSteps(pawn);
    const float ground = map_.heightAt(pos.x, pos.y);
    const float top = eyeHeight(reg.get<Posture>(pawn).stance) + 0.2f;
    const float g = combat_.gravity();
    const Incoming* threat = nullptr;
    float threatT = 0.0f;
    float awayX = 0.0f;
    float awayY = 0.0f;
    float threatMiss = 0.0f;
    for (const Incoming& p : incoming_) {
        if (p.launcher == pawn || p.level != pos.level || p.age < reaction) {
            continue;
        }
        const float v2 = p.vx * p.vx + p.vy * p.vy;
        if (v2 < 1e-8f) {
            continue;
        }
        // Closest approach on the ground, and the projectile's height there.
        const float t = ((pos.x - p.x) * p.vx + (pos.y - p.y) * p.vy) / v2;
        if (t <= 0.0f || (threat != nullptr && t >= threatT)) {
            continue;
        }
        const float cx = p.x + p.vx * t - pos.x;
        const float cy = p.y + p.vy * t - pos.y;
        const float miss = std::sqrt(cx * cx + cy * cy);
        const float z = p.z + p.vz * t - 0.5f * g * t * t - ground;
        if (miss > rules_.dodgeRadius || z < -0.2f || z > top) {
            continue;
        }
        if ((rules_.dodgeClearance - miss) / speed > t) {
            continue;  // too fast to get out of the way
        }
        threat = &p;
        threatT = t;
        threatMiss = miss;
        // Away from the line: from the closest point to the pawn, or to the left of the flight if dead on.
        const float len = std::sqrt(v2);
        awayX = miss > 0.05f ? -cx / miss : -p.vy / len;
        awayY = miss > 0.05f ? -cy / miss : p.vx / len;
    }
    if (threat == nullptr) {
        return false;
    }
    const CellPos here = cellOf(pos);
    for (const float sign : {1.0f, -1.0f}) {
        const Dir8Step d = dir8Step(dir8Toward(sign * awayX, sign * awayY));
        const CellPos n{here.x + d.dx, here.y + d.dy, here.level};
        if (!map_.walkable(n) || map_.obstacleDm(n) > 0 || map_.isDoor(n) ||
            !straightWalkIsClear(map_, pos.x, pos.y, fightCentre(n.x), fightCentre(n.y), kPawnRadius, here.level)) {
            continue;
        }
        // Far enough from the line once there, and there in time.
        const float mx = fightCentre(n.x) - pos.x;
        const float my = fightCentre(n.y) - pos.y;
        const float gain = mx * sign * awayX + my * sign * awayY;
        const float needed = rules_.dodgeClearance - (sign > 0.0f ? threatMiss : -threatMiss);
        if (gain < needed || gain <= 0.0f) {
            continue;
        }
        const float steps = std::sqrt(mx * mx + my * my) * (needed / gain) / speed;
        if (steps > threatT) {
            continue;
        }
        stop(pawn, f);
        combat_.cancelAim(pawn);
        stepTo(pawn, n);
        f.dodgeUntil = now() + static_cast<int64_t>(std::ceil(threatT)) + rules_.dodgeHoldSteps;
        f.nextPlan = std::min(f.nextPlan, f.dodgeUntil);
        if (events_ != nullptr) {
            Event e;
            e.type = EventType::PawnAction;
            e.action = PawnAction::Dodge;
            e.thing = pawn;
            e.def = reg.get<Thing>(pawn).def;
            e.target = threat->shot;
            e.cell = n;
            events_->push_back(std::move(e));
        }
        return true;
    }
    return false;
}

void FighterSystem::walkToSpot(Entity pawn, Fighter& f) {
    const CellPos here = cellOf(things_.registry().get<Position>(pawn));
    if (here == f.spot) {
        f.walk = 0;
        return;
    }
    f.path = pathfinder_.request(here, f.spot);
    f.walk = 1;
    f.repaths = 0;
}

// A one-cell step (out to fire, or back into cover), checked clear when it was planned.
void FighterSystem::stepTo(Entity pawn, CellPos cell) {
    entt::registry& reg = things_.registry();
    const Position& pos = reg.get<Position>(pawn);
    reg.get<Mover>(pawn).walkTo(pos.x, pos.y, std::vector<CellPos>{cell});
}

void FighterSystem::stop(Entity pawn, Fighter& f) {
    if (f.walk == 1) {
        pathfinder_.cancel(f.path);
    }
    f.path = PathTicket{};
    f.walk = 0;
    if (Mover* mover = things_.registry().try_get<Mover>(pawn)) {
        mover->path.clear();
        mover->next = 0;
        mover->gaveUp = false;
    }
}

void FighterSystem::forget(Entity thing) {
    entt::registry& reg = things_.registry();
    if (Fighter* f = reg.valid(thing) ? reg.try_get<Fighter>(thing) : nullptr) {
        if (f->walk == 1) {
            pathfinder_.cancel(f->path);
            f->path = PathTicket{};
            f->walk = 0;
        }
    }
}

bool FighterSystem::afterLoad(std::string& error) {
    const entt::registry& reg = things_.registry();
    for (const auto [pawn, f] : reg.view<const Fighter>().each()) {
        if (f.walk > 2 || !reg.all_of<Mover, Posture>(pawn)) {
            error = "the save has a fighter that can't fight";
            return false;
        }
        if (f.walk == 1 && pathfinder_.status(f.path) == PathStatus::Unknown) {
            error = "the save has a fighter waiting for a path nobody is looking for";
            return false;
        }
    }
    return true;
}

void FighterSystem::hashThing(Entity e, StateHasher& h) const {
    const entt::registry& reg = things_.registry();
    const auto cell = [&h](CellPos c) {
        h.add(c.x);
        h.add(c.y);
        h.add(c.level);
    };
    if (const Faction* faction = reg.try_get<Faction>(e)) {
        h.add(uint64_t{0xFAC7});
        h.add(faction->id);
    }
    if (const Fighter* f = reg.try_get<Fighter>(e)) {
        h.add(uint64_t{0xF16E});
        h.add(f->drafted);
        h.add(f->advances);
        cell(f->anchor);
        h.add(f->leash);
        h.add(f->moveOrdered);
        h.add(entt::to_integral(f->forcedTarget));
        h.add(entt::to_integral(f->target));
        h.add(f->hasSpot);
        cell(f->spot);
        cell(f->fireCell);
        h.add(static_cast<int>(f->fireStance));
        h.add(f->canFire);
        h.add(f->fallingBack);
        h.add(f->nextPlan);
        h.add(static_cast<int>(f->walk));
        h.add(static_cast<int>(f->repaths));
        h.add(f->path.slot);
        h.add(f->path.serial);
        h.add(static_cast<uint64_t>(f->known.size()));
        for (const KnownEnemy& k : f->known) {
            h.add(entt::to_integral(k.pawn));
            h.add(k.x);
            h.add(k.y);
            h.add(k.level);
            h.add(k.seenAt);
        }
        h.add(f->reactUntil);
        h.add(f->dodgeUntil);
        h.add(static_cast<int>(f->mode));
        h.add(f->defending);
        h.add(f->squad);
        h.add(f->hasGoal);
        cell(f->goal);
        h.add(f->retreat);
    }
}

}  // namespace colony
