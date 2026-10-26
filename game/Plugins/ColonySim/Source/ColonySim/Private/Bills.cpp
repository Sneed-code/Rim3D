#include "ColonySim/Bills.h"

#include "ColonySim/Colonist.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Genes.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

constexpr int kBillsDefaultMinCount = 1;
constexpr int kBillsMaxSkill = 20;
constexpr float kBillsMaxRadius = 2000.0f;

// An item is in a category by its `thingCategories` (FoodRaw, Textiles) or, for a stuff, by the categories
// of its `stuffProps` (Metallic, Fabric, Leathery): recipes ask for either (`categories`, `stuffCategories`).
bool billsHasCategory(const Def& item, const std::string& category) {
    for (size_t i = 0; i < item.count("thingCategories"); ++i) {
        if (item.textAt("thingCategories", i) == category) {
            return true;
        }
    }
    for (size_t i = 0; i < item.count("stuffProps.categories"); ++i) {
        if (item.textAt("stuffProps.categories", i) == category) {
            return true;
        }
    }
    return false;
}

}  // namespace

const char* billModeName(BillMode mode) {
    switch (mode) {
        case BillMode::Count: return "Count";
        case BillMode::UntilHave: return "UntilHave";
        case BillMode::Forever: return "Forever";
    }
    return "?";
}

int RecipeIngredient::unitsNeeded(double unitNutrition) const {
    if (!byNutrition) {
        return std::max(1, static_cast<int>(std::lround(amount)));
    }
    if (unitNutrition <= 0.0) {
        return 0;
    }
    // A small margin so that 0.5 / 0.05 is 10 and not 11.
    return std::max(1, static_cast<int>(std::ceil(amount / unitNutrition - 1.0e-6)));
}

BillSystem::BillSystem(Things& things, const DefDatabase& defs) : things_(things), defs_(defs) {
    if (const Def* rules = defs_.find("Rules", "BillRules")) {
        numbers_.maxBillsPerBuilding = static_cast<int>(rules->integer("maxBillsPerBuilding", numbers_.maxBillsPerBuilding));
        numbers_.maxBillCount = static_cast<int>(rules->integer("maxBillCount", numbers_.maxBillCount));
        numbers_.skillSpeedBase = rules->number("skillSpeedBase", numbers_.skillSpeedBase);
        numbers_.skillSpeedPerLevel = rules->number("skillSpeedPerLevel", numbers_.skillSpeedPerLevel);
        numbers_.defaultSkillLevel = static_cast<int>(rules->integer("defaultSkillLevel", numbers_.defaultSkillLevel));
        numbers_.maxIngredientStacks = static_cast<int>(rules->integer("maxIngredientStacks", numbers_.maxIngredientStacks));
    }

    nutrition_.assign(defs_.size(), 0.0);
    rawMeat_.assign(defs_.size(), 0);
    std::vector<DefId> items;
    for (uint32_t i = 0; i < defs_.size(); ++i) {
        const Def& def = defs_.get(DefId{i});
        if (def.kind() == "Things" && def.text("category") == "Item") {
            nutrition_[i] = def.number("stats.nutrition", 0.0);
            rawMeat_[i] = billsHasCategory(def, "MeatRaw") ? 1 : 0;
            items.push_back(def.id());
        }
    }

    for (const DefId id : defs_.idsOfKind("Recipes")) {
        const Def& def = defs_.get(id);
        Recipe recipe;
        recipe.def = id;
        recipe.defName = def.defName();
        recipe.label = def.label();
        for (size_t i = 0; i < def.count("recipeUsers"); ++i) {
            const DefId user = defs_.idOf("Things", def.textAt("recipeUsers", i));
            if (user.valid()) {
                recipe.users.push_back(user);
            }
        }
        recipe.work = static_cast<float>(def.number("workAmount", 0.0));
        recipe.skill = def.text("workSkill");
        recipe.minSkill = static_cast<int>(def.integer("minSkill", 0));
        recipe.skillXp = static_cast<float>(def.number("workSkillXp", 0.0));
        for (size_t i = 0; i < def.count("ingredients"); ++i) {
            const std::string base = "ingredients." + std::to_string(i);
            RecipeIngredient ingredient;
            ingredient.byNutrition = def.has((base + ".nutrition").c_str());
            ingredient.amount = ingredient.byNutrition ? def.number((base + ".nutrition").c_str())
                                                       : def.number((base + ".count").c_str(), 1.0);
            const std::string categories = base + ".categories";
            const std::string stuffCategories = base + ".stuffCategories";
            const std::string named = base + ".items";
            for (const DefId item : items) {
                const Def& itemDef = defs_.get(item);
                bool fits = false;
                for (size_t c = 0; c < def.count(categories.c_str()) && !fits; ++c) {
                    fits = billsHasCategory(itemDef, def.textAt(categories.c_str(), c));
                }
                for (size_t c = 0; c < def.count(stuffCategories.c_str()) && !fits; ++c) {
                    fits = billsHasCategory(itemDef, def.textAt(stuffCategories.c_str(), c));
                }
                for (size_t n = 0; n < def.count(named.c_str()) && !fits; ++n) {
                    fits = itemDef.defName() == def.textAt(named.c_str(), n);
                }
                // A nutrition ingredient only takes items that have some.
                if (fits && (!ingredient.byNutrition || nutrition_[item.value] > 0.0)) {
                    ingredient.items.push_back(item);
                }
            }
            recipe.ingredients.push_back(std::move(ingredient));
        }
        recipe.butcher = def.has("butcher");
        recipe.stuffFromIngredient = def.flag("productsMadeOfIngredient") || def.flag("productStuffFromIngredient");
        for (const std::string& name : def.keys("products")) {
            const DefId item = defs_.idOf("Things", name);
            if (item.valid()) {
                recipe.products.push_back(RecipeProduct{
                    item, std::max(1, static_cast<int>(def.integer(("products." + name).c_str(), 1)))});
            }
        }
        recipes_.push_back(std::move(recipe));
    }
}

