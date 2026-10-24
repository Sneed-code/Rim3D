#pragma once

// Things (pawns, items, buildings) are entities in an EnTT registry; see docs/design/things.md.
// This header is light on purpose: grid layers store entities without pulling in the registry.
#include <entt/entity/entity.hpp>

namespace colony {

using Entity = entt::entity;

// "No thing", e.g. a cell with no building. Compare with `e == kNoEntity`.
inline constexpr Entity kNoEntity = entt::null;

}  // namespace colony
