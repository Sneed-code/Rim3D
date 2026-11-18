# Shields

sneed's spec (2026-11-15, thread "Add shields with active blocking"). Not in RimWorld (its shield belt is a
different thing and still does nothing, BACKLOG). Builds on [armour.md](armour.md).

Code: `ShieldRules`, `Blocking`, `ApparelStats::isShield`, `catchChance`, `shieldBlock`, `twoHanded`, `activeShield` in
`ColonySim/Apparel.h`; `CombatSystem::armorPass`, `setBlocking`, `stepBlocking` in `Private/Combat.cpp`;
`FighterSystem::shieldAct` in `Private/Fighters.cpp`. Tests: `Tests/ShieldTests.cpp`.

## What a shield is

A shield is armour in its own slot (`gear.slots` `["Arm"]`, so a pawn wears one) with `gear.shield.catchChance`.
Three sizes, each made of one of four tiers of material (sneed, 2026-11-16: primitive, steel, composite, tech): wood,
steel, composite (ours: modern plastic, half steel's weight) and plasteel. 12 shields in all. Uranium makes none.
Defs in `data/defs/Things/Shields.json`.

- **Protection** works like armour (divides every hit, wherever it lands) while the shield is on the arm. A tower shield
  equals a chest piece of its tier: the defs hold steel's numbers (steel plate armour's 20 / 30 / 10), each size down
  halves them, and the material scales them as it scales plate armour (`armourStuff`). A material's offset (wood's -10%
  fire) is scaled by the size too (`gear.share` 0.25, 0.5, 1).
- **Weight**: 3 / 6 / 12 kg for steel, times the material's `shieldStuff.mass` (wood 0.8, plasteel 0.7: RimWorld's
  material weights against steel's; composite 0.5, sneed's "50% lighter"). It counts as armour: walking speed / (1 + kg / 100), attacks take
  (1 + kg / 100) times as long, work a quarter of that.
- **Durability**: 70 / 140 / 280 hit points for steel, times the material's hit point factor; it wears like any armour.
- **Hands**: a weapon with `twoHanded` (every rifle, shotgun, SMG, LMG and minigun, every bow, the launchers, the
  longsword and the monosword) slings the shield on the back. Slung, it still weighs but neither protects nor blocks,
  and the AI doesn't pick up a shield while it holds one. Switching to a one-handed weapon (pistols, knife, club, mace,
  spear, gladius, axes, pila) brings it back to the arm. The game has no dual wielding yet; it will count as two-handed.

| Material (block, kg factor) | Small | Medium | Tower |
| --- | --- | --- | --- |
| Wood (1, 0.8) | 5 / 4.5 / 0, fire -2.5 | 10 / 9 / 0, fire -5 | 20 / 18 / 0, fire -10 |
| Steel (2, 1) | 5 / 7.5 / 2.5 | 10 / 15 / 5 | 20 / 30 / 10 |
| Composite (2.5, 0.5) | 5 / 7.5 / 2.5 | 10 / 15 / 5 | 20 / 30 / 10 |
| Plasteel (3, 0.7) | 6 / 9.4 / 2.75 | 12 / 18.75 / 5.5 | 24 / 37.5 / 11 |

Catch chance: small 60%, medium 80%, tower 95%.

## Blocking

Blocking is held (`Blocking` component; `ShieldCommand{pawn, up, hasDir, dirX, dirY}`). With the shield up the pawn can
walk and reload but can't shoot or swing ("holding up a shield"). It comes down by itself when the shield is no longer on
the arm (dropped, or a two-handed weapon taken up), or the pawn is downed, knocked down or stunned.

A hit that comes from the front (within `ShieldRules.arcDegrees`, 60, either side of where the pawn faces) lands on the
raised shield with its catch chance, whatever the damage type. Then, before armour:

- damage x (1 - `blockCut`, 0.4), minus the material's flat block (`shieldStuff.block`: wood 1 to plasteel 3);
- a hit of `floorBaseDamage` (6) or more keeps at least `floorDamage` (1); a smaller one can be stopped entirely
  ("blocked" for a melee hit, `ProjectileHit` `Armor` for a bullet);
- the shield wears for what it stopped (1 hit point's worth per 4, as armour); event `ShieldBlocked` (`thing` the pawn,
  `target` the shield, `amount` taken off, `message` "stopped" or "softened");
- what is left goes through armour as usual, the shield's own % included.

Where the hit comes from: the bullet's flight, the attacker of a blow, a blast's centre (each fragment rolls).
Flames, traps and other hits from nowhere never land on a shield. A pawn with its shield up never parries with its weapon as well (`MeleeSystem::prepareDefense`): the shield takes
the blow. The roll is made only for a pawn with its shield up,
so games without shields draw the same numbers as before.

The player's block faces where the pawn faces, or the way the command gives (the camera). The fighter AI's faces the
attacker it raised it at.

## The fighter AI

