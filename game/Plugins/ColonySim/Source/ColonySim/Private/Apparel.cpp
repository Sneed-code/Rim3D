#include "ColonySim/Apparel.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Events.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Looks.h"
#include "ColonySim/Mood.h"
#include "ColonySim/Random.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

float apparelNum(const Def& def, const char* path, float fallback) {
    return static_cast<float>(def.number(path, fallback));
}

const char* const kApparelArmorKeys[3] = {"armorRatingSharp", "armorRatingBlunt", "armorRatingHeat"};

}  // namespace

ApparelRules ApparelRules::read(const DefDatabase& defs) {
    ApparelRules r;
    const Def* def = defs.find("Rules", "ApparelRules");
    if (def == nullptr) {
        return r;
    }
    r.absorbDamageDivisor = std::max(0.01f, apparelNum(*def, "absorbDamageDivisor", r.absorbDamageDivisor));
    r.wearHpLossChancePerDay = apparelNum(*def, "wearHpLossChancePerDay", r.wearHpLossChancePerDay);
    r.wearFactor = std::max(0.0f, apparelNum(*def, "wearFactor", r.wearFactor));
    r.deathHpLossMin = static_cast<float>(def->numberAt("deathHpLoss", 0, r.deathHpLossMin));
    r.deathHpLossMax = static_cast<float>(def->numberAt("deathHpLoss", 1, r.deathHpLossMax));
    r.tatteredBenefitFactor = std::clamp(apparelNum(*def, "tatteredBenefitFactor", r.tatteredBenefitFactor), 0.0f, 1.0f);
    r.taintedStageCap = std::max(1, static_cast<int>(def->integer("taintedStageCap", r.taintedStageCap)));
    r.defaultOutfit = def->text("defaultOutfit", r.defaultOutfit);
    r.wearSteps = std::max(1, static_cast<int>(def->integer("wearSteps", r.wearSteps)));
    r.stripStepsPerPiece = std::max(1, static_cast<int>(def->integer("stripStepsPerPiece", r.stripStepsPerPiece)));
    r.checkEverySteps = std::max(1, static_cast<int>(def->integer("checkEverySteps", r.checkEverySteps)));
    r.scoreBase = apparelNum(*def, "scoreBase", r.scoreBase);
    r.scoreSharp = apparelNum(*def, "scoreSharp", r.scoreSharp);
    r.scoreBlunt = apparelNum(*def, "scoreBlunt", r.scoreBlunt);
    r.scoreHeat = apparelNum(*def, "scoreHeat", r.scoreHeat);
    r.scoreInsulationPerDegree = apparelNum(*def, "scoreInsulationPerDegree", r.scoreInsulationPerDegree);
    r.scoreHpFloor = apparelNum(*def, "scoreHpFloor", r.scoreHpFloor);
    r.taintedScorePenalty = apparelNum(*def, "taintedScorePenalty", r.taintedScorePenalty);
    r.minGain = apparelNum(*def, "minGain", r.minGain);
    r.maxDistance = apparelNum(*def, "maxDistance", r.maxDistance);
    return r;
}

// ---- ApparelStats ---------------------------------------------------------------------------

