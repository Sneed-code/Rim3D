#include "ColonySim/Repair.h"

#include "ColonySim/Apparel.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Things.h"
#include "ColonySim/WeaponSlot.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

const std::vector<DefId> kRepairNoParts;

bool repairListHas(const Def& def, const char* path, const std::string& value) {
    for (size_t i = 0; i < def.count(path); ++i) {
        if (def.textAt(path, i) == value) {
            return true;
        }
    }
    return false;
}

}  // namespace

RepairSystem::RepairSystem(Things& things, ApparelSystem& apparel, CombatSystem& combat, const DefDatabase& defs)
    : things_(things), apparel_(apparel), combat_(combat), defs_(defs) {
    if (const Def* rules = defs_.find("Rules", "RepairRules")) {
        numbers_.workShare = static_cast<float>(rules->number("workShare", numbers_.workShare));
        numbers_.workPerKg = static_cast<float>(rules->number("workPerKg", numbers_.workPerKg));
        numbers_.materialShare = static_cast<float>(rules->number("materialShare", numbers_.materialShare));
        numbers_.materialPerKg = static_cast<float>(rules->number("materialPerKg", numbers_.materialPerKg));
        numbers_.weaponMaterial = rules->text("weaponMaterial", numbers_.weaponMaterial);
        numbers_.apparelMaterial = rules->text("apparelMaterial", numbers_.apparelMaterial);
        numbers_.materialRadius = static_cast<float>(rules->number("materialRadius", numbers_.materialRadius));
        numbers_.skill = rules->text("skill", numbers_.skill);
        numbers_.repairMinSkill = static_cast<int>(rules->integer("minSkill", numbers_.repairMinSkill));
    }
    upgradeOf_.assign(defs_.size(), -1);
    for (const DefId id : defs_.idsOfKind("Things")) {
        const Def& def = defs_.get(id);
        if (!def.has("upgrade.slot")) {
            continue;
        }
        UpgradeSpec spec;
        spec.part = id;
        spec.slot = def.text("upgrade.slot");
        spec.primary = repairListHas(def, "upgrade.weaponSlots", "Primary");
        spec.sidearm = repairListHas(def, "upgrade.weaponSlots", "Sidearm");
        spec.work = static_cast<float>(def.number("upgrade.work"));
        spec.minSkill = static_cast<int>(def.integer("upgrade.minSkill"));
        spec.range = static_cast<float>(def.number("upgrade.stats.rangeFactor", 1.0));
        spec.accuracy = static_cast<float>(def.number("upgrade.stats.accuracyFactor", 1.0));
        spec.magazine = static_cast<float>(def.number("upgrade.stats.magazineFactor", 1.0));
        spec.reload = static_cast<float>(def.number("upgrade.stats.reloadFactor", 1.0));
        spec.cooldown = static_cast<float>(def.number("upgrade.stats.cooldownFactor", 1.0));
        spec.warmup = static_cast<float>(def.number("upgrade.stats.warmupFactor", 1.0));
        spec.recoil = static_cast<float>(def.number("upgrade.stats.recoilFactor", 1.0));
        upgradeOf_[id.value] = static_cast<int>(upgrades_.size());
        upgrades_.push_back(std::move(spec));
    }
}

const UpgradeSpec* RepairSystem::upgrade(DefId part) const {
    if (!part.valid() || part.value >= upgradeOf_.size() || upgradeOf_[part.value] < 0) {
        return nullptr;
    }
    return &upgrades_[static_cast<size_t>(upgradeOf_[part.value])];
}

std::string RepairSystem::kindOf(DefId item) const {
    if (!item.valid()) {
        return std::string();
    }
    if (apparel_.stats().isApparel(item)) {
        return "Apparel";
    }
    if (combat_.isGun(item) || combat_.isMeleeWeapon(item)) {
        return "Weapons";
    }
    return std::string();
}

bool RepairSystem::repairable(Entity item) const {
    const entt::registry& reg = things_.registry();
    return things_.spawned(item) && reg.all_of<Thing, HitPoints>(item) && reg.get<HitPoints>(item).max > 0 &&
           !kindOf(reg.get<Thing>(item).def).empty();
}

bool RepairSystem::markRepair(Entity item, bool on, std::string& error) {
    entt::registry& reg = things_.registry();
    if (!on) {
        if (reg.valid(item)) {
            reg.remove<RepairMark>(item);
        }
        return true;
    }
    if (!reg.valid(item) || !reg.all_of<Thing>(item)) {
        error = "that thing does not exist";
        return false;
    }
    if (kindOf(reg.get<Thing>(item).def).empty()) {
        error = "only clothes, armor and weapons can be repaired";
        return false;
    }
    if (!things_.spawned(item)) {
        error = "take it off or put it down first: only gear lying on the ground can be repaired";
        return false;
    }
    const HitPoints* hp = reg.try_get<HitPoints>(item);
    if (hp == nullptr || hp->current >= hp->max) {
        error = "that isn't damaged";
        return false;
    }
    reg.emplace_or_replace<RepairMark>(item);
    return true;
}

