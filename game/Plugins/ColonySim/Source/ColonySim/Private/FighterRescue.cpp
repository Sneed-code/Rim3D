// Rescue under fire (task F10, docs/design/field-care.md): a fighter of the alarm carries a downed friend out of the
// enemies' sight, then fights on. FieldCare decides who and where to; this is the fighter's side. Part of the fighter
// system.
#include "ColonySim/Fighters.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Events.h"
#include "ColonySim/Health.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

// How close (metres) a fighter must be to lift a downed friend (the rescue job's reach), and how close once it has
// stopped at the cell it walked to (the friend may lie in a cell it can't stand in, a sandbag's).
constexpr float kRescueReach = 1.8f;
constexpr float kRescueReachStopped = 3.0f;

float rescueDist(const Position& a, const Position& b) {
    return std::hypot(a.x - b.x, a.y - b.y);
}

}  // namespace

Entity rescuerOf(const entt::registry& reg, Entity pawn) {
    if (pawn == kNoEntity) {
        return kNoEntity;
    }
    for (const auto [e, f] : reg.view<const Fighter>().each()) {
        if (f.mode == FightMode::Rescue && f.rescuing == pawn) {
            return e;
        }
    }
    return kNoEntity;
}

bool FighterSystem::startRescue(Entity pawn, Entity patient, CellPos to) {
    entt::registry& reg = things_.registry();
    Fighter* f = reg.valid(pawn) ? reg.try_get<Fighter>(pawn) : nullptr;
    if (f == nullptr || f->drafted || f->mode != FightMode::Fight || !things_.spawned(pawn) || !things_.spawned(patient) ||
        health_.incapacitated(pawn) || reg.all_of<PlayerControl>(pawn) || rescuerOf(reg, patient) != kNoEntity) {
        return false;
    }
    stop(pawn, *f);
    f->mode = FightMode::Rescue;
    f->rescuing = patient;
    f->lifted = false;
    f->rescueTo = to;
    f->hasGoal = true;
    f->goal = cellOf(reg.get<Position>(patient));
    f->hasSpot = false;
    f->target = kNoEntity;
    f->canFire = false;
    f->nextPlan = now();
    combat_.setStance(pawn, Stance::Standing);
    return true;
}

void FighterSystem::endRescue(Entity pawn, Fighter& f, bool laid) {
    entt::registry& reg = things_.registry();
    const Entity patient = f.rescuing;
    if (f.lifted && reg.valid(patient) && things_.spawned(patient) && things_.spawned(pawn)) {
        // Down on the cell the fighter stands in (the end of the carry, or wherever it had to let go).
        const CellPos at = cellOf(reg.get<Position>(pawn));
        Position& there = reg.get<Position>(patient);
        there.x = static_cast<float>(at.x) + 0.5f;
        there.y = static_cast<float>(at.y) + 0.5f;
        there.level = at.level;
        if (events_ != nullptr) {
            Event e;
            e.type = EventType::PawnAction;
            e.thing = pawn;
            e.def = reg.get<Thing>(pawn).def;
            e.action = PawnAction::LayPawn;
            e.target = patient;
            e.cell = at;
            e.message = laid ? "safe" : "let go";
            events_->push_back(std::move(e));
        }
    }
    f.rescuing = kNoEntity;
    f.lifted = false;
    f.mode = FightMode::Fight;
    f.hasGoal = false;
    f.hasSpot = false;
    f.nextPlan = std::min(f.nextPlan, now());
}

void FighterSystem::rescueStep(Entity pawn, Fighter& f) {
    entt::registry& reg = things_.registry();
    const Entity patient = f.rescuing;
    const Health* h = reg.valid(patient) ? reg.try_get<Health>(patient) : nullptr;
    if (h == nullptr || !things_.spawned(patient) || !h->downed || h->dead) {
        endRescue(pawn, f, false);  // it got up, died or is gone
        return;
    }
    const Position& at = reg.get<Position>(pawn);
    Position& there = reg.get<Position>(patient);
    const CellPos here = cellOf(at);
    if (!f.lifted) {
        const float d = rescueDist(at, there);
        const bool stopped = f.walk == 0 && f.hasSpot && here == f.spot;
        if (d <= kRescueReach || (stopped && d <= kRescueReachStopped)) {
            if (Mover* mover = reg.try_get<Mover>(patient)) {
                mover->path.clear();
                mover->next = 0;
                mover->vx = 0.0f;
                mover->vy = 0.0f;
            }
            there.x = at.x;
            there.y = at.y;
            there.level = at.level;
            f.lifted = true;
            f.goal = f.rescueTo;
            stop(pawn, f);
            f.hasSpot = false;
            f.nextPlan = now();
            if (events_ != nullptr) {
                Event e;
                e.type = EventType::PawnAction;
                e.thing = pawn;
                e.def = reg.get<Thing>(pawn).def;
                e.action = PawnAction::LiftPawn;
                e.target = patient;
                e.cell = here;
                events_->push_back(std::move(e));
            }
        } else if (stopped) {
            endRescue(pawn, f, false);  // it got as close as it could and the friend is still out of reach
        } else if (cellOf(there) != f.goal) {
            f.goal = cellOf(there);  // someone else moved it
            f.hasSpot = false;
            f.nextPlan = now();
        }
        return;
    }
    // Carried: it goes where the fighter goes.
    there.x = at.x;
    there.y = at.y;
    there.level = at.level;
    if (f.walk == 0 && f.hasSpot && here == f.spot) {
        endRescue(pawn, f, true);
    }
}

}  // namespace colony