// What a tame animal does (task N3, docs/design/husbandry.md): eats hay, kibble and meat as well as grazing, stays in
// its zone or near where it was tamed, walks round walls to get there, starves if nobody feeds it, and works if it
// has learned to haul. The wild animals' own behaviour is in Animals.cpp.
#include "ColonySim/Animals.h"

#include "ColonySim/Fighters.h"
#include "ColonySim/Health.h"
#include "ColonySim/Husbandry.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Pathfinder.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

constexpr int kAnimalCareWalkCheck = 5;   // seconds between looks at an animal that is walking
constexpr int kAnimalCareTries = 6;       // spots tried for one walk
constexpr float kAnimalCarePenWander = 1.5f;  // the shortest walk in a zone, metres

CellPos animalCareRingCell(CellPos centre, int r, int t) {
    const int side = 2 * r;
    const int k = t / side;
    const int i = t % side;
    switch (k) {
        case 0: return CellPos{centre.x - r + i, centre.y - r, centre.level};
        case 1: return CellPos{centre.x + r, centre.y - r + i, centre.level};
        case 2: return CellPos{centre.x + r - i, centre.y + r, centre.level};
        default: return CellPos{centre.x - r, centre.y + r - i, centre.level};
    }
}

float animalCareDist2(float ax, float ay, float bx, float by) {
    return (ax - bx) * (ax - bx) + (ay - by) * (ay - by);
}

}  // namespace

// ---- Food and work --------------------------------------------------------------------------

float AnimalSystem::foodCapacity(Entity animal) const {
    const Kind* kind = kindOf(animal);
    return kind != nullptr ? capacity(*kind, time_.seconds - things_.registry().get<Animal>(animal).bornAt) : 0.0f;
}

float AnimalSystem::foodLevel(Entity animal) const {
    const Kind* kind = kindOf(animal);
    if (kind == nullptr) {
        return 0.0f;
    }
    const Animal& a = things_.registry().get<Animal>(animal);
    const int64_t age = time_.seconds - a.bornAt;
    if (!a.started) {
        return capacity(*kind, age);
    }
    return std::clamp(a.food - foodFallPerSecond(*kind, age) * static_cast<float>(time_.seconds - a.foodAt), 0.0f,
                      capacity(*kind, age));
}

void AnimalSystem::addFood(Entity animal, float nutrition) {
    const Kind* kind = kindOf(animal);
    if (kind == nullptr) {
        return;
    }
    Animal& a = things_.registry().get<Animal>(animal);
    if (!a.started) {
        return;
    }
    const int64_t now = time_.seconds;
    const int64_t age = now - a.bornAt;
    a.food = std::max(0.0f, a.food - foodFallPerSecond(*kind, age) * static_cast<float>(now - a.foodAt));
    a.foodAt = now;
    a.food = std::min(capacity(*kind, age), a.food + nutrition);
}

bool AnimalSystem::goTo(Entity animal, CellPos goal) {
    entt::registry& reg = things_.registry();
    const Kind* kind = kindOf(animal);
    if (kind == nullptr || husbandry_ == nullptr || !reg.all_of<Mover, Position>(animal)) {
        return false;
    }
    Animal& a = reg.get<Animal>(animal);
    if (a.wild || !a.started || a.mode == AnimalMode::Sleep || a.mode == AnimalMode::WaitPath) {
        return false;
    }
    AnimalCare* care = husbandry_->careFor(animal);
    if (care == nullptr || !map_.inBounds(goal)) {
        return false;
    }
    return walkOrPath(animal, a, *kind, *care, goal, AnimalMode::Wander);
}

bool AnimalSystem::mayWork(Entity animal) const {
    const entt::registry& reg = things_.registry();
    const Kind* kind = kindOf(animal);
    if (kind == nullptr || husbandry_ == nullptr || !husbandry_->learned(animal, Trainable::Haul)) {
        return false;
    }
    const Animal& a = reg.get<Animal>(animal);
    const AnimalCare* care = husbandry_->care(animal);
    const Health* health = reg.try_get<Health>(animal);
    const int64_t now = time_.seconds;
    if (a.wild || !a.started || a.mode == AnimalMode::Sleep || sleepNow(*kind, a, now) || !isAdult(animal)) {
        return false;
    }
    if (health != nullptr && (health->incapacitated() || !health->injuries.empty())) {
        return false;
    }
    if (care != nullptr && care->heldUntil > now) {
        return false;
    }
    return foodShare(animal) >= static_cast<float>(husbandry_->rules().workMinFood);
}

