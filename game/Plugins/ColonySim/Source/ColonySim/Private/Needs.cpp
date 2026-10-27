#include "ColonySim/Needs.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Genes.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <string>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

float needsFloat(const Def& def, const char* path, float fallback) {
    return static_cast<float>(def.number(path, fallback));
}

// A trait stat for a pawn: `base` times the `statFactors.<stat>` and plus the `statOffsets.<stat>` of each of the
// pawn's trait degrees (Gourmand's hunger factor, Quick sleeper's rest offset).
float needsTraitStat(const DefDatabase& defs, const entt::registry& reg, Entity pawn, const char* stat, float base) {
    const Colonist* colonist = reg.try_get<Colonist>(pawn);
    if (colonist == nullptr) {
        return base;
    }
    float factor = 1.0f;
    float offset = 0.0f;
    for (const TraitRecord& record : colonist->traits) {
        const Def* trait = defs.find("Traits", record.def);
        if (trait == nullptr) {
            continue;
        }
        for (size_t i = 0; i < trait->count("degreeDatas"); ++i) {
            const std::string path = "degreeDatas." + std::to_string(i);
            if (trait->integer((path + ".degree").c_str(), 0) == record.degree) {
                factor *= static_cast<float>(trait->number((path + ".statFactors." + stat).c_str(), 1.0));
                offset += static_cast<float>(trait->number((path + ".statOffsets." + stat).c_str(), 0.0));
            }
        }
    }
    return base * factor + offset;
}

}  // namespace

const char* scheduleKindName(ScheduleKind kind) {
    switch (kind) {
        case ScheduleKind::Anything: return "Anything";
        case ScheduleKind::Work: return "Work";
        case ScheduleKind::Joy: return "Joy";
        case ScheduleKind::Sleep: return "Sleep";
    }
    return "?";
}

NeedRules NeedRules::read(const DefDatabase& defs) {
    NeedRules r;
    const Def* def = defs.find("Rules", "NeedRules");
    if (def == nullptr) {
        return r;
    }
    r.present = true;
    r.foodSeekBelow = needsFloat(*def, "food.seekBelow", r.foodSeekBelow);
    r.foodHungryBelow = needsFloat(*def, "food.hungryBelow", r.foodHungryBelow);
    r.foodRavenousBelow = needsFloat(*def, "food.ravenousBelow", r.foodRavenousBelow);
    r.foodFallFactorFed = needsFloat(*def, "food.fallFactor.Fed", r.foodFallFactorFed);
    r.foodFallFactorHungry = needsFloat(*def, "food.fallFactor.Hungry", r.foodFallFactorHungry);
    r.foodFallFactorRavenous = needsFloat(*def, "food.fallFactor.Ravenous", r.foodFallFactorRavenous);
    r.defaultFoodPerDay = needsFloat(*def, "food.defaultFoodPerDay", r.defaultFoodPerDay);
    r.malnutritionPerDay = needsFloat(*def, "food.malnutritionPerDay", r.malnutritionPerDay);
    r.ingestSeconds = def->number("food.ingestSeconds", r.ingestSeconds);
    r.maxNumToIngestAtOnce = std::max(1, static_cast<int>(def->integer("food.maxNumToIngestAtOnce", r.maxNumToIngestAtOnce)));
    r.eatSpotRadius = needsFloat(*def, "food.eatSpotRadius", r.eatSpotRadius);
    r.restFallPerDayRested = needsFloat(*def, "rest.fallPerDay.Rested", r.restFallPerDayRested);
    r.restFallPerDayDrowsy = needsFloat(*def, "rest.fallPerDay.Drowsy", r.restFallPerDayDrowsy);
    r.restFallPerDayTired = needsFloat(*def, "rest.fallPerDay.Tired", r.restFallPerDayTired);
    r.restFallPerDayExhausted = needsFloat(*def, "rest.fallPerDay.Exhausted", r.restFallPerDayExhausted);
    r.restDrowsyBelow = needsFloat(*def, "rest.drowsyBelow", r.restDrowsyBelow);
    r.restTiredBelow = needsFloat(*def, "rest.tiredBelow", r.restTiredBelow);
    r.restExhaustedBelow = needsFloat(*def, "rest.exhaustedBelow", r.restExhaustedBelow);
    r.restGainPerDay = needsFloat(*def, "rest.gainPerDay", r.restGainPerDay);
    r.groundRestEffectiveness = needsFloat(*def, "rest.groundEffectiveness", r.groundRestEffectiveness);
    r.fallAsleepBelow = needsFloat(*def, "rest.fallAsleepBelow", r.fallAsleepBelow);
    r.wakeAt = needsFloat(*def, "rest.wakeAt", r.wakeAt);
    r.wakeOutsideSleepAt = needsFloat(*def, "rest.wakeOutsideSleepAt", r.wakeOutsideSleepAt);
    r.sleepFromHour = std::clamp(static_cast<int>(def->integer("schedule.sleepFromHour", r.sleepFromHour)), 0, 23);
    r.sleepToHour = std::clamp(static_cast<int>(def->integer("schedule.sleepToHour", r.sleepToHour)), 0, 23);
    if (const Def* joy = defs.find("Rules", "JoyRules"); joy != nullptr && joy->count("joyHours") >= 2) {
        r.joyFromHour = std::clamp(static_cast<int>(joy->numberAt("joyHours", 0, -1)), 0, 23);
        r.joyToHour = std::clamp(static_cast<int>(joy->numberAt("joyHours", 1, -1)), 0, 23);
    }
    return r;
}

