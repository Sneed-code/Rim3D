# Grime: dirt and blood on bodies

sneed (2026-11-16): blood that lingers after a fight like Baldur's Gate 3 until rain or washing takes it off, dirt too,
drying and darkening over about 5 minutes, every creature its own fluid (red, yellow for insects, blue for machines),
and cheap. The ground's side (spray, stains on the ground and walls, the sim's filth piles, rain and cleaning) is in
[blood.md](blood.md). This is what sticks to **pawns: their skin, their clothes and the melee weapon in their hands**.

## The sim (`Grime`, `ColonySim/Grime.h`)

Each pawn has four levels, 0 (clean) to 1: **dirt on the legs**, **dirt on the body**, **blood on the body** and
**blood on its weapon**, and for each blood the Filth kind it is (`Filth::bloodKindOf` of the body it came from) and
the game second it was last fresh. Numbers in the `GrimeRules` def (ours: RimWorld has no grime on bodies). Times are
game seconds; 9,000 is 5 real minutes at 1x.

| What | Effect |
|------|--------|
| Walking on bare soil, sand or rough stone | legs towards `soilMax` 0.5 (dusty), a metre counting as a second of `soilFullSeconds` 7,200 |
| Walking in mud or marsh | legs towards 1 in `mudFullSeconds` 600 (about 600 m) |
| Digging, cutting, harvesting, sowing, building, taking down, taking up floors, butchering, beating out fires (standing at it) | body and legs towards `workMax` 0.7 over `workFullSeconds` 10,800 |
| Downed or dead on bare ground (lying, crawling) | body and legs towards 1 in `groundFullSeconds` 1,200 |
| An injury whose Hediffs def bleeds (`injuryProps.bleedRate` > 0: not bruises or burns) | body blood + `bloodPerDamage` 0.012 per point of damage, its own fluid |
| A melee blow that lands on a pawn (not deflected or blocked) | the attacker's body + `spatterPerDamage` 0.005 per point, the victim's fluid; its weapon + `weaponPerDamage` 0.03 (fists, teeth and claws: no weapon) |
| Rain under the open sky | everything off in `rainWashSeconds` 9,000 at rain rate 1 |
| Ankle-deep water | legs off in `ankleWashSeconds` 600 |
| Waist or chest deep | legs as above, body dirt and blood off in `waistWashSeconds` 900 |
| Swimming | everything, the weapon too, off in `swimWashSeconds` 600 (20 s at 1x) |
| Otherwise | it all wears off in `wearOffSeconds` 172,800 (two game days); a corpse keeps its grime |

A pawn on a floor, indoors or carried keeps what it has. Nothing in the game's rules reads grime (no mood or beauty
effects): it is there to be drawn, and it is saved (section `grime`) and hashed so a loaded game looks the same.

The snapshot gives each pawn `dirtLegs`, `dirtBody`, `blood`, `bloodFilth`, `bloodAge` and `weaponBlood`,
`weaponBloodFilth`, `weaponBloodAge` (ages in game seconds). Tests: `GrimeTests.cpp`.

## On screen (`UGrimeSubsystem`, cosmetic)

One overlay material, `M_GrimeOverlay` (made by `-run=BloodArt` with the blood art), is set as the **overlay material
of every skeletal mesh of the pawn** (the MakeHuman or MetaHuman body, face, clothes), so skin and clothes get the same
patches without touching their own materials. It takes the pre-skinned local position, so the patches stay put on the
body as it moves:

- **Dirt** from the feet up by the legs level (fading out by just above the knee, about half the body's height) plus
  the body level all over, in patches of procedural noise that grow from their low spots as the level rises; a dusty
  brown, matte.
- **Blood** in its own patches, in the fluid's colour drying from its `look.wet` to `look.dry` (the same curve and
  `Blood.DryMinutes` as the stains), glossy while wet, matte once dry.
- The held weapon (`UWeaponDrawSubsystem::GetHandsComponent`) gets the same material with blood only.

The local wound marks, soaked patches and body stains of [blood.md](blood.md) stay as they are and draw on top; they
now also wash off while swimming or wading waist deep.

**Cost:** one dynamic material per pawn shown (and per bloody weapon), updated four times a second; a clean pawn or
one further than `Grime.MaxDistance` (60 m) from the camera has no overlay and costs nothing. Console:
`Grime.Enabled`, `Grime.MaxDistance`, `Grime.Strength` (0 to 2). `Grime.BodyBlood` (0 to 1, default 0): how much of the blood level shows as flecks all over a
body. On the PC (2026-11-18) the flecks read as a cow print on skin and hair, so bodies show blood through the wound
marks of `UBloodSubsystem` alone (docs/design/blood.md); the overlay still draws dirt, and blood on weapons.

Seen on the PC (2026-11-17): with the first numbers a body went solid glossy red after a hit or two. Blood per damage
is now 0.012 (a 12-point bullet 0.14, about seven to fill) and the material's gains keep even a full level patchy (about
half covered); the blood layer also fades 3% every 10 s like the stains. The pre-skinned position reaches the pixel
shader through a vertex interpolator.

## Later

- Animals (`AAnimalActor`) drawn the same way; muddy paws.
- Showers and washing at a basin as a job.
- Footprints in blood and mud.