ApparelStats::ApparelStats(const DefDatabase& defs) {
    apparel_.resize(defs.size());
    stuff_.resize(defs.size());
    if (const Def* rules = defs.find("Rules", "QualityRules")) {
        const std::string defaultLevel = rules->text("defaultLevel", "Normal");
        for (size_t i = 0; i < rules->count("levels"); ++i) {
            const std::string path = "levels." + std::to_string(i);
            qualityArmor_.push_back(apparelNum(*rules, (path + ".armorFactor").c_str(), 1.0f));
            qualityInsulation_.push_back(apparelNum(*rules, (path + ".insulationFactor").c_str(), 1.0f));
            qualityNames_.push_back(rules->text((path + ".label").c_str()));
            if (rules->text((path + ".def").c_str()) == defaultLevel) {
                defaultQuality_ = static_cast<int>(i);
            }
        }
    }
    if (qualityArmor_.empty()) {
        qualityArmor_.push_back(1.0f);
        qualityInsulation_.push_back(1.0f);
        qualityNames_.push_back("normal");
        defaultQuality_ = 0;
    }
    defaultQuality_ = std::clamp(defaultQuality_, 0, static_cast<int>(qualityArmor_.size()) - 1);
    tatteredFactor_ = ApparelRules::read(defs).tatteredBenefitFactor;

    for (const DefId id : defs.idsOfKind("Things")) {
        const Def& def = defs.get(id);
        if (def.has("apparel")) {
            Apparel& a = apparel_[id.value];
            a.valid = true;
            a.stuffRequired = def.flag("stuffRequired");
            a.fragile = def.flag("apparel.fragile");
            for (int t = 0; t < 3; ++t) {
                a.fixedArmor[t] = apparelNum(def, (std::string("stats.") + kApparelArmorKeys[t]).c_str(), 0.0f);
            }
            a.armorMultiplier = apparelNum(def, "apparel.stuffEffectMultiplierArmor", 1.0f);
            a.coldMultiplier = apparelNum(def, "apparel.stuffEffectMultiplierInsulationCold", 0.0f);
            a.heatMultiplier = apparelNum(def, "apparel.stuffEffectMultiplierInsulationHeat", 0.0f);
            a.fixedCold = apparelNum(def, "stats.insulationCold", 0.0f);
            a.fixedHeat = apparelNum(def, "stats.insulationHeat", 0.0f);
            a.maxHitPoints = static_cast<int>(std::lround(def.number("stats.maxHitPoints", 0.0)));
            for (size_t i = 0; i < def.count("apparel.bodyPartGroups"); ++i) {
                a.groups.push_back(def.textAt("apparel.bodyPartGroups", i));
            }
            for (size_t i = 0; i < def.count("apparel.layers"); ++i) {
                a.layers.push_back(def.textAt("apparel.layers", i));
            }
        }
        if (def.has("stuffProps")) {
            Stuff& s = stuff_[id.value];
            s.valid = true;
            for (int t = 0; t < 3; ++t) {
                s.armor[t] = apparelNum(def, (std::string("stuffProps.statFactors.") + kApparelArmorKeys[t]).c_str(), 0.0f);
            }
            s.cold = apparelNum(def, "stuffProps.statOffsets.insulationCold", 0.0f);
            s.heat = apparelNum(def, "stuffProps.statOffsets.insulationHeat", 0.0f);
            s.hitPoints = apparelNum(def, "stuffProps.statFactors.maxHitPoints", 1.0f);
        }
    }
}

const std::vector<std::string>& ApparelStats::groups(DefId def) const {
    return isApparel(def) ? apparel_[def.value].groups : none_;
}

const std::vector<std::string>& ApparelStats::layers(DefId def) const {
    return isApparel(def) ? apparel_[def.value].layers : none_;
}

bool ApparelStats::covers(DefId def, const std::string& group) const {
    const std::vector<std::string>& list = groups(def);
    return std::find(list.begin(), list.end(), group) != list.end();
}

int ApparelStats::qualityOf(const entt::registry& reg, Entity item) const {
    if (const Quality* q = reg.try_get<Quality>(item)) {
        return std::min(static_cast<int>(q->level), qualityLevels() - 1);
    }
    return defaultQuality_;
}

const std::string& ApparelStats::qualityName(int level) const {
    return level >= 0 && level < qualityLevels() ? qualityNames_[static_cast<size_t>(level)] : noName_;
}

float ApparelStats::armor(const entt::registry& reg, Entity item, ArmorType type) const {
    const DefId def = reg.get<Thing>(item).def;
    if (!isApparel(def)) {
        return 0.0f;
    }
    const Apparel& a = apparel_[def.value];
    const int t = static_cast<int>(type);
    float value = a.fixedArmor[t];
    if (a.stuffRequired) {
        const MadeOf* made = reg.try_get<MadeOf>(item);
        if (made == nullptr || !made->stuff.valid() || !stuff_[made->stuff.value].valid) {
            return 0.0f;
        }
        value = stuff_[made->stuff.value].armor[t] * a.armorMultiplier;
    }
    return value * qualityArmor_[static_cast<size_t>(qualityOf(reg, item))] * (tattered(reg, item) ? tatteredFactor_ : 1.0f);
}

