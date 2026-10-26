#include "ColonySim/Animals.h"

#include "ColonySim/Combat.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Health.h"
#include "ColonySim/Husbandry.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Melee.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>
#include <utility>

// Animals in fights (task N5, docs/design/animal-fights.md): running from a fight, hunting, fighting
// back, manhunters. Bites and claws are F8's melee; the colony shoots back through the fighter AI,
// which takes an animal that is not calm for an enemy.

namespace colony {

namespace {

constexpr int kFightFleeCheck = 6;     // steps between looks at a fleeing animal
constexpr int kFightFarCheck = 3;      // and at a chasing one that is still far from its enemy
constexpr float kFightFarMetres = 8.0f;
constexpr float kFightEatReach = 1.2f; // a predator eats a corpse from this close
constexpr float kFightRetarget = 0.5f; // a chaser sets off again when its enemy is this far from the goal
constexpr int kFightGiveUpWait = 20;   // a chaser that cannot get to its enemy stops for this long

float fightDist2(float ax, float ay, float bx, float by) { return (ax - bx) * (ax - bx) + (ay - by) * (ay - by); }

// The stream of one decision, as animalRngFor in Animals.cpp but salted so the two never repeat each other.
Rng fightRngFor(uint64_t seed, Entity e, int64_t now) {
    return Rng(splitMix64(seed ^ 0x6E35466967687473ULL ^ (static_cast<uint64_t>(entt::to_integral(e)) * 0x9E3779B97F4A7C15ULL) ^
                          (static_cast<uint64_t>(now) << 19)),
               11);
}

bool fightDead(const entt::registry& reg, Entity e) {
    const Health* h = reg.try_get<Health>(e);
    return h != nullptr && h->dead;
}

bool fightPlayerSide(const entt::registry& reg, Entity e) {
    const Faction* f = reg.try_get<Faction>(e);
    return f == nullptr || f->id == kPlayerFaction;
}

void fightStop(Mover& mover) {
    mover.path.clear();
    mover.next = 0;
    mover.vx = 0.0f;
    mover.vy = 0.0f;
}

}  // namespace

// ---- Questions and commands ------------------------------------------------------------------

bool AnimalSystem::hostile(Entity animal) const {
    const entt::registry& reg = things_.registry();
    return reg.valid(animal) && reg.all_of<Animal>(animal) && reg.get<Animal>(animal).temper != AnimalTemper::Calm;
}

bool AnimalSystem::enrage(Entity animal, AnimalTemper temper, Entity target) {
    entt::registry& reg = things_.registry();
    const Kind* kind = kindOf(animal);
    if (kind == nullptr || temper == AnimalTemper::Calm || !reg.all_of<Mover, Position>(animal) || fightDead(reg, animal) ||
        !things_.spawned(animal)) {
        return false;
    }
    Animal& a = reg.get<Animal>(animal);
    const int64_t now = time_.seconds;
    if (temper == AnimalTemper::Manhunter) {
        a.temperUntil = now + rules_.manhunterSteps;
        ++saved_.stats.manhunters;
    } else if (temper == AnimalTemper::Revenge) {
        a.temperUntil = now + rules_.revengeSteps;
        ++saved_.stats.revenges;
    }
    if (target == kNoEntity || !reg.valid(target) || !reg.all_of<Position>(target)) {
        a.temper = temper;
        a.enemy = kNoEntity;
        a.mode = AnimalMode::Idle;
        a.nextThink = now;
        return true;
    }
    beginChase(animal, a, *kind, target, temper, std::max(a.temperUntil, now + rules_.huntChaseSteps));
    return true;
}

void AnimalSystem::beginChase(Entity e, Animal& a, const Kind& kind, Entity enemy, AnimalTemper temper, int64_t chaseUntil) {
    entt::registry& reg = things_.registry();
    (void)kind;
    // A manhunter that picks a new target stays a manhunter; a prey chase keeps the animal calm.
    if (temper != AnimalTemper::Calm || a.temper == AnimalTemper::Calm) {
        a.temper = temper;
    }
    a.enemy = enemy;
    a.mode = AnimalMode::Chase;
    a.until = chaseUntil;
    a.nextThink = time_.seconds;
    a.feeding = false;
    if (Mover* mover = reg.try_get<Mover>(e)) {
        fightStop(*mover);
    }
}

// ---- Hurt, scared, running ---------------------------------------------------------------------

void AnimalSystem::hurt(Entity victim, Entity attacker) {
    entt::registry& reg = things_.registry();
    if (!reg.valid(victim) || !reg.valid(attacker) || victim == attacker || !reg.all_of<Animal, Position>(victim) ||
        !reg.all_of<Position>(attacker) || fightDead(reg, victim)) {
        return;
    }
    const Kind* kindPtr = kindOf(victim);
    Animal& a = reg.get<Animal>(victim);
    if (kindPtr == nullptr || !a.started) {
        return;
    }
    const Kind& kind = *kindPtr;
    const int64_t now = time_.seconds;
    const Position& ap = reg.get<Position>(attacker);
    const float ax = ap.x;
    const float ay = ap.y;
    wake(victim);
    Rng rng = fightRngFor(seed_, victim, now);

    // Its herd mates run.
    if (a.herd != 0) {
        for (const auto [mate, ma, mp] : reg.view<Animal, const Position>().each()) {
            if (mate == victim || ma.herd != a.herd || ma.temper != AnimalTemper::Calm || ma.mode == AnimalMode::Flee ||
                ma.mode == AnimalMode::Chase || ma.mode == AnimalMode::Fight || fightDead(reg, mate) ||
                fightDist2(mp.x, mp.y, reg.get<Position>(victim).x, reg.get<Position>(victim).y) > rules_.herdAlert * rules_.herdAlert) {
                continue;
            }
            const Kind* mk = kindOf(mate);
            if (mk != nullptr && ma.started && !mk->predator) {
                startFlee(mate, ma, *mk, ax, ay, rng);
            }
        }
    }

    const bool byAnimal = reg.all_of<Animal>(attacker);
    if (byAnimal) {
        // Prey runs from what bites it; a predator bitten by another does not run.
        if (!kind.predator && a.temper == AnimalTemper::Calm && a.mode != AnimalMode::Flee && a.mode != AnimalMode::Seized) {
            startFlee(victim, a, kind, ax, ay, rng, rules_.preyFleeSpeed);
        }
        return;
    }
    if (!a.wild) {
        return;  // a tame animal never turns on its people
    }
    if (a.temper == AnimalTemper::Manhunter) {
        if (a.enemy == kNoEntity || !reg.valid(a.enemy) || fightDead(reg, a.enemy)) {
            beginChase(victim, a, kind, attacker, AnimalTemper::Manhunter, a.temperUntil);
        }
        return;
    }
    const float chance = kind.tusker ? rules_.manhunterChanceFlagged
                                     : (kind.predator ? rules_.manhunterChancePredator : rules_.manhunterChanceOther);
    if (rng.unit() < chance) {
        enrage(victim, AnimalTemper::Manhunter, attacker);
    } else if (kind.predator || kind.tusker) {
        // Fights back at the one who hurt it, for a while.
        if (a.temper == AnimalTemper::Revenge) {
            a.temperUntil = now + rules_.revengeSteps;
        } else {
            a.temperUntil = now + rules_.revengeSteps;
            ++saved_.stats.revenges;
        }
        beginChase(victim, a, kind, attacker, AnimalTemper::Revenge, a.temperUntil);
    } else if (a.mode != AnimalMode::Flee) {
        startFlee(victim, a, kind, ax, ay, rng);
    }
}

void AnimalSystem::scare(const std::vector<Event>& events, size_t from) {
    struct Point {
        float x, y;
    };
    std::vector<Point> points;
    for (size_t i = from; i < events.size(); ++i) {
        const Event& ev = events[i];
        if (ev.type == EventType::ShotFired) {
            points.push_back(Point{ev.x, ev.y});
        } else if (ev.type == EventType::MeleeHit && (ev.hit == ProjectileHitKind::Pawn || ev.hit == ProjectileHitKind::Armor)) {
            points.push_back(Point{ev.x, ev.y});
        } else if (ev.type == EventType::Explosion) {
            points.push_back(Point{ev.x, ev.y});
        }
    }
    if (points.empty()) {
        return;
    }
    entt::registry& reg = things_.registry();
    const int64_t now = time_.seconds;
    const float r2 = rules_.scareRadius * rules_.scareRadius;
    std::vector<Entity> scared;
    for (const auto [e, a, pos] : reg.view<Animal, const Position>().each()) {
        if (!a.started || a.temper != AnimalTemper::Calm || a.mode == AnimalMode::Flee || a.mode == AnimalMode::Chase ||
            a.mode == AnimalMode::Fight || a.mode == AnimalMode::Gorge || a.mode == AnimalMode::Feast || a.mode == AnimalMode::Leave ||
            a.mode == AnimalMode::Seized) {
            continue;
        }
        bool near = false;
        for (const Point& p : points) {
            near = near || fightDist2(pos.x, pos.y, p.x, p.y) <= r2;
        }
        if (near) {
            scared.push_back(e);
        }
    }
    std::sort(scared.begin(), scared.end());
    for (const Entity e : scared) {
        const Kind* kind = kindOf(e);
        Animal& a = reg.get<Animal>(e);
        if (kind == nullptr || kind->predator || fightDead(reg, e) || !things_.spawned(e)) {
            continue;
        }
        if (husbandry_ != nullptr) {
            const AnimalCare* care = reg.try_get<AnimalCare>(e);
            if (care != nullptr && care->heldUntil > now) {
                continue;  // a handler has it
            }
        }
        if (const Worker* worker = reg.try_get<Worker>(e); worker != nullptr && worker->job.type != JobType::None && worker->job.type != JobType::Wait) {
            continue;  // busy hauling
        }
        const Position& pos = reg.get<Position>(e);
        const Point* nearest = &points.front();
        for (const Point& p : points) {
            if (fightDist2(pos.x, pos.y, p.x, p.y) < fightDist2(pos.x, pos.y, nearest->x, nearest->y)) {
                nearest = &p;
            }
        }
        Rng rng = fightRngFor(seed_, e, now);
        startFlee(e, a, *kind, nearest->x, nearest->y, rng);
    }
}

bool AnimalSystem::walkAt(Entity e, Animal& a, const Kind& kind, float x, float y, float speedShare, AnimalMode mode) {
    entt::registry& reg = things_.registry();
    const Position& pos = reg.get<Position>(e);
    const CellPos cell{static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)), pos.level};
    if (!map_.inBounds(cell) || !map_.walkable(cell)) {
        return false;
    }
    if (!straightWalkIsClear(map_, pos.x, pos.y, static_cast<float>(cell.x) + 0.5f, static_cast<float>(cell.y) + 0.5f, kPawnRadius, pos.level) ||
        doorOnWay(pos.x, pos.y, static_cast<float>(cell.x) + 0.5f, static_cast<float>(cell.y) + 0.5f, pos.level)) {
        return false;
    }
    Mover& mover = reg.get<Mover>(e);
    mover.gaveUp = false;
    mover.metresPerStep = kind.topSpeed * speedShare;
    mover.walkTo(pos.x, pos.y, std::vector<CellPos>{cell});
    a.goal = cell;
    a.mode = mode;
    return true;
}