void AnimalSystem::starveCheck(Entity e, Animal& a, const Kind& kind, AnimalCare& care) {
    (void)kind;
    const int64_t now = time_.seconds;
    if (a.food > 0.0f) {
        care.starvingSince = -1;
        return;
    }
    if (care.starvingSince < 0) {
        care.starvingSince = now;
        return;
    }
    if (static_cast<double>(now - care.starvingSince) >= husbandry_->rules().starveDays * static_cast<double>(kSecondsPerDay)) {
        cancelPath(care);
        if (health_.kill(e, DeathCause::Starvation)) {
            husbandry_->noteStarved(e);
        }
    }
}

// ---- Where it may be ------------------------------------------------------------------------

bool AnimalSystem::tamedMayStand(const AnimalCare* care, CellPos cell) const {
    if (care == nullptr || husbandry_ == nullptr) {
        return true;
    }
    if (care->zone != kNoAnimalZone) {
        return husbandry_->zoneAt(cell) == care->zone;
    }
    if (care->hasHome) {
        const double r = husbandry_->rules().homeRadius;
        return animalCareDist2(static_cast<float>(cell.x) + 0.5f, static_cast<float>(cell.y) + 0.5f,
                               static_cast<float>(care->home.x) + 0.5f, static_cast<float>(care->home.y) + 0.5f) <=
               static_cast<float>(r * r);
    }
    return true;
}

Entity AnimalSystem::edibleItemAt(Entity animal, CellPos cell) const {
    const entt::registry& reg = things_.registry();
    for (const Entity item : things_.itemsAt(cell)) {
        if (reg.valid(item) && reg.all_of<Thing, StackCount>(item) && reg.get<StackCount>(item).count > 0 &&
            husbandry_->animalEats(animal, reg.get<Thing>(item).def)) {
            return item;
        }
    }
    return kNoEntity;
}

// The nearest food in the area the animal may be in: an item it eats, or (for grazers) a plant. The nearest one it can
// walk straight to is `out`; if there is none, the nearest one behind a wall (`blocked`), to be reached by a path.
bool AnimalSystem::findTamedFood(Entity e, Animal& a, const Kind& kind, const AnimalCare& care, Rng& rng, CellPos& out,
                                 bool& isItem, bool& blocked) const {
    (void)a;
    const Position& pos = things_.registry().get<Position>(e);
    const CellPos here = cellOf(pos);
    const int reach = static_cast<int>(husbandry_->rules().itemRadius);
    const int plantReach = static_cast<int>(rules_.grazeRadius);
    bool haveBlocked = false;
    CellPos blockedCell;
    bool blockedItem = false;
    blocked = false;
    for (int r = 0; r <= reach; ++r) {
        const int cells = r == 0 ? 1 : 8 * r;
        const int first = r == 0 ? 0 : static_cast<int>(rng.below(static_cast<uint32_t>(cells)));
        for (int i = 0; i < cells; ++i) {
            const CellPos c = r == 0 ? here : animalCareRingCell(here, r, (first + i) % cells);
            if (!map_.inBounds(c) || !map_.walkable(c) || !tamedMayStand(&care, c)) {
                continue;
            }
            bool item = edibleItemAt(e, c) != kNoEntity;
            if (!item) {
                if (!kind.grazer || r > plantReach || !plants_.has(c)) {
                    continue;
                }
                const DefId plant = plants_.defAt(c);
                if (nutrition_[plant.value] <= 0.0f || plants_.growth(c) < rules_.minPlantGrowth) {
                    continue;
                }
            }
            if (r > 0 && !straightWalkIsClear(map_, pos.x, pos.y, static_cast<float>(c.x) + 0.5f,
                                              static_cast<float>(c.y) + 0.5f, kPawnRadius, pos.level)) {
                if (!haveBlocked) {
                    haveBlocked = true;
                    blockedCell = c;
                    blockedItem = item;
                }
                continue;
            }
            out = c;
            isItem = item;
            return true;
        }
    }
    if (haveBlocked) {
        out = blockedCell;
        isItem = blockedItem;
        blocked = true;
        return true;
    }
    return false;
}

