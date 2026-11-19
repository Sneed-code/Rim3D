// Team play without fixed squads (sneed, 2026-11-18; docs/design/team-play.md). Every fighter always knows where the
// friends near it are and what they are doing (shared knowledge of its own side, separate from stealth's knowledge of
// enemies), and picks its own part each plan, F.E.A.R. style: suppress when nobody near is shooting its target, flank
// when a friend is pinning it, push when the target is hurt, cooling down or backing off, fall back when friends drop.
// How bold it is comes from its side, traits, skill and a seeded roll of its own. Part of the fighter system.
#include "ColonySim/Fighters.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Cover.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Health.h"
#include "ColonySim/Map.h"
#include "ColonySim/Melee.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Random.h"
#include "ColonySim/Regions.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

constexpr float kTeamDegToRad = 0.017453292519943295f;
constexpr float kTeamRunSpeed = 4.0f;  // metres a second a pawn closes at, roughly (sneed's ~4 m/s base pace)

float teamDist(float ax, float ay, float bx, float by) {
    const float dx = ax - bx;
    const float dy = ay - by;
    return std::sqrt(dx * dx + dy * dy);
}

int teamSkill(const entt::registry& reg, Entity pawn, const char* name) {
    if (const Colonist* colonist = reg.try_get<Colonist>(pawn)) {
        if (const SkillRecord* skill = colonist->skill(name)) {
            return std::clamp(skill->level, 0, 20);
        }
    }
    return 10;  // no skills (a test pawn, an animal): neither bold nor timid for it
}

}  // namespace

const char* calloutWords(Callout c) {
    switch (c) {
        case Callout::None: return "";
        case Callout::Suppressing: return "suppressing";
        case Callout::Flanking: return "flanking";
        case Callout::Pushing: return "pushing";
        case Callout::FallingBack: return "falling back";
        case Callout::ManDown: return "man down";
        case Callout::EnemyDown: return "enemy down";
    }
    return "";
}

float FighterSystem::aggression(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    float a = factionOf(pawn) == kPlayerFaction ? rules_.aggressionColonists : rules_.aggressionRaiders;
    // Its own temper, the same all game: rolled from the world seed and the pawn.
    Rng roll(splitMix64(seed_ ^ 0x7EA3ULL ^ (uint64_t{entt::to_integral(pawn)} * 0x9E3779B97F4A7C15ULL)));
    a += roll.range(-rules_.aggressionVariety, rules_.aggressionVariety);
    // A good fighter trusts its hands; a poor one hangs back.
    const int skill = std::max(teamSkill(reg, pawn, "Shooting"), teamSkill(reg, pawn, "Melee"));
    a += rules_.aggressionPerSkill * static_cast<float>(skill - 10);
    if (const Colonist* colonist = reg.try_get<Colonist>(pawn)) {
        for (const TraitRecord& trait : colonist->traits) {
            for (const auto& [name, add] : rules_.aggressionTraits) {
                if (trait.def == name) {
                    a += add;
                }
            }
        }
    }
    return std::clamp(a, 0.0f, 1.0f);
}

// Enemy shots passing close keep a fighter's head down for a while: cover counts more in its plans (suppression).
void FighterSystem::suppressionStep() {
    if (!rules_.teamPlay) {
        return;
    }
    entt::registry& reg = things_.registry();
    const float r2 = rules_.suppressRadius * rules_.suppressRadius;
    for (const Incoming& shot : incoming_) {
        if (shot.age <= 0 || !reg.valid(shot.launcher)) {
            continue;
        }
        const FactionId side = factionOf(shot.launcher);
        for (const PawnInfo& p : pawns_) {
            if (!p.fighter || !p.able || p.faction == side || p.level != shot.level) {
                continue;
            }
            const float dx = p.x - shot.x;
            const float dy = p.y - shot.y;
            if (dx * dx + dy * dy <= r2) {
                Fighter& f = reg.get<Fighter>(p.pawn);
                f.suppressedUntil = now() + rules_.suppressSteps;
            }
        }
    }
}