// Runs from (fromX, fromY): the straight line away if it is clear, else the nearest turn from it that is.
void AnimalSystem::startFlee(Entity e, Animal& a, const Kind& kind, float fromX, float fromY, Rng& rng, float speedShare) {
    entt::registry& reg = things_.registry();
    const Position pos = reg.get<Position>(e);
    float dx = pos.x - fromX;
    float dy = pos.y - fromY;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.01f) {
        const float angle = rng.range(0.0f, 6.2831853f);
        dx = std::cos(angle);
        dy = std::sin(angle);
    } else {
        dx /= len;
        dy /= len;
    }
    constexpr float kTurns[] = {0.0f, 0.785f, -0.785f, 1.571f, -1.571f, 2.356f, -2.356f, 3.1416f};
    for (const float share : {1.0f, 0.5f}) {
        for (const float turn : kTurns) {
            const float c = std::cos(turn);
            const float s = std::sin(turn);
            const float tx = pos.x + (dx * c - dy * s) * rules_.fleeDistance * share;
            const float ty = pos.y + (dx * s + dy * c) * rules_.fleeDistance * share;
            if (walkAt(e, a, kind, tx, ty, speedShare, AnimalMode::Flee)) {
                a.until = time_.seconds + rules_.fleeSteps;
                a.nextThink = time_.seconds + kFightFleeCheck;
                a.feeding = false;
                ++saved_.stats.scares;
                return;
            }
        }
    }
}

