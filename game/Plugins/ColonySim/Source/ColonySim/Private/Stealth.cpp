#include "ColonySim/Stealth.h"

#include "ColonySim/Animals.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Defense.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Health.h"
#include "ColonySim/Prisoners.h"
#include "ColonySim/Things.h"
#include "ColonySim/Trade.h"

#include <algorithm>
#include <cmath>

// Stealth: alert states, noise and calls. See docs/design/stealth.md. The looking itself is FighterSystem::see
// (Fighters.cpp).

namespace colony {

namespace {

constexpr float kStealthPi = 3.14159265358979f;

}  // namespace

StealthRules stealthReadRules(const DefDatabase& defs) {
    StealthRules r;
    const Def* def = defs.find("Rules", "StealthRules");
    if (def == nullptr) {
        return r;
    }
    const auto num = [&](const char* key, float& out) { out = static_cast<float>(def->number(key, out)); };
    const auto whole = [&](const char* key, int& out) { out = static_cast<int>(def->integer(key, out)); };
    num("coneDegrees", r.coneDegrees);
    num("alertedConeDegrees", r.alertedConeDegrees);
    num("closeSense", r.closeSense);
    num("instantRange", r.instantRange);
    whole("noticeSteps", r.noticeSteps);
    num("darkSight", r.darkSight);
    num("fullLight", r.fullLight);
    num("crouchSight", r.crouchSight);
    num("proneSight", r.proneSight);
    num("calloutRadius", r.calloutRadius);
    r.calledCanCall = def->flag("calledCanCall", r.calledCanCall);
    whole("searchSeconds", r.searchSeconds);
    whole("suspiciousSeconds", r.suspiciousSeconds);
    whole("surpriseSteps", r.surpriseSteps);
    num("walkNoise", r.walkNoise);
    num("runNoise", r.runNoise);
    num("runSpeed", r.runSpeed);
    num("crouchNoise", r.crouchNoise);
    num("gunNoise", r.gunNoise);
    num("meleeNoise", r.meleeNoise);
    num("hitNoise", r.hitNoise);
    num("explosionNoise", r.explosionNoise);
    r.fullLight = std::max(0.01f, r.fullLight);
    r.darkSight = std::clamp(r.darkSight, 0.0f, 1.0f);
    return r;
}

bool stealthInCone(float faceX, float faceY, float dx, float dy, float degrees) {
    if (degrees >= 360.0f) {
        return true;
    }
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.001f) {
        return true;
    }
    const float cosHalf = std::cos(degrees * 0.5f * kStealthPi / 180.0f);
    return (faceX * dx + faceY * dy) / len >= cosHalf;
}

float stealthSightShare(const StealthRules& rules, float light, Stance targetStance, bool watcherAlerted) {
    // An alerted fighter is looking for trouble: darkness and a low stance hide nothing more from it than the ground
    // does (the line of sight). So a fight, once on, goes as it did before stealth.
    if (watcherAlerted) {
        return 1.0f;
    }
    const float lit = std::clamp(light / rules.fullLight, 0.0f, 1.0f);
    const float share = rules.darkSight + (1.0f - rules.darkSight) * lit;
    return share * (targetStance == Stance::Crouching ? rules.crouchSight : targetStance == Stance::Prone ? rules.proneSight : 1.0f);
}

float stealthStepNoise(const StealthRules& rules, float speed, Stance stance) {
    if (speed <= 0.0001f || stance == Stance::Prone) {
        return 0.0f;
    }
    if (stance == Stance::Crouching) {
        return rules.crouchNoise;
    }
    return speed * static_cast<float>(kStepsPerRealSecondAt1x) >= rules.runSpeed ? rules.runNoise : rules.walkNoise;
}

// ---- FighterSystem's stealth half ----------------------------------------------------------------

float FighterSystem::noiseOf(DefId def, float fallback) const {
    return def.valid() && def.value < noise_.size() && noise_[def.value] > 0.0f ? noise_[def.value] : fallback;
}

// Sent to fight, or a site that has been raised: it doesn't calm down while that lasts.
bool FighterSystem::staysAlert(const Fighter& f) const {
    return f.onGuard || f.drafted || f.defending || f.squad != 0 || f.forcedTarget != kNoEntity;
}

bool FighterSystem::learnEnemy(Fighter& f, const KnownEnemy& k) {
    const auto it = std::lower_bound(f.known.begin(), f.known.end(), k.pawn, [](const KnownEnemy& a, Entity e) { return a.pawn < e; });
    if (it != f.known.end() && it->pawn == k.pawn) {
        if (k.seenAt > it->seenAt || (k.seenAt == it->seenAt && k.firsthand && !it->firsthand)) {
            *it = k;
            return true;
        }
        return false;
    }
    f.known.insert(it, k);
    return true;
}