Schedule NeedRules::standardSchedule() const {
    Schedule schedule;
    for (int hour = 0; hour < 24; ++hour) {
        const bool sleeps = sleepFromHour <= sleepToHour ? (hour >= sleepFromHour && hour <= sleepToHour)
                                                         : (hour >= sleepFromHour || hour <= sleepToHour);
        const bool joys = joyFromHour >= 0 && (joyFromHour <= joyToHour ? (hour >= joyFromHour && hour <= joyToHour)
                                                                        : (hour >= joyFromHour || hour <= joyToHour));
        schedule.hours[static_cast<size_t>(hour)] = sleeps ? ScheduleKind::Sleep : joys ? ScheduleKind::Joy : ScheduleKind::Anything;
    }
    return schedule;
}

FoodBand foodBandOf(const NeedRules& rules, float food) {
    if (food <= 0.0f) return FoodBand::Malnourished;
    if (food < rules.foodRavenousBelow) return FoodBand::Ravenous;
    if (food < rules.foodHungryBelow) return FoodBand::Hungry;
    return FoodBand::Fed;
}

RestBand restBandOf(const NeedRules& rules, float rest) {
    if (rest < rules.restExhaustedBelow) return RestBand::Exhausted;
    if (rest < rules.restTiredBelow) return RestBand::Tired;
    if (rest < rules.restDrowsyBelow) return RestBand::Drowsy;
    return RestBand::Rested;
}

NeedsSystem::NeedsSystem(Things& things, HealthSystem& health, const DefDatabase& defs)
    : things_(things), health_(health), defs_(defs), rules_(NeedRules::read(defs)) {}

float NeedsSystem::restGainPerDay(Entity bed) const {
    float effectiveness = rules_.groundRestEffectiveness;
    const entt::registry& reg = things_.registry();
    if (bed != kNoEntity && reg.valid(bed) && reg.all_of<Thing>(bed)) {
        effectiveness = static_cast<float>(defs_.get(reg.get<Thing>(bed).def).number("stats.bedRestEffectiveness", 1.0));
    }
    return rules_.restGainPerDay * effectiveness;
}

void NeedsSystem::step() {
    if (!rules_.present) {
        return;
    }
    entt::registry& reg = things_.registry();
    std::vector<Entity> pawns;
    for (const Entity pawn : reg.view<Needs>()) {
        pawns.push_back(pawn);
    }
    std::sort(pawns.begin(), pawns.end());
    constexpr float kPerSecond = 1.0f / static_cast<float>(kSecondsPerDay);
    for (const Entity pawn : pawns) {
        if (health_.tracks(pawn) && reg.get<Health>(pawn).dead) {
            continue;
        }
        Needs& needs = reg.get<Needs>(pawn);

        // Food falls slower the hungrier the pawn is (RimWorld's hunger bands).
        float perDay = rules_.defaultFoodPerDay;
        if (const Thing* thing = reg.try_get<Thing>(pawn)) {
            perDay = static_cast<float>(defs_.get(thing->def).number("race.foodPerDay", perDay));
        }
        if (const PawnGenes* genes = reg.try_get<PawnGenes>(pawn)) {
            perDay *= genes->effects.foodRate;
        }
        perDay *= health_.foodRateFactor(pawn);  // gut worms
        perDay *= needsTraitStat(defs_, reg, pawn, "hungerRateMultiplier", 1.0f);
        switch (foodBandOf(rules_, needs.food)) {
            case FoodBand::Fed: perDay *= rules_.foodFallFactorFed; break;
            case FoodBand::Hungry: perDay *= rules_.foodFallFactorHungry; break;
            case FoodBand::Ravenous: perDay *= rules_.foodFallFactorRavenous; break;
            case FoodBand::Malnourished: perDay = 0.0f; break;
        }
        needs.food = std::max(0.0f, needs.food - perDay * kPerSecond);
        if (needs.food <= 0.0f) {
            needs.malnutrition = std::min(1.0f, needs.malnutrition + rules_.malnutritionPerDay * kPerSecond);
        } else {
            needs.malnutrition = std::max(0.0f, needs.malnutrition - rules_.malnutritionPerDay * kPerSecond);
        }

        // Rest rises while lying down and falls otherwise, by band.
        const Worker* worker = reg.try_get<Worker>(pawn);
        const bool elsewhere = restingElsewhere_ && restingElsewhere_(pawn);
        if ((worker != nullptr && jobIsLyingDown(worker->job)) || elsewhere) {
            // Quick sleeper: `restRateMultiplier` +0.5 rests half as fast again.
            const float restRate = std::max(0.0f, needsTraitStat(defs_, reg, pawn, "restRateMultiplier", 1.0f));
            const Entity bed = elsewhere ? kNoEntity : worker->job.thing;
            needs.rest = std::min(1.0f, needs.rest + restGainPerDay(bed) * restRate * kPerSecond);
        } else {
            float fall = 0.0f;
            switch (restBandOf(rules_, needs.rest)) {
                case RestBand::Rested: fall = rules_.restFallPerDayRested; break;
                case RestBand::Drowsy: fall = rules_.restFallPerDayDrowsy; break;
                case RestBand::Tired: fall = rules_.restFallPerDayTired; break;
                case RestBand::Exhausted: fall = rules_.restFallPerDayExhausted; break;
            }
            if (restFallFactor_) {
                fall *= restFallFactor_(pawn);
            }
            needs.rest = std::max(0.0f, needs.rest - fall * health_.restFallFactor(pawn) * kPerSecond);  // sleeping sickness and drugs
        }

        if (needs.malnutrition >= 1.0f) {
            health_.kill(pawn, DeathCause::Starvation);
        }
    }
}

}  // namespace colony
