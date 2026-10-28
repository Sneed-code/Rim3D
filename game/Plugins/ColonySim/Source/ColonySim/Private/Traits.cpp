#include "ColonySim/Traits.h"

#include "ColonySim/Colonist.h"

#include <algorithm>
#include <string>

namespace colony {

namespace {
const TraitStats kNeutralTraitStats;
}  // namespace

const TraitStats& TraitBook::of(Entity pawn) const {
    const auto found = cache_.find(pawn);
    if (found != cache_.end()) {
        return found->second;
    }
    const Colonist* colonist = reg_.valid(pawn) ? reg_.try_get<Colonist>(pawn) : nullptr;
    if (colonist == nullptr || colonist->traits.empty()) {
        return kNeutralTraitStats;
    }
    float workSpeed = 0.0f;
    float learning = 0.0f;
    TraitStats stats;
    for (const TraitRecord& record : colonist->traits) {
        const Def* trait = defs_.find("Traits", record.def);
        if (trait == nullptr) {
            continue;
        }
        for (size_t i = 0; i < trait->count("degreeDatas"); ++i) {
            const std::string base = "degreeDatas." + std::to_string(i);
            if (trait->integer((base + ".degree").c_str(), 0) != record.degree) {
                continue;
            }
            const auto offset = [&](const char* stat) { return static_cast<float>(trait->number((base + ".statOffsets." + stat).c_str(), 0.0)); };
            const auto factor = [&](const char* stat) { return static_cast<float>(trait->number((base + ".statFactors." + stat).c_str(), 1.0)); };
            workSpeed += offset("workSpeedGlobal");
            learning += offset("globalLearningFactor");
            stats.moveSpeedStep += offset("moveSpeed");
            stats.shootingSkill += offset("shootingAccuracyPawn");
            stats.aimDelayFactor *= factor("aimingDelayFactor");
            stats.damageFactor *= factor("incomingDamageFactor");
            stats.painShockOffset += offset("painShockThreshold");
            stats.skillLossFactor *= static_cast<float>(trait->number((base + ".skillLossFactor").c_str(), 1.0));
        }
    }
    stats.workSpeed = std::max(0.0f, 1.0f + workSpeed);
    stats.learningFactor = std::max(0.0f, 1.0f + learning);
    return cache_.emplace(pawn, stats).first->second;
}

bool TraitBook::has(Entity pawn, const char* trait) const {
    const Colonist* colonist = reg_.valid(pawn) ? reg_.try_get<Colonist>(pawn) : nullptr;
    return colonist != nullptr && std::any_of(colonist->traits.begin(), colonist->traits.end(),
                                              [&](const TraitRecord& record) { return record.def == trait; });
}

}  // namespace colony
