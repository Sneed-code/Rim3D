// Recreation on the job system (task C18, docs/design/recreation.md): choosing something to do for
// joy, walking to the place, and playing, sitting or strolling there for a while.
#include "ColonySim/Jobs.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Map.h"
#include "ColonySim/Power.h"
#include "ColonySim/Random.h"
#include "ColonySim/Recreation.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

float recJobsDistSq(const Position& a, CellPos b) {
    const float dx = a.x - (static_cast<float>(b.x) + 0.5f);
    const float dy = a.y - (static_cast<float>(b.y) + 0.5f);
    return dx * dx + dy * dy;
}

// A number from 0 up to but not including 1, the same for the same pawn, second and salt: the
// sim's random stream is not used, so choosing for joy never shifts what else is rolled.
double recJobsUnit(Entity pawn, int64_t now, uint64_t salt) {
    const uint64_t h = splitMix64(splitMix64(static_cast<uint64_t>(entt::to_integral(pawn)) * 0x9E3779B97F4A7C15ULL ^
                                             static_cast<uint64_t>(now)) ^
                                  salt);
    return static_cast<double>(h >> 11) / 9007199254740992.0;
}

}  // namespace

// ---- Choosing -------------------------------------------------------------------------------

// An Anything hour with nothing to work on: a pawn whose recreation has run low goes to play.
bool JobSystem::thinkJoy(Entity pawn, Job& out) {
    const Joy* joy = things_.registry().try_get<Joy>(pawn);
    if (joy == nullptr || scheduleNow(pawn) != ScheduleKind::Anything || joy->level >= recreation_.seekBelow) {
        return false;
    }
    return makeRecreationJob(pawn, out, 1.0f);
}

bool JobSystem::isOutdoorsCell(CellPos cell) const {
    return rooms_ == nullptr || !rooms_->indoors(cell);
}

// The best activity this pawn can do now: the most joy an hour with its tolerance counted and the
// building's own factor, the nearest on a tie, then the first by name. A pawn with its recreation
// at `playUntil` or more wants nothing.
bool JobSystem::makeRecreationJob(Entity pawn, Job& out, float playUntil) {
    const entt::registry& reg = things_.registry();
    const Joy* joy = reg.try_get<Joy>(pawn);
    if (joy == nullptr || !recreation_.present || joy->level >= playUntil) {
        return false;
    }
    const Position& at = reg.get<Position>(pawn);
    bool found = false;
    float bestGain = 0.0f;
    float bestDist = 0.0f;
    bool bestPlaced = false;
    // Kinds the pawn is bored of come last: only when nothing else can be done now.
    for (int pass = 0; pass < 2 && !found; ++pass)
    for (const RecreationActivity& activity : recreation_.activities) {
        if ((pass == 0) == (joy->boredWith(recreation_.kinds[static_cast<size_t>(activity.kind)].defName) ||
                         joy->toleranceOf(recreation_.kinds[static_cast<size_t>(activity.kind)].defName) > recreation_.boredAbove)) {
            continue;
        }
        Job job;
        if (!recreationSpot(pawn, activity, job)) {
            continue;
        }
        float factor = 1.0f;
        if (job.thing != kNoEntity) {
            factor = static_cast<float>(defs_.get(reg.get<Thing>(job.thing).def).number("building.joyGainFactor", 1.0));
        }
        const float gain = recreation_.gainPerHour(activity, *joy) * factor;
        const float dist = recJobsDistSq(at, job.cell);
        // On equal joy a game at a building beats a stroll or stargazing (they cost no walk), then the nearest.
        const bool placed = job.thing != kNoEntity;
        const bool tie = std::fabs(gain - bestGain) <= 1e-6f;
        if (!found || gain > bestGain + 1e-6f || (tie && ((placed && !bestPlaced) || (placed == bestPlaced && dist < bestDist)))) {
            bestPlaced = placed;
            found = true;
            bestGain = gain;
            bestDist = dist;
            out = std::move(job);
        }
    }
    return found;
}

// The number of other pawns at a building now (or about to be: their jobs name it).
int JobSystem::recreationUsers(Entity building, Entity except) const {
    int users = 0;
    for (const auto [other, worker] : things_.registry().view<const Worker>().each()) {
        if (other != except && worker.job.type == JobType::Recreate && worker.job.thing == building) {
            ++users;
        }
    }
    return users;
}

