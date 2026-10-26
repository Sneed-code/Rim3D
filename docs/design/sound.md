# Sound

Task V20, cloud half. The sim makes no sound. It says what happened (events, snapshot state) and
the defs say what that sounds like. The Unreal half (`USoundSubsystem`, PC task V20) plays the
cues; real sound files come with V21. Until then every cue has a placeholder tone.

## Cues are defs

`data/defs/SoundCues/Cues.json` lists every cue, one def each:

| Field | Meaning |
| --- | --- |
| `loop` | true for a bed or a hum that runs until stopped |
| `volume` | 0 to 1, before the mixer sliders |
| `range` | metres it can be heard; 0 means heard everywhere (ambience, thunder, alarm, horn) |
| `priority` | 0 loudest, never dropped, to 3 first to be dropped when the voice budget (32) is full |
| `tone` | placeholder: `wave` (sine, square, saw, noise), `hz`, `seconds`. Unreal makes it until V21 |

Cue names read `Group_Name`: `Shot_`, `Impact_`, `Flesh_`, `Swing_`, `Step_`, `Work_`, `Door_`,
`Hum_`, `Fire_`, `Animal_<Family>_Idle|Hurt|Death`, `Weather_`, `Amb_`.

## `visual.sound` slots

A def names its cues under `visual.sound.<slot>`. Inheritance merges it like the rest of `visual`,
so a base def sets a default and a child overrides one slot. `tools/check_defs.py` fails on a cue
that is not in `SoundCues`, and on a cue nothing uses (events below count as users).

| Def kind | Slots |
| --- | --- |
| Gun (Things) | `shot` per gun, `reload`, `switch` (on `GunBase`). Turrets: `shot` on the turret's gun def |
| Melee weapon | `swing`, `hit` (flesh) |
| DamageTypes | `hit` (what the bullet or blow sounds like in flesh), `explosion` (blast cue) |
| Terrain and floors | `step`: grass, dirt, sand, stone, tile, wood, carpet, mud, water |
| Door | `open`, `close` |
| Work buildings and benches | `work` (a smithy rings, a stove sizzles) |
| Power, climate and fire buildings | `loop` while running (generator, heater, wind turbine, campfire, torch) |
| Human and animal pawns | `idle` (animals), `hurt`, `death`, `downed` (humans), `burning` |
| Weathers | `loop` (rain, heavy rain, wind, blizzard), `thunder` |
| Biomes | `day`, `night` (ambience beds), `indoor`, `cave` |

## Events to cues (`Sound.h`, `soundForEvent`)

Plain C++ in the sim library, tested, so Unreal only plays what it is told. It takes the event and
a callback that gives a thing's def (for events that name only an entity). The result is a list of
`SoundPlay`: cue, `Once`/`LoopStart`/`LoopStop`, a position or an entity to follow, or neither (heard
everywhere).

| Event | Cue |
| --- | --- |
| `ShotFired` | the gun's `shot` at the muzzle |
| `ProjectileHit` | pawn: the projectile's damage type `hit`; armor, building, ground, rock: `Impact_*`; gone: none |
| `MeleeSwing` / `MeleeHit` | the weapon's `swing` / `hit`; deflected `Melee_Clang`, blocked `Melee_Block`, dodged `Dodge`, missed none; fists `Punch`, teeth `Flesh_Bite` |
| `Explosion` | the damage type's `explosion` (default `Explosion_Blast`) |
| `GrenadeThrown`, `GrenadeLanded`, `TrapSprung`, `TrapRearmed` | `Grenade_*`, `Trap_*` |
| `PawnAction` | work cue for the action (a bench's `work` slot for cook, research, butcher), pick up and put down, `Reload` and `Switch` from the gun's slots, roll and dodge |
| `PawnInjured`, `PawnPartLost`, `PawnDowned`, `PawnDied` | the pawn def's `hurt`, `downed`, `death` |
| `PawnCaughtFire`, `PawnFireOut` | `burning` loop start and stop on the pawn |
| `FireStarted`, `FireOut` | `Fire_Ignite` and a `Fire_Burning` loop at the cell; `Fire_Out` when it ends |
| `PowerChanged`, `ShortCircuit`, `RoofCollapsed` | `Power_On/Off`, `Short_Circuit`, `Roof_Collapse` |
| `AlarmChanged`, `RaidArrived`, `ResearchDone` | the `Alarm` loop; `Raid_Horn`; `Research_Done` (the last two heard everywhere) |

Not events, so Unreal reads them from the snapshot each frame: **footsteps** (pawn speed and the
terrain under the foot: `soundStepCue`), **doors** (`doorOpen` going from shut to open and back:
`open`, `close`), **machine hums and fire loops** (a running building's `loop` slot), animals'
**idle calls** (random, by the animal's `idle` slot, from Unreal's own random: cosmetic), and the
**weather and ambience beds**.

## The Unreal half (PC, later)

- `USoundSubsystem` reads the snapshot's events each step, runs them through `soundForEvent`, and plays.
- Distance attenuation from `range`; a voice budget of 32 where the lowest `priority`, then the
  farthest, is dropped first.
- Occlusion: a sound whose line to the listener crosses roofed cells or walls is muffled (low-pass
  and quieter), by the sim's own cell data.
- Ambience: one bed fades in by biome and time of day (`day`, `night`), `indoor` under a roof,
  `cave` under a mountain roof, and the weather `loop` on top of it, louder with the weather's strength.
- Mixer: sliders for master, effects, ambience, voices and (V21) music, saved in the settings.
- Placeholder tones until V21, then the real files by cue name.

Not here: music (V21), a sound for every def (the cues above are the first set; new defs add a slot
and a cue and `check_defs.py` keeps the two in step).

## Real sounds (V21, cloud half)

`assets/sound_sources.json` lists the free packs (licence, page, direct link) and a `cueMap` that
sends every cue to the packs to cut it from; `python tools/sound_assets.py --check` fails if a cue
has no row (it runs in CI) and `--list` prints the table. `python tools/sound_assets.py` downloads
the packs into `game/Content/ThirdParty/Sounds/<id>/` (needs internet, so run it on the PC; the cloud
container cannot reach these sites) and writes `CREDITS.txt` for the CC-BY ones.

- Covered by CC0 packs: guns (OpenGameArt firearm library), impacts, swings, doors, items (Kenney),
  rain and thunder, cave bed, short stingers.
- Needs a Sonniss GDC pack or Freesound CC0 by hand (no scriptable link): fire, hums, alarm, horn,
  explosions, wind, blizzard, day and night beds.
- **Gap:** no free animal call pack was found (39 animal cues); they keep placeholder tones until
  someone picks calls from Freesound (CC0 filter) or Sonniss.
- Music: Indie Meditations (CC-BY 4.0, credit needed) for calm play, Kenney jingles for stingers;
  danger music is still open.
- Nothing was downloaded or listened to in the cloud. The PC half (V21) imports the files, cuts them
  to the cue names and checks how they sound.
