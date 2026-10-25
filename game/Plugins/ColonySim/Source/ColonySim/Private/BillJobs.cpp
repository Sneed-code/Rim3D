// The DoBill job: fetch ingredients to a work building, work, make the product (task C7,
// docs/design/bills.md). Part of the job system.
#include "ColonySim/Jobs.h"

#include "ColonySim/Bills.h"
#include "ColonySim/Health.h"
#include "ColonySim/Map.h"
#include "ColonySim/Things.h"

#include <algorithm>

namespace colony {

bool JobSystem::failCheckDoBill(Entity pawn, Job& job, std::string& why) {
    const entt::registry& reg = things_.registry();
    if (!canUseHands(pawn)) {
        why = "it can't use its hands";
        return false;
    }
    if (!things_.spawned(job.site) || !reg.all_of<BillList>(job.site)) {
        why = "the work building is gone";
        return false;
    }
    const Bill* bill = bills_->find(job.site, job.bill);
    if (bill == nullptr) {
        why = "the bill is gone";
        return false;
    }
    if (bill->suspended) {
        why = "the bill was suspended";
        return false;
    }
    if (job.step <= kDoBillPickUpStep) {
        if (!things_.spawned(job.thing)) {
            why = "the ingredient is gone";
            return false;
        }
    } else if (job.step <= kDoBillDepositStep) {
        const Carrying* carrying = reg.try_get<Carrying>(pawn);
        if (carrying == nullptr || carrying->thing != job.thing || !reg.valid(job.thing)) {
            why = "it lost what it carried";
            return false;
        }
    }
    for (const BillPick& pick : job.picks) {
        if (!things_.spawned(pick.item)) {
            why = "an ingredient is gone";
            return false;
        }
    }
    return true;
}

// The carried ingredient goes into the job (it is used up when the product is made). While more
// stacks are to be fetched the job starts its steps again with the next.
JobSystem::StepResult JobSystem::depositIngredient(Entity pawn, Job& job, std::string& why) {
    entt::registry& reg = things_.registry();
    if (cellOf(reg.get<Position>(pawn)) != job.cell) {
        why = "the pawn isn't beside the work building";
        return StepResult::Failed;
    }
    const Entity carried = job.thing;
    const DefId def = reg.get<Thing>(carried).def;
    const int count = reg.get<StackCount>(carried).count;
    release(pawn, job);
    reg.remove<Carrying>(pawn);
    things_.destroy(carried);
    pawnDid(pawn, PawnAction::PutDown, job.site);
    auto used = std::find_if(job.used.begin(), job.used.end(), [def](const BillUsed& u) { return u.def == def; });
    if (used != job.used.end()) {
        used->count += count;
    } else {
        job.used.push_back(BillUsed{def, count});
    }
    if (job.picks.empty()) {
        job.thing = kNoEntity;
        job.count = 0;
        reserve(pawn, job);
        return StepResult::Next;
    }
    job.thing = job.picks.front().item;
    job.count = job.picks.front().count;
    job.picks.erase(job.picks.begin());
    reserve(pawn, job);
    return StepResult::Repeat;
}

JobSystem::StepResult JobSystem::billWork(Entity pawn, Job& job, std::string& why) {
    entt::registry& reg = things_.registry();
    if (cellOf(reg.get<Position>(pawn)) != job.cell) {
        why = "the pawn left its place beside the work building";
        return StepResult::Failed;
    }
    const Bill* bill = bills_->find(job.site, job.bill);
    const Recipe* recipe = bill != nullptr ? bills_->recipe(bill->recipe) : nullptr;
    if (recipe == nullptr) {
        why = "the bill is gone";
        return StepResult::Failed;
    }
    if (job.walk == 0) {
        job.walk = 2;
        pawnDid(pawn, PawnAction::Cook, job.site);
    }
    job.progress += bills_->workSpeed(pawn, *recipe) * health_.capacityValue(pawn, "Manipulation") * lightWorkFactor(pawn);
    return job.progress < recipe->work ? StepResult::Running : StepResult::Next;
}

void JobSystem::dropNewItems(Entity pawn, DefId def, int count) {
    entt::registry& reg = things_.registry();
    const int limit = stackLimit(def);
    for (int left = count; left > 0; left -= limit) {
        const Entity item = things_.make(def, std::min(left, limit));
        if (item == kNoEntity) {
            return;
        }
        reg.emplace_or_replace<Carrying>(pawn, Carrying{item});
        dropCarried(pawn);
    }
}

JobSystem::StepResult JobSystem::billFinish(Entity pawn, Job& job, std::string& why) {
    const Bill* bill = bills_->find(job.site, job.bill);
    const Recipe* recipe = bill != nullptr ? bills_->recipe(bill->recipe) : nullptr;
    if (recipe == nullptr) {
        why = "the bill is gone";
        return StepResult::Failed;
    }
    // The ingredients are gone for good; the product lies by the pawn, where haulers find it.
    job.used.clear();
    int made = 0;
    for (const RecipeProduct& product : recipe->products) {
        dropNewItems(pawn, product.item, product.count);
        made += product.count;
    }
    const DefId recipeDef = recipe->def;
    const bool ended = bills_->iterationDone(job.site, job.bill);
    Event event;
    event.type = EventType::RecipeDone;
    event.thing = pawn;
    event.def = recipeDef;
    event.target = job.site;
    event.cell = job.cell;
    event.amount = static_cast<float>(made);
    event.message = ended ? "bill finished" : std::string();
    events_.push_back(std::move(event));
    return StepResult::Next;
}

void JobSystem::returnIngredients(Entity pawn, const Job& job) {
    for (const BillUsed& used : job.used) {
        dropNewItems(pawn, used.def, used.count);
    }
}

}  // namespace colony
