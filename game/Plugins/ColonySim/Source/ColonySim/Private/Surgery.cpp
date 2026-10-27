#include "ColonySim/Surgery.h"

#include "ColonySim/Hash.h"
#include "ColonySim/Health.h"
#include "ColonySim/Medicine.h"
#include "ColonySim/Research.h"
#include "ColonySim/Things.h"

#include <algorithm>

namespace colony {

namespace {

constexpr const char* kSurgeryRecipesKind = "Recipes";

// Reads a table of [x, y] pairs from a rules def, sorted by x; the fallback if the def doesn't have it.
void surgeryReadTable(const Def& rules, const char* path, std::vector<std::pair<float, float>>& table) {
    const size_t n = rules.count(path);
    if (n == 0) {
        return;
    }
    table.clear();
    for (size_t i = 0; i < n; ++i) {
        const std::string at = std::string(path) + "." + std::to_string(i);
        table.emplace_back(static_cast<float>(rules.number((at + ".0").c_str())), static_cast<float>(rules.number((at + ".1").c_str())));
    }
    std::sort(table.begin(), table.end());
}

// A value read in between the points of a table.
float surgeryInterpolate(const std::vector<std::pair<float, float>>& table, float x) {
    if (table.empty()) {
        return 1.0f;
    }
    if (x <= table.front().first) {
        return table.front().second;
    }
    for (size_t i = 1; i < table.size(); ++i) {
        if (x <= table[i].first) {
            const float span = std::max(1.0e-6f, table[i].first - table[i - 1].first);
            return table[i - 1].second + (table[i].second - table[i - 1].second) * (x - table[i - 1].first) / span;
        }
    }
    return table.back().second;
}

}  // namespace

SurgeryNumbers SurgeryNumbers::read(const DefDatabase& defs) {
    SurgeryNumbers n;
    const Def* rules = defs.find("Rules", "MedicineRules");
    if (rules == nullptr) {
        return n;
    }
    surgeryReadTable(*rules, "surgerySuccessBySkill", n.successBySkill);
    surgeryReadTable(*rules, "surgeryCleanlinessFactor", n.cleanlinessFactor);
    n.noMedicineFactor = static_cast<float>(rules->number("surgeryNoMedicineFactor", n.noMedicineFactor));
    n.medicinePotencyWeight = static_cast<float>(rules->number("surgeryMedicinePotencyWeight", n.medicinePotencyWeight));
    n.maxSuccessChance = static_cast<float>(rules->number("surgeryMaxSuccessChance", n.maxSuccessChance));
    n.failDeathChanceFactor = static_cast<float>(rules->number("surgeryFailDeathChanceFactor", n.failDeathChanceFactor));
    n.botchHpFraction = static_cast<float>(rules->number("surgeryBotchHpFraction", n.botchHpFraction));
    n.xp = static_cast<float>(rules->number("surgeryXp", n.xp));
    n.maxOperations = std::max(1, static_cast<int>(rules->integer("surgeryMaxOperations", n.maxOperations)));
    return n;
}

SurgerySystem::SurgerySystem(Things& things, const DefDatabase& defs, HealthSystem& health, const ResearchSystem* research)
    : things_(things), defs_(defs), health_(health), research_(research), numbers_(SurgeryNumbers::read(defs)) {
    index_.assign(defs.size(), -1);
    for (const DefId id : defs.idsOfKind(kSurgeryRecipesKind)) {
        const Def& def = defs.get(id);
        if (!def.has("surgery")) {
            continue;
        }
        SurgeryRecipe r;
        r.def = id;
        r.defName = def.defName();
        r.label = def.label();
        r.minSkill = static_cast<int>(def.integer("minSkill", 0));
        r.work = static_cast<float>(def.number("workAmount", 0.0));
        r.xp = static_cast<float>(def.number("workSkillXp", 0.0));
        r.successChance = static_cast<float>(def.number("surgery.successChance", 1.0));
        r.needsBed = def.flag("surgery.needsBed", true);
        r.requiresMissing = def.flag("surgery.requiresMissing");
        r.removes = def.flag("surgery.removes");
        r.limbsOnly = def.flag("surgery.limbsOnly");
        r.harvests = def.flag("surgery.harvests");
        r.partDef = def.text("surgery.part");
        r.adds = defs.idOf("Hediffs", def.text("surgery.addsHediff"));
        r.ingredient = defs.idOf("Things", def.text("surgery.ingredient"));
        r.install = r.adds.valid();
        index_[id.value] = static_cast<int32_t>(recipes_.size());
        recipes_.push_back(std::move(r));
    }
}

const SurgeryRecipe* SurgerySystem::recipe(DefId def) const {
    if (!def.valid() || def.value >= index_.size() || index_[def.value] < 0) {
        return nullptr;
    }
    return &recipes_[static_cast<size_t>(index_[def.value])];
}

const SurgeryRecipe* SurgerySystem::recipe(const std::string& defName) const {
    return recipe(defs_.idOf(kSurgeryRecipesKind, defName));
}

DefId SurgerySystem::organItem(Entity patient, int part) const {
    return defs_.idOf("Things", "Organ" + health_.partDef(patient, part));
}

bool SurgerySystem::eligible(Entity patient, const SurgeryRecipe& recipe, int part, std::string& why) const {
    const entt::registry& reg = things_.registry();
    if (!health_.tracks(patient) || reg.get<Health>(patient).dead) {
        why = "that has no living body to operate on";
        return false;
    }
    if (research_ != nullptr) {
        std::string missing;
        if (!research_->unlocked(defs_.get(recipe.def), missing)) {
            why = missing;
            return false;
        }
    }
    bool exists = false;
    const PartCondition condition = health_.partCondition(patient, part, &exists);
    if (!exists) {
        why = "the body has no such part";
        return false;
    }
    const DefId fitted = health_.addedAt(patient, part);
    if (recipe.install) {
        if (health_.partDef(patient, part) != recipe.partDef) {
            why = "that is the wrong part for this operation";
        } else if (fitted.valid()) {
            why = "there is an artificial part there already";
        } else if (condition == PartCondition::Under || condition == PartCondition::Covered) {
            why = "the part above it is gone";
        } else if (recipe.requiresMissing && condition != PartCondition::Missing) {
            why = "the part must be gone first";
        } else {
            return true;
        }
        return false;
    }
    if (recipe.removes) {
        if (recipe.harvests) {
            if (condition != PartCondition::Whole || !organItem(patient, part).valid()) {
                why = "there is no organ to harvest there";
            } else if (!health_.canRemovePart(patient, part)) {
                why = "the patient could not live without it";
            } else {
                return true;
            }
            return false;
        }
        if (condition == PartCondition::Fitted) {
            return true;  // the artificial part comes off
        }
        if (condition != PartCondition::Whole) {
            why = "there is nothing to take off there";
        } else if (!health_.canRemovePart(patient, part)) {
            why = "the patient could not live without it";
        } else if (recipe.limbsOnly && !health_.partIsLimb(patient, part)) {
            why = "only limbs can be amputated";
        } else {
            return true;
        }
        return false;
    }
    why = "nothing to do";
    return false;
}

std::vector<int> SurgerySystem::targets(Entity patient, const SurgeryRecipe& recipe) const {
    std::vector<int> out;
    if (!health_.tracks(patient)) {
        return out;
    }
    const int parts = health_.partCount(things_.registry().get<Thing>(patient).def);
    std::string why;
    for (int part = 0; part < parts; ++part) {
        if (eligible(patient, recipe, part, why)) {
            out.push_back(part);
        }
    }
    return out;
}

uint32_t SurgerySystem::add(Entity patient, const std::string& recipeName, int part, std::string& error) {
    if (!things_.exists(patient) || !health_.tracks(patient)) {
        error = "that has no body to operate on";
        return 0;
    }
    const SurgeryRecipe* r = recipe(recipeName);
    if (r == nullptr) {
        error = "no operation '" + recipeName + "'";
        return 0;
    }
    if (!eligible(patient, *r, part, error)) {
        return 0;
    }
    entt::registry& reg = things_.registry();
    Operations& ops = reg.get_or_emplace<Operations>(patient);
    if (static_cast<int>(ops.list.size()) >= numbers_.maxOperations) {
        error = "that patient has too many operations planned";
        return 0;
    }
    for (const Operation& op : ops.list) {
        if (op.recipe == r->def && op.part == part) {
            error = "that operation is planned already";
            return 0;
        }
    }
    Operation op;
    op.id = ops.nextId++;
    op.recipe = r->def;
    op.part = static_cast<uint16_t>(part);
    ops.list.push_back(op);
    return op.id;
}

bool SurgerySystem::remove(Entity patient, uint32_t operation, std::string& error) {
    entt::registry& reg = things_.registry();
    Operations* ops = things_.exists(patient) ? reg.try_get<Operations>(patient) : nullptr;
    if (ops == nullptr) {
        error = "that patient has no operations planned";
        return false;
    }
    const auto found = std::find_if(ops->list.begin(), ops->list.end(), [operation](const Operation& o) { return o.id == operation; });
    if (found == ops->list.end()) {
        error = "no such operation";
        return false;
    }
    ops->list.erase(found);
    return true;
}

const Operations* SurgerySystem::of(Entity patient) const {
    const entt::registry& reg = things_.registry();
    return reg.valid(patient) ? reg.try_get<Operations>(patient) : nullptr;
}

const Operation* SurgerySystem::find(Entity patient, uint32_t operation) const {
    const Operations* ops = of(patient);
    if (ops == nullptr) {
        return nullptr;
    }
    const auto found = std::find_if(ops->list.begin(), ops->list.end(), [operation](const Operation& o) { return o.id == operation; });
    return found == ops->list.end() ? nullptr : &*found;
}

bool SurgerySystem::pending(Entity patient) const {
    const Operations* ops = of(patient);
    return ops != nullptr && !ops->list.empty();
}

std::vector<OperationLine> SurgerySystem::lines(Entity patient) const {
    std::vector<OperationLine> out;
    const Operations* ops = of(patient);
    if (ops == nullptr || !health_.tracks(patient)) {
        return out;
    }
    const std::vector<std::string> labels = health_.partLabels(things_.registry().get<Thing>(patient).def);
    for (const Operation& op : ops->list) {
        const SurgeryRecipe* r = recipe(op.recipe);
        if (r == nullptr) {
            continue;
        }
        OperationLine line;
        line.id = op.id;
        line.recipe = r->defName;
        line.label = r->label;
        line.part = op.part < labels.size() ? labels[op.part] : std::string();
        line.minSkill = r->minSkill;
        out.push_back(std::move(line));
    }
    return out;
}

float SurgerySystem::skillFactor(int level) const { return surgeryInterpolate(numbers_.successBySkill, static_cast<float>(level)); }

float SurgerySystem::cleanlinessFactor(float cleanliness) const { return surgeryInterpolate(numbers_.cleanlinessFactor, cleanliness); }

float SurgerySystem::successChance(const SurgeryRecipe& recipe, const SurgeryConditions& c) const {
    float medicine = numbers_.noMedicineFactor;
    if (c.medicine != nullptr) {
        medicine = std::max(numbers_.noMedicineFactor, numbers_.noMedicineFactor + numbers_.medicinePotencyWeight * c.medicine->potency);
    }
    const float chance = recipe.successChance * skillFactor(c.skill) * std::max(0.0f, c.manipulation) * std::max(0.0f, c.sight) *
                         c.bedFactor * cleanlinessFactor(c.cleanliness) * medicine;
    return std::clamp(chance, 0.0f, numbers_.maxSuccessChance);
}

SurgeryResult SurgerySystem::perform(Entity patient, uint32_t operation, float chance, bool success, float tendQuality, float deathRoll01) {
    SurgeryResult result;
    const Operation* found = find(patient, operation);
    if (found == nullptr) {
        return result;
    }
    const Operation op = *found;
    std::string error;
    remove(patient, operation, error);
    const SurgeryRecipe* r = recipe(op.recipe);
    std::string why;
    if (r == nullptr || !eligible(patient, *r, op.part, why)) {
        prune(patient);
        return result;
    }
    result.done = true;
    if (success) {
        if (r->install) {
            result.success = health_.installPart(patient, op.part, r->adds, tendQuality);
        } else if (health_.addedAt(patient, op.part).valid()) {
            const DefId taken = health_.uninstallPart(patient, op.part, tendQuality);
            std::string partDef;
            DefId item;
            float efficiency = 1.0f;
            result.success = taken.valid();
            if (result.success && health_.addedInfo(taken, partDef, item, efficiency)) {
                result.returned = item;
            }
        } else {
            const DefId organ = r->harvests ? organItem(patient, op.part) : DefId{};
            result.success = health_.removePart(patient, op.part, tendQuality);
            if (result.success && organ.valid()) {
                result.harvested = organ;
                result.harvestedPart = op.part;
            }
        }
    } else {
        health_.botch(patient, op.part, numbers_.botchHpFraction);
        if (deathRoll01 < (1.0f - chance) * numbers_.failDeathChanceFactor) {
            result.died = health_.kill(patient, DeathCause::Surgery);
        }
    }
    prune(patient);
    return result;
}

void SurgerySystem::prune(Entity patient) {
    entt::registry& reg = things_.registry();
    Operations* ops = things_.exists(patient) ? reg.try_get<Operations>(patient) : nullptr;
    if (ops == nullptr) {
        return;
    }
    ops->list.erase(std::remove_if(ops->list.begin(), ops->list.end(),
                                   [&](const Operation& op) {
                                       const SurgeryRecipe* r = recipe(op.recipe);
                                       std::string why;
                                       return r == nullptr || !eligible(patient, *r, op.part, why);
                                   }),
                    ops->list.end());
}

void SurgerySystem::hashPawn(Entity pawn, StateHasher& h) const {
    const Operations* ops = of(pawn);
    if (ops == nullptr) {
        return;
    }
    h.add(static_cast<uint64_t>(ops->list.size()));
    h.add(ops->nextId);
    for (const Operation& op : ops->list) {
        h.add(op.id);
        h.add(op.recipe.value);
        h.add(static_cast<int>(op.part));
    }
}

}  // namespace colony
