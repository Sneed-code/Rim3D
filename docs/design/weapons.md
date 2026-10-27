# Several weapons: a rifle, a sidearm and a melee weapon

Code: `CombatSystem::carryWeapon`, `switchWeapon` and friends in `Private/Combat.cpp`, `WeaponSlot.h`;
the fighter AI's part in `FighterSystem::chooseWeapon` (`Private/Fighters.cpp`); the load in
`JobSystem::bodyWeaponMass`. Tests: `Tests/WeaponsTests.cpp`. Data: `weaponSlot` on gun defs,
`weaponSwitchSteps` in `CombatRules`, `switch*` in `FighterRules`. Task P1. It stands on guns
([combat.md](combat.md)), melee ([melee.md](melee.md)) and carrying by mass ([jobs.md](jobs.md), C24).

The game design doc wants the protagonist to carry several weapons. Everyone can: the player's colonists
and the raiders alike, so the fighter AI picks among them too.

## What a pawn carries

One weapon to a **slot**: a **Primary** (rifle, shotgun), a **Sidearm** (revolver, autopistol) and a **Melee**
weapon. A gun def says which in `weaponSlot` ("Sidearm" or "Primary"; Primary if it says nothing; ours), a melee
weapon is always Melee. One of the carried weapons is in the hands (`Gear::weapon`, as before: the one that
shoots or swings); the others are on the body (`Gear::stowed`). Everything that read `Gear::weapon` still
means "what the pawn fights with now", so shooting, melee, reloading and the snapshot's `weapon` did not change.

- `CarryWeaponCommand{pawn, item}` adds a weapon: into the hands if they are empty, else on the body. The item
  may lie on the map. If the slot is taken, the weapon carried there (hands or body) is dropped where the pawn
  stands. Fails for a pawn with no body or a dead one, an item that is no weapon or is someone else's.
- `EquipCommand` works as before (the item goes in the hands, what was in the hands is dropped), and also drops
  a body weapon of the same slot, so there is never more than one to a slot.
- Death, stripping (`ApparelSystem::strip`, `hasGear`), `unequip` and a raider leaving the map treat the body
  weapons like the one in the hands.

## Picking guns up

A colonist with no gun in a gun slot (`Primary`, `Sidearm`) picks up the nearest loose gun for it (rifles first), in the same
think node as clothes (`JobSystem::findLooseWeapon`, job `EquipWeapon`, steps walk and `wearSteps` of work, then
`carryWeapon`); a rifle goes to the hands. Ours: only while fewer than 3 other colonists carry a gun (a colonist with one
revolver stands and fights three wolves, a colonist with none shelters; the year run lost most of its colonists to the
first when everybody was armed), melee weapons are never picked up (a berserk colonist with a knife killed its friends),
weapons are never swapped for better ones, and a gun marked for repair or an upgrade (`RepairMark`, `FitOrder`) is left for
the crafter. Tests: `ColdAndArmsTests.cpp`, `RepairTests.cpp`.

## Switching

`SwitchWeaponCommand{pawn, slot}` starts a switch to the weapon carried in that slot. It takes
`CombatRules.weaponSwitchSteps` for the slot switched **to** (ours: rifle 36 steps, sidearm 18, melee 27: a
long gun is the slowest to bring up, a pistol the quickest). Until the steps run out the old weapon is still in
the hands in the snapshot, but the pawn **cannot shoot, reload or swing** (`shoot`, `reload` and `MeleeSystem::attack`
refuse; `ready` is false). When they run out the two weapons trade places and a `WeaponSwitched` event is
sent. A switch fails, changing nothing, if:

- nothing is carried in that slot, or it is in the hands already;
- the pawn is downed or dead, knocked down, rolling, or already switching;
- it is aiming, firing or reloading a gun, or mid-swing (a switch waits for the weapon to be ready; a gun
  cooling down after a burst is fine).

A switch that loses its weapon (dropped, stripped) or its pawn (downed) simply ends. Each gun keeps its own
magazine (it sits on the gun item), so a half-empty rifle is half empty when it comes back.

## The fighter AI picks by range

Each step a fighter that has body weapons looks at the distance to its target (`chooseWeapon`) and takes:

| Distance to the target | First choice, then the others |
| --- | --- |
| up to 2.5 m (`switchMeleeRange`) | melee weapon, sidearm, rifle |
| up to 9 m (`switchSidearmRange`) | sidearm, rifle, melee weapon |
| further | rifle, sidearm, melee weapon |

A gun that cannot reach the target (its `verbs.0.range`) is passed over; if no gun reaches, the one in the hands
is kept, else the longest-ranged is taken. The weapon in the hands keeps its band 2 m longer
(`switchMargin`), so a target standing on the edge of a band does not make the fighter switch back and forth.
A fighter with only one weapon never switches, so none of F2 to F8 changed. While it switches it neither
shoots nor swings; once a melee weapon is in the hands F8's charge and swing rules apply, and once a gun is
the usual cover and shooting rules do. All numbers are ours (RimWorld has no weapon switching).

## Weight counts

The weapons **on the body** (not the one in the hands: that is the pawn's usual outfit and nothing changes
for a pawn with one weapon) add to the load of C24: `JobSystem::bodyWeaponMass` is their mass. The load
slows walking like a hauled stack does (up to 25% at the carrying capacity: `loadSpeedFactor`, now also
counting the body weapons and a stack together) and takes room from what the hands can haul
(`carryLimit` is the capacity less the body weapons' mass, but at least one). A rifle, a revolver and a breach
axe on a 35 kg pawn: 3.9 kg, about a 3% slowdown.

## Snapshot and events

`ThingView::carried[3]` is the weapon def in each slot (hands or body; invalid for none), `switchSlot` (255 when
not switching) and `switchStepsLeft` say what Unreal shows during a switch, and `weapon` is still what is in the
hands. `PawnAction::Switch` (`target` the weapon coming to the hands, `amount` the steps) starts the animation
and `WeaponSwitched` (`thing`, `def`, `target`) ends it. Unreal's weapon-switch key (Q) and the holstered weapons are U22 ([melee-draw.md](melee-draw.md)).

## Save

Section `weapons` 1: `stowed` (a pawn's body weapons, in order) and `switch` (the weapon switched to and the steps
left). A save from before has no section and loads with nobody carrying more than the hands. Both are in the
state hash.