float ApparelStats::insulationCold(const entt::registry& reg, Entity item) const {
    const DefId def = reg.get<Thing>(item).def;
    if (!isApparel(def)) {
        return 0.0f;
    }
    const Apparel& a = apparel_[def.value];
    float value = a.fixedCold;
    if (a.stuffRequired) {
        const MadeOf* made = reg.try_get<MadeOf>(item);
        if (made == nullptr || !made->stuff.valid()) {
            return 0.0f;
        }
        value = stuff_[made->stuff.value].cold * a.coldMultiplier;
    }
    return value * qualityInsulation_[static_cast<size_t>(qualityOf(reg, item))] * (tattered(reg, item) ? tatteredFactor_ : 1.0f);
}

float ApparelStats::insulationHeat(const entt::registry& reg, Entity item) const {
    const DefId def = reg.get<Thing>(item).def;
    if (!isApparel(def)) {
        return 0.0f;
    }
    const Apparel& a = apparel_[def.value];
    float value = a.fixedHeat;
    if (a.stuffRequired) {
        const MadeOf* made = reg.try_get<MadeOf>(item);
        if (made == nullptr || !made->stuff.valid()) {
            return 0.0f;
        }
        value = stuff_[made->stuff.value].heat * a.heatMultiplier;
    }
    return value * qualityInsulation_[static_cast<size_t>(qualityOf(reg, item))] * (tattered(reg, item) ? tatteredFactor_ : 1.0f);
}

int ApparelStats::maxHitPoints(DefId def, DefId stuff) const {
    if (!isApparel(def)) {
        return 0;
    }
    float factor = 1.0f;
    if (stuff.valid() && stuff.value < stuff_.size() && stuff_[stuff.value].valid) {
        factor = stuff_[stuff.value].hitPoints;
    }
    return std::max(1, static_cast<int>(std::lround(static_cast<float>(apparel_[def.value].maxHitPoints) * factor)));
}

float ApparelStats::hitPointShare(const entt::registry& reg, Entity item) {
    const HitPoints* hp = reg.try_get<HitPoints>(item);
    return hp == nullptr || hp->max <= 0 ? 1.0f : static_cast<float>(hp->current) / static_cast<float>(hp->max);
}

float ApparelStats::condition(const entt::registry& reg, Entity item) {
    const Durability* durability = reg.try_get<Durability>(item);
    const float own = durability != nullptr ? durability->share : 1.0f;
    return std::clamp(std::min(own, hitPointShare(reg, item)), 0.0f, 1.0f);
}

bool ApparelStats::tattered(const entt::registry& reg, Entity item) {
    return condition(reg, item) <= 0.0f;
}

// ---- ApparelSystem --------------------------------------------------------------------------

ApparelSystem::ApparelSystem(Things& things, CombatSystem& combat, HealthSystem& health, const DefDatabase& defs)
    : things_(things), combat_(combat), health_(health), defs_(defs), rules_(ApparelRules::read(defs)) {
    // Every apparel def, for outfits that name what they allow instead of what they don't.
    std::vector<std::string> apparelDefs;
    for (const DefId id : defs.idsOfKind("Things")) {
        if (combat_.apparelStats().isApparel(id)) {
            apparelDefs.push_back(defs.get(id).defName());
        }
    }
    for (const DefId id : defs.idsOfKind("Outfits")) {
        const Def& def = defs.get(id);
        OutfitPolicy outfit;
        outfit.id = nextOutfitId_++;
        outfit.name = def.label();
        std::set<std::string> blocked;
        if (def.has("allowed")) {
            std::set<std::string> allowedNames;
            for (size_t i = 0; i < def.count("allowed"); ++i) {
                allowedNames.insert(def.textAt("allowed", i));
            }
            for (const std::string& name : apparelDefs) {
                if (allowedNames.count(name) == 0) {
                    blocked.insert(name);
                }
            }
        }
        for (size_t i = 0; i < def.count("disallowed"); ++i) {
            blocked.insert(def.textAt("disallowed", i));
        }
        outfit.disallowed.assign(blocked.begin(), blocked.end());
        outfit.hitPointsMax = static_cast<float>(def.numberAt("hitPoints", 1, 1.0));
        outfit.hitPointsMin = static_cast<float>(def.numberAt("hitPoints", 0, 0.0));
        const int levels = combat_.apparelStats().qualityLevels();
        outfit.qualityMin = static_cast<uint8_t>(std::clamp(static_cast<int>(def.numberAt("quality", 0, 0.0)), 0, levels - 1));
        outfit.qualityMax = static_cast<uint8_t>(std::clamp(static_cast<int>(def.numberAt("quality", 1, 6.0)), 0, levels - 1));
        outfits_.push_back(std::move(outfit));
    }
    if (outfits_.empty()) {
        OutfitPolicy outfit;
        outfit.id = nextOutfitId_++;
        outfit.name = "Anything";
        outfit.qualityMax = static_cast<uint8_t>(combat_.apparelStats().qualityLevels() - 1);
        outfits_.push_back(std::move(outfit));
    }
}

