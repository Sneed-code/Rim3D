// The DoBill job: fetch ingredients to a work building, work, make the product (task C7,
// docs/design/bills.md). Part of the job system.
#include "ColonySim/Jobs.h"
#include "ColonySim/Spoilage.h"

#include "ColonySim/Apparel.h"
#include "ColonySim/Bills.h"
#include "ColonySim/Crafting.h"
#include "ColonySim/Fuel.h"
#include "ColonySim/Health.h"
#include "ColonySim/Inspirations.h"
#include "ColonySim/Map.h"
#include "ColonySim/Power.h"
#include "ColonySim/Things.h"

#include <algorithm>

namespace colony {

bool JobSystem::buildingRuns(Entity building) const {
    return power_ == nullptr || power_->running(building);
}

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
    if (!buildingRuns(job.site)) {
        why = "the work building has no power";
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
    if (const Recipe* recipe = bills_->recipe(bill->recipe);
        recipe != nullptr && !fuel_->canUse(job.site, fuel_->specOf(job.site).perUse * static_cast<float>(recipe->fuelUses()))) {
        why = "the building is out of fuel";
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
    DefId def = reg.get<Thing>(carried).def;
    int count = reg.get<StackCount>(carried).count;
    // A smelter keeps what the thing melts down to, not the thing: it goes where the products do (an interrupted run drops the metal).
    const Bill* smeltBill = bills_->find(job.site, job.bill);
    const Recipe* smeltRecipe = smeltBill != nullptr ? bills_->recipe(smeltBill->recipe) : nullptr;
    std::vector<RecipeProduct> melted;
    const bool smelting = smeltRecipe != nullptr && smeltRecipe->smeltFraction > 0.0;
    if (smelting) {
        const MadeOf* madeOf = reg.try_get<MadeOf>(carried);
        melted = bills_->smeltYield(def, madeOf != nullptr ? madeOf->stuff : DefId{}, smeltRecipe->smeltFraction, count);
    }
    release(pawn, job);
    reg.remove<Carrying>(pawn);
    things_.destroy(carried);
    pawnDid(pawn, PawnAction::PutDown, job.site);
    if (smelting) {
        for (const RecipeProduct& metal : melted) {
            auto have = std::find_if(job.used.begin(), job.used.end(), [&](const BillUsed& u) { return u.def == metal.item; });
            if (have != job.used.end()) {
                have->count += metal.count;
            } else {
                job.used.push_back(BillUsed{metal.item, metal.count});
            }
        }
    } else {
        auto used = std::find_if(job.used.begin(), job.used.end(), [def](const BillUsed& u) { return u.def == def; });
        if (used != job.used.end()) {
            used->count += count;
        } else {
            job.used.push_back(BillUsed{def, count});
        }
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
    // A bench works at its own speed, and some stuffs (wood against steel) take more or less work.
    float benchFactor = 1.0f;
    float stuffFactor = 1.0f;
    if (crafting_ != nullptr) {
        benchFactor = crafting_->benchSpeed(reg.get<Thing>(job.site).def);
        if (recipe->stuffFromIngredient && !recipe->products.empty()) {
            stuffFactor = crafting_->stuffWorkFactor(crafting_->dominantStuff(recipe->products.front().item, job.used));
        }
    }
    job.progress += bills_->workSpeed(pawn, *recipe) * health_.capacityValue(pawn, "Manipulation") * workFactor(pawn) *
                    benchFactor / std::max(stuffFactor, 0.01f);
    // The recipe's experience is what a run gives at 100% speed, so it comes in a steady stream
    // while the pawn works, whatever its speed (a slow worker earns more from a run).
    if (recipe->work > 0.0f && !recipe->skill.empty()) {
        learnSkill(pawn, recipe->skill.c_str(), static_cast<double>(recipe->skillXp) / static_cast<double>(recipe->work));
    }
    return job.progress < recipe->work ? StepResult::Running : StepResult::Next;
}

void JobSystem::dropNewItems(Entity pawn, DefId def, int count, const std::vector<BillUsed>* madeFrom, const Recipe* recipe,
                             uint32_t storeZone) {
    entt::registry& reg = things_.registry();
    const int limit = stackLimit(def);
    for (int left = count; left > 0; left -= limit) {
        const Entity item = things_.make(def, std::min(left, limit));
        if (item == kNoEntity) {
            return;
        }
        if (madeFrom != nullptr && spoilage_ != nullptr) {
            spoilage_->madeBy(item, *madeFrom, pawn, cellOf(reg.get<Position>(pawn)));
        }
        if (recipe != nullptr && crafting_ != nullptr) {
            // An inspired crafter's next thing with a quality is better; making it uses the inspiration up.
            const int shift = inspirations_ != nullptr && crafting_->hasQuality(def) ? inspirations_->takeQualityShift(pawn) : 0;
            crafting_->finishProduct(item, *recipe, bills_->skillLevel(pawn, recipe->skill), madeFrom != nullptr ? *madeFrom : std::vector<BillUsed>{},
                                     storeZone, shift);
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
    // The ingredients are gone for good (and the fuel burnt); the product lies by the pawn, where haulers
    // find it.
    const std::vector<BillUsed> used = std::move(job.used);
    job.used.clear();
    fuel_->use(job.site, fuel_->specOf(job.site).perUse * static_cast<float>(recipe->fuelUses()));
    int made = 0;
    const uint32_t storeZone = bill->storeZone;
    if (recipe->smeltFraction > 0.0) {
        // The metal was worked out when the ingredient went in (depositIngredient).
        for (const BillUsed& metal : used) {
            dropNewItems(pawn, metal.def, metal.count);
            made += metal.count;
        }
    }
    for (const RecipeProduct& product : recipe->products) {
        dropNewItems(pawn, product.item, product.count, &used, recipe, storeZone);
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
