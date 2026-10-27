# Weapon quality and stuff

Code: `CombatSystem::meleeDamageFactor`, `rangedAccuracyFactor`, `rangedDamageFactor` (`Private/Combat.cpp`), used by
`MeleeSystem::strike` and `CombatSystem::shoot` / `fire` / `impact`. Data: `QualityRules.levels` (`Rules.json`), the
stuff's `stuffProps.statFactors`. Tests: `Tests/WeaponQualityTests.cpp`. Made by crafting ([crafting.md](crafting.md));
the quality and the stuff sit on the item (`Quality`, `MadeOf`).

## Melee

A blow's damage **and armor penetration** are multiplied by the weapon's quality factor, and by its stuff's
factor for the kind of damage: `meleeSharpDamage` for cuts and stabs, `meleeBluntDamage` for blunt blows (1 when
the stuff says nothing; steel says nothing, wood is 0.4 sharp and 0.9 blunt). Fists, teeth and claws are not
weapons and are never scaled.

| Quality | Melee damage and penetration | Gun accuracy | Bullet damage and penetration |
| --- | --- | --- | --- |
| awful | 0.80 | 0.80 | 0.90 |
| poor | 0.90 | 0.90 | 1.00 |
| normal | 1.00 | 1.00 | 1.00 |
| good | 1.10 | 1.10 | 1.00 |
| excellent | 1.20 | 1.20 | 1.00 |
| masterwork | 1.45 | 1.35 | 1.25 |
| legendary | 1.65 | 1.50 | 1.50 |

(RimWorld wiki, Quality.) An item with no `Quality` is normal.

## Guns

The accuracy factor multiplies the gun's hit chance where the shooter's skill, the gun's own accuracy and the
weather already do (`CombatSystem::spread`), so it stays capped at a sure hit (RimWorld: "can't exceed 100%"). The
damage factor is stored on each bullet when it leaves the barrel (`Projectile::damageFactor`) and scales its damage
and penetration on hitting a pawn or a building. Guns are not made of a stuff. No recipe makes a gun yet, so a gun
gets a quality from a script, a trader or a raid's loot; the factors work for any gun item that has `Quality`.

## Save

Section `combat` 2 adds `damageFactor` to a projectile in flight (older saves: 1).

## Not yet

- Hit points of weapons by stuff and quality, repair and upgrades (P4).
- Gun recipes.
