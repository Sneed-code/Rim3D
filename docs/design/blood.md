# Blood

sneed (2026-11-16): blood like Cyberpunk 2077 in every fight: real spray that matches the kind of wound, its direction
and its force; stains that stay like Baldur's Gate 3 until rain or cleaning removes them; blood that dries and darkens
over about 5 minutes; dirt drawn too; every creature its own fluid (red blood, yellow insect blood, blue machine fluid);
cheap enough for big fights.

## The sim

- The sim already decides who is hurt, how badly, which wounds bleed (`WoundView::bleeding`), how fast a pawn bleeds
  (`ThingView::bleedPerDay`) and where blood filth lies (Filth, docs/design/room-stats.md). Bleeding pawns drop filth;
  rain and time fade outdoor filth (`filth.disappearsInDays`, `FilthRules.rainWashFactor`); colonists clean it.
- New: each body bleeds its own Filth kind, `Filth::bloodKindOf(pawnDef)`: `race.bloodFilth` if the def names one ("" for
  none), else **InsectBlood** for chitinids (RimWorld's Filth_BloodInsect), **MachineFluid** for mechanical pawns
  (ours: RimWorld's mechanoids do not bleed, but sneed wants blue fluid when they are hit; they have filthRate 0 so they
  drop none on the ground) and **Blood** for the rest.
- New: Filth defs have a `look` for Unreal: `wet` and `dry` linear colours and `fluid` (drawn as wet splats that dry).
  Ours, not RimWorld's numbers. Tests: `RoomStatsTests` "Filth: each body bleeds its own fluid".

## On screen (`UBloodSubsystem`, all cosmetic)

Whoever draws a blow calls `UBloodSubsystem::AddHit(FBloodHit)` with where it met the skin, the way it travelled, the
damage, the sim pawn (if any) and the mesh. Bullet arrivals and melee blows in `UFightSubsystem` do; the creator's
fight demo and the gun range call it the same way (no sim pawn: they pass the fluid and a bleed time). The hit is drawn
at the next tick, when the sim's `PawnInjured` for that step is in, which gives the true kind (Gunshot, Cut, Stab,
Bite, Crush...). Injuries with no bullet or blow (a trap, a blast, dev damage) get blood from the body's middle, a
blast's thrown away from its centre.

**Force** = sqrt(damage / reference), 0.2 to 2.5 (reference: bullet 12, cut and stab 11, blunt 13, bite 9, blast 15).
Counts scale with force, `Blood.Quality` and how full the drop budget is.

| Kind | Spray | Marks on the body |
|------|-------|-------------------|
| Gunshot | small back-spatter cone toward the shooter; through-and-through (force >= 0.55, else by chance): a 22 degree cone of up to ~55 drops and a jet out of the far side, a mist cloud, bits of tissue on heavy hits; else more blood out of the entry | a small hole; a bigger torn exit on the far side |
| Cut | cast-off fan along the blade's path and a tight ribbon of drops | a gash along the blade |
| Stab | short burst out of the wound | a hole |
| Bite | wide burst | a torn patch |
| Blunt | small wide burst (a light blow, under 5 damage, almost none) | a split above 8 damage |
| Blast | all-round spray away from the blast, mist, bits | |
| Burn | none | |

**Drops** fly with gravity and air drag (small drops slow far sooner than big ones), drawn as glossy instanced spheres
stretched along their flight. The bigger ones are traced against the world (320 traces a frame at most); the rest land
on the ground height found once per hit. A drop that lands leaves a splat on ground, walls and things, or a splash on a
body it hits. The splat's length / width = 1 / sin(angle of impact), with the tail ahead (forensic spatter); a big drop
on a wall can run down it. Small splats close together run together into one (a puddle under drips) instead of
another decal. **Mist**: soft camera-facing puffs that spread and fade in under a second.

**For now (sneed, 2026-11-16, after the gun range):** blood only on the hit, no ongoing bleeding effects, so
`Blood.Bleeding` is 0 (the drips, runs, spurts and pools below stay in the code for later), and the tissue bits are off
(`Blood.Bits` 0: they read as low-poly chunks). Each hit leaves the wound mark exactly where it landed and a soaked
patch round it (and round the exit), which lands on clothes where they cover the spot. The spray leans on a fine
pinpoint mist of many small puffs plus small drops.