// ---- Walking --------------------------------------------------------------------------------

void AnimalSystem::cancelPath(AnimalCare& care) {
    if (care.path.valid() && pathfinder_ != nullptr) {
        pathfinder_->cancel(care.path);
    }
    care.path = PathTicket{};
}

// A straight walk if the way is clear, else a path (answered a step or more later, see pollPath).
bool AnimalSystem::walkOrPath(Entity e, Animal& a, const Kind& kind, AnimalCare& care, CellPos goal, AnimalMode mode) {
    const int64_t now = time_.seconds;
    const Position pos = things_.registry().get<Position>(e);
    if (walkTo(e, a, kind, static_cast<float>(goal.x) + 0.5f, static_cast<float>(goal.y) + 0.5f, mode)) {
        a.nextThink = now + kAnimalCareWalkCheck;
        return true;
    }
    if (pathfinder_ == nullptr || now < care.noPathUntil || care.path.valid()) {
        return false;
    }
    const PathTicket ticket = pathfinder_->request(cellOf(pos), goal);
    if (pathfinder_->status(ticket) == PathStatus::Failed) {
        pathfinder_->cancel(ticket);
        care.noPathUntil = now + static_cast<int64_t>(husbandry_->rules().repathSeconds);
        return false;
    }
    care.path = ticket;
    care.pathGoal = goal;
    care.pathMode = mode;
    a.mode = AnimalMode::WaitPath;
    a.nextThink = now + 1;
    return true;
}

// True when the animal is free to decide again; false while it waits or has set off along its path.
bool AnimalSystem::pollPath(Entity e, Animal& a, const Kind& kind, AnimalCare& care) {
    entt::registry& reg = things_.registry();
    const int64_t now = time_.seconds;
    if (pathfinder_ == nullptr || !care.path.valid()) {
        care.path = PathTicket{};
        a.mode = AnimalMode::Idle;
        return true;
    }
    const PathStatus status = pathfinder_->status(care.path);
    if (status == PathStatus::Queued) {
        a.nextThink = now + 1;
        return false;
    }
    std::vector<CellPos> route;
    const bool found = status == PathStatus::Found && pathfinder_->takePath(care.path, route);
    if (!found) {
        pathfinder_->cancel(care.path);
    }
    care.path = PathTicket{};
    if (!found) {
        care.noPathUntil = now + static_cast<int64_t>(husbandry_->rules().repathSeconds);
        a.mode = AnimalMode::Idle;
        return true;
    }
    for (const CellPos& step : route) {
        if (map_.isDoor(step)) {  // animals can't open doors: the way is shut
            care.noPathUntil = now + static_cast<int64_t>(husbandry_->rules().repathSeconds);
            a.mode = AnimalMode::Idle;
            return true;
        }
    }
    const Position& pos = reg.get<Position>(e);
    smoothPath(map_, pos.x, pos.y, route, pos.level);
    Mover& mover = reg.get<Mover>(e);
    mover.metresPerStep = kind.topSpeed * rules_.wanderSpeed;
    mover.walkTo(pos.x, pos.y, std::move(route));
    a.goal = care.pathGoal;
    a.mode = care.pathMode;
    a.nextThink = now + kAnimalCareWalkCheck;
    return false;
}

// ---- Eating ---------------------------------------------------------------------------------