ApparelSystem::~ApparelSystem() = default;

const ApparelStats& ApparelSystem::stats() const {
    return combat_.apparelStats();
}

// ---- Items ----------------------------------------------------------------------------------

bool ApparelSystem::setMadeOf(Entity item, DefId stuff, int quality, std::string* why) {
    entt::registry& reg = things_.registry();
    const auto fail = [why](const char* message) {
        if (why != nullptr) {
            *why = message;
        }
        return false;
    };
    if (!reg.valid(item) || !reg.all_of<Thing>(item) || !stats().isApparel(reg.get<Thing>(item).def)) {
        return fail("that is not apparel");
    }
    if (quality >= stats().qualityLevels()) {
        return fail("there is no such quality");
    }
    if (stuff.valid()) {
        reg.emplace_or_replace<MadeOf>(item, MadeOf{stuff});
    } else {
        reg.remove<MadeOf>(item);
    }
    if (quality >= 0 && quality != stats().defaultQuality()) {
        reg.emplace_or_replace<Quality>(item, Quality{static_cast<uint8_t>(quality)});
    } else {
        reg.remove<Quality>(item);
    }
    const int max = stats().maxHitPoints(reg.get<Thing>(item).def, stuff);
    reg.emplace_or_replace<HitPoints>(item, HitPoints{max, max});
    return true;
}

Entity ApparelSystem::make(DefId def, DefId stuff, int quality, CellPos cell, std::string* why) {
    const Entity item = things_.make(def, 1);
    if (item == kNoEntity) {
        if (why != nullptr) {
            *why = "that is not an item def";
        }
        return kNoEntity;
    }
    if (!setMadeOf(item, stuff, quality, why)) {
        things_.destroy(item);
        return kNoEntity;
    }
    if (!things_.spawnAt(item, cell)) {
        things_.destroy(item);
        if (why != nullptr) {
            *why = "it can't be put there";
        }
        return kNoEntity;
    }
    return item;
}

// ---- Outfits --------------------------------------------------------------------------------

const OutfitPolicy* ApparelSystem::outfit(uint32_t id) const {
    for (const OutfitPolicy& o : outfits_) {
        if (o.id == id) {
            return &o;
        }
    }
    return nullptr;
}

uint32_t ApparelSystem::outfitOf(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    if (reg.valid(pawn)) {
        if (const PawnOutfit* own = reg.try_get<PawnOutfit>(pawn); own != nullptr && outfit(own->outfit) != nullptr) {
            return own->outfit;
        }
    }
    for (const OutfitPolicy& o : outfits_) {
        if (o.name == rules_.defaultOutfit) {
            return o.id;
        }
    }
    return outfits_.front().id;
}

bool ApparelSystem::setOutfit(Entity pawn, uint32_t outfitId, std::string& error) {
    entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || !reg.all_of<Thing, Mover>(pawn) || reg.get<Thing>(pawn).category != ThingCategory::Pawn) {
        error = "that is not a pawn";
        return false;
    }
    if (outfit(outfitId) == nullptr) {
        error = "there is no such outfit";
        return false;
    }
    PawnOutfit& own = reg.get_or_emplace<PawnOutfit>(pawn);
    own.outfit = outfitId;
    own.nextCheck = 0;
    return true;
}

