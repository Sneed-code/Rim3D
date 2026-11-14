// A pawn's bag (docs/design/inventory.md): InventoryCommand takes items into Gear::inventory and drops them.
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace colony {

namespace {

constexpr float kInventoryReachSq = 2.0f * 2.0f + 0.01f;  // within 2 cells (metres) of the pawn

}  // namespace

void Sim::applyInventoryCommand(const InventoryCommand& command, uint64_t id) {
    entt::registry& reg = things_->registry();
    const Entity pawn = command.pawn;
    if (!reg.valid(pawn) || !reg.all_of<Thing>(pawn) || reg.get<Thing>(pawn).category != ThingCategory::Pawn ||
        !health_->tracks(pawn)) {
        commandFailed(id, "that is not a pawn with a body");
        return;
    }
    if (reg.get<Health>(pawn).dead) {
        commandFailed(id, "that pawn is dead");
        return;
    }
    if (command.count < 0) {
        commandFailed(id, "the count can't be below 0");
        return;
    }
    const Position* at = reg.try_get<Position>(pawn);
    if (at == nullptr) {
        commandFailed(id, "that pawn isn't on the map");
        return;
    }

    if (command.action == "Take") {
        if (health_->incapacitated(pawn)) {
            commandFailed(id, "that pawn is down");
            return;
        }
        const Entity item = command.item;
        if (!reg.valid(item) || !reg.all_of<Thing>(item) || reg.get<Thing>(item).category != ThingCategory::Item ||
            !things_->spawned(item)) {
            commandFailed(id, "that is not an item lying on the map");
            return;
        }
        if (reg.all_of<EquippedBy>(item)) {
            commandFailed(id, "someone has that");
            return;
        }
        const Position& there = reg.get<Position>(item);
        const float dx = there.x - at->x;
        const float dy = there.y - at->y;
        if (cellOf(there).level != cellOf(*at).level || dx * dx + dy * dy > kInventoryReachSq) {
            commandFailed(id, "that is too far away to pick up");
            return;
        }
        const DefId def = reg.get<Thing>(item).def;
        const StackCount* stack = reg.try_get<StackCount>(item);
        const int have = stack != nullptr ? stack->count : 1;
        const int wanted = command.count == 0 ? have : std::min(command.count, have);
        // As many as fit under the carry capacity next to everything else it carries.
        const double room = jobs_->carryCapacity(pawn) - jobs_->loadMass(pawn);
        const double each = std::max(jobs_->massFor(pawn, def), 1e-9);
        const int fits = room <= 0.0 ? 0 : static_cast<int>(std::floor(room / each + 1e-9));
        const int take = std::min(wanted, fits);
        if (take <= 0) {
            commandFailed(id, "that is too heavy: the pawn can't carry any more");
            return;
        }
        Entity taken = item;
        if (take < have) {
            taken = things_->splitOff(item, take);
            if (taken == kNoEntity) {
                commandFailed(id, "that stack can't be split");
                return;
            }
        } else {
            jobs_->forget(item);
            things_->despawn(item);
        }
        Gear& gear = reg.get_or_emplace<Gear>(pawn);
        // The same def already in the bag takes it on, as far as a stack goes.
        for (const Entity mine : gear.inventory) {
            if (!reg.valid(mine) || reg.get<Thing>(mine).def != def || !reg.all_of<StackCount>(mine) ||
                !reg.all_of<StackCount>(taken)) {
                continue;
            }
            const int limit = jobs_->stackLimit(def);
            const int join = std::min(reg.get<StackCount>(taken).count, limit - reg.get<StackCount>(mine).count);
            if (join <= 0) {
                continue;
            }
            things_->addToStack(mine, taken, join);
            reg.get<StackCount>(taken).count -= join;
            if (reg.get<StackCount>(taken).count <= 0) {
                things_->destroy(taken);
                taken = kNoEntity;
                break;
            }
        }
        if (taken != kNoEntity) {
            gear.inventory.push_back(taken);
            reg.emplace_or_replace<EquippedBy>(taken, EquippedBy{pawn});
        }
        return;
    }

    if (command.action == "Drop" || command.action == "DropAll") {
        Gear* gear = reg.try_get<Gear>(pawn);
        std::vector<Entity> items;
        if (command.action == "DropAll") {
            if (gear != nullptr) {
                items = gear->inventory;
            }
        } else {
            if (gear == nullptr || std::find(gear->inventory.begin(), gear->inventory.end(), command.item) == gear->inventory.end()) {
                commandFailed(id, "that item is not in the pawn's bag");
                return;
            }
            Entity item = command.item;
            const StackCount* stack = reg.try_get<StackCount>(item);
            const int have = stack != nullptr ? stack->count : 1;
            if (command.count > 0 && command.count < have) {
                // Part of the stack: the part dropped is split off; the rest stays in the bag.
                const Entity part = things_->splitOff(item, command.count);
                if (part == kNoEntity) {
                    commandFailed(id, "that stack can't be split");
                    return;
                }
                if (!things_->spawnAt(part, cellOf(*at))) {
                    things_->addToStack(item, part, command.count);
                    things_->destroy(part);
                    commandFailed(id, "there is no room to drop it here");
                }
                return;
            }
            items.push_back(item);
        }
        for (const Entity item : items) {
            combat_->unequip(pawn, item);
        }
        return;
    }

    commandFailed(id, "no inventory action '" + command.action + "' (Take, Drop, DropAll)");
}

}  // namespace colony
