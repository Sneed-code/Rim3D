// Changing clothes and stripping, as jobs (docs/design/apparel.md).
#include "ColonySim/Apparel.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Repair.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace colony {

namespace {
constexpr int kMaxArmedColonists = 3;  // ours
}  // namespace

// The think node: take off what the outfit doesn't allow, else put on the clothes that gain most.
// Once nothing is found a pawn doesn't look again for ApparelRules.checkEverySteps (it keeps looking
// while it keeps finding something, so it dresses in one go).
bool JobSystem::thinkApparel(Entity pawn, Job& out) {
    if (apparel_ == nullptr) {
        return false;
    }
    entt::registry& reg = things_.registry();
    if (!reg.all_of<PawnOutfit>(pawn)) {
        reg.emplace<PawnOutfit>(pawn, PawnOutfit{apparel_->outfitOf(pawn), 0});
    }
    PawnOutfit& own = reg.get<PawnOutfit>(pawn);
    if (now() < own.nextCheck) {
        return false;
    }
    const CellPos at = cellOf(reg.get<Position>(pawn));
    const Entity remove = apparel_->wornToRemove(pawn);
    if (remove != kNoEntity) {
        out = Job{};
        out.type = JobType::WearApparel;
        out.thing = remove;
        out.count = 1;
        out.cell = at;
        return true;
    }
    const Entity best = apparel_->bestToWear(pawn, [&](Entity item) {
        const JobTarget target{item, CellPos{}};
        return !reservedByOther(target, pawn) && !avoiding(pawn, target) && canReach(at, cellOf(reg.get<Position>(item)));
    });
    if (best != kNoEntity) {
        out = Job{};
        out.type = JobType::WearApparel;
        out.thing = best;
        out.count = 0;
        out.cell = cellOf(reg.get<Position>(best));
        return true;
    }
    if (thinkMendWorn(pawn, out)) {
        return true;
    }
    if (findLooseWeapon(pawn, out)) {
        return true;
    }
    own.nextCheck = now() + apparel_->rules().checkEverySteps;
    return false;
}

// A colonist with no gun in a gun slot picks up the nearest gun lying about that fits it (rifles first, then sidearms): the
// guns left at the landing and what a fallen colonist dropped arm the rest of the colony. Weapons already carried are never
// swapped for better ones. Melee weapons are left lying: a colonist with only a knife stands and fights a wolf, and a
// berserk one with a knife kills its friends. Ours.
bool JobSystem::findLooseWeapon(Entity pawn, Job& out) {
    if (combat_ == nullptr) {
        return false;
    }
    entt::registry& reg = things_.registry();
    if (!reg.all_of<Colonist>(pawn) || health_.incapacitated(pawn)) {
        return false;
    }
    bool free[kWeaponSlotCount];
    for (int slot = 0; slot < kWeaponSlotCount; ++slot) {
        free[slot] = combat_->weaponIn(pawn, static_cast<WeaponSlot>(slot)) == kNoEntity;
    }
    // At most kMaxArmedColonists carry a gun: a colonist with a revolver goes to meet three wolves (the year run lost most of
    // its colonists that way, and to berserk ones with guns), while one with none shelters.
    bool armed = false;
    for (int slot = 0; slot < kWeaponSlotCount; ++slot) {
        armed = armed || !free[slot];
    }
    if (!armed) {
        int others = 0;
        for (const auto [other, colonist] : reg.view<const Colonist>().each()) {
            (void)colonist;
            if (other != pawn && !health_.incapacitated(other) &&
                (combat_->weaponIn(other, WeaponSlot::Primary) != kNoEntity || combat_->weaponIn(other, WeaponSlot::Sidearm) != kNoEntity)) {
                ++others;
            }
        }
        if (others >= kMaxArmedColonists) {
            return false;
        }
    }
    const CellPos at = cellOf(reg.get<Position>(pawn));
    Entity best = kNoEntity;
    int bestSlot = 0;
    float bestDist = 0.0f;
    for (const auto [item, thing, position] : reg.view<const Thing, const Position>().each()) {
        WeaponSlot slot = WeaponSlot::Primary;
        if (thing.category != ThingCategory::Item || reg.all_of<EquippedBy>(item) || !combat_->weaponSlot(thing.def, slot) ||
            slot == WeaponSlot::Melee || !free[static_cast<int>(slot)] || !things_.spawned(item) || position.level != at.level ||
            reg.any_of<RepairMark, FitOrder>(item)) {  // marked for the bench: leave it for the crafter
            continue;
        }
        const JobTarget target{item, CellPos{}};
        if (reservedByOther(target, pawn) || avoiding(pawn, target)) {
            continue;
        }
        const float dx = position.x - (static_cast<float>(at.x) + 0.5f);
        const float dy = position.y - (static_cast<float>(at.y) + 0.5f);
        const float dist = dx * dx + dy * dy;
        if (best != kNoEntity && (static_cast<int>(slot) > bestSlot || (static_cast<int>(slot) == bestSlot && dist >= bestDist))) {
            continue;
        }
        if (!canReach(at, cellOf(position))) {
            continue;
        }
        best = item;
        bestSlot = static_cast<int>(slot);
        bestDist = dist;
    }
    if (best == kNoEntity) {
        return false;
    }
    out = Job{};
    out.type = JobType::EquipWeapon;
    out.thing = best;
    out.cell = cellOf(reg.get<Position>(best));
    return true;
}