bool ApparelSystem::editOutfit(uint32_t outfitId, const std::vector<std::string>& disallowed, float hitPointsMin,
                               float hitPointsMax, int qualityMin, int qualityMax, std::string& error) {
    OutfitPolicy* target = nullptr;
    for (OutfitPolicy& o : outfits_) {
        if (o.id == outfitId) {
            target = &o;
        }
    }
    if (target == nullptr) {
        error = "there is no such outfit";
        return false;
    }
    const int top = stats().qualityLevels() - 1;
    if (!(hitPointsMin >= 0.0f) || !(hitPointsMax <= 1.0f) || hitPointsMin > hitPointsMax) {
        error = "the hit point range is a share from 0 to 1, lowest first";
        return false;
    }
    if (qualityMin < 0 || qualityMax > top || qualityMin > qualityMax) {
        error = "the quality range is from 0 to " + std::to_string(top) + ", lowest first";
        return false;
    }
    for (const std::string& name : disallowed) {
        const Def* def = defs_.find("Things", name);
        if (def == nullptr || !def->has("apparel")) {
            error = "'" + name + "' is not an apparel def";
            return false;
        }
    }
    std::set<std::string> blocked(disallowed.begin(), disallowed.end());
    target->disallowed.assign(blocked.begin(), blocked.end());
    target->hitPointsMin = hitPointsMin;
    target->hitPointsMax = hitPointsMax;
    target->qualityMin = static_cast<uint8_t>(qualityMin);
    target->qualityMax = static_cast<uint8_t>(qualityMax);
    return true;
}

uint32_t ApparelSystem::addOutfit(const std::string& name, std::string& error) {
    if (name.empty() || name.size() > 40) {
        error = "an outfit's name is 1 to 40 letters";
        return 0;
    }
    if (outfits_.size() >= 64) {
        error = "there are too many outfits";
        return 0;
    }
    OutfitPolicy outfit;
    outfit.id = nextOutfitId_++;
    outfit.name = name;
    outfit.qualityMax = static_cast<uint8_t>(stats().qualityLevels() - 1);
    outfits_.push_back(std::move(outfit));
    return outfits_.back().id;
}

bool ApparelSystem::removeOutfit(uint32_t outfitId, std::string& error) {
    if (outfitId == outfits_.front().id) {
        error = "the first outfit can't be removed";
        return false;
    }
    const auto it = std::find_if(outfits_.begin(), outfits_.end(), [&](const OutfitPolicy& o) { return o.id == outfitId; });
    if (it == outfits_.end()) {
        error = "there is no such outfit";
        return false;
    }
    outfits_.erase(it);
    return true;
}

void ApparelSystem::restoreOutfits(std::vector<OutfitPolicy> outfits, uint32_t nextId) {
    if (!outfits.empty()) {
        outfits_ = std::move(outfits);
        nextOutfitId_ = nextId;
    }
}

bool ApparelSystem::allowed(const OutfitPolicy& policy, Entity item) const {
    const entt::registry& reg = things_.registry();
    const DefId def = reg.get<Thing>(item).def;
    if (std::binary_search(policy.disallowed.begin(), policy.disallowed.end(), defs_.get(def).defName())) {
        return false;
    }
    const float share = ApparelStats::condition(reg, item);
    if (share < policy.hitPointsMin - 1e-4f || share > policy.hitPointsMax + 1e-4f) {
        return false;
    }
    const int quality = stats().qualityOf(reg, item);
    return quality >= policy.qualityMin && quality <= policy.qualityMax;
}

// ---- Wearing --------------------------------------------------------------------------------

const std::vector<Entity>& ApparelSystem::worn(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Gear* gear = reg.valid(pawn) ? reg.try_get<Gear>(pawn) : nullptr;
    return gear != nullptr ? gear->apparel : none_;
}

bool ApparelSystem::covered(Entity pawn, const std::string& group) const {
    const entt::registry& reg = things_.registry();
    for (const Entity piece : worn(pawn)) {
        if (stats().covers(reg.get<Thing>(piece).def, group)) {
            return true;
        }
    }
    return false;
}

void ApparelSystem::insulation(Entity pawn, float& cold, float& heat) const {
    const entt::registry& reg = things_.registry();
    cold = 0.0f;
    heat = 0.0f;
    for (const Entity piece : worn(pawn)) {
        cold += stats().insulationCold(reg, piece);
        heat += stats().insulationHeat(reg, piece);
    }
}

float ApparelSystem::worstCondition(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    float worst = 1.0f;
    for (const Entity piece : worn(pawn)) {
        worst = std::min(worst, ApparelStats::condition(reg, piece));
    }
    return worst;
}