bool FighterSystem::shaken(const Fighter& f, Entity pawn) const {
    if (!rules_.teamPlay) {
        return false;
    }
    if (f.alliesDown == 0 || now() - f.allyDownAt > rules_.allyDownSteps) {
        return false;
    }
    // A timid fighter backs off when one friend near it drops, a bold one only after three or four.
    const int limit = 1 + static_cast<int>(std::lround(aggression(pawn) * 3.0f));
    return static_cast<int>(f.alliesDown) >= limit;
}

bool FighterSystem::pushReason(Entity pawn, const Fighter& f, const PawnInfo& target, float range) const {
    (void)f;
    const entt::registry& reg = things_.registry();
    if (target.turret) {
        return false;
    }
    if (badlyHurt(target.pawn)) {
        return true;  // it is bleeding or limping: finish it
    }
    if (const Fighter* theirs = reg.try_get<Fighter>(target.pawn)) {
        if (theirs->fallingBack || theirs->mode == FightMode::Flee) {
            return true;  // it is backing off: keep on it
        }
    }
    // Its gun has gone quiet: the store is empty and the cooldown has a while to run (the reload of other games).
    int stored = 0;
    int size = 0;
    int refillLeft = 0;
    int refillSteps = 0;
    // Only when the cooldown lasts long enough to close half the gap before it is ready again (at a run, about 4 m/s).
    if (combat_.shotStore(target.pawn, stored, size, refillLeft, refillSteps) && size > 0 && stored == 0 &&
        refillLeft >= rules_.pushCooldownSteps) {
        const PawnInfo* self = info(pawn);
        const float gap = self != nullptr ? teamDist(self->x, self->y, target.x, target.y) : 0.0f;
        if (static_cast<float>(refillLeft) >= gap * 0.5f / kTeamRunSpeed * 30.0f) {
            return true;
        }
    }
    // A melee fighter: a friend is already at its target, so two beat one.
    if (range <= 0.0f) {
        const PawnInfo* self = info(pawn);
        if (self == nullptr) {
            return false;
        }
        for (const PawnInfo& p : pawns_) {
            if (p.pawn != pawn && p.faction == self->faction && p.able && !p.animal && !p.turret && p.level == target.level &&
                teamDist(p.x, p.y, target.x, target.y) <= rules_.helpReach) {
                return true;
            }
        }
    }
    return false;
}