// A cell to stroll to (`wander`: a few to twenty metres away, found by a hash of pawn and time), or
// the nearest open-air cell (a place to lie and look up). Both are outdoors, walkable and reachable.
bool JobSystem::strollCell(Entity pawn, CellPos& out, bool wander) {
    const entt::registry& reg = things_.registry();
    const Position& at = reg.get<Position>(pawn);
    const CellPos from = cellOf(at);
    const auto usable = [&](CellPos c) {
        return map_.inBounds(c) && map_.walkable(c) && isOutdoorsCell(c) && !avoiding(pawn, JobTarget{kNoEntity, c}) &&
               canReach(from, c);
    };
    if (wander) {
        // Integer offsets from the hash, no trigonometry: the same answer on every platform.
        const int reach = std::max(5, static_cast<int>(recreation_.walkRadius));
        for (uint64_t attempt = 0; attempt < 8; ++attempt) {
            const int dx = static_cast<int>(std::floor(recJobsUnit(pawn, now(), attempt * 2) * (2 * reach + 1))) - reach;
            const int dy = static_cast<int>(std::floor(recJobsUnit(pawn, now(), attempt * 2 + 1) * (2 * reach + 1))) - reach;
            const CellPos c{from.x + dx, from.y + dy, from.level};
            if (std::max(std::abs(dx), std::abs(dy)) >= 4 && usable(c)) {
                out = c;
                return true;
            }
        }
        return false;
    }
    if (usable(from)) {
        out = from;
        return true;
    }
    const int reach = static_cast<int>(recreation_.walkRadius);
    for (int ring = 1; ring <= reach; ++ring) {
        for (int dy = -ring; dy <= ring; ++dy) {
            for (int dx = -ring; dx <= ring; ++dx) {
                if (std::max(std::abs(dx), std::abs(dy)) != ring) {
                    continue;
                }
                const CellPos c{from.x + dx, from.y + dy, from.level};
                if (usable(c)) {
                    out = c;
                    return true;
                }
            }
        }
    }
    return false;
}