void ApparelSystem::wearDown(Entity item, float share) {
    entt::registry& reg = things_.registry();
    if (!reg.valid(item) || share <= 0.0f) {
        return;
    }
    const float now = ApparelStats::condition(reg, item);
    reg.emplace_or_replace<Durability>(item, Durability{std::max(0.0f, now - share)});
}

float ApparelSystem::score(Entity item) const {
    const entt::registry& reg = things_.registry();
    float value = rules_.scoreBase + rules_.scoreSharp * stats().armor(reg, item, ArmorType::Sharp) +
                  rules_.scoreBlunt * stats().armor(reg, item, ArmorType::Blunt) +
                  rules_.scoreHeat * stats().armor(reg, item, ArmorType::Heat) +
                  rules_.scoreInsulationPerDegree * (stats().insulationCold(reg, item) + stats().insulationHeat(reg, item));
    value *= rules_.scoreHpFloor + (1.0f - rules_.scoreHpFloor) * ApparelStats::condition(reg, item);
    if (reg.all_of<Tainted>(item)) {
        value -= rules_.taintedScorePenalty;
    }
    return value;
}

bool ApparelSystem::conflicts(Entity item, Entity wornItem) const {
    const entt::registry& reg = things_.registry();
    const DefId a = reg.get<Thing>(item).def;
    const DefId b = reg.get<Thing>(wornItem).def;
    bool sharedLayer = false;
    for (const std::string& layer : stats().layers(a)) {
        const std::vector<std::string>& others = stats().layers(b);
        sharedLayer = sharedLayer || std::find(others.begin(), others.end(), layer) != others.end();
    }
    if (!sharedLayer) {
        return false;
    }
    for (const std::string& group : stats().groups(a)) {
        if (stats().covers(b, group)) {
            return true;
        }
    }
    return false;
}

Entity ApparelSystem::wornToRemove(Entity pawn) const {
    const OutfitPolicy* policy = outfit(outfitOf(pawn));
    if (policy == nullptr) {
        return kNoEntity;
    }
    for (const Entity piece : worn(pawn)) {
        if (!allowed(*policy, piece)) {
            return piece;
        }
    }
    return kNoEntity;
}

Entity ApparelSystem::bestToWear(Entity pawn, const std::function<bool(Entity)>& usable) const {
    const entt::registry& reg = things_.registry();
    const OutfitPolicy* policy = outfit(outfitOf(pawn));
    const Position* at = reg.try_get<Position>(pawn);
    if (policy == nullptr || at == nullptr) {
        return kNoEntity;
    }
    const std::vector<Entity>& wearing = worn(pawn);
    const float maxDistSq = rules_.maxDistance * rules_.maxDistance;
    Entity best = kNoEntity;
    float bestGain = 0.0f;
    float bestDist = 0.0f;
    for (const auto [item, thing, pos] : reg.view<const Thing, const Position>().each()) {
        if (thing.category != ThingCategory::Item || !stats().isApparel(thing.def) || pos.level != at->level) {
            continue;
        }
        const float dx = pos.x - at->x;
        const float dy = pos.y - at->y;
        const float dist = dx * dx + dy * dy;
        if (dist > maxDistSq || !allowed(*policy, item)) {
            continue;
        }
        float gain = score(item);
        for (const Entity piece : wearing) {
            if (conflicts(item, piece)) {
                gain -= score(piece);
            }
        }
        if (gain < rules_.minGain) {
            continue;
        }
        const bool better =
            best == kNoEntity || gain > bestGain || (gain == bestGain && (dist < bestDist || (dist == bestDist && item < best)));
        if (!better) {
            continue;
        }
        if (usable && !usable(item)) {
            continue;
        }
        best = item;
        bestGain = gain;
        bestDist = dist;
    }
    return best;
}

bool ApparelSystem::putOn(Entity pawn, Entity item, std::string& error) {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(item) || !reg.all_of<Thing>(item) || !stats().isApparel(reg.get<Thing>(item).def)) {
        error = "that is not apparel";
        return false;
    }
    return combat_.equip(pawn, item, &error);
}

bool ApparelSystem::takeOff(Entity pawn, Entity item, std::string& error) {
    if (!combat_.unequip(pawn, item)) {
        error = "the pawn isn't wearing that";
        return false;
    }
    return true;
}