void FighterSystem::chooseRole(Entity pawn, Fighter& f, const PawnInfo& self, const PawnInfo* target, float tx, float ty,
                               float range) {
    if (!rules_.teamPlay) {
        f.role = TeamRole::None;
        f.flankSide = 0;
        return;
    }
    const entt::registry& reg = things_.registry();
    const TeamRole before = f.role;
    TeamRole role = TeamRole::None;
    const float bold = aggression(pawn);
    // A roll of this plan, so two alike fighters in the same spot don't always do the same thing.
    Rng rng(splitMix64(seed_ ^ 0x501EULL ^ (static_cast<uint64_t>(now()) << 20) ^ uint64_t{entt::to_integral(pawn)}));
    const float r = rng.unit();
    const bool held = before != TeamRole::None && now() - f.roleSince < rules_.roleHoldSteps;
    if (f.fallingBack) {
        role = TeamRole::FallBack;
    } else if (target != nullptr && target->able && f.forcedTarget != target->pawn && !f.moveOrdered) {
        // (The player's own move or attack order wins: such a fighter holds where it was told, as before.)
        const bool keepsHead = now() < f.suppressedUntil;
        // Push: the bold nearly always, the timid now and then; pinned down, less.
        const float pushChance = 0.3f + 0.65f * bold - (keepsHead ? 0.3f : 0.0f);
        if (pushReason(pawn, f, *target, range) && ((held && before == TeamRole::Push) || r < pushChance)) {
            role = TeamRole::Push;
        } else if (range > 0.0f) {
            // Who near it is shooting its target, and who is going round.
            int pinners = 0;
            int flankers = 0;
            int8_t takenSide = 0;
            for (const PawnInfo& p : pawns_) {
                if (!p.fighter || !p.able || p.pawn == pawn || p.faction != self.faction) {
                    continue;
                }
                if (p.level != self.level || teamDist(p.x, p.y, self.x, self.y) > rules_.allyRadius) {
                    continue;
                }
                const Fighter& other = reg.get<Fighter>(p.pawn);
                if (other.target != target->pawn || other.mode != FightMode::Fight) {
                    continue;
                }
                if (other.role == TeamRole::Flank) {
                    ++flankers;
                    takenSide = other.flankSide;
                } else if (other.canFire && other.role != TeamRole::FallBack) {
                    ++pinners;
                }
            }
            if (pinners == 0) {
                role = TeamRole::Suppress;
            } else if ((held && before == TeamRole::Flank) ||
                       (flankers < rules_.maxFlankers && rng.unit() < 0.2f + 0.6f * bold - (keepsHead ? 0.2f : 0.0f))) {
                role = TeamRole::Flank;
                if (before != TeamRole::Flank || f.flankSide == 0) {
                    // The other way round from a friend already going; otherwise either.
                    f.flankSide = takenSide != 0 ? static_cast<int8_t>(-takenSide) : (rng.unit() < 0.5f ? int8_t{1} : int8_t{-1});
                }
            } else if (held && before == TeamRole::Suppress) {
                role = TeamRole::Suppress;
            }
        }
    }
    (void)tx;
    (void)ty;
    if (role != before) {
        f.role = role;
        f.roleSince = now();
        ++roleCounts_[static_cast<int>(role)];
        switch (role) {
            case TeamRole::Suppress: callOut(pawn, f, Callout::Suppressing); break;
            case TeamRole::Flank: callOut(pawn, f, Callout::Flanking); break;
            case TeamRole::Push: callOut(pawn, f, Callout::Pushing); break;
            case TeamRole::FallBack: callOut(pawn, f, Callout::FallingBack); break;
            case TeamRole::None: break;
        }
    }
    if (role != TeamRole::Flank) {
        f.flankSide = 0;
    }
}

// A shout to the friends near it, unless it shouted lately or a friend near made the same call just now.
void FighterSystem::callOut(Entity pawn, Fighter& f, Callout c) {
    if (events_ == nullptr || things_.registry().all_of<PlayerControl>(pawn)) {
        return;
    }
    if (f.calloutAt > 0 && now() - f.calloutAt < rules_.calloutEvery) {
        return;
    }
    const entt::registry& reg = things_.registry();
    const PawnInfo* self = info(pawn);
    if (self == nullptr) {
        return;
    }
    for (const PawnInfo& p : pawns_) {
        if (!p.fighter || p.pawn == pawn || p.faction != self->faction || p.level != self->level ||
            teamDist(p.x, p.y, self->x, self->y) > rules_.calloutHearing) {
            continue;
        }
        const Fighter& other = reg.get<Fighter>(p.pawn);
        if (other.lastCall == c && other.calloutAt > 0 && now() - other.calloutAt < rules_.calloutEvery) {
            return;
        }
    }
    f.lastCall = c;
    f.calloutAt = now();
    Event e;
    e.type = EventType::PawnAction;
    e.action = PawnAction::Callout;
    e.thing = pawn;
    e.target = f.target;
    e.cell = cellOf(Position{self->x, self->y, self->level});
    e.amount = static_cast<float>(c);
    e.message = calloutWords(c);
    if (c == Callout::Flanking) {
        e.message = f.flankSide < 0 ? "flanking left" : "flanking right";
    }
    events_->push_back(e);
}