// ---- Each look at an animal in a fight ------------------------------------------------------------

Entity AnimalSystem::nearestColonist(Entity e, float range) const {
    const entt::registry& reg = things_.registry();
    const Position& pos = reg.get<Position>(e);
    Entity best = kNoEntity;
    bool bestAble = false;
    float bestD = 0.0f;
    for (const auto [other, thing, op] : reg.view<const Thing, const Position>().each()) {
        if (thing.category != ThingCategory::Pawn || other == e || op.level != pos.level || reg.all_of<Animal>(other) ||
            !fightPlayerSide(reg, other) || !things_.spawned(other)) {
            continue;
        }
        const Health* h = reg.try_get<Health>(other);
        if (h != nullptr && h->dead) {
            continue;
        }
        const bool able = h == nullptr || !h->downed;
        const float d = fightDist2(pos.x, pos.y, op.x, op.y);
        if (d > range * range) {
            continue;
        }
        // The able before the downed, then the nearest, then the lower id.
        const bool better = best == kNoEntity || (able && !bestAble) ||
                            (able == bestAble && (d < bestD || (d == bestD && other < best)));
        if (better) {
            best = other;
            bestAble = able;
            bestD = d;
        }
    }
    return best;
}

bool AnimalSystem::thinkFight(Entity e, Animal& a, const Kind& kind, int64_t now) {
    entt::registry& reg = things_.registry();
    Mover& mover = reg.get<Mover>(e);
    // A temper that has run its time.
    if ((a.temper == AnimalTemper::Revenge || a.temper == AnimalTemper::Manhunter) && now >= a.temperUntil) {
        a.temper = AnimalTemper::Calm;
        if (a.mode == AnimalMode::Chase || a.mode == AnimalMode::Fight) {
            fightStop(mover);
            a.mode = AnimalMode::Idle;
        }
        a.enemy = kNoEntity;
    }
    switch (a.mode) {
        case AnimalMode::Flee:
            if (now < a.until && !mover.arrived() && !mover.gaveUp) {
                a.nextThink = now + kFightFleeCheck;
                return true;
            }
            mover.gaveUp = false;
            fightStop(mover);
            a.mode = AnimalMode::Idle;
            return false;
        case AnimalMode::Chase:
        case AnimalMode::Fight: return chaseStep(e, a, kind, now);
        case AnimalMode::Feast:
        case AnimalMode::Gorge: return feastStep(e, a, kind, now);
        case AnimalMode::Seized:
            if (now < a.until) {
                fightStop(mover);
                a.nextThink = a.until;
                return true;
            }
            a.mode = AnimalMode::Idle;
            return false;
        default: break;
    }
    if (a.temper == AnimalTemper::Manhunter) {
        // Idle, but after the colony: takes the nearest colonist it can see.
        if (a.mode == AnimalMode::Sleep) {
            a.mode = AnimalMode::Idle;
        }
        const Entity target = nearestColonist(e, rules_.manhunterSight);
        if (target != kNoEntity) {
            beginChase(e, a, kind, target, AnimalTemper::Manhunter, a.temperUntil);
            return chaseStep(e, a, kind, now);
        }
    }
    return false;
}