**Bleeding** (`Blood.Bleeding 1`): each wound that bleeds is stuck to its bone. While the sim says that part's wound bleeds it drips (0.4 to
7 drips a second by its share of `bleedPerDay`), a run grows down the skin while the body is upright, a very bad
gunshot, cut or stab (force >= 1.3, half of them) spurts with the heartbeat (every 0.7 to 0.85 s, weaker as blood is
lost), and a downed or dead body bleeds a pool that spreads (to 40-80 cm by blood loss). A dead body's bleeding runs
down in half a minute. Tended or stopped wounds stop; the run stays as a stain.

**Splatter on the ground at every hit** (sneed, 2026-11-17: "realistic blood splatter, like Baldur's Gate 3 after combat",
not a tint; it must read from afar): besides what the flying drops leave, each hit that draws blood lays a pattern on
the ground at once: a spatter fan 60 to 190 cm across, filled with 4 to 12 round drops, thrown the way the blow went (out of the exit of a shot through,
back toward the shooter otherwise), 3 to 8 slanted drops with tails trailing further out, a few round drops near the
feet and, for a bad wound or a body that is down, a patch under it. These are never culled by distance. Drops' own
splats are bigger too (at least 2.4 cm across). On the body each such hit leaves the wound mark, a larger soaked patch
at full strength, a spatter pattern 14 to 26 cm across and one or two drops with tails; a body keeps 32 stains (400 in
all).

**Fading** (sneed, 2026-11-17): every stain on the world and on bodies, and the blood layer of [grime.md](grime.md),
loses 3% of its strength every 10 seconds from when it was put down (`Blood.FadeStep` 0.03, at game speed), so it is
gone after about 5.5 minutes. The sim's blood piles (Blood, InsectBlood, MachineFluid: `filth.fadeSeconds` 10,200, ours)
follow the same rule, indoors too: a pile is gone 5 min 40 s after blood last landed on it, and its decal fades by the
pile's age from the snapshot (`FilthView::age`). Dirt, vomit and ash keep RimWorld's days.

