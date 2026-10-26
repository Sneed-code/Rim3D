#include "ColonySim/Crafting.h"

#include "ColonySim/Apparel.h"
#include "ColonySim/Bills.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Things.h"
#include "ColonySim/Zones.h"

#include <algorithm>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

constexpr int kCraftingMaxSkill = 20;

}  // namespace

CraftingSystem::CraftingSystem(Things& things, ApparelSystem& apparel, const Zones& zones, const DefDatabase& defs,
                               uint64_t seed)
    : things_(things), apparel_(apparel), zones_(zones), defs_(defs), rng_(splitMix64(seed ^ 0x43524654ULL)) {
    if (const Def* rules = defs_.find("Rules", "CraftingRules")) {
        for (size_t skill = 0; skill < rules->count("qualityChances"); ++skill) {
            std::vector<double> row;
            const std::string base = "qualityChances." + std::to_string(skill);
            for (size_t q = 0; q < rules->count(base.c_str()); ++q) {
                row.push_back(rules->numberAt(base.c_str(), q));
            }
            chances_.push_back(std::move(row));
        }
    }
    if (chances_.empty()) {
        chances_.push_back({0.0, 0.0, 1.0, 0.0, 0.0, 0.0});  // no table: everything is normal
    }
}

double CraftingSystem::qualityChance(int skillLevel, int quality) const {
    const std::vector<double>& row = chances_[static_cast<size_t>(std::clamp(skillLevel, 0, static_cast<int>(chances_.size()) - 1))];
    return quality >= 0 && quality < static_cast<int>(row.size()) ? row[static_cast<size_t>(quality)] : 0.0;
}

int CraftingSystem::rollQuality(int skillLevel, int shift) {
    const std::vector<double>& row = chances_[static_cast<size_t>(std::clamp(skillLevel, 0, std::min(kCraftingMaxSkill, static_cast<int>(chances_.size()) - 1)))];
    const double roll = static_cast<double>(rng_.unit());
    double edge = 0.0;
    int level = static_cast<int>(row.size()) - 1;
    for (size_t q = 0; q < row.size(); ++q) {
        edge += row[q];
        if (roll < edge) {
            level = static_cast<int>(q);
            break;
        }
    }
    return std::clamp(level + shift, 0, kQualityLevels - 1);
}

bool CraftingSystem::hasQuality(DefId item) const {
    if (!item.valid()) {
        return false;
    }
    const Def& def = defs_.get(item);
    return apparel_.stats().isApparel(item) || def.flag("hasQuality") || def.count("tools") > 0;
}

bool CraftingSystem::takesStuff(DefId item) const {
    return item.valid() && defs_.get(item).count("stuffCategories") > 0;
}

DefId CraftingSystem::dominantStuff(DefId product, const std::vector<BillUsed>& used) const {
    if (!takesStuff(product)) {
        return DefId{};
    }
    const Def& productDef = defs_.get(product);
    DefId best;
    int bestCount = 0;
    for (const BillUsed& ingredient : used) {
        const Def& def = defs_.get(ingredient.def);
        bool stuff = false;
        for (size_t c = 0; c < productDef.count("stuffCategories") && !stuff; ++c) {
            for (size_t k = 0; k < def.count("stuffProps.categories") && !stuff; ++k) {
                stuff = def.textAt("stuffProps.categories", k) == productDef.textAt("stuffCategories", c);
            }
        }
        if (stuff && (ingredient.count > bestCount || (ingredient.count == bestCount && ingredient.def.value < best.value))) {
            best = ingredient.def;
            bestCount = ingredient.count;
        }
    }
    return best;
}

float CraftingSystem::stuffWorkFactor(DefId stuff) const {
    return stuff.valid() ? static_cast<float>(defs_.get(stuff).number("stuffProps.statFactors.workToMake", 1.0)) : 1.0f;
}

float CraftingSystem::benchSpeed(DefId buildingDef) const {
    return buildingDef.valid() ? static_cast<float>(defs_.get(buildingDef).number("stats.workTableWorkSpeedFactor", 1.0)) : 1.0f;
}

void CraftingSystem::finishProduct(Entity item, const Recipe& recipe, int skillLevel, const std::vector<BillUsed>& used,
                                   uint32_t storeZone) {
    entt::registry& reg = things_.registry();
    const DefId def = reg.get<Thing>(item).def;
    const DefId stuff = recipe.stuffFromIngredient ? dominantStuff(def, used) : DefId{};
    if (hasQuality(def)) {
        const int quality = rollQuality(skillLevel);
        if (apparel_.stats().isApparel(def)) {
            apparel_.setMadeOf(item, stuff, quality, nullptr);
        } else {
            if (stuff.valid()) {
                reg.emplace_or_replace<MadeOf>(item, MadeOf{stuff});
            }
            if (quality != apparel_.stats().defaultQuality()) {
                reg.emplace_or_replace<Quality>(item, Quality{static_cast<uint8_t>(quality)});
            }
        }
    } else if (stuff.valid()) {
        if (apparel_.stats().isApparel(def)) {
            apparel_.setMadeOf(item, stuff, -1, nullptr);
        } else {
            reg.emplace_or_replace<MadeOf>(item, MadeOf{stuff});
        }
    }
    if (storeZone != 0) {
        reg.emplace_or_replace<StoreIn>(item, StoreIn{storeZone});
    }
}

void CraftingSystem::step() {
    entt::registry& reg = things_.registry();
    std::vector<Entity> done;
    for (const auto [item, store] : reg.view<const StoreIn>().each()) {
        const Position* pos = reg.try_get<Position>(item);
        const StockpileZone* zone = zones_.find(static_cast<ZoneId>(store.zone));
        if (zone == nullptr || (pos != nullptr && zones_.zoneAt(cellOf(*pos)) == zone->id)) {
            done.push_back(item);
        }
    }
    for (const Entity item : done) {
        reg.remove<StoreIn>(item);
    }
}

void CraftingSystem::hashState(StateHasher& hasher) const {
    hasher.add(rng_.state());
    hasher.add(rng_.increment());
}

}  // namespace colony