bool ApparelSystem::hasGear(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Gear* gear = reg.valid(pawn) ? reg.try_get<Gear>(pawn) : nullptr;
    return gear != nullptr && (gear->weapon != kNoEntity || !gear->apparel.empty() || !gear->stowed.empty());
}

bool ApparelSystem::strip(Entity pawn) {
    if (!hasGear(pawn)) {
        return false;
    }
    combat_.dropGear(pawn);
    return true;
}

// ---- Strip marks ----------------------------------------------------------------------------

bool ApparelSystem::stripMarked(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    return reg.valid(pawn) && reg.all_of<StripMark>(pawn);
}

bool ApparelSystem::markStrip(Entity target, bool on, std::string& error) {
    entt::registry& reg = things_.registry();
    if (!reg.valid(target) || !reg.all_of<Thing, Position>(target) || reg.get<Thing>(target).category != ThingCategory::Pawn ||
        !health_.tracks(target)) {
        error = "that is not a pawn on the map";
        return false;
    }
    if (!on) {
        reg.remove<StripMark>(target);
        return true;
    }
    if (!health_.incapacitated(target)) {
        error = "only a downed or dead pawn can be stripped";
        return false;
    }
    if (!hasGear(target)) {
        error = "it has nothing on it to take";
        return false;
    }
    reg.emplace_or_replace<StripMark>(target);
    return true;
}

// ---- Thoughts -------------------------------------------------------------------------------

bool ApparelSystem::hasTrait(Entity pawn, const char* trait) const {
    const Colonist* colonist = things_.registry().try_get<Colonist>(pawn);
    return colonist != nullptr && std::any_of(colonist->traits.begin(), colonist->traits.end(),
                                              [&](const TraitRecord& record) { return record.def == trait; });
}

bool ApparelSystem::isFemale(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    if (const Colonist* colonist = reg.try_get<Colonist>(pawn)) {
        return colonist->female;
    }
    const PawnLooks* looks = reg.try_get<PawnLooks>(pawn);
    return looks != nullptr && looks->female;
}

// RimWorld's "psychologically nude": a man with his legs uncovered, a woman with her legs or chest.
bool ApparelSystem::naked(Entity pawn) const {
    return !covered(pawn, "Legs") || (isFemale(pawn) && !covered(pawn, "Torso"));
}

bool ApparelSystem::nudistClothed(Entity pawn) const {
    return covered(pawn, "Torso") || covered(pawn, "Legs");
}

int ApparelSystem::taintedWorn(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    int count = 0;
    for (const Entity piece : worn(pawn)) {
        count += reg.all_of<Tainted>(piece) ? 1 : 0;
    }
    return count;
}

int ApparelSystem::humanLeatherWorn(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    int count = 0;
    for (const Entity piece : worn(pawn)) {
        const MadeOf* made = reg.try_get<MadeOf>(piece);
        count += made != nullptr && made->stuff.valid() && defs_.get(made->stuff).flag("humanlike") ? 1 : 0;
    }
    return count;
}

void ApparelSystem::registerSituations(MoodSystem& mood) const {
    mood.addSituation("noApparel", [this](Entity pawn) { return naked(pawn) ? 0 : -1; });
    mood.addSituation("apparelOnNudist", [this](Entity pawn) { return nudistClothed(pawn) ? 0 : -1; });
    mood.addSituation("apparelTattered", [this](Entity pawn) {
        if (worn(pawn).empty()) {
            return -1;
        }
        return worstCondition(pawn) <= 0.0f ? 0 : -1;
    });
    mood.addSituation("apparelTainted", [this](Entity pawn) {
        const int count = taintedWorn(pawn);
        return count <= 0 ? -1 : std::min(count, rules_.taintedStageCap) - 1;
    });
    mood.addSituation("apparelHumanLeather", [this](Entity pawn) { return humanLeatherWorn(pawn) > 0 ? 0 : -1; });
}

// ---- Stepping -------------------------------------------------------------------------------

