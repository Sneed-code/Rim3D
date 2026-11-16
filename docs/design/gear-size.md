# Gear sized to the body

sneed (2026-11-16): weapons, clothes and armour should fit the size of whoever wears or holds them, so nothing looks
cartoonish and grips line up for the animations. This is a drawing rule only: the sim's numbers (reach, damage, weight)
don't change with height.

Pawns are 0.88 to 1.09 times the 1.80 m man (LooksRules `heightMale` 0.95 to 1.05, `heightFemale` 0.88 to 0.97,
hulks +0.04), so about 1.58 m to 1.96 m. Unreal scales the whole body by that (`ASimPawnActor::ApplyLooks`).

## Clothes and armour

Already follow the body. Every worn piece is a skinned mesh on the body's skeleton (leader pose, the body's morphs), and
a hat or helmet sits on the `head` socket, so it grows, shrinks, bends and fattens with the body
([clothes-draw.md](clothes-draw.md)). Nothing new was needed.

## Weapons and shields

These are separate meshes on the pawn, placed in actor space at spots measured on the 1.80 m man (a gun at the chest,
118 cm up; holsters at the hip and back; the shield on the left arm). Before this, a 1.58 m woman held her gun at
chin height and wore her holster at her ribs. Now (`UWeaponDrawSubsystem::UpdatePawn`, `UpdateShield`):

- **Where it sits** scales with the body's height (`colony::gearSizeFor(...).place`): hands, swing and block poses,
  holsters, the back sling and the shield arm all come down or up with the body, standing or crouched.
- **How big it is** follows only half way (`size`, LooksRules `gearSizeFollow` 0.5): a 1.58 m woman's longsword is
  drawn 6% smaller than the man's (not 12%), a 1.96 m hulk's 4.5% bigger. A real sword is the same sword in anyone's
  hands; half way keeps it in proportion to smaller or bigger hands without turning into a toy or a giant's blade.
  1 makes gear grow fully with the body, 0 keeps every weapon its real size.

The helper is in the sim's `Looks.h` (no sim rule reads it; it is there so it is tested: `LooksTests`).

## First person

The protagonist's eyes (165 cm on the 1.80 m man) and the third person camera's pivot (150 cm) scale with their
height, so a short protagonist sees the world from lower down (`AProtagonistPawn::UpdateCamera`). The body and weapons
stay hidden in first person; first person arms and a held gun (a view model) are not built yet.

## Not done

- **First person arms and weapon** (a view model): needs the hands animation set (aim, reload, swing, block) and is a
  PC task for after the real-time combat animations (phase 4, [real-time-combat.md](real-time-combat.md)). Its arms
  would come from the protagonist's own body, so they'd be the right length by this same rule.
- **Grip sockets in the hands**: in the game the gun floats at the chest and melee weapons follow a procedural swing
  ([melee-draw.md](melee-draw.md)); hands holding the grip by bone and IK come with the phase 4 animations. The
  creator's fight demo already holds weapons by the hand bones ([creator-weapons.md](creator-weapons.md), on the
  `makehuman-characters` branch); there the weapon is a child of a scaled mesh and so grows fully with it; when that
  merges it should take `gearSizeFor(...).size` too.
- **Animals and big creatures** carry no gear, so nothing to scale.
