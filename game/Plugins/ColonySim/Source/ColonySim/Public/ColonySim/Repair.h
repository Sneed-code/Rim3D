#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Grid.h"

#include <cstdint>
#include <string>
#include <vector>

namespace colony {

class ApparelSystem;
class CombatSystem;
class Things;

// Repairing clothes, armor and weapons, and fitting upgrade parts to guns (task P4, docs/design/repair.md).
//
// Unreal does not include this header: it reads the marks and the fitted parts through the snapshot and
// changes them through commands (docs/design/bridge.md).

// On an item lying on the map (apparel or a weapon) that colonists are to repair at a bench.
struct RepairMark {};

// On a gun lying on the map: a colonist is to fit this upgrade part (a Things def) to it at a bench.
struct FitOrder {
    DefId part;
};

// On a gun that has upgrade parts fitted: their Things defs, in the order fitted, one to a slot.
struct FittedParts {
    std::vector<DefId> parts;
};

// What an upgrade part does (the `upgrade` field of its Things def): the slot it takes, which weapon slots
// ("Primary", "Sidearm") it fits, the work and Crafting skill fitting it takes, and the factors it puts on the
// gun's own numbers (all 1 when it does not change that number).
struct UpgradeSpec {
    DefId part;
    std::string slot;  // Optic, Magazine, Barrel, Muzzle, Trigger
    bool primary = true;
    bool sidearm = true;
    float work = 0.0f;  // game seconds at work speed 1
    int minSkill = 0;
    float range = 1.0f;
    float accuracy = 1.0f;
    float magazine = 1.0f;
    float reload = 1.0f;
    float cooldown = 1.0f;
    float warmup = 1.0f;
    float recoil = 1.0f;
};

// What mending an item takes: the material, how many units, and the work in game seconds at speed 1.
struct RepairCost {
    DefId material;
    int count = 0;
    float work = 0.0f;
};

// The numbers in RepairRules (all ours; RimWorld cannot repair gear).
struct RepairNumbers {
    float workShare = 0.25f;        // full repair work is this share of the item's `stats.workToMake`
    float workPerKg = 300.0f;       // items without workToMake: work per kg of mass
    float materialShare = 0.25f;    // full repair takes this share of the item's `costStuffCount`
    float materialPerKg = 3.0f;     // items without one: units of material per kg of mass
    // Apparel's own shares (docs/design/apparel-durability.md): mending clothes is cheap and quick. The material
    // is rounded down (35 cloth to make: 3 for a full repair), at least 1.
    float apparelWorkShare = 0.1f;
    float apparelWorkPerKg = 100.0f;
    float apparelMaterialShare = 0.1f;
    float apparelMaterialPerKg = 1.0f;
    std::string weaponMaterial = "Steel";   // for a weapon not made of a stuff
    std::string apparelMaterial = "Cloth";  // for apparel not made of a stuff
    float materialRadius = 12.0f;   // metres round the bench the material and the part are taken from
    std::string skill = "Crafting";
    int repairMinSkill = 0;
};

class COLONY_API RepairSystem {
public:
    RepairSystem(Things& things, ApparelSystem& apparel, CombatSystem& combat, const DefDatabase& defs);

    const RepairNumbers& numbers() const { return numbers_; }
    const std::vector<UpgradeSpec>& upgrades() const { return upgrades_; }
    // The spec of an upgrade part's Things def, or null if it is not one.
    const UpgradeSpec* upgrade(DefId part) const;

    // ---- Commands ----------------------------------------------------------------------------
    // Marks an item lying on the map for repair, or takes the mark off. Fails for a thing that is not apparel or a
    // weapon, one that is worn or carried, and one that is not damaged (turning the mark off never fails).
    bool markRepair(Entity item, bool on, std::string& error);
    // Orders a part (a Things defName) fitted to a gun lying on the map, or with `on` false cancels the order.
    // Fails if it is not a gun, not on the map, the part is unknown, does not fit that sort of gun, or its slot
    // is taken.
    bool orderFit(Entity weapon, const std::string& part, bool on, std::string& error);

    // ---- Looking up --------------------------------------------------------------------------
    // Whether the item is apparel or a weapon with hit points, lying on the map.
    bool repairable(Entity item) const;
    // The kind of gear: "Weapons" or "Apparel" (empty for neither). A bench's `repairs` list names the kinds.
    std::string kindOf(DefId item) const;
    // What it takes to mend the item now: the bigger of its lost hit points and its lost durability (Apparel.h), as a
    // share of a full repair. One repair brings both back to full.
    RepairCost costOf(Entity item) const;
    // The parts fitted to a gun, in the order fitted.
    const std::vector<DefId>& fitted(Entity weapon) const;
    // Why the part cannot be fitted to the gun (empty if it can).
    std::string fitBlocker(Entity weapon, DefId part) const;
    // Whether a building def can repair gear of this kind, and fit upgrade parts.
    bool benchRepairs(DefId buildingDef, const std::string& kind) const;
    bool benchFits(DefId buildingDef) const;
    // Items of a def lying within `materialRadius` of a cell on a level, all stacks together.
    int stockNear(DefId item, CellPos around, int level) const;

    // ---- For the job system ------------------------------------------------------------------
    // Takes `count` of an item def from the stacks nearest the cell, within `materialRadius`. False, taking
    // nothing, if there are not enough.
    bool takeNear(DefId item, int count, CellPos around, int level);
    // The item is mended to its full hit points and durability and loses its mark. Returns the hit points restored.
    int finishRepair(Entity item);
    // The part is fitted to the gun: the gun lists it and its numbers change. The caller has taken the part.
    void finishFit(Entity weapon, DefId part);
    // Works out the factors of a gun's fitted parts into a `WeaponMods` (Combat.h) on the gun, or removes it.
    void refreshMods(Entity weapon);
    // The skill level and speed of work are the bill system's (BillNumbers); the skill a worker needs.
    int repairMinSkill() const { return numbers_.repairMinSkill; }

    // One game minute: marks and orders that no longer mean anything go.
    void step();
    // After a load: the `WeaponMods` of every gun with parts.
    void restoreMods();

private:
    Things& things_;
    ApparelSystem& apparel_;
    CombatSystem& combat_;
    const DefDatabase& defs_;
    RepairNumbers numbers_;
    std::vector<UpgradeSpec> upgrades_;  // in def id order
    std::vector<int> upgradeOf_;         // per def id: index in upgrades_, -1
};

}  // namespace colony