std::string RepairSystem::fitBlocker(Entity weapon, DefId part) const {
    const entt::registry& reg = things_.registry();
    const UpgradeSpec* spec = upgrade(part);
    if (spec == nullptr) {
        return "that is not an upgrade part";
    }
    if (!reg.valid(weapon) || !reg.all_of<Thing>(weapon) || !combat_.isGun(reg.get<Thing>(weapon).def)) {
        return "upgrade parts fit guns only";
    }
    WeaponSlot slot = WeaponSlot::Primary;
    combat_.weaponSlot(reg.get<Thing>(weapon).def, slot);
    if ((slot == WeaponSlot::Sidearm && !spec->sidearm) || (slot != WeaponSlot::Sidearm && !spec->primary)) {
        return "that part doesn't fit " + std::string(slot == WeaponSlot::Sidearm ? "a sidearm" : "a rifle");
    }
    for (const DefId fittedPart : fitted(weapon)) {
        if (const UpgradeSpec* other = upgrade(fittedPart); other != nullptr && other->slot == spec->slot) {
            return "the gun already has a " + defs_.get(fittedPart).text("label", defs_.get(fittedPart).defName()) + " in its " +
                   spec->slot + " slot";
        }
    }
    return std::string();
}

bool RepairSystem::orderFit(Entity weapon, const std::string& part, bool on, std::string& error) {
    entt::registry& reg = things_.registry();
    if (!on) {
        if (reg.valid(weapon)) {
            reg.remove<FitOrder>(weapon);
        }
        return true;
    }
    const DefId partDef = defs_.idOf("Things", part);
    if (!partDef.valid() || upgrade(partDef) == nullptr) {
        error = part + " is not an upgrade part";
        return false;
    }
    if (!things_.spawned(weapon)) {
        error = "the gun has to lie on the ground (or in a stockpile) to be upgraded";
        return false;
    }
    const std::string blocker = fitBlocker(weapon, partDef);
    if (!blocker.empty()) {
        error = blocker;
        return false;
    }
    reg.emplace_or_replace<FitOrder>(weapon, FitOrder{partDef});
    return true;
}

const std::vector<DefId>& RepairSystem::fitted(Entity weapon) const {
    const entt::registry& reg = things_.registry();
    const FittedParts* parts = reg.valid(weapon) ? reg.try_get<FittedParts>(weapon) : nullptr;
    return parts != nullptr ? parts->parts : kRepairNoParts;
}

RepairCost RepairSystem::costOf(Entity item) const {
    const entt::registry& reg = things_.registry();
    RepairCost cost;
    const HitPoints* hp = reg.valid(item) ? reg.try_get<HitPoints>(item) : nullptr;
    if (hp == nullptr || hp->max <= 0 || hp->current >= hp->max) {
        return cost;
    }
    const Def& def = defs_.get(reg.get<Thing>(item).def);
    const float missing = static_cast<float>(hp->max - hp->current) / static_cast<float>(hp->max);
    const float mass = static_cast<float>(def.number("stats.mass", 1.0));
    const float fullWork = def.has("stats.workToMake") ? static_cast<float>(def.number("stats.workToMake")) * numbers_.workShare
                                                       : mass * numbers_.workPerKg;
    cost.work = std::max(1.0f, std::ceil(fullWork * missing));
    const MadeOf* made = reg.try_get<MadeOf>(item);
    const float fullMaterial = def.has("costStuffCount") ? static_cast<float>(def.number("costStuffCount")) * numbers_.materialShare
                                                         : mass * numbers_.materialPerKg;
    cost.material = made != nullptr && made->stuff.valid()
                        ? made->stuff
                        : defs_.idOf("Things", kindOf(reg.get<Thing>(item).def) == "Weapons" ? numbers_.weaponMaterial
                                                                                              : numbers_.apparelMaterial);
    cost.count = std::max(1, static_cast<int>(std::ceil(fullMaterial * missing - 1.0e-4f)));
    return cost;
}

bool RepairSystem::benchRepairs(DefId buildingDef, const std::string& kind) const {
    return buildingDef.valid() && repairListHas(defs_.get(buildingDef), "repairs", kind);
}

bool RepairSystem::benchFits(DefId buildingDef) const {
    return buildingDef.valid() && defs_.get(buildingDef).flag("fitsUpgrades");
}