// The job for one activity, or false if the pawn can't do it now: no building with a free place,
// no open air nearby, the wrong time of day.
bool JobSystem::recreationSpot(Entity pawn, const RecreationActivity& activity, Job& out) {
    const entt::registry& reg = things_.registry();
    const Position& at = reg.get<Position>(pawn);
    const CellPos from = cellOf(at);
    if (activity.daytimeOnly) {
        const int hour = time_.hourOfDay();
        if (hour < recreation_.dayFromHour || hour >= recreation_.dayToHour) {
            return false;
        }
    }
    if (activity.nightOnly) {
        const int hour = time_.hourOfDay();
        if (hour >= recreation_.dayFromHour && hour < recreation_.dayToHour) {
            return false;
        }
    }
    out = Job{};
    out.type = JobType::Recreate;
    out.activity = activity.def;
    if (activity.building.empty() && activity.buildingFlag.empty() && !activity.chairs) {
        if (!strollCell(pawn, out.cell, activity.walks)) {
            return false;
        }
        out.count = activity.walks ? 1 : 0;
        return true;
    }

    const float radiusSq = recreation_.spotRadius * recreation_.spotRadius;
    const DefId buildingDef = activity.building.empty() ? DefId{} : defs_.idOf("Things", activity.building);
    if (activity.buildingFlag.empty() && !activity.chairs && !buildingDef.valid()) {
        return false;
    }
    const std::string flagPath = activity.buildingFlag.empty() ? std::string() : "building." + activity.buildingFlag;
    const auto isTheBuilding = [&](DefId def) {
        if (!flagPath.empty()) {
            return defs_.get(def).flag(flagPath.c_str());
        }
        return def == buildingDef;
    };
    const auto free = [&](Entity owner, CellPos c) {
        return c.level == from.level && map_.inBounds(c) && map_.walkable(c) && recJobsDistSq(at, c) <= radiusSq &&
               !reservedByOther(JobTarget{kNoEntity, c}, pawn) && !avoiding(pawn, JobTarget{owner, CellPos{}}) &&
               !avoiding(pawn, JobTarget{kNoEntity, c}) && canReach(from, c);
    };

    bool found = false;
    float bestDist = 0.0f;
    // Relaxing socially: any free chair, beside the others already doing it when there are some. The
    // first sitter's chair names the group (`thing`), so they chat like players at a table.
    if (activity.chairs) {
        struct Mate {
            CellPos cell;
            Entity group;
        };
        std::vector<Mate> mates;
        for (const auto [other, worker] : reg.view<const Worker>().each()) {
            if (other != pawn && worker.job.type == JobType::Recreate && worker.job.activity == activity.def &&
                worker.job.thing != kNoEntity && recreationUsers(worker.job.thing, pawn) < activity.usersMax) {
                mates.push_back(Mate{worker.job.cell, worker.job.thing});
            }
        }
        std::sort(mates.begin(), mates.end(), [](const Mate& a, const Mate& b) {
            return a.cell.x != b.cell.x ? a.cell.x < b.cell.x : a.cell.y < b.cell.y;
        });
        for (const auto [chair, thing, footprint] : reg.view<const Thing, const Footprint>().each()) {
            if (isChair_[thing.def.value] == 0 || !things_.spawned(chair)) {
                continue;
            }
            const CellRect rect = footprint.rect();
            const CellPos c{rect.minX, rect.minY, rect.level};
            float score = recJobsDistSq(at, c);
            Entity group = chair;
            if (!mates.empty()) {
                // Next to the nearest mate, not across the map: their distance first.
                float nearest = 1e30f;
                for (const Mate& m : mates) {
                    const float dx = static_cast<float>(c.x - m.cell.x);
                    const float dy = static_cast<float>(c.y - m.cell.y);
                    if (dx * dx + dy * dy < nearest) {
                        nearest = dx * dx + dy * dy;
                        group = m.group;
                    }
                }
                if (nearest > 25.0f) {
                    continue;
                }
                score = nearest * 1000.0f + score;
            }
            if ((found && score >= bestDist) || !free(chair, c)) {
                continue;
            }
            found = true;
            bestDist = score;
            out.thing = group;
            out.chair = chair;
            out.cell = c;
        }
        return found;
    }

    // Chatting at a table: a chair next to any table.
    const bool atTable = flagPath == "building.isTable";
    if (atTable) {
        for (const auto [chair, thing, footprint] : reg.view<const Thing, const Footprint>().each()) {
            if (isChair_[thing.def.value] == 0 || !things_.spawned(chair)) {
                continue;
            }
            const CellRect rect = footprint.rect();
            for (int y = rect.minY; y < rect.minY + rect.sizeY; ++y) {
                for (int x = rect.minX; x < rect.minX + rect.sizeX; ++x) {
                    const CellPos c{x, y, rect.level};
                    const CellPos around[4] = {{c.x + 1, c.y, c.level}, {c.x - 1, c.y, c.level}, {c.x, c.y + 1, c.level}, {c.x, c.y - 1, c.level}};
                    for (const CellPos& n : around) {
                        const Entity table = map_.inBounds(n) ? map_.buildingAt(n) : kNoEntity;
                        if (table == kNoEntity || !isTheBuilding(reg.get<Thing>(table).def)) {
                            continue;
                        }
                        const int usersMax = std::min(activity.usersMax, std::max(1, static_cast<int>(defs_.get(reg.get<Thing>(table).def).integer("building.maxSimultaneousUsers", activity.usersMax))));
                        const float dist = recJobsDistSq(at, c);
                        if ((found && dist >= bestDist) || recreationUsers(table, pawn) >= usersMax || !free(table, c)) {
                            continue;
                        }
                        found = true;
                        bestDist = dist;
                        out.thing = table;
                        out.chair = chair;
                        out.cell = c;
                    }
                }
            }
        }
        return found;
    }

    // A game at a building: a free cell beside it.
    for (const auto [building, thing, footprint] : reg.view<const Thing, const Footprint>().each()) {
        if (!isTheBuilding(thing.def) || !things_.spawned(building)) {
            continue;
        }
        const int usersMax = std::min(activity.usersMax, std::max(1, static_cast<int>(defs_.get(thing.def).integer("building.maxSimultaneousUsers", activity.usersMax))));
        if (recreationUsers(building, pawn) >= usersMax) {
            continue;
        }
        // A television needs power, a telescope the open sky.
        if (defs_.get(thing.def).has("power.consumption") && power_ != nullptr && !power_->running(building)) {
            continue;
        }
        const CellRect rect = footprint.rect();
        if (activity.openSky && !isOutdoorsCell(CellPos{rect.minX, rect.minY, rect.level})) {
            continue;
        }
        const int reach = activity.reach;
        for (int y = rect.minY - reach; y <= rect.minY + rect.sizeY - 1 + reach; ++y) {
            for (int x = rect.minX - reach; x <= rect.minX + rect.sizeX - 1 + reach; ++x) {
                const bool insideX = x >= rect.minX && x < rect.minX + rect.sizeX;
                const bool insideY = y >= rect.minY && y < rect.minY + rect.sizeY;
                if (activity.onBuilding ? !(insideX && insideY)
                                        : ((insideX && insideY) || (reach == 1 && !insideX && !insideY))) {
                    continue;  // not where the pawn goes: the building itself, or a corner beside a small one
                }
                const CellPos c{x, y, rect.level};
                const float dist = recJobsDistSq(at, c);
                if ((found && dist >= bestDist) || !free(building, c)) {
                    continue;
                }
                found = true;
                bestDist = dist;
                out.thing = building;
                out.cell = c;
            }
        }
    }
    return found;
}