Every fighter with a shield on its arm (not one the player walks) raises it when it sees an enemy aiming or firing at
it, or winding up a swing at it, and keeps it up `aiHoldSteps` (15) after the last one, facing that attacker. It lowers it
as soon as it can strike itself: a gun ready with a shot to take, or a melee weapon ready with an enemy in reach. So a
mace-and-shield fighter walks up under fire behind its shield and swings whenever its mace is ready, and a pistol
fighter shoots, then hides behind the shield while it cools down or reloads. A shield the player raised is left to
the player.

## Making them

One recipe per shield, 30 / 60 / 120 of the material (`data/defs/Recipes/WeaponAndArmorRecipes.json`; work and cost
ours):

| Tier | Small and medium | Tower |
| --- | --- | --- |
| Wood | crafting spot or smithy, no research (the tropical start) | smithy, Plate armor |
| Steel | smithy, Smithing | smithy, Plate armor |
| Composite | machining table, Flak armor | machining table, Flak armor |
| Plasteel | machining table, Shields | machining table, Shields |

Composite (`Composite`, stuff category Composite, `data/defs/Things/Items.json`) is made at a machining table with Flak
armor: 10 steel and 10 chemfuel make 20. It takes steel's armour factors; flat block 2.5.

## Snapshot and saving

`ThingView.shieldUp` (and `facingX/Y` is where it points), `ThingView.shieldSlung`, and `ThingView.shield` /
`shieldStuff` (the shield's def and material, for drawing it). The `Blocking` component is saved in section `apparel` 4.

## On screen (Unreal)

`UWeaponDrawSubsystem` (`game/Source/Rim3D/WeaponDrawSubsystem.cpp`):

- **Right mouse button.** In direct mode, with a shield on the arm, holding the right mouse button sends
  `ShieldCommand{up, camera direction}`; it is sent again when the camera turns more than 15 degrees (at most five
  times a second), and letting go sends the shield down. Without a shield the button does what it did before (a gun
  aims, a melee weapon blocks). Leaving direct mode, picking up a building, or taking over another colonist puts the
  shield down. With a shield on the arm a one-handed gun doesn't aim on the right button (`AProtagonistPawn`).
- **The shield is drawn** with its real model for its material (`visual.models`, below); when that model hasn't been
  made, with the def's `visual.parts` (a plate and a boss) tinted the material's colour (plasteel has a fallback colour
  in code). It hangs on the left arm facing
  out, swings up in front of the chest while `shieldUp`, and rides on the back while `shieldSlung`. It is hidden on a
  downed, dead or ragdolled pawn and with the look-only `hideGear` switch.
- **Pose.** While the shield is up the body takes the block lean (`ASimPawnActor::ApplyBodyPose`) and the pawn turns
  to face where the shield points.
- **Hits.** `ShieldBlocked` throws sparks off the shield's face (`UFightSubsystem::SpawnMeleeImpact`, Armor) and a
  softened blow plays the block clang (`Melee_Block`, `Sound.cpp`).
- Smoke test `melee`, phase `shield`: a steel medium shield on the protagonist, the right button held from code
  (`ScriptRightMouse`), a clubber beats on it, then it is let go.

## Models

12 models, one per material and size (`SM_Shield_<Wood|Steel|Composite|Plasteel>_<Small|Medium|Tower>`), made by
`tools/shield_models.py`, a Blender script Codex wrote for sneed (2026-11-16/17): real size in metres, face along +X,
width along Y, up along Z, origin in the middle where the forearm sits; a grip across the forearm and a strap 25 cm
toward the elbow on the back. Wood: planks, iron rim, rivets and boss. Steel: brushed plate, rivets, boss. Composite:
a charcoal riot/ballistic shield with a rubber edge (the tower has a viewport). Plasteel: dark frame with pale plates
and cyan light lines. Each is 60 to 400 KB, under 3000 / 4000 / 6000 triangles.

    blender -b --python tools/shield_models.py -- game/Content/ThirdParty/Shields
    UnrealEditor-Cmd game/Rim3D.uproject -run=ShieldArt -unattended -nosplash -AllowCommandletRendering

The commandlet (`game/Source/Rim3DEditor/ShieldArtCommandlet.cpp`) imports each with its own materials into
`/Game/Rim3D/Generated/Shields/<name>/<name>` (not in Git). The smoke test `melee` (phase `shield`) checks the model
is drawn and stands the right way up (thin along x, about 60 x 75 cm for the medium).

## Not done yet

- A proper raised-shield animation for the body (now the procedural block lean); shields sized to the wielder (thread
  "Scale weapons and gear to wielder size"). The plasteel and composite models could use a pass by hand in Blender.
- In Unreal the imported models draw near-black although the .glb colour maps are right (wood is brown in the file),
  and the raised plate may sit rolled 30-40 degrees: one look in the editor at the imported material and the raised
  rotation is due.
- Raiders and traders don't carry shields yet (no pawn kind lists them).
- Shield bash, and blocking with the weapon (melee.md) together with a shield.
