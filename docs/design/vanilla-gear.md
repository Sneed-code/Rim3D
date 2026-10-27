# Vanilla weapons, armor and belts

Task: check every base-game weapon and armor piece exists (thread "Check all vanilla weapons and armour exist").
Data: `data/defs/Things/WeaponsMore.json`, `ExplosivesMore.json`, `MeleeUltratech.json`, `ArmorMore.json`,
`Materials.json`, `BuildingsFabrication.json`, `Projectiles/BulletsMore.json`, `Arrows.json`, `Rockets.json`,
`GrenadesMore.json`, `DamageTypes/DamageTypesMore.json`, `Recipes/WeaponAndArmorRecipes.json`. Tests:
`Tests/VanillaGearTests.cpp`. Numbers are the RimWorld wiki's (cooldowns and warm-ups in ticks halved to steps,
work in ticks times 1.44 like the older recipes); what the wiki page did not give is in `unverified` or `ours`.

## What there is now

- **Guns** (all with a bullet, recoil kicks, a magazine and a reload time of ours): revolver, autopistol, machine pistol,
  pump shotgun, chain shotgun, bolt-action rifle, assault rifle, heavy SMG, LMG, minigun, sniper rifle, charge rifle,
  charge lance; short bow, recurve bow, greatbow (arrows do the new `Arrow` damage, a stab) and the pila; the doomsday
  and triple rocket launchers (not craftable), EMP, incendiary and smoke launchers; frag, EMP and molotov grenades.
- **Melee**: knife, club, gladius, spear, mace, longsword, breach axe as before, and the ultratech monosword, plasma
  sword and zeushammer (trade only, no stuff).
- **Armor**: flak vest, pants, jacket, helmet; simple helmet; plate armor (any metal or wood); marine, recon and
  cataphract armor with their helmets; shield belt, smokepop pack and firefoam pop pack (belt layer, waist).
- **Materials**: plasteel and uranium (both metals, so swords and armor can be made of them), gold, chemfuel and the
  advanced component. The fabrication bench (research Fabrication, 5 by 2, 250 W) makes the plasteel gear and the
  advanced component; the machining table the guns, grenades, flak helmet, shield belt and pop packs; smithies the
  bows, pila, plate armor and simple helmet; tailor benches the flak vest, pants and jacket.
- **Research**: new projects `MarineArmor`, `ReconArmor`, `PulseChargedMunitions`, `CataphractArmor` (the wiki page
  with their costs was cut off: cost and prerequisites are ours; cataphract armor needs two techprints in RimWorld,
  here it follows marine armor). Every recipe is unlocked by the project the wiki names.

## What the sim does with them

Everything that was already in the sim works for the new guns, bows and armor with no change: burst, cooldown, accuracy,
armor layers and wear, stuff factors. One thing is new: a projectile def with an `explosion` block (rockets, shells)
bursts where it ends, on a pawn, a wall, the ground or at the end of its flight, through the same blast as a grenade
(`CombatSystem::impact`, `explode`). Its own `projectile.damageAmountBase` is 0; a shot with no damage and no blast
hurts nobody (the EMP and smoke shells).

Not in the sim yet (the defs carry the numbers; see `BACKLOG.md`): shield belt blocking, pop packs, EMP stun, smoke
clouds, fires from incendiary and molotov (they do 10 burn damage in the blast), single-use launchers, the launchers'
forced miss radius. Raiders, traders and outfit policies still use only the older guns and armor.

## Numbers worth a second look

Charge rifle, charge lance and sniper bullet speeds, arrow speeds, the incendiary radius, the grenade and flak jacket
crafting costs (the wiki extract was ambiguous) and the research costs above.
