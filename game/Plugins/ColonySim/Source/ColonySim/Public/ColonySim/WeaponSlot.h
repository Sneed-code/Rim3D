#pragma once

#include <cstdint>

namespace colony {

// Where a carried weapon sits (P1, docs/design/weapons.md): one rifle, one sidearm and one melee weapon.
// A gun def says which in `weaponSlot` ("Primary" or "Sidearm", Primary if it says nothing); a melee
// weapon is always Melee. A save and a command name a slot by its number.
enum class WeaponSlot : uint8_t { Primary, Sidearm, Melee };
constexpr int kWeaponSlotCount = 3;

}  // namespace colony
