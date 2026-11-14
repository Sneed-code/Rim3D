# Animation plan: people and tropical animals

Written 2026-11-14 from sneed's list. It details and replaces the art stage's real-animation tasks
**V11 to V14** and the realistic-animals task **L9** (`art-stage.md`); those rows now point here.
Everything in it is adult and non-explicit: lovin is a couple lying under the covers in a cuddling
pose with a hearts icon (no nudity, no sex positions), showering is hidden by steam and frosted glass.

## Where we start

- **Bodies.** The character creator (branch `makehuman-characters`, thread "Make real MetaHuman
  nude bodies") builds every pawn from MakeHuman through MPFB2 on **one skeleton**,
  `game_engine_with_breast`. It uses the UE4 mannequin's bone names, plus breast, buttock and genital
  soft bones and joined hair bones. `MakeHumanArtCommandlet` already builds the IK rig and an IK
  retargeter from the UEFN mannequin (Epic's Game Animation Sample) to it. So **every animation is
  retargeted once, to that one skeleton, and every body, man or woman, baseline, imp or orcoid, plays
  it.** Body proportions and morphs ride on top.
- **Locomotion.** The 20 nearest pawns are motion matched from the Game Animation Sample (GASP);
  the rest play walk, run and idle loops (`pawns.md`). GASP also has crouch movement, starts, stops,
  pivots, jumps and traversal (vaults, hurdles, mantles).
- **Fights.** Melee is procedural key poses (`melee-draw.md`), the roll is a somersault, hits are a
  lean, downed and dead pawns are ragdolls (`fights.md`). Guns float at the chest. Work has no
  animation (`handson.md`).
- **Animals.** Quaternius cartoon models (`assets/animal_art.json`), each with its own skeleton.
  Capybara, rhinoceros, monkey and elephant have no model at all and are drawn as huskies (BACKLOG).
- **The sim already says most of what to play:** `PawnAction` events (`Events.h`: pick up, eat, lie
  down, mine, cut, reload, throw, block, roll, lift and lay a downed pawn, tend, chat...), the stance
  (standing, crouching, prone), aiming, recoil kick, melee swing timing, downed and dead. What is
  missing is listed under "Sim hooks".

## How the animation is put together

**The sim decides, Unreal shows.** Every clip is cut or sped up to the sim's own timing: a pick hits
on the sim's `Mine` event, a punch lands on the sim's hit step, a bow looses on `ShotFired`, a
grenade leaves the hand on `GrenadeThrown`. An animation never moves a pawn somewhere the sim did
not put it; a stumble or a knockdown that travels ends where the sim's position is.

Each pawn's pose is built in layers, from the bottom up:

| Layer | What it does | Driven by |
| --- | --- | --- |
| 1. Base | locomotion (motion matching), or a posture: sit, lie, sleep, downed, crawl | sim speed, route, stance, posture |
| 2. Grip | the upper body from the spine up holds the item the right way (see "Grips") | the held item's `visual.grip` |
| 3. Action | the one-off clip: a swing, a reload, a bite of food, a throw, a stagger | `PawnAction` and fight events |
| 4. Additive | breathing, aim offset (up/down, left/right), recoil kick, hit flinch, tired or hurt slouch | sim aim, `kickUp`/`kickSide`, pain, rest |
| 5. IK | hands snap to the item's grip points, feet on the ground and steps, head and eyes look at the target | sockets on the item mesh, ground trace |
| 6. Physics | partial ragdoll on hits (the hit bone gives, then recovers), jiggle (V6), hair and cloth ends | hit events, Physics Control |
| 7. Face | emotion morphs, blinks, gaze, talking mouth | mood, pain, interactions (see "Faces") |

**Code shape.** All the choosing lives in a C++ anim instance (`URim3DPawnAnimInstance`). The anim
graph itself (the node wiring for layers, IK and physics) is one Animation Blueprint, the kind of
unavoidable glue CLAUDE.md allows. A Python editor script builds it, so it is never hand-edited.
Which clip plays for what is data: a `visual.anim` table per `PawnAction`, per grip and per
interaction, read by the anim instance. A new weapon or job only needs a data line.

## Where the clips come from

Ranked by what to reach for first. Nothing paid unless sneed says so.

| Source | Licence and access | Good for | Notes |
| --- | --- | --- | --- |
| **Game Animation Sample** (GASP) | Epic content, free, already installed | locomotion, crouch, jumps, vaults, hurdles, mantles | already retargeted |
| **Lyra Starter Game** | Epic content, free on Fab | pistol, rifle and shotgun: idle, aim offsets, fire, reload, equip, jog | UE5 mannequin skeleton, so it retargets like GASP |
| **Paragon characters** | Epic content, free on Fab (sneed clicks "Add to library"; checked at task start) | bow (Sparrow: draw, hold, loose), two-hand swords and hammers (Kwang, Greystone), heavy hits and knockdowns | each hero has its own skeleton, retargeted once |
| **Mixamo** | free, needs an Adobe sign-in; free to use in games, but the files may not be shared on their own | the bulk: boxing (jab, cross, hook, block, hit reactions), sitting, lying, sleeping, eating, crying, laughing, talking gestures, knockdowns and get-ups, stumbles, injured walks, crawling, carrying, throws, pick swings, great sword pack, cover and parkour moves | **files never go in Git** (the repo is public); downloaded on the PC like the other packs, logged in `assets/manifest.json` |
| **CMU motion capture database** | free for any use, no account | backup for everyday motions (sitting, eating, boxing, washing) | raw mocap, needs cleaning in Cascadeur |
| **Bandai Namco motion dataset** | CC BY-NC 4.0 (fine: the game is personal and never sold), no account | the same walk and actions in moods: angry, sad, happy, tired | emotional walk variants |
| **Cascadeur** | free for indies (the free tier limits scene length; checked at task start) | game-specific moves nobody sells: the back-to-back carry, the shower wash, pick swings at our timing, cleaning up mocap | keyframing with physics help (AutoPhysics); Claude writes the shot list, sneed or Claude poses |
| **Mocap on sneed's PC** | free, see next section | anything sneed wants to act out: flirting, giggling, a particular punch, eating at a table | optional; needs a phone or webcam |
| **Procedural** | our code | aim offsets, recoil, bow draw strength, hand IK, foot IK, look-at, breathing, flinches, partial ragdoll, blinks, talking mouth | covers whole families with no clips |

## Free motion capture on sneed's PC

sneed asked what free, open-source mocap would run on the PC, after FlowHMR's sign-in got stuck.

| Tool | Input | Account needed? | Quality | Verdict |
| --- | --- | --- | --- | --- |
| **GVHMR** (Zhejiang University, open source) | one phone video | **yes, indirectly**: like FlowHMR, WHAM and 4DHumans, it needs the SMPL body model files, which need a free sign-up (email and password) at the SMPL website. That sign-up is probably what got stuck. | best single-video results of the free tools: steady feet, real-world motion | try it if the SMPL sign-up works |
| **FreeMoCap** (open source, pip install) | 1 to 3 ordinary webcams or phone videos | **no** | good body, rough hands and fingers; more cameras means better | **the no-account route**; start here if the sign-up is the problem |
| Rokoko Vision, Move.ai, DeepMotion | phone video, uploaded to their cloud | yes, free tiers with limits | good | fine for a few clips, but the video leaves the PC |

**The RTX 5080 catch.** The 50-series cards need a recent PyTorch (CUDA 12.8 or newer). Most
research repos pin an older one that will not run on the card, so the install has to swap in the new
PyTorch. The mocap task does this for sneed, through a Remote Control session, so sneed does not
have to fight the install.

**The route from video to game:** record (plain clothes that show the body's shape, full body in
frame, phone on a stand, good light) → GVHMR or FreeMoCap → BVH or FBX → Blender script retargets
onto the MPFB skeleton (`tools/mocap/`) → Cascadeur cleans foot sliding and hands → the import
commandlet takes it like any other clip. Fingers and faces never come from body mocap: hands use
grip poses, faces use the face system below.

**When to use it:** only for gaps the libraries leave (the shower wash, a seated eating style, flirt
and giggle body language, fights in sneed's own style). Everything else comes from the libraries, so
mocap stays optional.

## Grips: one set of animations per way of holding, not per item

Each held thing names its grip in data (`visual.grip`) and carries sockets on its mesh: `gripR`,
`gripL` (the hand placements), `muzzle` or `tip`, and `string` for bows. The grip picks the upper-body
set; the hands are pinned to the sockets by IK, so a short axe and a long pick share one set and
still hold right.

| Grip | Items | Set it plays |
| --- | --- | --- |
| `Unarmed` | nothing held | arms free, GASP's base |
| `Fists` | fists, knuckle weapons | boxing guard, jabs, crosses, hooks, strong blows, hold-to-block |
| `OneHandTool` | hammer, knife as a tool, hoe | one-hand work swings |
| `TwoHandTool` | pick, axe, sledge | two-hand overhead and side swings |
| `OneHandMelee` | knife, club, gladius, mace | slash, stab, overhead; block with the blade |
| `TwoHandMelee` | longsword, breach axe, spear (thrust set) | heavy swings, thrusts, two-hand block |
| `Pistol` | revolver, autopistol, machine pistol | two-hand pistol aim (one-hand when the other hand is busy) |
| `Rifle` | bolt-action, assault rifle, shotguns, SMG, sniper, charge rifle | shouldered aim, low ready, reloads by type |
| `Heavy` | LMG, minigun, launchers | hip-braced carry and fire |
| `Bow` | short bow, recurve bow, greatbow | draw, hold, loose, recoil |
| `Throwable` | grenades, pila, molotovs | wind-up, throw on the release event |
| `CarryItem` | hauled stacks | two arms under the load, size from the stack |
| `CarryPawn` | a downed person | the carrier's half of the carry (see "Downed and carried") |

## The animation list

"Source" is where the first version comes from; "When" is the sim signal that plays it.

### Moving, posture and the body at rest

| Animation | When | Source |
| --- | --- | --- |
| Walk, run, sprint, starts, stops, turns | sim speed and route | GASP (done) |
| Mood walks: tired, sad, angry, hurt limp | rest low, mood, a leg injury | Bandai Namco styles, Mixamo injured walks |
| Crouch and crouch-walk | stance Crouching | GASP |
| Prone, crawl | stance Prone; downed and crawling | Mixamo |
| Jump | Space (protagonist) | GASP |
| Sit down, sit idle, stand up (chair, stool, bench, ground) | new posture `Sit` (sim hook) | Mixamo |
| Lie down, lie idle, get up (bed, ground) | `LieDown`, `GetUp`, posture `Lie` | Mixamo |
| Sleep: eyes closed, slow breathing, turning over now and then | asleep | Mixamo plus breathing additive and closed-eye face |
| Sick in bed | resting in a bed for health | lie pose plus pain face |
| Idle fidgets (look around, stretch, scratch, shift weight) | standing with no job | Mixamo, GASP |

### Faces and emotions

**How faces work.** MakeHuman has an expression library (anger, fear, laugh, sad, surprise, smile
and more). Its expressions are poses of the face bones on MakeHuman's default rig, which our game
skeleton does not have. So the export script bakes each one into a **morph target** on every body:
it poses the default rig, keeps the shape and drops the bones. In game, an emotion is a blend of a
few of those morphs, plus procedural blinks, eye saccades, gaze at whoever the pawn talks to, and a
talking mouth (a jaw and lip cycle with a little noise; there are no voices to lip-sync).

| Emotion | Face | Body layer | When (sim signal) |
| --- | --- | --- | --- |
| Neutral | resting face, blinks | idle | default |
| Talking | moving mouth, brows up and down | talking gestures (Mixamo) | a social interaction is running (chitchat, deep talk) |
| Happy, smiling | smile | | mood high, kind words |
| Giggling | laugh morph in short bursts | shoulders shake, hand to mouth (Mixamo laughing) | a recreation with others, a joke, a good chat |
| Curious | brows up, head tilt | lean in | deep talk, looking at something new (a trader's goods, an event) |
| Flirt | soft smile, lowered eyes, one brow | head tilt, small shy gestures | romance attempt, partners chatting |
| Surprised | brows high, mouth open, eyes wide | small step back | raid alarm, an explosion nearby, a letter event |
| Angry | brows down, jaw set, lips tight | stiff, fists closed | insult, slight, social fight starting, berserk |
| Sad | brows pinched up, mouth down | slouch, head low | mood low, a death, a breakup |
| Crying | sad plus squint, mouth open, tear sheen | hands to face, shaking (Mixamo crying) | sad wander break, grief |
| Pain | squeeze shut, teeth bared | hand to the hurt part, hunch | pain stage from health, a fresh wound |

The mapping is data: each `Interactions` def gets `visual.emotion` (`Insult` → angry,
`RomanceAttempt` → flirt...), and mood bands and health pain stages get theirs in one table.

### Daily life

| Animation | When | Source |
| --- | --- | --- |
| Eat standing: the food in one hand, bites, chewing face | `Eat` with no table | Mixamo eating plus hand IK to the meal |
| Eat sitting at a table: fork or hand to mouth loop | `Eat` at a table (posture `Sit`) | Mixamo seated, upper-body loop from CMU or Cascadeur |
| Drink | drugs, beer | Mixamo |
| Chat, deep talk, social gestures | interactions | Mixamo talking set |
| Recreation: horseshoes throw, chess at a table, billiards cue, cloud watching lying down | `Recreate` job by kind | Mixamo where it exists, Cascadeur for the cue |
| **Showering** (rare, a mood booster as recreation): step in, a wash loop (hair, arms, shoulders), step out | the new shower job (sim hook) | Cascadeur or sneed's mocap; the stall's frosted glass and steam hide the body below the shoulders |
| **Lovin** (partners in a shared double bed; the game runs in real time, so there is no fade or cut) | the lovin job (thread "Add lovin for couples" owns the sim side) | both stay clothed under the blanket in one of three poses: **side by side, spooning or embracing** (picked by the lovin job or a hash of the couple), with a slow breathing and stroking loop, an occasional kiss, and a hearts icon over the bed; **no nudity and no sex-position animations**. Source: Mixamo or Cascadeur lying poses, paired so the two bodies line up |
| Kiss (standing or lying) and hug | partners greeting, a romance attempt that succeeds, lovin | Mixamo or Cascadeur, paired clip with head IK so lips and arms meet on any two body heights |

### Work and tools

| Animation | When | Source |
| --- | --- | --- |
| Pick swing at a rock face, a hit on every `Mine` event | `Mine` | Mixamo mining, retimed |
| Axe chop, felling | `Cut` | Mixamo or Cascadeur |
| Hammer and build | `Build`, `Deconstruct` | Mixamo hammering |
| Sow, harvest, pick berries | sow and `Harvest` | Mixamo kneel and pick, CMU |
| Cook, craft, research at a bench | `Cook`, `Research`, bills | Mixamo standing work, seated typing for research |
| Clean, beat out fire | `Clean`, `Extinguish` | Mixamo sweeping, Cascadeur |
| Tend a patient | `Tend` | Mixamo kneeling, hands at the patient's wound (IK) |
| Haul: pick up, carry, put down | `PickUp`, `PutDown` | Mixamo carry plus `CarryItem` IK |
| Handle animals: tame, feed, milk, shear | `Handle` | Mixamo kneel, Cascadeur |
| Refuel | `Refuel` | Mixamo |

### Guns

| Animation | When | Source |
| --- | --- | --- |
| Hold, low ready, aim (pistol, rifle, heavy) | grip, aiming | Lyra plus aim offset |
| Fire | `ShotFired` | Lyra fire additive plus the sim's recoil kick |
| Reloads: magazine (pistols, rifles, SMG), revolver, bolt cycle, shotgun pump and load | `Reload` | Lyra; the bolt and pump are hand IK to a socket on the gun |
| Equip, holster, switch | `Switch` | Lyra |
| Crouched and prone fire | stance | Lyra crouch, Mixamo prone |
| Cover: crouch behind low cover, lean out and fire, back in | aiming from a cover cell (sim hook) | Mixamo cover moves, procedural lean |

### Bows

Draw, hold and loose are one pose blend driven by draw strength (0 to 1) over the sim's warm-up, so
any warm-up length looks right.

| Animation | When | Source |
| --- | --- | --- |
| Nock an arrow | aiming starts | Paragon Sparrow |
| Draw, then hold at full draw (slight shake when held long) | warm-up steps | Sparrow, plus the procedural draw blend; the string follows the right hand (socket `string`) |
| Loose and recoil | `ShotFired` | Sparrow |
| Greatbow: heavier draw, wider stance | greatbow | the same, slower and lower |

### Melee and fists

Fist fights are social fights and fist weapons. Every blow is cut to the sim's wind-up and hit step
(`melee-draw.md`), which the procedural poses already follow.

| Animation | When | Source |
| --- | --- | --- |
| Boxing guard and footwork | `Fists` grip in a fight | Mixamo boxing |
| Left jab, right jab, cross, hook, uppercut, body blow | each swing; left and right alternate, the sim's damage picks light or strong | Mixamo boxing, or sneed's mocap |
| Strong blow (wind-up, heavy follow-through) | a strong attack (sim hook: light or strong) | Mixamo, Paragon |
| Hold-to-block, blocked blow, guard broken | `Block`, right mouse held, the `blocked` result | Mixamo boxing block, Paragon |
| One-hand slash, stab, overhead | `OneHandMelee` | Mixamo sword and shield set without the shield, Paragon |
| Two-hand sweep, overhead, thrust | `TwoHandMelee` | Mixamo great sword pack, Paragon Kwang and Greystone |
| Dodge sidestep, dodge roll | `Dodge`, `Roll` | GASP roll, Mixamo |

### Hits, staggers and falls

Physical animation does the small ones (the hit bone gives and springs back). Clips do the big ones.
Each pawn has a few variants per kind so a crowd does not move in step.

| Animation | When | Source |
| --- | --- | --- |
| Flinch (head, chest, gut, arm, leg; front, back, left, right) | any hit | procedural partial ragdoll plus Mixamo hit reactions |
| Punch stagger (head snaps, a step back) | fist hit | Mixamo boxing hits |
| Heavy-block stagger (arms pushed back, a step) | blocked a heavy blow | Paragon, Mixamo |
| Slash stagger (twist away from the cut) | cut | Mixamo |
| Bullet stagger (slowed, hunched) | sim stagger (sim hook, RimWorld's stopping power) | Mixamo plus slowdown |
| Explosion stagger, or thrown down | blast within its radius | ragdoll launch then get-up, or Mixamo blast reactions |
| Stumble (trip, lose footing, catch balance) | jostled, walking hurt, tripping on rough ground | Mixamo stumbles, procedural |
| Knockdown and get-up (from the back, from the front) | a knockdown (sim hook) | Mixamo get-ups, Paragon |
| Going down (downing): knees buckle, collapse | `PawnDowned` | Mixamo dying start, blended to ragdoll |
| Death: falls and clips, then ragdoll | `PawnDied` | Mixamo deaths, then ragdoll (as now) |

### Downed and carried

| Animation | When | Source |
| --- | --- | --- |
| Downed idle (breathing hard, slight writhing, pain face) | downed | Mixamo injured lying plus pain face |
| Downed crawl | downed and crawling | Mixamo |
| **Back-to-back carry of a limp downed pawn**: the carrier bends, hooks the person's arms over the shoulders, lifts them onto their back facing away, and walks bent forward | `LiftPawn`, carrying, `LayPawn` | **Cascadeur**: one paired clip (carrier plus carried) so the two bodies line up; the carried body plays its half with physics on the dangling arms, legs and head |
| Fireman's carry (over the shoulder) for rescues | the same events; a choice in data | Mixamo has the carrier's half; the carried half from Cascadeur |
| Lay down in a bed | `LayPawn` | the paired clip's end |

### Parkour, climbing and cover

| Animation | When | Source |
| --- | --- | --- |
| Vault, hurdle (low walls, fences, sandbags) | the path crosses a low obstacle (sim hook) | GASP traversal |
| Mantle and climb up (waist to head height) | climb links (`levels.md`) | GASP traversal |
| Ladder up and down, stairs | climb links | Mixamo ladder, foot IK on stairs |
| Slide under, wall run, roll after a drop | later, protagonist only | Mixamo free-running set |
| Take cover, peek, lean out | cover cell (sim hook) | Mixamo, procedural lean |

## Sim hooks needed

Small cloud tasks with doctest tests, each before the Unreal task that needs it. RimWorld's numbers
where it has them; ours, marked `ours` in the defs, where it does not.

| Hook | What the sim adds | Numbers |
| --- | --- | --- |
| Posture | `Snapshot` posture (stand, sit, lie, sleep) and the seat or bed it uses; sitting already happens at tables and recreation, only the field is missing | none |
| Emotion signals | the interaction kind and partner in the snapshot while it runs; pain stage; mood band (most exist; the task fills gaps) | none |
| Bullet stagger | RimWorld's rule: a bullet whose stopping power is at least the target's body size staggers it (slowed for a short time) | RimWorld's (`stoppingPower` is already in the defs, unused) |
| Knockdown | already in (F9, `explosives.md`): a blast throws a pawn down for a few seconds, snapshot `knockedDown`, event `PawnKnockedDown`; heavy blows may join later | ours (RimWorld has none) |
| Light and strong blows | each melee swing says light or strong, from the weapon's verbs | RimWorld's verbs |
| Shower | a shower building and a rare recreation kind that also gives a short mood thought | ours (vanilla RimWorld has no shower) |
| Cover and vault flags | the snapshot says when a pawn is in cover and which way, and when its path steps over a low obstacle | none |

### AN1 in the snapshot (done)

Per pawn (`ThingView`): `posture` (Stand, Sit, Lie) and `seat` (the chair or bed); `pain` (0 to 1); `talkDef`,
`talkWith`, `talkStarted`, `talkOk`, `talkSecondsLeft` (the interaction showing, `social.md`); `staggeredFor`
(`combat.md` "Stagger", event `PawnStaggered`); `meleeTool` and `meleeStrong` (`melee.md` "Strong swings"; the
MeleeSwing event's `count` is 1 when strong); `coverHeight` and `coverDir` (what stands in the next cell toward the
fight target); `vaultHeight` (a low obstacle in the cell a walking pawn is in). Already there before AN1: `stance`,
`asleep`, `knockedDown`, `downed`, `mood`, `moodBand`, `mentalBreak`, `blocking`, `rollPhase`, `reloading`, `kickUp`,
`kickSide`, `rescuing`, `carriedDef`. Tests `AnimationHookTests`.

## Tasks

Order: hooks and the frame first, then what shows most on screen.

| ✓ | ID | Task | Where | Model | Time | Needs |
| --- | --- | --- | --- | --- | --- | --- |
| ✓ | AN1 | **Sim hooks** _(done 2026-11-14)_: posture, emotion signals, bullet stagger, light and strong blows, cover and vault flags (the table above; snapshot fields listed under "AN1 in the snapshot"). | Cloud | Sonnet, high | 120 min | |
| | AN2 | **Shower** building and recreation (sim and data). | Cloud | Sonnet, medium | 60 min | |
| | AN3 | **Animation frame**: `URim3DPawnAnimInstance`, the script-built anim graph with the seven layers, grips and sockets in data (`visual.grip`, `visual.anim`), hand and foot IK, look-at; the procedural melee poses keep working on top. | PC Unreal | Opus, high | 120 min | creator merged |
| | AN4 | **Clip library**: download Lyra, Paragon and the Mixamo list (sneed signs in to Mixamo once; Claude downloads in the browser), retarget everything to the MPFB skeleton by commandlet, log it all in `assets/manifest.json`. | PC Unreal | Sonnet, high | 120 min | AN3 |
| | AN5 | **Faces**: bake MakeHuman expressions into morphs in the export script; emotion blending, blinks, gaze, talking mouth; the emotion table in data. | PC Blender and Unreal | Opus, high | 120 min | creator merged |
| | AN6 | **Daily life**: sit, lie, sleep, eat (standing and sitting), drink, chat, recreation, shower, lovin's three bed poses with hearts, kisses and hugs. | PC Unreal | Sonnet, high | 120 min | AN1, AN2, AN4, AN5 |
| | AN7 | **Work and tools** (was V14): every work job above with the tool in the hand, hits on the sim's events. | PC Unreal | Sonnet, high | 120 min | AN4 |
| | AN8 | **Guns and throwables**: hold, aim offsets, fire, reloads by type, equip, crouch and prone fire, cover lean, grenade and pila throws. | PC Unreal | Sonnet, high | 120 min | AN4 |
| | AN9 | **Bows**: nock, draw blend, hold, loose and recoil, the string on the hand. | PC Unreal | Sonnet, high | 90 min | AN8 |
| | AN10 | **Melee and fists** (was V12's first half): boxing, left and right jabs, strong blows, hold-to-block, one- and two-hand weapons, dodges and the roll. | PC Unreal | Opus, high | 120 min | AN1, AN4 |
| | AN11 | **Hits and falls** (was V12's second half): flinches by part and direction, staggers by kind, stumbles, knockdown and get-up, downing, deaths into ragdoll, downed idle and crawl. | PC Unreal | Opus, high | 120 min | AN10 |
| | AN12 | **Downed carry**: the back-to-back carry and the fireman's carry, both halves, from Cascadeur. | PC Unreal plus Cascadeur | Opus, high | 120 min | AN11 |
| | AN13 | **Parkour and climbing** (was V13): vaults, hurdles, mantles, ladders, stairs, foot IK on slopes. | PC Unreal | Sonnet, high | 90 min | AN1, AN4 |
| | AN14 | **Mocap kit (optional)**: install FreeMoCap or GVHMR with the RTX 50 PyTorch, `tools/mocap/` from video to the game skeleton, then fill gaps sneed acts out. | PC (Remote Control) | Sonnet, high | 90 min | AN4 |
| | AN15 | **Animation check**: a bench scene that plays every animation on three body types, recorded as a clip for sneed; what looks wrong goes to `BACKLOG.md`. | PC Unreal | Sonnet, medium | 60 min | AN6 to AN13 |

Budget: animation detail drops with distance, as now (full layers within 20 m, base and grip only
beyond, plain loops far away), so the G3 scene stays above 90 fps.

## Tropical animals: photoreal and well rigged

Tropics first (sneed, 2026-11-13). The tropical maps' animals, from `Biomes.json`:

| Animal | Common? | Body plan | Hardest part |
| --- | --- | --- | --- |
| Monkey | most common (weight 1) | primate | fur, hands, sitting and grooming |
| Capybara | 0.5 | rodent | short wet-looking fur, swimming later |
| Wild boar | 0.5 | ungulate | bristles, the charge and tusk gore |
| Elephant | 0.5 | pachyderm | wrinkled skin, the trunk, ear flaps, its weight |
| Rhinoceros | 0.5 | pachyderm | armour-plate skin, the horn charge |
| Blastbuck (our boomalope) | 0.5 | ungulate | invented: an antelope with a swollen gas sac on its back |
| Alpaca | 0.5 | ungulate (camelid) | thick wool groom |

Later for the tropics, when their defs exist: RimWorld's cobra and panther.

### What "photoreal and well rigged" means here

- **Looks**: real proportions from reference photos; 4K colour, normal and roughness textures;
  skin with subsurface scattering; wet eyes with a proper eye shader; fur as real strands (Unreal
  groom) within about 15 m and hair cards further out. Wet look in rain. Elephant and rhino
  wrinkles from the normal map.
- **Rig**: a spine of 4 to 6 bones, neck of 3 or more, tail chain, jaw, ears, eyelids (blink),
  tongue; the elephant's trunk a chain of about 12 bones; monkey fingers. Corrective shapes where
  legs meet the body, so joints do not pinch.
- **Life on top**: feet planted on slopes (IK), head turning to look, breathing, ears and tail and
  trunk swinging with physics, belly and skin jiggle on the big ones, weight shown in the walk.
- **One skeleton per body plan** (ungulate, pachyderm, rodent, primate), with an IK retargeter for
  each, so a clip made for one boar plays on every ungulate of that build.

### Where the models come from

There is no free, complete, photoreal tropical animal pack. The plan mixes sources, best first, and
sneed picks from side-by-side pictures before anything is swapped.

| Source | Licence | What it gives | Notes |
| --- | --- | --- | --- |
| **Fab free content** | Epic content, free for Unreal projects | the odd realistic animal in the free list and the free-every-two-weeks packs | sneed claims any that fit; checked at task start |
| **Sketchfab** downloadable models | CC BY or CC0 per model; free account | scanned and artist-made elephants, rhinos, boars, capybaras, monkeys, several already rigged and animated | quality varies a lot; credit logged in `assets/manifest.json` |
| **Truebones** free animal sets | free, see their terms at download | rigged animals with walk, run and idle clips | good for motion even where the mesh is replaced |
| **AI mesh from photos** (TRELLIS or Hunyuan3D, both run locally on the RTX 5080, both in Pinokio) | open models; Hunyuan3D's licence checked at task start | a textured shape when nothing else exists (the blastbuck) | not photoreal up close; good from the overseer camera after cleanup |
| Paid realistic packs on Fab | paid | the best quality | only if sneed says so; a thread lists the exact packs and prices first |

### How each animal is made

1. **Pick the mesh** from the sources above (task Z1 shows sneed the shortlist as pictures).
2. **Clean it in Blender by script** (`tools/animals/`): scale to real size, fix the orientation,
   remesh to about 40 to 80 thousand triangles with lower-detail versions for distance, bake textures.
3. **Rig it.** If the model comes rigged, map its bones onto the body plan's skeleton. If not:
   Rigify's free quadruped base (Blender), fitted by script to landmarks; UniRig (open source,
   rigs any mesh, animals included) is tested as a faster route. Automatic rigging of a scan is
   the riskiest step, and the task reports honestly if one needs fixing by hand.
4. **Fur and skin.** Blender hair curves exported as an Unreal groom; skin materials with scattering.
5. **Animate.** The model's own clips and Truebones first; Cascadeur (it handles four-legged rigs)
   for the missing ones; Unreal's stride and orientation warping and foot IK let a few walk and
   run clips cover every speed and turn.
6. **Bench it**: the look bench (L1) takes the same shots as the reference photos; sneed judges.

### Animal animations

| Animation | When (sim) | Notes |
| --- | --- | --- |
| Idle (2 or 3 variants), look around, ear and tail flicks | standing | plus breathing |
| Walk, trot, gallop, turn in place | speed | warping fills the speeds between |
| Graze or eat | eating | elephant: trunk to mouth (IK) |
| Lie down, sleep, get up | `asleep` | |
| Alert (head up, freeze) and flee | a threat near, hunted | |
| Attack: monkey bite and scratch, boar and rhino charge and gore, elephant trunk swat and stomp, capybara bite | animal fights (`animal-fights.md`) | timed to the sim's hit step |
| Flinch, stagger, downed, death into ragdoll | hits, downed, dead | like people |
| Monkey sit and groom, capybara sit by water | idle near others | |
| Carried pack (alpaca), being handled (stands still, accepts) | caravans, `Handle` | |
| Young following the mother | breeding | the same clips scaled |

### Animal tasks

| ✓ | ID | Task | Where | Model | Time | Needs |
| --- | --- | --- | --- | --- | --- | --- |
| ✓ | Z1 | **Shortlist** _(done 2026-11-14: [animal-shortlist.md](animal-shortlist.md); sneed chose free models, upgraded by us)_: search Fab, Sketchfab and Truebones for every tropical animal, a picture sheet of the best two per animal with licence and rig notes; sneed picks. | Cloud | Sonnet, high | 90 min | |
| | Z2 | **Pipeline and the capybara**: `tools/animals/` (clean, rig, groom, export), the ungulate and rodent skeletons and retargeters, the capybara end to end, bench shots. _(Capybara: the free Rectus model on Sketchfab, see the shortlist.)_ | PC Blender and Unreal | Opus, high | 120 min | Z1 |
| | Z3 | **Monkey** (primate skeleton, fur, hands). | PC | Sonnet, high | 120 min | Z2 |
| | Z4 | **Elephant and rhinoceros** (pachyderm skeleton, trunk, skin). | PC | Opus, high | 120 min | Z2 |
| | Z5 | **Wild boar, alpaca and blastbuck** (ungulate). | PC | Sonnet, high | 120 min | Z2 |
| | Z6 | **Animal animation fill**: the missing clips in Cascadeur, attacks timed to the sim, warping and foot IK on every animal. | PC | Sonnet, high | 120 min | Z3 to Z5 |
| | Z7 | **Animal check** on the rainforest map: 30 minutes of sneed playing, notes to `BACKLOG.md`. | PC | Sonnet, medium | 60 min | Z6 |

Z1 can start now in the cloud. The rest share the PC with the people tasks, one Unreal task at a time.

## What sneed does

- **Mixamo, once (for AN4):** sign in at mixamo.com with a free Adobe account and leave the browser
  signed in; the thread then downloads the list itself through the computer link.
- **Fab clicks (for AN4 and Z1):** "Add to My Library" on Lyra and the Paragon heroes the thread
  names (it sends the links), and on any free animal pack Z1 finds.
- **Pick from pictures:** the animal shortlist (Z1), and say what looks wrong at each bench clip.
- **Optional, mocap:** if you want to act moves out, the mocap kit task (AN14) installs everything;
  you film yourself on a phone and drop the videos in the project folder.

## Open points

- Shower placement and look: a stall with frosted glass (planned) or an open outdoor shower.
- Whether downed carries default to back-to-back or the fireman's carry (both are planned; data picks).
- Paid animal packs: none planned; Z1 says where a paid pack would be much better and what it costs.