void FighterSystem::noteLosses(const std::vector<Event>& events, size_t from) {
    entt::registry& reg = things_.registry();
    if (!rules_.teamPlay || reg.view<Fighter>().empty()) {
        return;
    }
    const size_t end = events.size();  // call-outs made here are not looked at
    for (size_t i = from; i < end; ++i) {
        const EventType type = events[i].type;
        if (type != EventType::PawnDowned && type != EventType::PawnDied) {
            continue;
        }
        const Entity lost = events[i].thing;
        if (!reg.valid(lost) || !reg.all_of<Position>(lost)) {
            continue;
        }
        // A pawn that dies lying downed was counted when it went down.
        if (type == EventType::PawnDied && events[i].amount > 0.5f) {
            continue;
        }
        const Position pos = reg.get<Position>(lost);
        const FactionId side = factionOf(lost);
        // The nearest friend calls it ("man down"); the enemy that was shooting at it nearest calls "enemy down".
        Entity friendCaller = kNoEntity;
        float friendBest = 0.0f;
        Entity enemyCaller = kNoEntity;
        float enemyBest = 0.0f;
        for (const auto [e, f] : reg.view<Fighter>().each()) {
            if (e == lost || !things_.spawned(e) || health_.incapacitated(e)) {
                continue;
            }
            const Position& at = reg.get<Position>(e);
            if (at.level != pos.level) {
                continue;
            }
            const float d = teamDist(at.x, at.y, pos.x, pos.y);
            if (d > rules_.allyRadius) {
                continue;
            }
            if (factionOf(e) == side) {
                // Friends near it are shaken; ones it went down with lately count together.
                if (now() - f.allyDownAt > rules_.allyDownSteps) {
                    f.alliesDown = 0;
                }
                f.alliesDown = static_cast<uint8_t>(std::min(255, f.alliesDown + 1));
                f.allyDownAt = now();
                // And think again soon, spread over half a plan interval by id so a loss doesn't make the whole side plan at once.
                const int64_t spread = 1 + static_cast<int64_t>(entt::to_integral(e) % static_cast<uint32_t>(std::max(1, rules_.planEvery / 2)));
                f.nextPlan = std::min(f.nextPlan, now() + spread);
                if (friendCaller == kNoEntity || d < friendBest || (d == friendBest && e < friendCaller)) {
                    friendCaller = e;
                    friendBest = d;
                }
            } else if (f.target == lost && (enemyCaller == kNoEntity || d < enemyBest || (d == enemyBest && e < enemyCaller))) {
                enemyCaller = e;
                enemyBest = d;
            }
        }
        // Only fighters the fighters' step saw this step call (one that joined since has no place in it yet).
        if (friendCaller != kNoEntity && info(friendCaller) != nullptr) {
            callOut(friendCaller, reg.get<Fighter>(friendCaller), Callout::ManDown);
        }
        if (enemyCaller != kNoEntity && info(enemyCaller) != nullptr) {
            callOut(enemyCaller, reg.get<Fighter>(enemyCaller), Callout::EnemyDown);
        }
    }
}

FighterSystem::TeamTerms FighterSystem::teamTerms(Entity pawn, const Fighter& f, const PawnInfo* target, float tx,
                                                  float ty) const {
    TeamTerms t;
    t.role = f.role;
    t.tx = tx;
    t.ty = ty;
    t.side = f.flankSide;
    if (target == nullptr) {
        t.role = TeamRole::None;
        return t;
    }
    t.targetCell = cellOf(Position{tx, ty, target->level});
    if (const PawnInfo* me = info(pawn)) {
        t.selfX = me->x;
        t.selfY = me->y;
    }
    if (t.role != TeamRole::Flank) {
        return t;
    }
    // Where its friends shoot the target from: the middle of those with a shot at it.
    const entt::registry& reg = things_.registry();
    const PawnInfo* self = info(pawn);
    float sx = 0.0f;
    float sy = 0.0f;
    int n = 0;
    for (const PawnInfo& p : pawns_) {
        if (!p.fighter || !p.able || p.pawn == pawn || self == nullptr || p.faction != self->faction || p.level != self->level ||
            teamDist(p.x, p.y, self->x, self->y) > rules_.allyRadius) {
            continue;
        }
        const Fighter& other = reg.get<Fighter>(p.pawn);
        if (other.target == target->pawn && other.canFire && other.role != TeamRole::Flank) {
            sx += p.x;
            sy += p.y;
            ++n;
        }
    }
    if (n == 0) {
        t.role = TeamRole::None;  // nobody pins it any more: plain scoring until the next plan picks again
        return t;
    }
    t.hasPin = true;
    t.pinX = sx / static_cast<float>(n);
    t.pinY = sy / static_cast<float>(n);
    return t;
}

