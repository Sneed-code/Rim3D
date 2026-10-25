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
            } else {
                static_assert(std::is_same_v<Type, DestroyThingCommand>, "name every command type");
                return "DestroyThing";
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
        case EventType::PawnAction: return "PawnAction";
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

const ThingView* Snapshot::find(Entity id) const {
    const auto it = std::lower_bound(things.begin(), things.end(), id,
                                     [](const ThingView& view, Entity value) { return view.id < value; });
    return it != things.end() && it->id == id ? &*it : nullptr;
}

}  // namespace colony