bool JobSystem::failCheckApparel(Entity pawn, Job& job, std::string& why) {
    const entt::registry& reg = things_.registry();
    if (apparel_ == nullptr) {
        why = "there is no clothing system";
        return false;
    }
    if (job.type == JobType::EquipWeapon) {
        if (!things_.spawned(job.thing) || reg.all_of<EquippedBy>(job.thing)) {
            why = "the weapon is gone";
            return false;
        }
        return true;
    }
    if (job.type == JobType::WearApparel) {
        if (job.count != 0) {
            const std::vector<Entity>& worn = apparel_->worn(pawn);
            if (std::find(worn.begin(), worn.end(), job.thing) == worn.end()) {
                why = "it isn't wearing that any more";
                return false;
            }
        } else if (!things_.spawned(job.thing)) {
            why = "the clothes are gone";
            return false;
        }
        return true;
    }
    // StripPawn
    if (!reg.valid(job.thing) || !reg.all_of<Position>(job.thing) || !apparel_->stripMarked(job.thing)) {
        why = "the strip order is gone";
        return false;
    }
    if (!health_.incapacitated(job.thing) || !apparel_->hasGear(job.thing)) {
        why = "there is nothing to take any more";
        return false;
    }
    return true;
}

JobSystem::StepResult JobSystem::apparelGoto(Entity pawn, Job& job, std::string& why) {
    if (job.count != 0) {
        return StepResult::Next;  // a worn piece comes off where the pawn stands
    }
    return walkTo(pawn, job, cellOf(things_.registry().get<Position>(job.thing)), why);
}

JobSystem::StepResult JobSystem::apparelWork(Entity pawn, Job& job, std::string& why) {
    entt::registry& reg = things_.registry();
    if (job.count == 0 && cellOf(reg.get<Position>(pawn)) != cellOf(reg.get<Position>(job.thing))) {
        why = "the pawn isn't where the clothes lie";
        return StepResult::Failed;
    }
    job.progress += 1.0f;
    if (job.progress < static_cast<float>(apparel_->rules().wearSteps)) {
        return StepResult::Running;
    }
    std::string error;
    if (job.type == JobType::EquipWeapon) {
        WeaponSlot slot = WeaponSlot::Primary;
        const bool primary = combat_->weaponSlot(reg.get<Thing>(job.thing).def, slot) && slot == WeaponSlot::Primary;
        if (!combat_->carryWeapon(pawn, job.thing, &error)) {
            why = error;
            return StepResult::Failed;
        }
        // The rifle in the hands, the sidearm on the belt, whichever came first.
        if (primary && combat_->weaponIn(pawn, WeaponSlot::Primary) == job.thing && reg.get<Gear>(pawn).weapon != job.thing) {
            combat_->switchWeapon(pawn, WeaponSlot::Primary);
        }
        return StepResult::Next;
    }
    const bool ok = job.count != 0 ? apparel_->takeOff(pawn, job.thing, error) : apparel_->putOn(pawn, job.thing, error);
    if (!ok) {
        why = error;
        return StepResult::Failed;
    }
    return StepResult::Next;
}

JobSystem::StepResult JobSystem::stripWork(Entity pawn, Job& job, std::string& why) {
    entt::registry& reg = things_.registry();
    const Position& at = reg.get<Position>(pawn);
    const Position& there = reg.get<Position>(job.thing);
    const float dx = at.x - there.x;
    const float dy = at.y - there.y;
    if (dx * dx + dy * dy > 4.0f) {
        why = "the pawn isn't beside the one it strips";
        return StepResult::Failed;
    }
    const Gear* gear = reg.try_get<Gear>(job.thing);
    const size_t pieces = gear == nullptr ? 0 : gear->apparel.size() + gear->stowed.size() + (gear->weapon != kNoEntity ? 1 : 0);
    job.progress += 1.0f;
    if (job.progress < static_cast<float>(pieces) * static_cast<float>(apparel_->rules().stripStepsPerPiece)) {
        return StepResult::Running;
    }
    apparel_->strip(job.thing);
    std::string error;
    apparel_->markStrip(job.thing, false, error);
    return StepResult::Next;
}

}  // namespace colony