bool AnimalSystem::chaseStep(Entity e, Animal& a, const Kind& kind, int64_t now) {
    entt::registry& reg = things_.registry();
    Mover& mover = reg.get<Mover>(e);
    if (melee_ != nullptr && melee_->winding(e)) {
        fightStop(mover);  // mid-swing: it holds still
        a.nextThink = now + 1;
        return true;
    }
    const Entity enemy = a.enemy;
    const bool valid = enemy != kNoEntity && reg.valid(enemy) && reg.all_of<Position, Posture>(enemy) && things_.spawned(enemy);
    const bool dead = valid && fightDead(reg, enemy);
    const bool enemyAnimal = valid && reg.all_of<Animal>(enemy);

    // A kill: a predator goes to eat it.
    if (valid && dead && enemyAnimal && kind.predator && a.temper != AnimalTemper::Manhunter) {
        ++saved_.stats.kills;
        fightStop(mover);
        a.mode = AnimalMode::Feast;
        a.temper = AnimalTemper::Calm;
        a.nextThink = now;
        return true;
    }
    bool lost = !valid || dead || reg.get<Position>(enemy).level != reg.get<Position>(e).level || now >= a.until;
    if (!lost) {
        const Position& p = reg.get<Position>(e);
        const Position& q = reg.get<Position>(enemy);
        lost = fightDist2(p.x, p.y, q.x, q.y) > rules_.chaseGiveUp * rules_.chaseGiveUp;
        const Health* h = reg.try_get<Health>(enemy);
        // Revenge is over once the one it fought can't fight on.
        lost = lost || (a.temper == AnimalTemper::Revenge && h != nullptr && h->downed);
    }
    const auto finish = [&](bool cannotReach) {
        fightStop(mover);
        a.mode = AnimalMode::Idle;
        a.enemy = kNoEntity;
        a.nextThink = now + (cannotReach ? kFightGiveUpWait : 2);
        if (kind.predator && a.temper != AnimalTemper::Manhunter) {
            a.huntAfter = now + rules_.huntCooldown;
        }
        if (a.temper == AnimalTemper::Hunting || a.temper == AnimalTemper::Revenge) {
            a.temper = AnimalTemper::Calm;
        }
    };
    if (lost) {
        finish(false);
        return true;
    }
    const Position& pos = reg.get<Position>(e);
    const Position& target = reg.get<Position>(enemy);
    // Prey keeps running while it is hunted.
    if (enemyAnimal) {
        Animal& pa = reg.get<Animal>(enemy);
        const bool caught = melee_ != nullptr && melee_->inReach(e, enemy);
        if (caught && pa.temper == AnimalTemper::Calm && pa.started && !fightDead(reg, enemy)) {
            // In the predator's jaws: it freezes (and so can be bitten), for as long as the predator keeps at it.
            if (pa.mode != AnimalMode::Seized || pa.until < now + rules_.seizedSteps / 3) {
                Mover& pm = reg.get<Mover>(enemy);
                fightStop(pm);
                pa.mode = AnimalMode::Seized;
                pa.until = now + rules_.seizedSteps;
                pa.nextThink = pa.until;
            }
        } else if (pa.temper == AnimalTemper::Calm && pa.mode != AnimalMode::Flee && pa.mode != AnimalMode::Seized && pa.started) {
            if (const Kind* pk = kindOf(enemy)) {
                Rng rng = fightRngFor(seed_, enemy, now);
                wake(enemy);
                startFlee(enemy, pa, *pk, pos.x, pos.y, rng, rules_.preyFleeSpeed);
            }
        }
    }
    if (melee_ != nullptr && melee_->inReach(e, enemy)) {
        fightStop(mover);
        a.mode = AnimalMode::Fight;
        if (melee_->ready(e)) {
            melee_->attack(e, enemy, false);
        }
        a.nextThink = now + 1;
        return true;
    }
    // Out of reach: run at it. The goal is set again when it has moved or the last walk is over.
    const float d = std::sqrt(fightDist2(pos.x, pos.y, target.x, target.y));
    const bool moved = fightDist2(static_cast<float>(a.goal.x) + 0.5f, static_cast<float>(a.goal.y) + 0.5f, target.x, target.y) >
                       kFightRetarget * kFightRetarget;
    if (a.mode != AnimalMode::Chase || mover.arrived() || mover.gaveUp || moved) {
        mover.gaveUp = false;
        if (!walkAt(e, a, kind, target.x, target.y, 1.0f, AnimalMode::Chase)) {
            finish(true);
            return true;
        }
    }
    a.nextThink = now + (d > kFightFarMetres ? kFightFarCheck : 1);
    return true;
}

