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

## The Unreal half (`USoundSubsystem`, PC V20)

`game/Source/Rim3D/SoundSubsystem.cpp`, smoke test `sound`.

- **Plays:** every sim event goes through `soundForEvent` right after its step; the plays are handled on the next frame.
  A cue is a `FCue` read from the `SoundCues` defs; each play picks one of the cue's cut files at random (one-shots get
  a pitch change of up to 5%), or the def's `tone`, made in memory, when the cue has no file.
- **Voices:** a voice is an audio component playing a procedural wave (`UCueVoiceWave`) that copies the clip's samples
  on the audio thread. `Sound.Voices` (32) at most. Each frame every sound is ranked by `priority`, then by how loud it
  is from the ears; the first 32 have a voice. A one-shot pushed out or never given one is dropped; a loop waits without
  one and picks up where it would be by now when it comes back.
- **Ears:** the overseer camera's focus (300 cm up) while the overseer view is on, else the camera (direct mode, the
  smoke tests' camera). Distance falloff is the square of `1 - distance / range`; panning is Unreal's.
- **Occlusion:** walls (cells whose passage is blocked) on the line from the sound to the ears, plus one when only one
  end is under a roof, at most 3: each makes it 0.55 times as loud and lowers the low-pass (20 kHz / (1 + 3n)).
  Rain and wind are muffled twice when the ears are under a roof.
- **From the snapshot:** footsteps every 0.7 m a pawn walks (animals by body size) within 26 m, by the floor or
  ground under it (`soundStepCue`; upper floors sound like wood); doors on `doorOpen` crossing 5%; building loops
  (`loop` slot) while powered or fuelled, checked every 0.1 s; animals' `idle` calls every 20 to 60 s (Unreal's random).
- **Beds:** the biome's `day` (06:00 to 20:00) or `night`, `indoor` under a thin roof and `cave` under a thick one; the
  weather's `loop` fades in with the transition and the old one out. Thunder follows the sky's lightning flashes by
  0.4 to 2.5 s.
- **Music:** tense with raiders (a pawn of another faction in a squad) or the alarm, combat when a colonist or a raider
  is shooting at someone; it rises at once and falls after 20 quiet seconds. Calm tracks play once with 45 to 150 s of
  quiet between; tense and combat tracks loop until the mood changes (3 s fade). Combat uses the tense tracks while it
  has none. Each Letter event plays a stinger.
- **Mixer:** master, effects (everything else), ambience (`Amb_`, `Weather_`), voices (`Pawn_`, `Animal_`, `Work_Chat`)
  and music, in the overseer's folded "Sound" panel, saved in `GameUserSettings.ini` (`[/Script/Rim3D.Sound]`).
- Console: `Sound.Enable`, `Sound.Master`, `Sound.Effects`, `Sound.Ambience`, `Sound.VoicesVolume`, `Sound.Music`,
  `Sound.Voices`, `Sound.Tones` (0 no tones, 1 short sounds only (default), 2 loops too: a hissing noise bed is worse
  than silence), `Sound.Debug`.

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

## Real sounds (V21, PC half)

`python tools/sound_assets.py` downloaded the ten scriptable packs (2026-10-27; the firearm library is a .7z, unpacked
with 7-Zip) and then cuts what `assets/sound_picks.json` names, with ffmpeg, into
`game/Content/ThirdParty/Sounds/cues/<Cue>/<n>.wav` (mono with a place, stereo heard everywhere, peak at -1 dB, gun
shots trimmed to their first second or two) and `Sounds/music/<calm|tense|stinger>/`. `--cut` cuts again after an edit
to the picks. The game reads the WAV files at the start (about 50 MB, 288 files) and a music track when it starts;
the packaged build stages both folders as plain files (`DefaultGame.ini`).

- **68 of 140 cues have real sounds:** all shots (the firearm library's near-distance takes), reload and switch,
  impacts, flesh hits, swings, clangs, steps on every surface, work, items, doors, power switches, research chime,
  rain and heavy rain (two parts of the one rain recording), the cave bed, pawn hurt and falls (soft thuds, no voices).
- **Still tones (gaps):** explosions, fire, hums, alarm, raid horn, wind and blizzard, thunder, the day and night beds,
  indoor, all 39 animal calls, `Flesh_Burn`, `Short_Circuit`, `Work_Eat` and `Work_Chat`. These need Sonniss GDC or
  Freesound CC0 files by hand (no scriptable link). Loops without a file stay silent by default (`Sound.Tones`).
- **Music:** calm is seven Indie Meditations tracks, tense the three loudest of them (the volcanic sea shore, the royal
  palace, the desert) by measured loudness, not by ear; no combat tracks yet. Stingers: Kenney's steel jingles.
- The picks were made from file names and the packs' notes, not by listening: sneed should judge them in play.
- `CREDITS.txt` in the Sounds folder lists the two CC-BY credits (the footsteps and the music); a credits screen is V24.
