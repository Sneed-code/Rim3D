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

**Bleeding**: each wound that bleeds is stuck to its bone. While the sim says that part's wound bleeds it drips (0.4 to
7 drips a second by its share of `bleedPerDay`), a run grows down the skin while the body is upright, a very bad
gunshot, cut or stab (force >= 1.3, half of them) spurts with the heartbeat (every 0.7 to 0.85 s, weaker as blood is
lost), and a downed or dead body bleeds a pool that spreads (to 40-80 cm by blood loss). A dead body's bleeding runs
down in half a minute. Tended or stopped wounds stop; the run stays as a stain.

**Stains stay**: world stains, body stains and pools dry from the fluid's `wet` to `dry` colour over `Blood.DryMinutes`
(5) at 1x, glossy and raised while wet, matte once dry. Outdoors, rain washes a fresh stain away in about 4 minutes of
full rain (ours: spatter is thin), and without rain it lasts as long as the sim's blood filth (6 game days). Indoors it
stays until a colonist cleans that cell (the sim's blood pile there thins or goes: stains within a cell of it go too).
A body's stains wash off while it stands in the rain. Caps: `Blood.MaxStains` (900) on the world, oldest small spatter
first; 20 per body, 260 on bodies.

**The sim's filth** (blood, insect blood, machine fluid, dirt, vomit, ash, animal filth) is drawn as one decal per pile,
sized by thickness, placed and turned by its cell. Piles there when a game loads are dry; new ones dry like spray.

## Assets

`UnrealEditor-Cmd.exe Rim3D.uproject -run=BloodArt` (game/Source/Rim3DEditor/BloodArtCommandlet.cpp) makes
`/Game/Rim3D/Generated/Blood`: `T_BloodAtlas` (4 x 4 splat pictures drawn from shapes in `BloodSplatAtlas.h`: crowned
drops, slanted drops with tails, fine spatter, pools, a run, a smear, a hole, a gash; R coverage, G thickness, B wound
core), `T_BloodAtlasN` (normals from thickness), `M_BloodSplatter` (the decal; its decal colour's alpha packs wetness
and opacity), `M_BloodDrop` and `M_BloodMist`. Without them the fights keep their old simple blood.

## Cost

One instanced mesh each for drops, bits and mist; decals are the main cost, so they are capped and merged; stain
colours update a slice a frame (every stain about every half second, only when it changed). `Blood.Quality 0` for big
fights on a slow PC.

Console: `Blood.Enabled`, `Blood.Quality`, `Blood.MaxDrops`, `Blood.MaxStains`, `Blood.DryMinutes`, `Blood.Filth`,
`Blood.DropScale`, `Blood.FlipU` / `Blood.FlipV` / `Blood.SwapUV` (if the stain pictures come out mirrored or turned),
`Blood.Debug`.

## Later

- Niagara fluid sims for close-ups (the creator) if this is not enough.
- Blood soaking into clothes as its own layer; blood on weapons; showers wash bodies.
- Wound decals that change as the sim's wound is tended (bandage) or scars.
