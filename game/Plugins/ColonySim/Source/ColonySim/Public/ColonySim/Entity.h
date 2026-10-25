#pragma once

// Things (pawns, items, buildings) are entities in an EnTT registry; see docs/design/things.md.
// This header is light on purpose: grid layers store entities without pulling in the registry.
#include <entt/entity/entity.hpp>

#include <cstdint>

namespace colony {

using Entity = entt::entity;

// "No thing", e.g. a cell with no building. Compare with `e == kNoEntity`.
inline constexpr Entity kNoEntity = entt::null;

// What a thing is, from its def's `category` (as RimWorld's ThingDef). Plants are a grid layer,
// not things. Here rather than in Things.h so the snapshot and events can use it without the
// registry.
enum class ThingCategory : uint8_t { Item, Building, Pawn };

// A building that is not built yet (docs/design/construction.md): a blueprint is the plan, a frame
// has its first materials and is being built. Both are things of category Building that stand on
// the map layer's side, not in it: they block nothing. `Built` is every other building.
enum class ConstructionStage : uint8_t { Built, Blueprint, Frame };

}  // namespace colony