void FighterSystem::alertTo(Entity pawn, Fighter& f, Alert to, const char* why, bool viaCall) {
    const Alert was = f.alert;
    f.alertAt = now();
    if (was == to) {
        return;
    }
    f.alert = to;
    if (to == Alert::Alerted) {
        f.called = viaCall;
        f.suspect = kNoEntity;
        if (was == Alert::Unaware) {
            f.reactUntil = std::max(f.reactUntil, now() + stealth_.surpriseSteps);  // caught off guard
        }
    } else if (to == Alert::Unaware) {
        f.called = false;
        f.hasSuspect = false;
        f.suspect = kNoEntity;
    }
    entt::registry& reg = things_.registry();
    if (events_ != nullptr) {
        Event e;
        e.type = EventType::PawnAction;
        e.action = PawnAction::Alert;
        e.thing = pawn;
        e.amount = static_cast<float>(static_cast<int>(to));
        e.message = why;
        if (const Position* pos = reg.try_get<Position>(pawn)) {
            e.cell = cellOf(*pos);
        }
        if (const Thing* thing = reg.try_get<Thing>(pawn)) {
            e.def = thing->def;
        }
        e.x = f.suspectX;
        e.y = f.suspectY;
        events_->push_back(std::move(e));
    }
    // A mission site that finds an intruder stays on its guard for good, all of it: they know there is a threat even
    // when the intruder hides again (sneed). Not the player's own people, and not when the word only came by a call.
    if (to == Alert::Alerted && staysAlerted_ && !viaCall) {
        const FactionId side = factionOf(pawn);
        if (side != kPlayerFaction) {
            f.onGuard = true;
            std::vector<Entity> others;
            for (const auto [other, g] : reg.view<Fighter>().each()) {
                if (other != pawn && !g.onGuard && factionOf(other) == side) {
                    others.push_back(other);
                }
            }
            std::sort(others.begin(), others.end());
            for (const Entity other : others) {
                Fighter& g = reg.get<Fighter>(other);
                g.onGuard = true;
                if (g.alert != Alert::Alerted) {
                    alertTo(other, g, Alert::Alerted, "site raised", true);
                }
            }
        }
    }
}

// Something half seen or heard at a place: an unaware fighter grows suspicious and turns to look.
void FighterSystem::noticePoint(Entity pawn, Fighter& f, float x, float y, const char* why) {
    f.hasSuspect = true;
    f.suspectX = x;
    f.suspectY = y;
    if (f.alert == Alert::Unaware) {
        alertTo(pawn, f, Alert::Suspicious, why, false);
    } else {
        f.alertAt = now();
    }
    // It turns to look, unless it is walking somewhere or busy with a target.
    entt::registry& reg = things_.registry();
    if (f.walk == 0 && f.target == kNoEntity) {
        const Position* pos = reg.try_get<Position>(pawn);
        Posture* posture = reg.try_get<Posture>(pawn);
        if (pos != nullptr && posture != nullptr) {
            const float dx = x - pos->x;
            const float dy = y - pos->y;
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len > 0.05f) {
                posture->facingX = dx / len;
                posture->facingY = dy / len;
            }
        }
    }
}

// Left alone long enough, a fighter that nobody sent to fight relaxes: alerted, then suspicious, then unaware.
void FighterSystem::calmDown(Entity pawn, Fighter& f) {
    const int64_t t = now();
    if (f.alertAt == 0 || !f.known.empty()) {
        f.alertAt = t;
        return;
    }
    if (staysAlert(f)) {
        if (f.alert != Alert::Alerted) {
            alertTo(pawn, f, Alert::Alerted, "on guard", true);
        }
        return;
    }
    if (f.alert == Alert::Alerted && t - f.alertAt > stealth_.searchSeconds) {
        alertTo(pawn, f, Alert::Suspicious, "lost them", false);
    } else if (f.alert == Alert::Suspicious && f.suspect == kNoEntity && t - f.alertAt > stealth_.suspiciousSeconds) {
        alertTo(pawn, f, Alert::Unaware, "calm", false);
    }
}

