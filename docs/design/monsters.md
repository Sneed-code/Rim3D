# Mechanoids and chitinid hives

Code: `ColonySim/Monsters.h`, `Private/Monsters.cpp` (`MonsterSystem`); small changes in `Combat.cpp`
(natural armor), `Health.cpp` (`race.mechanical`) and `Melee.cpp`. Tests: `Tests/MonsterTests.cpp`.
Data: `Things/Mechanoids.json`, `MechanoidWeapons.json`, `Chitinids.json`, `Hives.json`,
`Projectiles/MechBullets.json`. Names are in [naming.md](naming.md): mechanoids are the Remnant, insects
are chitinids, a hive is a brood mound (`Hive`), insect jelly is brood jelly (`InsectJelly`).

## What there is

| Def | RimWorld name | Health scale | Size | Speed (cells/s) | Armor sharp / blunt / heat | Power | Weapon |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `Mech_Scyther` | scyther | 1.32 | 1 | 4.7 | 40% / 20% / 200% | 150 | two blades (20 cut and stab, 30% AP) |
| `Mech_Lancer` | lancer | 0.72 | 1 | 4.7 | 40% / 20% / 200% | 190 | charge lance (30 damage, 45% AP, 32.9 m) |
| `Mech_Pikeman` | pikeman | 0.85 | 1 | 2.5 | 40% / 20% / 200% | 110 | needle gun (15, 35% AP, 44.9 m) |
| `Mech_Centipede` | centipede (blaster) | 4.32 | 3 | 1.9 | 72% / 22% / 200% | 400 | heavy charge blaster (24 shots of 15, 27 m) |
| `Megascarab` | megascarab | 0.4 | 0.2 | 3.75 | 72% / 18% | 40 | mandibles 5, head 4 |
| `Spelopede` | spelopede | 1.7 | 0.8 | 3.65 | 18% / 18% | 75 | claw 7, head 6 |
| `Megaspider` | megaspider | 2.5 | 1.2 | 3.6 | 27% / 18% | 150 | claw 12, head 7 |

All from the RimWorld wiki (pages of each, 2026-10-26). Ours: `race.combatPower` and `race.weapon` on the
mechs (the wiki's combat power, a def field for what they carry), the mech and bug body shapes (the
human body for scyther and lancer, a four-legged one for the rest), the missing needle gun and blaster
shot spacing (4 steps), and the insects' single life stage and no hunger.

## What changed in the sim

- **Natural armor** (`race.armor.sharp|blunt|heat`): the innermost layer of the armor roll (below any
  apparel), same rule as apparel: armor less penetration is the share of hits it changes, half of those
  bounce, the rest do half damage and a sharp hit turns blunt. Nothing wears out.
- **`race.mechanical`**: no pain and no bleeding. (Bleeding and pain are cut in `HealthSystem::derive`;
  a mech still goes down from lost legs and dies from lost vital parts. It has the flesh body's part
  names; a machine body with its own parts is for art/health later.)
- Pawns with `race.intelligence: "Mechanoid"` use the animal reach rule in melee (reach by body size).

## `MonsterSystem` (no state, so nothing in the save)

`StartThreatCommand{kind, points, edge}`, "Infestation" or "RemnantCluster", points 0 meaning what the
colony is worth now (the raid rule). The narrator's T3 incidents (`Infestation`, `RemnantCluster`) can call
`sim.monsters().infest(points, error)` and `.remnantCluster(points, edge, error)`.

**Infestation.** One brood mound for every 500 points (1 to 4), on a roofed cell 20 to 60 m from the
middle of the colony and at least 20 m from every colonist (open ground if nowhere is roofed); the points
buy insects like raiders by combat power, who come out as manhunters after the nearest colonist.
**The narrator's** incident (C25) is RimWorld's: the first mound comes up only **under overhead mountain** (a thick
rock roof, `Roof::Thick`), a random such cell in that ring, and the incident fails ("no overhead mountain near the
colony") when there is none. Before, it came up under the colony's own built roofs, or on open ground, and in the
year run it wiped out all three colonies between days 34 and 45. The dev command (`StartThreatCommand`) still
takes any roof or open ground.

**Mounds** (`Hive` building, 130 hit points, RimWorld's): check every 30 game seconds; each mound's
timings come from its entity id and the clock, so they need no saved state:
- **Jelly**: 20 every 26,000 to 30,000 RimWorld ticks (0.433 to 0.5 day) while fewer than 40 lie on the
  cells around it. A destroyed mound leaves 30.
- **Brood** (ours): one new insect every 0.4 to 0.6 day while the map has fewer than 6 per mound.
- **Spread** (interval ours, 2 to 3.5 days): a new mound on a random roofed free cell within 10 m, not
  touching another mound, up to 30 mounds.
- **Insects hunt** while a colonist is within 60 m: every check, an insect that is not already chasing
  is enraged (`AnimalSystem::enrage`, Manhunter) at the nearest colonist. Insects are animals, so they
  walk straight lines (no pathing round walls yet) and flee nothing.

**Remnant cluster.** Mechs bought with the points (random among those still affordable, at least one) walk
in at an edge cell that can reach the colony, 3 m apart in a block. Each gets the `Remnant` faction,
its weapon, and a `Fighter` that advances to the colony: the fighter AI, alarm and defense roles treat
them like raiders. They never rout or give up.

## Not done

- Nobody attacks a mound on purpose (colonists only shoot pawns); stray shots, fire and blasts hurt it.
  A "destroy the hive" order is a backlog item.
- No letter or narrator hook yet (the `Infestation` and `RemnantCluster` incident defs exist, T3 sends them).
- Dormant hives in caves, jelly eating and the jelly recreation, centipede burner and gunner variants,
  termites, EMP stun and fire immunity of mechs, mech death wrecks.
- All numbers marked `unverified` in the defs are recalled; the mech guns' spacing and the brood and
  spread intervals are ours.