**Stains stay**: world stains, body stains and pools dry from the fluid's `wet` to `dry` colour over `Blood.DryMinutes`
(5) at 1x, glossy and raised while wet, matte once dry. Outdoors, rain washes a fresh stain away in about 4 minutes of
full rain (ours: spatter is thin), and without rain it lasts as long as the sim's blood filth (6 game days). Indoors it
stays until a colonist cleans that cell (the sim's blood pile there thins or goes: stains within a cell of it go too).
A body's stains wash off while it stands in the rain, swims (about 20 s at 1x) or wades waist deep (twice that). Caps: `Blood.MaxStains` (900) on the world, oldest small spatter
first; 32 per body, 400 on bodies.

**The sim's filth** (blood, insect blood, machine fluid, dirt, vomit, ash, animal filth) is drawn as one decal per pile,
sized by thickness, placed and turned by its cell. Piles there when a game loads are dry; new ones dry like spray.

## Blood in the skin (`USkinBlood`, sneed 2026-11-18)

Decals on a moving body slid, clipped and went into the hair. A MakeHuman body now takes its blood **in its skin
texture**, as Baldur's Gate 3 does:

- Each bloodied body near the camera (at most `Blood.SkinMasks`, 8; 16 MB each) gets a **blood mask**, a 1024 x 1024
  render target in its skin's UV space. R: how much blood, G: how wet, B: a wound's dark open core.
- `-run=BloodArt` bakes one **position map** per body base (`T_SkinPos_<Base>`): the bind-pose position of the skin
  at each texel. A hit is taken back into the bind pose through the bone it moves with (the nearest bone span with a
  physics body, through the mesh's inverse bind matrix), and `M_SkinBloodPaint` adds the marks on the GPU: the wound,
  blood soaked round it, spatter, and runs down from it along the bind-pose "down" of that moment, broken up by noise.
  Only skin near the shot line takes it (not the far side). Because it is painted in body space, seams match and the
  blood bends and stretches with the skin.
- Sizes: gunshot core 1.3 + 0.6 x force cm, soak 4.5 + 3 x force, spatter 8 + 4 x force, run 6 + 6 x force; the exit
  bigger; a cut a short gash along the blade; stab, bite, blast and heavy blunt blows their own. Drops landing on the
  body paint small splashes; a bleeding wound paints its run longer as it bleeds (same pattern each time).
- The skin material (`M_MH_SkinDetail`, the blood added by BloodArt and by MakeHumanArt when it makes it) reads the
  mask: a pink-brown stain where there is little, the body's fluid colour where there is more, **wet: bright, glossy
  and a little raised; dry: dark brown and matte** over `Blood.DryMinutes`; the core of a hole or gash darker and sunk.
  Every 10 s a fade pass takes 3% off (`Blood.FadeStep`) and dries it; the mask goes when it is all gone, when the pawn
  washes (the sim's blood level falls to nothing) or when a newer body needs the room.
- Head and neck wounds darken and redden the hair (its `Tint`), fading the same way. Hair takes no stamps.
- Bodies without a position map (MetaHumans, animals) keep the decals. `Blood.SkinPaint 0` goes back to decals.
- Clothes: each wound also leaves a **cloth spot** (its place in the body's bind pose, radius 1.5 x soak + 2 x core,
  8 newest per body, fading and drying like the skin). `UGrimeSubsystem` gives every mesh but the skin its own copy of
  `M_GrimeOverlay` with the spots (`Spot0-7`, `SpotAmountA/B`), which soaks an uneven dark blot into the cloth there.
  It relies on clothes sharing the body's bind pose. MakeHuman bodies wear no clothes yet (only MetaHumans do, and
  they keep decals, which land on their clothes), so this shows once MakeHuman clothes come.

Splat shapes (PC look, 2026-11-19): drops are uneven round blots with a few short bulges and droplets near them,
slanted drops a rounded body with a short tail, pools many melted lobes with drops on the rim. The old crown of thin
spines with beads read as a cartoon starburst or a virus.
Ground and wall splats (PC look, 2026-11-20): the shared colour keeps a little green and blue so wounds on shaded faces
are not black, and on sunlit ground that read as flat pink paint, so ground and wall decals take `WorldTint`
(Blood.GroundShade 0.6: red x0.6, green and blue x0.33), deep red wet and dark brown dry. They are drawn 1.6x
(Blood.SplatScale), since the round shapes fill less of their square and read as specks from 10-20 m. The sim's filth
stains keep their 1 m cell size. The pool under a bleeding body spreads faster (3-7 cm/s at first) and grows to a
half size of 35-90 cm by blood loss, about a metre across at most, so it shows round a body lying on it (second PC look,
2026-11-20: at 26-52 cm it hid under the body and nothing read from 12 m).

## Assets

`UnrealEditor-Cmd.exe Rim3D.uproject -run=BloodArt` (game/Source/Rim3DEditor/BloodArtCommandlet.cpp) makes
`/Game/Rim3D/Generated/Blood`: `T_BloodAtlas` (4 x 4 splat pictures drawn from shapes in `BloodSplatAtlas.h`: round
drops, slanted drops with tails, fine spatter, pools, a run, a smear, a hole, a gash; R coverage, G thickness, B wound
core), `T_BloodAtlasN` (normals from thickness), `M_BloodSplatter` (the decal; its decal colour's alpha packs wetness
and opacity), `M_BloodDrop` and `M_BloodMist`; `T_SkinPos_<Base>` and `M_SkinBloodPaint` for blood in the skin, and the mask
added to `M_MH_SkinDetail` (with `-force` it is taken out and added again). Without them the fights keep their old simple blood.

## Cost

One instanced mesh each for drops, bits and mist; decals are the main cost, so they are capped and merged; stain
colours update a slice a frame (every stain about every half second, only when it changed). `Blood.Quality 0` for big
fights on a slow PC.

Console: `Blood.Enabled`, `Blood.Quality`, `Blood.MaxDrops`, `Blood.MaxStains`, `Blood.DryMinutes`, `Blood.FadeStep`, `Blood.Filth`,
`Blood.DropScale`, `Blood.Bleeding`, `Blood.Bits`, `Blood.FlipU` / `Blood.FlipV` / `Blood.SwapUV` (if the stain pictures come out mirrored or turned),
`Blood.SkinPaint`, `Blood.SkinMasks`, `Blood.Debug`.

## Later

- Niagara fluid sims for close-ups (the creator) if this is not enough.
- Showers wash bodies (dirt and blood on bodies, clothes and weapons, and washing them in rain and water, are in
  [grime.md](grime.md)).
- Wound decals that change as the sim's wound is tended (bandage) or scars.