const Recipe* BillSystem::recipe(DefId def) const {
    for (const Recipe& r : recipes_) {
        if (r.def == def) {
            return &r;
        }
    }
    return nullptr;
}

double BillSystem::nutritionOf(DefId item) const {
    return item.valid() && item.value < nutrition_.size() ? nutrition_[item.value] : 0.0;
}

bool BillSystem::canMake(DefId buildingDef, const Recipe& recipe) const {
    return std::find(recipe.users.begin(), recipe.users.end(), buildingDef) != recipe.users.end();
}

const BillList* BillSystem::billsOf(Entity building) const {
    const entt::registry& reg = things_.registry();
    return reg.valid(building) ? reg.try_get<BillList>(building) : nullptr;
}

const Bill* BillSystem::find(Entity building, uint32_t bill) const {
    const BillList* list = billsOf(building);
    if (list == nullptr) {
        return nullptr;
    }
    for (const Bill& b : list->bills) {
        if (b.id == bill) {
            return &b;
        }
    }
    return nullptr;
}

uint32_t BillSystem::addBill(Entity building, const std::string& recipeName, BillMode mode, int count, std::string& error) {
    entt::registry& reg = things_.registry();
    if (!things_.spawned(building) || !reg.all_of<Thing, Footprint>(building) || reg.all_of<ConstructionSite>(building)) {
        error = "that isn't a finished building";
        return 0;
    }
    const Def* def = defs_.find("Recipes", recipeName);
    if (def == nullptr) {
        error = "no Recipes def '" + recipeName + "'";
        return 0;
    }
    const Recipe* r = recipe(def->id());
    if (r == nullptr || !canMake(reg.get<Thing>(building).def, *r)) {
        error = "that building can't make that";
        return 0;
    }
    if (mode > BillMode::Forever) {
        error = "no such bill mode";
        return 0;
    }
    if (mode != BillMode::Forever && (count < kBillsDefaultMinCount || count > numbers_.maxBillCount)) {
        error = "the count is 1 to " + std::to_string(numbers_.maxBillCount);
        return 0;
    }
    BillList& list = reg.get_or_emplace<BillList>(building);
    if (static_cast<int>(list.bills.size()) >= numbers_.maxBillsPerBuilding) {
        error = "a building holds at most " + std::to_string(numbers_.maxBillsPerBuilding) + " bills";
        return 0;
    }
    Bill bill;
    bill.id = list.nextId++;
    bill.recipe = r->def;
    bill.mode = mode;
    bill.count = mode == BillMode::Forever ? 0 : count;
    list.bills.push_back(bill);
    return bill.id;
}