void ApparelSystem::step(int64_t now, Rng& rng, const std::vector<Event>& events) {
    entt::registry& reg = things_.registry();

    // The dead: what they wore is worn down (not its hit points), and what a human wore is tainted for good. Collected
    // first, because damaging apparel adds events to the very list being read.
    std::vector<Entity> dead;
    for (const Event& event : events) {
        if (event.type == EventType::PawnDied && reg.valid(event.thing) && reg.all_of<Gear>(event.thing)) {
            dead.push_back(event.thing);
        }
    }
    for (const Entity pawn : dead) {
        const bool human = reg.all_of<PawnLooks>(pawn) || reg.all_of<Colonist>(pawn);
        const std::vector<Entity> pieces = worn(pawn);
        for (const Entity piece : pieces) {
            wearDown(piece, ApparelStats::condition(reg, piece) * rng.range(rules_.deathHpLossMin, rules_.deathHpLossMax));
            if (human && reg.valid(piece) && reg.all_of<EquippedBy>(piece)) {
                reg.emplace_or_replace<Tainted>(piece);
            }
        }
    }

    // Wear and tear: once an hour each worn piece wears a 24th of the day's hit points' worth of durability
    // (RimWorld's 0.4 hit points a day as a steady rate: a 100 hit point shirt wears 24% in a 60-day year). Never
    // hit points, so normal use can't destroy clothes. Pawns in id order.
    if (now % kSecondsPerHour == 0) {
        scratch_.clear();
        for (const Entity pawn : reg.view<Gear, Position>()) {
            scratch_.push_back(pawn);
        }
        std::sort(scratch_.begin(), scratch_.end());
        const float perHour = rules_.wearHpLossChancePerDay * rules_.wearFactor / 24.0f;
        for (const Entity pawn : scratch_) {
            if (health_.tracks(pawn) && reg.get<Health>(pawn).dead) {
                continue;
            }
            for (const Entity piece : worn(pawn)) {
                const HitPoints* hp = reg.try_get<HitPoints>(piece);
                if (hp != nullptr && hp->max > 0) {
                    wearDown(piece, perHour / static_cast<float>(hp->max));
                }
            }
        }
    }

    // Strip marks that no longer make sense go: the pawn got up, or has nothing left to take.
    scratch_.clear();
    for (const Entity pawn : reg.view<StripMark>()) {
        scratch_.push_back(pawn);
    }
    for (const Entity pawn : scratch_) {
        if (!reg.valid(pawn) || !health_.incapacitated(pawn) || !hasGear(pawn)) {
            reg.remove<StripMark>(pawn);
        }
    }
}

// ---- Hash -----------------------------------------------------------------------------------

void ApparelSystem::hashThing(Entity e, StateHasher& h) const {
    const entt::registry& reg = things_.registry();
    const MadeOf* made = reg.try_get<MadeOf>(e);
    const Quality* quality = reg.try_get<Quality>(e);
    const PawnOutfit* outfitOfPawn = reg.try_get<PawnOutfit>(e);
    const Durability* durability = reg.try_get<Durability>(e);
    const uint64_t mask = (made != nullptr ? 1u : 0u) | (quality != nullptr ? 2u : 0u) | (reg.all_of<Tainted>(e) ? 4u : 0u) |
                          (reg.all_of<StripMark>(e) ? 8u : 0u) | (outfitOfPawn != nullptr ? 16u : 0u) |
                          (durability != nullptr ? 32u : 0u);
    if (mask == 0) {
        return;  // most things have none of these: their hash stays what it was
    }
    h.add(mask);
    if (made != nullptr) {
        h.add(made->stuff.value);
    }
    if (quality != nullptr) {
        h.add(static_cast<int>(quality->level));
    }
    if (outfitOfPawn != nullptr) {
        h.add(outfitOfPawn->outfit);
        h.add(outfitOfPawn->nextCheck);
    }
    if (durability != nullptr) {
        h.add(durability->share);
    }
}

void ApparelSystem::hashState(StateHasher& h) const {
    h.add(nextOutfitId_);
    h.add(static_cast<uint64_t>(outfits_.size()));
    for (const OutfitPolicy& o : outfits_) {
        h.add(o.id);
        h.add(o.name.data(), o.name.size());
        h.add(static_cast<uint64_t>(o.disallowed.size()));
        for (const std::string& name : o.disallowed) {
            h.add(name.data(), name.size());
        }
        h.add(o.hitPointsMin);
        h.add(o.hitPointsMax);
        h.add(static_cast<int>(o.qualityMin));
        h.add(static_cast<int>(o.qualityMax));
    }
}

}  // namespace colony
