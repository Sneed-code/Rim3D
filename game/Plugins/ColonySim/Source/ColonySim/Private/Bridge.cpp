// Small helpers for the bridge types: commands, events and the snapshot.
#include "ColonySim/Commands.h"
#include "ColonySim/Events.h"
#include "ColonySim/Snapshot.h"

#include <algorithm>
#include <type_traits>

namespace colony {

const char* commandName(const Command& command) {
    return std::visit(
        [](const auto& c) -> const char* {
            using Type = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<Type, SpawnThingCommand>) {
                return "SpawnThing";
            } else if constexpr (std::is_same_v<Type, DestroyThingCommand>) {
                return "DestroyThing";
            } else if constexpr (std::is_same_v<Type, DamagePawnCommand>) {
                return "DamagePawn";
            } else if constexpr (std::is_same_v<Type, TendPawnCommand>) {
                return "TendPawn";
            } else if constexpr (std::is_same_v<Type, SetWorkPriorityCommand>) {
                return "SetWorkPriority";
            } else if constexpr (std::is_same_v<Type, CreateStockpileCommand>) {
                return "CreateStockpile";
            } else if constexpr (std::is_same_v<Type, RemoveZoneCellsCommand>) {
                return "RemoveZoneCells";
            } else if constexpr (std::is_same_v<Type, SetStockpilePriorityCommand>) {
                return "SetStockpilePriority";
            } else if constexpr (std::is_same_v<Type, SetStockpileAllowedCommand>) {
                return "SetStockpileAllowed";
            } else if constexpr (std::is_same_v<Type, OrderHaulCommand>) {
                return "OrderHaul";
            } else if constexpr (std::is_same_v<Type, EquipCommand>) {
                return "Equip";
            } else if constexpr (std::is_same_v<Type, ShootCommand>) {
                return "Shoot";
            } else {
                static_assert(std::is_same_v<Type, SetStanceCommand>, "name every command type");
                return "SetStance";
            }
        },
        command);
}

const char* eventTypeName(EventType type) {
    switch (type) {
        case EventType::ThingSpawned: return "ThingSpawned";
        case EventType::ThingDespawned: return "ThingDespawned";
        case EventType::ThingDestroyed: return "ThingDestroyed";
        case EventType::CommandFailed: return "CommandFailed";
        case EventType::PawnInjured: return "PawnInjured";
        case EventType::PawnPartLost: return "PawnPartLost";
        case EventType::PawnDowned: return "PawnDowned";
        case EventType::PawnGotUp: return "PawnGotUp";
        case EventType::PawnDied: return "PawnDied";
        case EventType::JobStarted: return "JobStarted";
        case EventType::JobEnded: return "JobEnded";
        case EventType::PawnAction: return "PawnAction";
        case EventType::ShotFired: return "ShotFired";
        case EventType::ProjectileHit: return "ProjectileHit";
    }
    return "?";
}

const char* pawnActionName(PawnAction action) {
    switch (action) {
        case PawnAction::PickUp: return "PickUp";
        case PawnAction::PutDown: return "PutDown";
    }
    return "?";
}

const char* projectileHitKindName(ProjectileHitKind kind) {
    switch (kind) {
        case ProjectileHitKind::None: return "None";
        case ProjectileHitKind::Ground: return "Ground";
        case ProjectileHitKind::Rock: return "Rock";
        case ProjectileHitKind::Building: return "Building";
        case ProjectileHitKind::Pawn: return "Pawn";
        case ProjectileHitKind::Armor: return "Armor";
        case ProjectileHitKind::Gone: return "Gone";
    }
    return "?";
}

const ThingView* Snapshot::find(Entity id) const {
    const auto it = std::lower_bound(things.begin(), things.end(), id,
                                     [](const ThingView& view, Entity value) { return view.id < value; });
    return it != things.end() && it->id == id ? &*it : nullptr;
}

}  // namespace colony