// ---- Predators ---------------------------------------------------------------------------------------

bool AnimalSystem::startHunt(Entity e, Animal& a, const Kind& kind, Rng& rng) {
    (void)rng;
    entt::registry& reg = things_.registry();
    const int64_t now = time_.seconds;
    const Position& pos = reg.get<Position>(e);
    const float myCap = capacity(kind, now - a.bornAt);
    const float maxSize = myCap * rules_.preyMaxRatio;
    const float r2 = rules_.huntRadius * rules_.huntRadius;
    struct Candidate {
        Entity pawn;
        float d2;
        bool colonist;
    };
    std::vector<Candidate> found;
    for (const auto [other, thing, op] : reg.view<const Thing, const Position>().each()) {
        if (thing.category != ThingCategory::Pawn || other == e || op.level != pos.level || !things_.spawned(other) ||
            fightDead(reg, other)) {
            continue;
        }
        const float d2 = fightDist2(pos.x, pos.y, op.x, op.y);
        if (d2 > r2) {
            continue;
        }
        if (const Animal* oa = reg.try_get<Animal>(other)) {
            const Kind* ok = kindOf(other);
            if (ok == nullptr || ok->predator || !oa->started || oa->mode == AnimalMode::Leave) {
                continue;
            }
            if (capacity(*ok, now - oa->bornAt) > maxSize) {
                continue;
            }
            found.push_back(Candidate{other, d2, false});
        } else if (fightPlayerSide(reg, other)) {
            if (defs_.get(thing.def).number("race.baseBodySize", 1.0) > static_cast<double>(maxSize)) {
                continue;
            }
            // Alone: no other colonist who can fight within loneRadius.
            bool alone = true;
            for (const auto [friendPawn, ft, fp] : reg.view<const Thing, const Position>().each()) {
                if (friendPawn == other || ft.category != ThingCategory::Pawn || fp.level != op.level || reg.all_of<Animal>(friendPawn) ||
                    !fightPlayerSide(reg, friendPawn) || !things_.spawned(friendPawn)) {
                    continue;
                }
                const Health* fh = reg.try_get<Health>(friendPawn);
                if ((fh == nullptr || !fh->incapacitated()) && fightDist2(op.x, op.y, fp.x, fp.y) <= rules_.loneRadius * rules_.loneRadius) {
                    alone = false;
                    break;
                }
            }
            if (alone) {
                found.push_back(Candidate{other, d2, true});
            }
        }
    }
    std::sort(found.begin(), found.end(), [](const Candidate& x, const Candidate& y) {
        return x.d2 != y.d2 ? x.d2 < y.d2 : x.pawn < y.pawn;
    });
    for (size_t i = 0; i < found.size() && i < 3; ++i) {
        const Candidate& c = found[i];
        const Position& op = reg.get<Position>(c.pawn);
        const CellPos cell = cellOf(op);
        if (!map_.walkable(cell) ||
            !straightWalkIsClear(map_, pos.x, pos.y, static_cast<float>(cell.x) + 0.5f, static_cast<float>(cell.y) + 0.5f, kPawnRadius, pos.level)) {
            continue;
        }
        ++saved_.stats.hunts;
        beginChase(e, a, kind, c.pawn, c.colonist ? AnimalTemper::Hunting : AnimalTemper::Calm, now + rules_.huntChaseSteps);
        if (c.colonist) {
            a.temperUntil = now + rules_.huntChaseSteps;
        } else {
            // The herd of the prey runs too.
            Animal& pa = reg.get<Animal>(c.pawn);
            const uint32_t herd = pa.herd;
            if (herd != 0) {
                std::vector<Entity> mates;
                for (const auto [mate, ma, mp] : reg.view<Animal, const Position>().each()) {
                    if (ma.herd == herd && mate != c.pawn && ma.temper == AnimalTemper::Calm && ma.started &&
                        fightDist2(mp.x, mp.y, op.x, op.y) <= rules_.herdAlert * rules_.herdAlert && !fightDead(reg, mate)) {
                        mates.push_back(mate);
                    }
                }
                std::sort(mates.begin(), mates.end());
                for (const Entity mate : mates) {
                    if (const Kind* mk = kindOf(mate)) {
                        Rng mr = fightRngFor(seed_, mate, now);
                        startFlee(mate, reg.get<Animal>(mate), *mk, pos.x, pos.y, mr, rules_.preyFleeSpeed);
                    }
                }
            }
        }
        return true;
    }
    a.huntAfter = now + rules_.huntLook;
    return false;
}