void FighterSystem::noise(float x, float y, int level, float radius, Entity maker, bool knowsMaker, const char* why) {
    entt::registry& reg = things_.registry();
    if (radius <= 0.0f) {
        return;
    }
    const bool makerValid = maker != kNoEntity && reg.valid(maker);
    // Animals fighting and hunting are the jungle's own noise: nobody takes up arms for it.
    if (makerValid && reg.all_of<Animal>(maker)) {
        return;
    }
    const FactionId makerSide = makerValid ? factionOf(maker) : kPlayerFaction;
    const Position* makerPos = makerValid ? reg.try_get<Position>(maker) : nullptr;
    hearers_.clear();
    for (const auto [pawn, f, pos] : reg.view<Fighter, const Position>().each()) {
        if (pawn == maker || pos.level != level || f.mode != FightMode::Fight) {
            continue;
        }
        const float dx = pos.x - x;
        const float dy = pos.y - y;
        if (dx * dx + dy * dy > radius * radius) {
            continue;
        }
        hearers_.push_back(pawn);
    }
    std::sort(hearers_.begin(), hearers_.end());
    const std::vector<Entity> hearers = hearers_;  // alertTo may raise a site, which doesn't use the scratch, but be safe
    for (const Entity pawn : hearers) {
        if (!things_.spawned(pawn) || reg.all_of<PlayerControl>(pawn) || health_.incapacitated(pawn)) {
            continue;
        }
        Fighter& f = reg.get<Fighter>(pawn);
        const bool enemy = makerValid && makerSide != factionOf(pawn) && !reg.all_of<Prisoner>(maker) && !reg.all_of<Visitor>(maker);
        if (makerValid && !enemy && f.alert == Alert::Alerted) {
            continue;  // a friend's shot tells an alerted fighter nothing new
        }
        f.hasSuspect = true;
        f.suspectX = x;
        f.suspectY = y;
        // A turret gives itself away only to those who take turrets on (fighters.md "Turrets").
        if (enemy && knowsMaker && makerPos != nullptr && (!reg.all_of<Turret>(maker) || shootsTurrets(pawn))) {
            learnEnemy(f, KnownEnemy{maker, makerPos->x, makerPos->y, makerPos->level, now(), true});
        }
        alertTo(pawn, f, Alert::Alerted, why, false);
    }
}

void FighterSystem::hear(const std::vector<Event>& events, size_t from) {
    const entt::registry& reg = things_.registry();
    if (reg.view<Fighter>().empty()) {
        return;
    }
    const size_t end = events.size();  // alerts raised here add events of their own: not looked at
    for (size_t i = from; i < end; ++i) {
        // Copied: `noise` appends to the same list.
        const EventType type = events[i].type;
        const Entity thing = events[i].thing;
        const Entity target = events[i].target;
        const DefId def = events[i].def;
        const float ex = events[i].x;
        const float ey = events[i].y;
        const CellPos cell = events[i].cell;
        const ProjectileHitKind hit = events[i].hit;
        if (type == EventType::ShotFired) {
            const Position* pos = reg.valid(thing) ? reg.try_get<Position>(thing) : nullptr;
            if (pos != nullptr) {
                noise(pos->x, pos->y, pos->level, noiseOf(def, stealth_.gunNoise), thing, true, "heard a shot");
            }
        } else if (type == EventType::MeleeHit) {
            const Position* pos = reg.valid(thing) ? reg.try_get<Position>(thing) : nullptr;
            if (pos != nullptr && hit != ProjectileHitKind::None) {
                noise(pos->x, pos->y, pos->level, noiseOf(def, stealth_.meleeNoise), thing, true, "heard a blow");
            }
        } else if (type == EventType::ProjectileHit) {
            const Position* pos = target != kNoEntity && reg.valid(target) ? reg.try_get<Position>(target) : nullptr;
            if (pos != nullptr && hit == ProjectileHitKind::Pawn) {
                noise(pos->x, pos->y, pos->level, stealth_.hitNoise, kNoEntity, false, "a friend was hit");
            }
        } else if (type == EventType::Explosion) {
            noise(ex, ey, cell.level, stealth_.explosionNoise, kNoEntity, false, "heard a blast");
        }
    }
}

void FighterSystem::struck(Entity victim, Entity attacker) {
    entt::registry& reg = things_.registry();
    Fighter* f = reg.valid(victim) ? reg.try_get<Fighter>(victim) : nullptr;
    if (f == nullptr || f->mode != FightMode::Fight || !things_.spawned(victim) || reg.all_of<PlayerControl>(victim)) {
        return;
    }
    const Position* from = attacker != kNoEntity && reg.valid(attacker) ? reg.try_get<Position>(attacker) : nullptr;
    if (from != nullptr && !reg.all_of<Animal>(attacker) && factionOf(attacker) != factionOf(victim)) {
        f->hasSuspect = true;
        f->suspectX = from->x;
        f->suspectY = from->y;
        learnEnemy(*f, KnownEnemy{attacker, from->x, from->y, from->level, now(), true});
    }
    alertTo(victim, *f, Alert::Alerted, "was hit", false);
}

bool FighterSystem::setAlert(Entity pawn, Alert alert, std::string& error) {
    entt::registry& reg = things_.registry();
    Fighter* f = reg.valid(pawn) ? reg.try_get<Fighter>(pawn) : nullptr;
    if (f == nullptr) {
        error = "that pawn is not a fighter";
        return false;
    }
    if (alert != Alert::Alerted) {
        f->known.clear();
        f->target = kNoEntity;
        f->onGuard = false;
    }
    if (alert == Alert::Suspicious && f->alert != Alert::Suspicious) {
        f->hasSuspect = false;
    }
    alertTo(pawn, *f, alert, "ordered", false);
    f->alertAt = now();
    return true;
}

}  // namespace colony