// ---- Playing --------------------------------------------------------------------------------

bool JobSystem::failCheckRecreate(Entity, Job& job, std::string& why) {
    if (job.thing != kNoEntity && !things_.spawned(job.thing)) {
        why = "the place is gone";
        return false;
    }
    if (job.chair != kNoEntity && !things_.spawned(job.chair)) {
        why = "the chair is gone";
        return false;
    }
    return true;
}

// The pawn stays for the activity's time (a random half hour to an hour and a half, by the
// activity), gaining joy every second at the activity's rate with tolerance counted; it stops at
// full joy. A normal finish wears the kind of recreation in a little (tolerance).
JobSystem::StepResult JobSystem::play(Entity pawn, Job& job, std::string& why) {
    entt::registry& reg = things_.registry();
    Joy* joy = reg.try_get<Joy>(pawn);
    const RecreationActivity* activity = nullptr;
    for (const RecreationActivity& a : recreation_.activities) {
        if (a.def == job.activity) {
            activity = &a;
        }
    }
    if (joy == nullptr || activity == nullptr) {
        why = "it can't do that any more";
        return StepResult::Failed;
    }
    if (job.until == 0) {
        const double hours = activity->hoursMin + (activity->hoursMax - activity->hoursMin) * recJobsUnit(pawn, now(), 99);
        job.until = now() + std::max<int64_t>(1, static_cast<int64_t>(std::llround(hours * static_cast<double>(kSecondsPerHour))));
        return StepResult::Running;
    }
    float factor = 1.0f;
    if (job.thing != kNoEntity) {
        factor = static_cast<float>(defs_.get(reg.get<Thing>(job.thing).def).number("building.joyGainFactor", 1.0));
    }
    // Tolerance to the kind grows by two thirds of the joy gained (wiki "Recreation").
    const float before = joy->level;
    joy->level = std::min(1.0f, joy->level + recreation_.gainPerHour(*activity, *joy) * factor / static_cast<float>(kSecondsPerHour));
    if (joy->level > before) {
        joy->addTolerance(recreation_.kinds[static_cast<size_t>(activity->kind)].defName,
                          (joy->level - before) * recreation_.toleranceGainFraction, recreation_.toleranceMax);
    }

    if (now() >= job.until || joy->level >= 1.0f) {
        return StepResult::Next;
    }
    if (activity->walks) {
        // A stroll goes from place to place: on arriving, off to another.
        std::string ignored;
        const StepResult leg = walkTo(pawn, job, job.cell, ignored);
        if (leg == StepResult::Next || leg == StepResult::Failed) {
            CellPos next;
            if (!strollCell(pawn, next, true)) {
                return StepResult::Next;
            }
            job.cell = next;
            job.walk = 0;
            job.repaths = 0;
        }
    }
    return StepResult::Running;
}

}  // namespace colony