void AnimalSystem::finishEatingItem(Entity e, Animal& a, const Kind& kind) {
    entt::registry& reg = things_.registry();
    const int64_t now = time_.seconds;
    const Entity item = edibleItemAt(e, a.goal);
    if (item != kNoEntity) {
        const float perUnit = husbandry_->nutritionOf(reg.get<Thing>(item).def);
        const int count = reg.get<StackCount>(item).count;
        const float cap = capacity(kind, now - a.bornAt);
        const float want = std::max(0.0f, cap * rules_.fullAt - foodLevel(e));
        const int units = std::clamp(static_cast<int>(std::ceil(want / perUnit)), 1, count);
        addFood(e, perUnit * static_cast<float>(units));
        if (units >= count) {
            things_.destroy(item);
        } else {
            reg.get<StackCount>(item).count = count - units;
        }
    }
    a.mode = AnimalMode::Idle;
}

// ---- Deciding -------------------------------------------------------------------------------

void AnimalSystem::decideTamed(Entity e, Animal& a, const Kind& kind, Rng& rng) {
    entt::registry& reg = things_.registry();
    const int64_t now = time_.seconds;
    const Position pos = reg.get<Position>(e);
    const float cap = capacity(kind, now - a.bornAt);
    AnimalCare* care = husbandry_->careFor(e);
    if (care == nullptr) {
        a.nextThink = now + kSecondsPerHour;
        return;
    }

    // Hungry: eat what is close, hay and kibble as well as plants.
    if (a.food < rules_.hungryBelow * cap) {
        a.feeding = true;
    } else if (a.food >= rules_.fullAt * cap) {
        a.feeding = false;
    }
    if (a.feeding) {
        CellPos food;
        bool item = false;
        bool blocked = false;
        if (findTamedFood(e, a, kind, *care, rng, food, item, blocked)) {
            if (!blocked && cellOf(pos) == food) {
                a.goal = food;
                a.mode = item ? AnimalMode::EatItem : AnimalMode::Eat;
                a.until = now + rules_.eatSeconds;
                a.nextThink = a.until;
                return;
            }
            if (walkOrPath(e, a, kind, *care, food, item ? AnimalMode::GoToItem : AnimalMode::GoToFood)) {
                return;
            }
        }
    }

    // Outside the zone, or too far from home: walk back to the nearest spot it may stand on.
    const CellPos here = cellOf(pos);
    if (!tamedMayStand(care, here)) {
        CellPos target = care->home;
        if (care->zone != kNoAnimalZone) {
            if (const AnimalZone* zone = husbandry_->findZone(care->zone)) {
                float best = 0.0f;
                bool any = false;
                for (const CellPos& c : zone->cells) {
                    const float d = animalCareDist2(static_cast<float>(c.x) + 0.5f, static_cast<float>(c.y) + 0.5f, pos.x, pos.y);
                    if (!any || d < best) {
                        best = d;
                        target = c;
                        any = true;
                    }
                }
            }
        }
        if (care->hasHome || care->zone != kNoAnimalZone) {
            if (walkOrPath(e, a, kind, *care, target, AnimalMode::Wander)) {
                return;
            }
        }
    }

    // Wander about where it may be.
    const bool zoned = care->zone != kNoAnimalZone;
    const float reachMax = a.feeding ? 2.0f * rules_.wanderMax : rules_.wanderMax;
    const float reachMin = zoned ? kAnimalCarePenWander : rules_.wanderMin;
    if (rng.unit() < rules_.wanderChance) {
        for (int tries = 0; tries < kAnimalCareTries; ++tries) {
            const float dx = rng.range(-reachMax, reachMax);
            const float dy = rng.range(-reachMax, reachMax);
            const float d2 = dx * dx + dy * dy;
            if (d2 < reachMin * reachMin || d2 > reachMax * reachMax) {
                continue;
            }
            const CellPos target{static_cast<int>(std::floor(pos.x + dx)), static_cast<int>(std::floor(pos.y + dy)), pos.level};
            if (!tamedMayStand(care, target)) {
                continue;
            }
            if (walkTo(e, a, kind, pos.x + dx, pos.y + dy, AnimalMode::Wander)) {
                a.nextThink = now + kAnimalCareWalkCheck;
                return;
            }
        }
    }
    a.mode = AnimalMode::Idle;
    a.until = now + rules_.idleMin + static_cast<int64_t>(rng.below(static_cast<uint32_t>(rules_.idleMax - rules_.idleMin + 1)));
    a.nextThink = a.until;
}

}  // namespace colony