int RepairSystem::stockNear(DefId item, CellPos around, int level) const {
    int total = 0;
    const float cx = static_cast<float>(around.x) + 0.5f;
    const float cy = static_cast<float>(around.y) + 0.5f;
    const float radiusSq = numbers_.materialRadius * numbers_.materialRadius;
    things_.registry().view<const Thing, const Position, const StackCount>().each(
        [&](const Thing& thing, const Position& pos, const StackCount& stack) {
            const float dx = pos.x - cx;
            const float dy = pos.y - cy;
            if (thing.def == item && pos.level == level && dx * dx + dy * dy <= radiusSq) {
                total += stack.count;
            }
        });
    return total;
}

bool RepairSystem::takeNear(DefId item, int count, CellPos around, int level) {
    if (count <= 0) {
        return true;
    }
    entt::registry& reg = things_.registry();
    const float cx = static_cast<float>(around.x) + 0.5f;
    const float cy = static_cast<float>(around.y) + 0.5f;
    const float radiusSq = numbers_.materialRadius * numbers_.materialRadius;
    struct Stack {
        float distSq;
        Entity entity;
    };
    std::vector<Stack> stacks;
    int total = 0;
    reg.view<const Thing, const Position, const StackCount>().each([&](Entity e, const Thing& thing, const Position& pos, const StackCount& stack) {
        const float dx = pos.x - cx;
        const float dy = pos.y - cy;
        if (thing.def == item && pos.level == level && dx * dx + dy * dy <= radiusSq) {
            stacks.push_back(Stack{dx * dx + dy * dy, e});
            total += stack.count;
        }
    });
    if (total < count) {
        return false;
    }
    std::sort(stacks.begin(), stacks.end(), [](const Stack& a, const Stack& b) {
        return a.distSq != b.distSq ? a.distSq < b.distSq : a.entity < b.entity;
    });
    int left = count;
    for (const Stack& stack : stacks) {
        if (left <= 0) {
            break;
        }
        StackCount& have = reg.get<StackCount>(stack.entity);
        const int take = std::min(left, have.count);
        left -= take;
        if (take >= have.count) {
            things_.destroy(stack.entity);
        } else {
            have.count -= take;
        }
    }
    return true;
}

int RepairSystem::finishRepair(Entity item) {
    entt::registry& reg = things_.registry();
    HitPoints* hp = reg.valid(item) ? reg.try_get<HitPoints>(item) : nullptr;
    if (hp == nullptr) {
        return 0;
    }
    const int restored = std::max(0, hp->max - hp->current);
    hp->current = hp->max;
    reg.remove<RepairMark>(item);
    return restored;
}

void RepairSystem::finishFit(Entity weapon, DefId part) {
    entt::registry& reg = things_.registry();
    reg.get_or_emplace<FittedParts>(weapon).parts.push_back(part);
    reg.remove<FitOrder>(weapon);
    refreshMods(weapon);
}

void RepairSystem::refreshMods(Entity weapon) {
    entt::registry& reg = things_.registry();
    const FittedParts* parts = reg.valid(weapon) ? reg.try_get<FittedParts>(weapon) : nullptr;
    if (parts == nullptr || parts->parts.empty()) {
        if (reg.valid(weapon)) {
            reg.remove<WeaponMods>(weapon);
        }
        return;
    }
    WeaponMods mods;
    for (const DefId part : parts->parts) {
        if (const UpgradeSpec* spec = upgrade(part)) {
            mods.range *= spec->range;
            mods.accuracy *= spec->accuracy;
            mods.magazine *= spec->magazine;
            mods.reload *= spec->reload;
            mods.cooldown *= spec->cooldown;
            mods.warmup *= spec->warmup;
            mods.recoil *= spec->recoil;
        }
    }
    reg.emplace_or_replace<WeaponMods>(weapon, mods);
}

void RepairSystem::restoreMods() {
    entt::registry& reg = things_.registry();
    for (const Entity weapon : reg.view<FittedParts>()) {
        refreshMods(weapon);
    }
}

void RepairSystem::step() {
    entt::registry& reg = things_.registry();
    std::vector<Entity> done;
    for (const Entity item : reg.view<RepairMark>()) {
        const HitPoints* hp = reg.try_get<HitPoints>(item);
        if (hp == nullptr || hp->current >= hp->max) {
            done.push_back(item);
        }
    }
    for (const Entity item : done) {
        reg.remove<RepairMark>(item);
    }
    done.clear();
    for (const Entity weapon : reg.view<FitOrder>()) {
        if (!fitBlocker(weapon, reg.get<FitOrder>(weapon).part).empty()) {
            done.push_back(weapon);
        }
    }
    for (const Entity weapon : done) {
        reg.remove<FitOrder>(weapon);
    }
}

}  // namespace colony