bool BillSystem::removeBill(Entity building, uint32_t billId, std::string& error) {
    BillList* list = things_.registry().valid(building) ? things_.registry().try_get<BillList>(building) : nullptr;
    if (list != nullptr) {
        for (auto it = list->bills.begin(); it != list->bills.end(); ++it) {
            if (it->id == billId) {
                list->bills.erase(it);
                return true;
            }
        }
    }
    error = "there is no such bill";
    return false;
}

bool BillSystem::setBill(Entity building, uint32_t billId, BillMode mode, int count, bool suspended, std::string& error) {
    BillList* list = things_.registry().valid(building) ? things_.registry().try_get<BillList>(building) : nullptr;
    if (list != nullptr) {
        for (Bill& bill : list->bills) {
            if (bill.id != billId) {
                continue;
            }
            if (mode > BillMode::Forever) {
                error = "no such bill mode";
                return false;
            }
            if (mode != BillMode::Forever && (count < kBillsDefaultMinCount || count > numbers_.maxBillCount)) {
                error = "the count is 1 to " + std::to_string(numbers_.maxBillCount);
                return false;
            }
            bill.mode = mode;
            bill.count = mode == BillMode::Forever ? 0 : count;
            bill.suspended = suspended;
            bill.satisfied = false;
            return true;
        }
    }
    error = "there is no such bill";
    return false;
}

bool BillSystem::setOptions(Entity building, uint32_t billId, const Options& options, std::string& error) {
    BillList* list = things_.registry().valid(building) ? things_.registry().try_get<BillList>(building) : nullptr;
    Bill* bill = nullptr;
    if (list != nullptr) {
        for (Bill& b : list->bills) {
            bill = b.id == billId ? &b : bill;
        }
    }
    if (bill == nullptr) {
        error = "there is no such bill";
        return false;
    }
    if (options.skillMin < 0 || options.skillMax > kBillsMaxSkill || options.skillMin > options.skillMax) {
        error = "the skill range is 0 to " + std::to_string(kBillsMaxSkill) + ", lowest first";
        return false;
    }
    if (!(options.radius >= 0.0f) || options.radius > kBillsMaxRadius) {
        error = "the ingredient radius is 0 (anywhere) to " + std::to_string(static_cast<int>(kBillsMaxRadius)) + " metres";
        return false;
    }
    if (options.unpauseAt < 0 || options.unpauseAt > numbers_.maxBillCount) {
        error = "the restart stock is 0 to " + std::to_string(numbers_.maxBillCount);
        return false;
    }
    std::vector<DefId> disallowed;
    for (const std::string& name : options.disallowed) {
        const Def* item = defs_.find("Things", name);
        if (item == nullptr || item->text("category") != "Item") {
            error = "no item '" + name + "'";
            return false;
        }
        disallowed.push_back(item->id());
    }
    std::sort(disallowed.begin(), disallowed.end(), [](DefId a, DefId b) { return a.value < b.value; });
    disallowed.erase(std::unique(disallowed.begin(), disallowed.end()), disallowed.end());
    bill->skillMin = options.skillMin;
    bill->skillMax = options.skillMax;
    bill->radius = options.radius;
    bill->storeZone = options.storeZone;
    bill->unpauseAt = options.unpauseAt;
    bill->satisfied = false;
    bill->disallowed = std::move(disallowed);
    return true;
}