float FighterSystem::teamScore(const TeamTerms& t, const Fighter& f, CellPos c, float cx, float cy) {
    switch (t.role) {
        case TeamRole::Suppress:
            return f.hasSpot && c == f.spot ? rules_.suppressStayBonus : 0.0f;
        case TeamRole::Push: {
            const float dt = teamDist(cx, cy, t.tx, t.ty);
            float s = -rules_.pushWeight * dt;
            if (dt < rules_.tooClose) {
                // The plain score keeps a shooter tooClose off; a pusher comes nearer, to pushTooClose.
                s += rules_.rangeWeight * (rules_.tooClose - std::max(dt, rules_.pushTooClose));
            }
            return s;
        }
        case TeamRole::Flank: {
            if (!t.hasPin) {
                return 0.0f;
            }
            const float vx = cx - t.tx;
            const float vy = cy - t.ty;
            const float px = t.pinX - t.tx;
            const float py = t.pinY - t.ty;
            const float lv = std::sqrt(vx * vx + vy * vy);
            const float lp = std::sqrt(px * px + py * py);
            if (lv < 0.01f || lp < 0.01f) {
                return 0.0f;
            }
            const float cosA = std::clamp((vx * px + vy * py) / (lv * lp), -1.0f, 1.0f);
            // 0 on the pinning friends' side, rising evenly to full at flankDegrees round or more.
            float gain = std::clamp(std::acos(cosA) / std::max(0.01f, rules_.flankDegrees * kTeamDegToRad), 0.0f, 1.0f);
            const float cross = px * vy - py * vx;
            if (t.side != 0 && (cross > 0.0f ? 1 : -1) != t.side) {
                gain *= rules_.flankOtherSide;
            }
            // And where the target's own cover doesn't face.
            const float theirCover = map_.cover().coverAgainst(t.targetCell, Stance::Crouching, cx, cy);
            // The plain score charges the walk at travelWeight a metre; a flanker means to move, so it pays only part.
            const float refund = (1.0f - rules_.flankTravel) * rules_.travelWeight * teamDist(cx, cy, t.selfX, t.selfY);
            return rules_.flankWeight * gain - rules_.flankExposeWeight * theirCover + refund;
        }
        case TeamRole::None:
        case TeamRole::FallBack:
            break;
    }
    return 0.0f;
}

CellPos FighterSystem::chargeGoal(Entity pawn, const PawnInfo& self, CellPos targetCell, float tx, float ty) const {
    if (!rules_.teamPlay || teamDist(tx, ty, self.x, self.y) <= rules_.surroundMin) {
        return targetCell;
    }
    // A friend already at the target: go round to its far side, so they come at it from two sides.
    const PawnInfo* at = nullptr;
    float best = 0.0f;
    for (const PawnInfo& p : pawns_) {
        if (p.pawn == pawn || p.faction != self.faction || !p.able || p.animal || p.turret || p.level != targetCell.level) {
            continue;
        }
        const float d = teamDist(p.x, p.y, tx, ty);
        if (d <= rules_.helpReach && (at == nullptr || d < best)) {
            at = &p;
            best = d;
        }
    }
    if (at == nullptr || best < 0.05f) {
        return targetCell;
    }
    const float ux = (tx - at->x) / best;
    const float uy = (ty - at->y) / best;
    const CellPos far = cellOf(Position{tx + ux * 1.2f, ty + uy * 1.2f, targetCell.level});
    if (far == targetCell || !map_.walkable(far) || map_.obstacleDm(far) > 0 || map_.isDoor(far) ||
        !map_.regions().canReach(cellOf(Position{self.x, self.y, self.level}), far)) {
        return targetCell;
    }
    return far;
}

}  // namespace colony