float AnimalSystem::meatNutritionOf(Entity corpse) const {
    const entt::registry& reg = things_.registry();
    const Kind* kind = kindOf(corpse);
    if (kind == nullptr) {
        return 0.0f;
    }
    const Animal& a = reg.get<Animal>(corpse);
    const double meat = defs_.get(reg.get<Thing>(corpse).def).number("race.meatAmount", 0.0);
    const float growth = kind->bodySize > 0.0f ? capacity(*kind, a.diedAt >= 0 ? a.diedAt - a.bornAt : time_.seconds - a.bornAt) / kind->bodySize : 1.0f;
    return static_cast<float>(meat) * growth * rules_.meatNutrition;
}

bool AnimalSystem::feastStep(Entity e, Animal& a, const Kind& kind, int64_t now) {
    entt::registry& reg = things_.registry();
    Mover& mover = reg.get<Mover>(e);
    const Entity corpse = a.enemy;
    const bool here = corpse != kNoEntity && reg.valid(corpse) && reg.all_of<Position, Animal>(corpse) && fightDead(reg, corpse);
    if (!here) {
        fightStop(mover);
        a.mode = AnimalMode::Idle;
        a.enemy = kNoEntity;
        a.nextThink = now + 2;
        return true;
    }
    const Position& pos = reg.get<Position>(e);
    const Position& cp = reg.get<Position>(corpse);
    if (a.mode == AnimalMode::Gorge) {
        if (now < a.until) {
            a.nextThink = a.until;
            return true;
        }
        const float cap = capacity(kind, now - a.bornAt);
        a.food = std::min(cap, a.food - foodFallPerSecond(kind, now - a.bornAt) * static_cast<float>(now - a.foodAt) + meatNutritionOf(corpse));
        a.foodAt = now;
        a.feeding = false;
        ++saved_.stats.meals;
        things_.destroy(corpse);
        fightStop(mover);
        a.mode = AnimalMode::Idle;
        a.enemy = kNoEntity;
        a.nextThink = now + 2;
        return true;
    }
    if (fightDist2(pos.x, pos.y, cp.x, cp.y) <= kFightEatReach * kFightEatReach) {
        fightStop(mover);
        a.mode = AnimalMode::Gorge;
        a.until = now + rules_.gorgeSteps;
        a.nextThink = a.until;
        return true;
    }
    if (mover.arrived() || mover.gaveUp || a.goal.x != static_cast<int>(std::floor(cp.x)) || a.goal.y != static_cast<int>(std::floor(cp.y))) {
        mover.gaveUp = false;
        if (!walkAt(e, a, kind, cp.x, cp.y, 0.8f, AnimalMode::Feast)) {
            fightStop(mover);
            a.mode = AnimalMode::Idle;
            a.enemy = kNoEntity;
            a.huntAfter = now + rules_.huntCooldown;
            a.nextThink = now + kFightGiveUpWait;
            return true;
        }
    }
    a.nextThink = now + kFightFarCheck;
    return true;
}

}  // namespace colony