bool BillSystem::moveBill(Entity building, uint32_t billId, int index, std::string& error) {
    BillList* list = things_.registry().valid(building) ? things_.registry().try_get<BillList>(building) : nullptr;
    if (list != nullptr) {
        for (size_t from = 0; from < list->bills.size(); ++from) {
            if (list->bills[from].id != billId) {
                continue;
            }
            const size_t to = static_cast<size_t>(std::clamp(index, 0, static_cast<int>(list->bills.size()) - 1));
            Bill moved = std::move(list->bills[from]);
            list->bills.erase(list->bills.begin() + static_cast<std::ptrdiff_t>(from));
            list->bills.insert(list->bills.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
            return true;
        }
    }
    error = "there is no such bill";
    return false;
}

bool BillSystem::allowsIngredient(const Bill& bill, DefId item) {
    return !std::binary_search(bill.disallowed.begin(), bill.disallowed.end(), item,
                               [](DefId a, DefId b) { return a.value < b.value; });
}

void BillSystem::step() {
    for (const auto [building, list] : things_.registry().view<BillList>().each()) {
        for (Bill& bill : list.bills) {
            if (bill.mode != BillMode::UntilHave || bill.unpauseAt <= 0) {
                bill.satisfied = false;
                continue;
            }
            const Recipe* r = recipe(bill.recipe);
            int stock = 0;
            if (r == nullptr || !stockFor(*r, stock)) {
                continue;
            }
            if (stock >= bill.count) {
                bill.satisfied = true;
            } else if (stock < bill.unpauseAt) {
                bill.satisfied = false;
            }
        }
    }
}

int BillSystem::stockOf(DefId item) const {
    int total = 0;
    things_.registry().view<const Thing, const StackCount, const Position>().each(
        [&](const Thing& thing, const StackCount& stack, const Position&) { total += thing.def == item ? stack.count : 0; });
    return total;
}

bool BillSystem::stockFor(const Recipe& recipe, int& stock) const {
    if (recipe.butcher) {
        stock = 0;
        things_.registry().view<const Thing, const StackCount, const Position>().each(
            [&](const Thing& thing, const StackCount& count, const Position&) {
                stock += thing.def.value < rawMeat_.size() && rawMeat_[thing.def.value] != 0 ? count.count : 0;
            });
        return true;
    }
    if (recipe.products.empty()) {
        return false;
    }
    stock = stockOf(recipe.products.front().item);
    return true;
}

bool BillSystem::shouldDo(const Bill& bill) const {
    if (bill.suspended) {
        return false;
    }
    switch (bill.mode) {
        case BillMode::Count: return bill.count > 0;
        case BillMode::Forever: return true;
        case BillMode::UntilHave: {
            if (bill.satisfied) {
                return false;
            }
            const Recipe* r = recipe(bill.recipe);
            int stock = 0;
            return r != nullptr && stockFor(*r, stock) && stock < bill.count;
        }
    }
    return false;
}

int BillSystem::skillLevel(Entity pawn, const std::string& skill) const {
    int level = numbers_.defaultSkillLevel;
    if (const Colonist* colonist = things_.registry().try_get<Colonist>(pawn)) {
        if (const SkillRecord* record = colonist->skill(skill)) {
            level = record->level;
        }
    }
    if (const PawnGenes* genes = things_.registry().try_get<PawnGenes>(pawn)) {
        level = std::max(0, level + geneAptitude(*genes, skill));
    }
    return level;
}

float BillSystem::workSpeed(Entity pawn, const Recipe& recipe) const {
    return static_cast<float>(
        std::max(0.0, numbers_.skillSpeedBase + numbers_.skillSpeedPerLevel * skillLevel(pawn, recipe.skill)));
}

bool BillSystem::iterationDone(Entity building, uint32_t billId) {
    BillList* list = things_.registry().valid(building) ? things_.registry().try_get<BillList>(building) : nullptr;
    if (list == nullptr) {
        return false;
    }
    for (auto it = list->bills.begin(); it != list->bills.end(); ++it) {
        if (it->id != billId) {
            continue;
        }
        if (it->mode == BillMode::Count && --it->count <= 0) {
            list->bills.erase(it);
            return true;
        }
        return false;
    }
    return false;
}

}  // namespace colony
