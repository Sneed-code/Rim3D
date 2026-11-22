# Body health: zones, downing, blood and poise

sneed, 2026-11-16 (thread "Make combat real-time with smart AI"). Part of the real-time combat rework
([real-time-combat.md](real-time-combat.md)). It sits on top of [health.md](health.md): the body of parts, organs,
injuries, scars, infections and diseases stay as they are; what decides **downed, unconscious, bleeding and death**
for people changes. Animals keep RimWorld's rules for now.

## sneed's rules

| Rule | Number |
| --- | --- |
| Zone hit points | head 50, torso 100, each arm 50, each leg 75 |
| Downed | torso, head or a leg under 25%, or 3 or more zones under 50% |
| Unconscious | head under 25%, or blood at 1000 or less (wakes above 1000) |
| Torso-downed | crawls slowly away (fleeing) while conscious |
| Bleeding (each zone, until patched) | 75% or less: 1 a second; 50% or less: 2; 25% or less: 3 |
| Blood | 3000 (hidden). Dead at 0 |
| Patching a zone | 1 / 2 / 3 seconds (light / moderate / heavy) |
| After patching | blood comes back at 10 a second, starting 30 seconds after |
| AI and patching | patches only when safe; under 1500 blood it takes cover or runs to patch |
| Protagonist | never downed; about 100 hit points over the whole body; dies at 0 (not a pawn sneed pilots) |
| Downed and dead | can still be shot |
| Poise | base 50. An attack's force over the target's poise knocks it down; then 30 s before it can be knocked down again |
| Death (2026-11-17) | head or torso at 0 (or blood at 0; poison later). Also: two or more limbs (arms, legs) at 0 with the head or the torso at 50% or less |
| Hurt arms (2026-11-17) | an arm under 50% slows attacks by 0.5% for every percent under 50, both arms added (both at 25%: 25% slower) |
| Hurt legs (2026-11-17) | the same for the walk |
| Healing (2026-11-17) | a zone not hurt for 10 s with its bleeding patched gets back 0.1% of what it lost a second, 0.2% asleep |
| Dismemberment (2026-11-17, replaces the 50% rule and the gibs; piling up 2026-11-19) | only on a dead body: damage past a zone's 0 piles up over any number of hits (before or after death), and once the body is dead a zone with 25% of its hit points piled up comes off. Corpses still take bullets, blows and blasts. Upper arm: the whole arm; lower arm: below the elbow; legs likewise; head: off; torso: the head, arms and legs come apart. No viscera yet. Protagonist: never |
| Severed parts (2026-11-17, not built) | decay over 2 days; a head on a pike turns to bone over 4 days |

"No active bleeding for now": no dripping effect on screen yet; the blood loss itself counts.

## How it maps onto the body

- **Zones** are subtrees of the body: the **head** zone is the neck and everything under it, each **arm** a shoulder
  and everything under it, each **leg** a leg and everything under it, the **torso** everything else (the torso and its
  organs). Listed in `HealthRules` `zones` by body part def, so another body can get zones by data.
- **Hit points**: every part in a zone has its hit points scaled so the zone's root has the zone's hit points (head
  25 to 50: x2; torso 40 to 100: x2.5; shoulder 30 to 50: x1.67; leg 30 to 75: x2.5). Organs keep their share, so a
  shot can still destroy a heart or a brain (death), as now.
- **A zone's health** is 1 minus the damage on its outside parts (inside hits hurt the outside part over them, so
  each blow counts once) over the zone's hit points. A lost part counts its full hit points.
- **Blood** is the existing blood loss (0 to 1) shown as 3000 x (1 - loss); unconscious at a loss of 2/3, dead at 1.
- Time: "a second" is a real second at 1x speed (30 sim steps).

## Phases

| Part | What | Where |
| --- | --- | --- |
| A | Zones, scaled part HP, downed / unconscious by zone, zone bleeding, blood store, regrowth after patching, protagonist 100 HP, shooting the downed and dead | sim |
| B | Self-patching (1/2/3 s a zone), AI patches when safe, under 1500 blood takes cover or runs to patch | sim |
| C | Poise and attack force: knockdown with 30 s immunity | sim |
| D | Torso-downed crawl | sim + an Unreal crawl clip |
| E | Damage numbers, layered upper-body staggers by zone and a limp after a leg hit, blood on clothes | Unreal (PC) |
| F | Dismemberment and gibs (pre-cut limbs with caps, gib chunk sets, skull and brain interior) | sim rule + Unreal art (PC) |

## Part A as built

Code: `HealthModel::buildZones`, `HealthEval::workOutZones`, the zone branch of `HealthSystem::derive` and the blood
lines of `HealthSystem::step` (`Private/Health.cpp`). Data: `HealthRules` `zones` (`Rules/Rules.json`): the bodies
that have zones (`bodies`, now only `Human`), the zone list (name, root part def, hit points) and every number in the
table above. Tests: `Tests/BodyZoneTests.cpp`. `Tests/HealthTests.cpp` takes the Human zones off and keeps checking
RimWorld's rules, which animals still use.

- **Zones**: a rule's root part def starts a zone at every part of that def (two shoulders, two legs); the rule whose
  root is the body's core part (Torso) takes what is left. Every part's hit points are scaled by its zone (torso 40 to
  100: a heart has 37.5).
- **Zone health** (`Health::zones`, `HealthSystem::zoneHealth`, by `zoneNames` order): 1 minus the damage on the
  zone's outside parts (each part at most its hit points), a lost part's full hit points, over the zone's hit points.
- **Downed** by sneed's rules only: pain shock and RimWorld's capacity thresholds no longer down a person (they still
  slow it). A vital organ destroyed or a vital capacity at 0 still kills, as before. So the Wimp trait (a lower pain shock
  threshold) and numb (resonance, takes pain away) no longer change whether a person is down; they still work for
  bodies without zones. sneed (2026-11-16) made Wimp psychological instead: see part B.
- **Unconscious** (`Health::unconscious`): head zone under 25%, or blood 1000 or less. It is a kind of downed.
- **Bleeding**: a zone at 75% or less with an untended wound that bleeds (a cut, stab or gunshot, not a bruise or
  burn) or an untended lost part in it bleeds 1, 2 or 3 blood a
  real second (`bleedPerDay` holds it as blood loss a day, so the HUD and AI keep working). Tending is "patching" until
  part B adds patching yourself in 1 to 3 seconds. RimWorld's blood loss stages (consciousness offsets) are off for a
  zoned body: blood only matters at 1000 and 0.
- **Blood comes back**: once nothing bleeds, `Health::bleedStoppedAt` (saved, health section 7) holds the step; from
  30 seconds (900 steps) on, 10 blood a second.
- **Protagonist**: never downed or unconscious; dies (`DeathCause::Wounds`, "wounds") when its zones have lost 100 hit
  points in all. Its parts are no longer doubled (that stays for an unzoned body). It still bleeds, and a destroyed
  brain or heart still kills.
- Not yet: shooting corpses (part A2: the dead have no hitboxes yet; the downed can already be shot), patching (B),
  poise (C), crawling (D).

## Part B as built

Code: `ColonySim/Patch.h`, `Private/Patch.cpp` (`PatchSystem`), `HealthSystem::patchZone` / `worstBleedingZone` /
`zoneBleedLevel`, `FighterSystem::badlyHurt`. Data: `HealthRules` `zones` `patchSeconds` [1, 2, 3] (sneed's),
`patchQuality` 0.3 and `coverBlood` 1500 (sneed's); `PatchRules` in code (`safeSeenSteps` 60, `safeRange` 30 m,
`thinkEvery` 15; ours). Tests: the "Patching" cases in `Tests/BodyZoneTests.cpp`.

- **Patching** (`Patching` component, saved in section `patch`): the pawn stands still (aim dropped, fighter
  stopped, job paused, no swings) for 1, 2 or 3 real seconds by how badly its worst zone bleeds, then that zone's
  untended bleeding wounds and lost parts are tended at quality 0.3. One zone at a time. Downed mid-patch: the patch
  is lost. `PawnAction` `Patch`: "patching" (`part` the zone, `amount` the steps) and "patched".
- **The AI** looks every half second: a person (not the player's pawn) who bleeds and is not downed, stunned,
  casting or patching patches itself when it is **safe**: not a fighter, or a fighter whose side has seen no able
  enemy within 30 m for 2 seconds.
- **Under 1500 blood** a bleeding fighter counts as badly hurt and falls back to cover (the fall-back it already had),
  where it patches once it is safe. The old rule stays too: bad pain, 30% blood lost or a bad leg send a fighter back
  (the raid soak's fighters stood in the open more without it).
- **Wimp** (sneed, 2026-11-16): a Wimp fighter hurt by an enemy falls back to cover for 12 to 20 real seconds
  (`Traits` Wimp `fleeWhenHurtSeconds`, `Fighter::cowerUntil`, saved in fighters section 4), then fights again. Not
  the player's pawn. An enemy at arm's length still makes it stand and fight.
- **The player's pawn** patches on `PatchCommand` (a key on the PC, to be bound with the stagger work). The snapshot's
  `patchZone` / `patchStepsLeft` let Unreal play a patching animation.

## Part C as built

- **Poise** (CombatRules `poise`, ours): a blow's force is its damage before armour times its damage type's force
  factor (blunt and crush 1.5, burn 0, the rest 1). Force over the pawn's poise (50) knocks it down for the blast
  knock-down's time (20 steps plus 0.6 per point of force, at most 90) and then no blow can knock it down for 900 steps
  (30 real seconds). `CombatSystem::jolt`, `Poise` component (saved in combat section 4, hashed). The event is
  `PawnKnockedDown` with message "poise". A knocked-down pawn can't swing, block or dodge.
- **Staggers** (sneed, 10:46): a hit staggers a person only if what got through armour is over 10% of the struck
  zone's hit points, and at most once per 150 steps (5 s). This replaces RimWorld's stopping-power stagger for bodies
  with zones, for bullets and melee alike; animals and mechs keep the old rule.
- **Sweeping blows**: cut, slash, blunt, crush and scratch blows sweep their arc (MeleeRules `arcHalfDegrees` 30,
  `arcRaysPerSide` 2). The aimed line is traced first; if it meets nothing, the lines either side are traced in turn
  and the first pawn met is hit. Stabs and bites only go straight. A man who steps straight back is still missed.
- **Knock-on fixes**: a person on their feet never walks slower than 40% (`zones.minMoving`; pain no longer downs, so
  it doesn't leave them crawling upright either; a raider at 3% speed held a raid open for hours). A hurt fighter
  whose side outnumbers the enemies near two to one presses on instead of backing off (a bruised trio and a lone
  colonist all backing off left a brawl with nobody coming on); bleeding under 1500 it still goes to patch. A fighter
  falling back counts cover double when it picks where to go (hurt fighters were waiting in the open far away).
- Physics wobble at the struck spot belongs to part E (Unreal).

## Part D as built

- A person downed by the torso and awake (`Health::crawls`, derived) drags itself away from the nearest able enemy
  within 20 m, one cell at a time, at 5% of its walk (`zones.crawlMoving`, ours; about 3 m in 20 s; was 10% until sneed found crawlers too quick, 2026-11-22). Downed by a leg,
  by many zones, or knocked out, it lies still; with no enemy near it lies still; a rescuer or a hauler carrying it
  stops it (`PatchSystem::crawl`). The snapshot's `crawling` tells Unreal to play a crawl when it moves. The player's
  own colonist counts as an enemy whether or not it is drafted (2026-11-18: before, only fighters did, so downed
  raiders lay beside the player and never crawled).

## Part E as built

What a hurt body shows, all in Unreal and all cosmetic (the sim decided the hit, the stagger and the knockdown).

- **The hit layer** (`FSimPawnHitLayerNode`, the root of the pawn's animation, over the montage slot and the
  locomotion): each of the six zones is a damped spring about its joint (head; spine for the torso; upper arms;
  thighs). A hit pushes the struck zone along the blow and it swings back with a wobble (about 3 swings a second,
  gone in under a second), then sits exactly on the animation again. Nothing underneath is stopped, restarted or
  blended out, so a hit never breaks a walk, a swing or a react clip, and a small hit that staggers nothing still
  shows. The head and torso stand on their joint (the top goes with the blow), arms and legs hang (the far end does);
  a struck torso drags the head, a struck arm turns the shoulders, a struck leg tips the body. Full strength is 20
  damage; the bend is capped at 34 degrees. `ASimPawnActor::KickZone` takes the zone from the sim's body part
  (`HealthSystem::partZone`), or from where the hit landed on the body when the sim named no part.
- **Bullets and blows** both come through `UFightSubsystem::SpawnImpact`, which kicks the zone (a quarter strength
  when armour stopped it) on top of the lean and the react clip that were there before.
- **Limp**: a leg under 85% health makes the body tip over that leg and forward, drop a little and keep the head
  level, while that foot is the lower one in the walk underneath; full at 35% and less. Only while walking.
- **Patching**: while the sim patches (`patchZone`), the spine bends over the wound, the head looks down and a hand
  goes to the place (the right hand, the left for the right arm), eased in and out over 0.3 s. Procedural: there is
  no clip for it.
- **Crawl**: a pawn downed by the torso and awake (`crawling`) is no ragdoll: it plays the forward crawl clip from
  `Mixamo/Crawling` (found by name through the asset registry), as fast as the sim moves it and all but still when it
  does not. The clip's travel is in its hips, so the body is set back by it like the clips of
  [body-motion.md](body-motion.md). Knocked out or dead it goes limp as before.
- **Damage numbers**: your hits float over where they landed (as before: bullets and blows); a hit **on you** shows in
  red under the crosshair. `Fight.DamageNumbers 0` hides both.
- **H** patches your worst bleeding zone when you play a pawn (`UFightSubsystem::Patch`); the gun panel says
  "Bleeding   H patch the wound" while something bleeds and counts the patching down.
- Smoke test `hurt` (`SmokeTestHurt.cpp`): six hits, one a zone, must bend the body 4 to 36 degrees and settle under 1
  degree; a bruised leg must limp over a 14 m walk; a cut torso must be patched with the body bent over it; a colonist
  downed by the torso must crawl and never be a ragdoll.
- Not done here: blood on clothes (the blood system stains bodies; cloth comes with part F's wounds), and a limp for
  far pawns' simple loops is the same lean (no separate clip).

## Part F as built (sim)

sneed's rules of 2026-11-17 (the table above); the first version (50% overkill on the living, bursting heads and
torsos, whole-corpse gibs) was never merged.

- **Death** (`HealthSystem::derive`, `HealthRules` `zones.deadLimbs`, `deadLimbsCoreAtOrBelow`): a person dies
  (cause Wounds) when the head or the torso zone is at 0, or when two or more arm and leg zones are at 0 and the
  head or the torso is at half or less. Blood at 0 and RimWorld's vital organs kill as before. The protagonist keeps
  its own rule (100 hit points over the body).
- **A limb at 0 stays on** (`damagePart`): for bodies with zones, damage no longer destroys the zone's root, the Arm,
  Hand, Leg, Foot or Head part: the part sits at 0 and can heal. Fingers, toes, eyes, ears and organs are still lost
  the RimWorld way. A part at 0 takes no more damage, so a hit aimed at it does nothing.
- **Hurt limbs** (`zones.limbSlowBelow` 0.5, `limbSlowPerShare` 0.5): `Health::limbAttack` is 1 minus half of every
  arm's share under half, `limbMove` the same for legs. `HealthSystem::attackSpeedFactor` (never under 0.5) divides
  `CombatSystem::attackTimeFactor`, so aiming, cooldown, reloading and melee swings all slow; `limbMove` multiplies
  the walking factor after the 40% floor (two legs at 25% walk 25% slower than the floor).
  Reading of sneed's example: the count starts at the 50% line, so one arm at 40% is 5% slower.
- **Healing** (`HealthSystem::step`, `healDelaySeconds` 10, `healPerSecond` 0.001, `healAsleepFactor` 2,
  `healGoneBelow` 0.25): once a real second (30 steps), every wound in a zone that was last hurt 10 real seconds ago
  or more (`Health::zoneHurtAt`, saved in health section 8, hashed) and is not bleeding shrinks by 0.1%, 0.2% when
  the pawn lies asleep (`setAsleep`, a worker lying down). A wound under a quarter of a hit point is gone. Scars and
  old wounds never heal. RimWorld's slow daily healing still runs beside it.
- **Dismemberment** (`hitPart`, `dismember`, `zones.dismember.overkillShare` 0.25; piling up: sneed, 2026-11-19):
  whatever a hit does past what its zone had left piles up on that zone (`Health::zoneOverkill`, saved in health
  section 9 and hashed), hit after hit, on the living and the dead; a zone that heals back above 0 starts again from
  nothing. Once the body is dead, a zone with a quarter of its hit points piled up comes off: 13 for an arm or the
  head, 19 for a leg, 25 for the torso. So one big killing blow still does it, and so do many knife stabs at a corpse. The torso is the exception (`torsoSingleHit` 1, sneed's pick): it piles up too, but comes apart only from one hit that alone went 25 past 0 (a blast, a huge blow), not from stabs piling up on a chest. **Living limbs** (sneed, 2026-11-20: "i cant seem to dismember anyone - even if i shoot an arm repeatedly"; `livingLimbs` 1): an arm or leg now comes off the living too once a quarter of its hit points has piled up past 0 (13 past 0 for an arm, 19 for a leg); the head and torso still only on the dead (a living head or torso at 0 kills first). **A limb at 0 keeps its hitbox** (sneed, 2026-11-22: "dead body parts accumulate damage so shooting it to do the % will do it"): hit boxes and traces skipped any part at 0 hit points, so once a limb reached 0 the next shots passed through it and the damage past 0 never piled up; now only a part that is really gone (`hasPart` false) has no box. Severed parts are pushed by shots and blows that pass within their reach and thrown by blasts (`ASeveredPieceActor::Push`). **Blasts on corpses** tear: a blast's whole share lands on one part of a corpse that sticks out (a pick on the torso is tried again, up to 8 times) (not split into small fragments that never reached the threshold), and that part's hit points count as gone, so a rocket takes a limb off a corpse; the blast also pushes the corpse (`knockbackMetres` x share; a plain push now moves a body that can't move itself, downed or dead) and Unreal throws its ragdoll with an impulse from the blast's centre (300 to 900 cm/s by radius). Corpses lie still: the ragdoll pauses the body's animation (fingers and toes twitched), turns off soft tissue, gets more damping and is put to sleep once still for half a second.
  **Overkill kept until it is in pieces** (sneed, 2026-11-22: "if a dismembered corpse is attacked keep overkill
  damage until it gibs; if the torso is maxed another limb comes off and so forth until none"): on a dead body, what a
  cut limb had piled past its quarter goes on to the torso's pile, and every quarter of the torso's hit points (25)
  piled past 0 takes one more piece off: the limb with the most piled on it (the left before the right on a tie),
  then the head once no limb is left, until only the trunk lies there. One hit 25 past 0 on its own still takes the
  head and every limb at once. A part that is gone has no hitbox, so later blows land on what is left.
  **Blasts tear living limbs** (sneed, 2026-11-22: "dismembering seems hard to do even with a rocket"; `blastTear`
  0.3): a blast's piece that lands on a living arm or leg and does 30% of the zone's hit points (15 for an arm, 23
  for a leg) tears it off, whole or below the elbow or knee as the part struck says (`HealthSystem::blastTear`); a
  piece after the one that killed tears like a blast at a corpse. A rocket at someone's feet takes a piece off about
  every other person (test "Blasts: a rocket at people's feet").
  **Corpses jolt when hit** (Unreal, `ASimPawnActor::PushRagdollAt`): a shot or blow on a ragdoll knocks the bone it
  struck along the hit (250 to 900 cm/s by damage, a little up) and wakes the body, which settles and sleeps again.
  A body killed or knocked out by a blast is thrown by it as it goes limp (`UBlastFxSubsystem::ThrowNewRagdoll`).
  **The player's swing at nobody** with no living one to take it goes down at a corpse lying within reach before it
  (`MeleeSystem::corpseAhead`: 60 degrees, the swing's reach plus `corpseReachExtra` 0.6 m), with the downward clip.
  **The dead are silent**: a shot, blow or torn part on a corpse gives no grunt or hurt cry (`SoundAliveOf`); the
  killing blow's scream (PawnDied) stays.
  **Trunks burst** (sneed, 2026-11-22: "keep overkill damage until it gibs ... until none"): once nothing is left on a
  corpse but the trunk, another quarter of the torso (25) piled past 0 bursts it. One hit that goes far past 0 keeps
  what went past the quarter that took the head and limbs, so a hit 50 past 0 (a rocket in the chest) takes everything
  and bursts the trunk at once. In the sim the torso (part 0) is destroyed, so every part is gone: `HealthSystem::gibbed`,
  SeveredBit `kSeveredTrunk` (with all the others), no hitbox, no more corpse hits; the body stays as a record (who
  died, what they wore). Events: a PawnPartLost for the torso (the wet tear) and PawnGibbed (`amount` the pile that burst
  it; three chunk slaps follow). Unreal (`ASimPawnActor::BurstIntoGibs`, `AGoreGibActor`): 18 chunks (`Gore.GibCount`),
  procedural like the stumps in M_Gore (`GoreMakeGibChunk`): lumps of torn muscle with a patch of skin and fat, curved
  ribs with meat on them, broken bone shards with marrow at the ends, dark wet lumps; thrown out and up at 3 to 7 m/s
  (harder for more overkill, `Gore.GibSpeed`), tumbling, bouncing on the sim's ground and leaving a splat where every
  other one first lands, with six blast sprays of the body's blood and a heavy one onto where it lay. The body hides a
  few frames later (after the limbs that came off in the same hit are copied into their pieces), with no collision or
  physics. Gibs lie 5 minutes like blood (`Gore.GibSeconds`), then shrink away; at most 240 at once (`Gore.GibMax`).
  Smoke `gore` phases `gibs` and `gibsclose`.
  The struck zone is looked at on every hit; on the blow that kills every other zone too (a leg already battered
  past 0 comes off with the death). After a cut the zone's pile starts again, so a forearm comes off first and the
  rest of the arm later. A corpse that can be cut up (`takesCorpseHits`: a body with zones, not the protagonist)
  keeps its hitboxes (lying), so bullets, the player's drawn aim, melee swings and blasts land on it
  (`applyHit` takes it; `applyDamage`, fire, falls and roofs, still don't); such a hit staggers no one and credits
  or angers no one. Struck on the hand, the foot, a part of them or the radius or tibia: the limb
  comes off below the elbow or knee (the Hand or Foot part goes); anywhere else on the limb: the whole Arm or Leg.
  Head zone: the Head part. Torso: the head and every limb. One `PawnDismembered` event for each piece, message
  "left forearm", "right arm", "left lower leg", "right leg", "head".
- **Never** the protagonist, animals, mechanoids (bodies without zones).
- **For drawing**: the snapshot's `severed` (SeveredBits: each forearm, arm, lower leg, leg, and the head), worked out
  from the lost parts, so a surgeon's amputation shows too.
- **Severed pieces** (`HealthSystem::severedPieces`, `SeveredPiece`): each piece the killing blow took off is
  remembered with an id, the body it came off, which piece it is (one SeveredBit) and where the body was. It rots away
  after `zones.dismember.pieceDecayDays` (2 game days, looked at once a game minute). Saved (section `severed`),
  hashed, and in the snapshot as `severedPieces` with `decay` 0 to 1. It is not a thing yet: nobody can pick it up.
- Not yet: pieces as things to carry (the head trophy, and the head on a pike that turns to bone over 4 days), blood
  and a flesh cap at the cut.

## Part F as built (Unreal)

- **The body** (`ASimPawnActor::ApplySevered`, `SeveredPieceActor.cpp`): for every bit in the snapshot's `severed`,
  every skinned mesh of the pawn (body, clothes, face) hides the bone and all below it (`lowerarm`, `upperarm`,
  `calf`, `thigh`, `head`), which folds that part of the mesh into the joint, so the limb is gone and the hole closes
  on a point. What hangs on a socket of it (a hat, horns, a held thing) and the hair (for the head) is hidden.
- **The piece** (`ASimPawnActor::SpawnSeveredPiece`, `ASeveredPieceActor`): when the snapshot first shows a piece, the
  fight subsystem has its body throw it: a copy of each visible mesh (same materials and colours) as a poseable mesh,
  the piece's bones where the body has them at that moment and every other bone folded into the cut. It is thrown up
  and away from the body, tumbles, bounces on the sim's ground height and comes to rest (its own small sums: the
  drawn ground has no physics shape). It goes when the sim drops the piece. Cosmetic: where it lands is not in the sim.
- **Flesh and blood**: a ball of the blood drops' wet red (`M_BloodDrop`) sits over each stump (on the bone above
  the cut, so it moves with the body) and over the cut end of each piece, sized to the limb. When the piece is thrown
  the blood system gets a heavy cut at the stump (a gush along the limb that left, a bleeding wound, a pool), and a
  small splash where the piece first lands.
- **Clothes**: the piece copies every visible skinned mesh, so the sleeve, trouser leg, boot or hat comes off with
  it, and the same garments are folded away on the body.
- Bones are hidden (`HideBoneByName`) only on the mannequin and on meshes that show another's pose (clothes, a
  MetaHuman body). A mesh with its own animation must not have bones hidden: the bone drops out of its pose and its
  nodes read bone -1 (a crash). The MakeHuman body folds the bone at the end of its own animation
  (`FMakeHumanSeverNode`, `SetSeveredBones`); a MetaHuman face is hidden whole when the head goes and still joins
  the head's piece.
- As built after looking at it on the PC (#355): the flesh at a cut is a flat disc across the limb (a ball read as a
  pom-pom); a piece takes only meshes with skin on it (a head only what is mostly head: a shirt collar drawn out
  from the head bone was a jagged shard); below the cut the piece is in the mesh's reference pose (straight);
  a limb falls over to lie flat once it lands; pieces and discs take no decals; hair cards and hats go with the
  head (hair strands cannot: an unbound groom copy did not draw). Smoke test `hurt` takes noon close-ups of each
  piece from two sides (`dismember_close<N>a/b.png`).
- **Gore** (sneed, 2026-11-19: "dismembered parts are simple red dots at joints"; `GoreCut.h`, `GoreStump.cpp`): the
  fold into a point (and the flat red disc over it) is replaced by a clean cut. On the MakeHuman body the sever node
  straightens the cut bone onto its parent (its reference pose) and squashes it flat along the limb at the joint, so
  the stump keeps its full width and ends in a flat face; everything below folds into the middle of that face. The
  piece does the mirror image: the bone above the cut is placed straight on and squashed flat onto the joint across
  the piece's line (`GoreCut::FlattenParent`). A bone whose limb line is more than about 30 degrees off all its own
  axes (a thigh's pelvis, a whole arm's collarbone) can't be squashed cleanly and still folds to a point. Over each
  flat face sits a stump cap (`GoreMakeStump`, a procedural mesh in `M_Gore`, which BloodArt makes: vertex colours,
  alpha = dryness, a fine bump): a torn, uneven rim of the pawn's own skin colour that curls up and hangs a little
  back over the limb (covering the squashed skin's edge), a thin yellow fat layer, wet dark muscle in uneven bundles
  bulging out, and the bone standing out of it broken and jagged with dark marrow: one bone for an upper arm or thigh,
  two for a forearm or lower leg; a neck shows the spine with its pale cord behind and the windpipe in front (the
  front from the face bones). Body and piece both get one, facing out of the cut. Blood keeps dripping from a fresh
  stump for `Gore.DripSeconds` (120 s; a piece for 40% of it once it lands). `Gore.StumpScale` (1.08) sizes the caps
  against the limb thickness table. On a clothed limb (another drawn mesh covers the cut bone) the cap's rim spreads out
  ends in a band of torn, blood-soaked cloth at the limb's own width (spread wider it read as a loose hoop), and the whole face stands out past the cut by as much as any of the pawn's meshes still bulges there (skin weighted partly to both sides of the cut ends in a rounded dome, which hid the meat in the PC pictures; measured once per cut from the meshes' skin weights, logged as "bulges N cm"), the soaked cloth or skin hanging back over the gap. Where that can't be measured, a clothed limb uses `Gore.ClothLift` (0.25) radii. Without `M_Gore` (BloodArt not run) the caps draw in the blood drops' red.
- Corpses and the unconscious were already ragdolls (`StartRagdoll`); a cut limb's physics shapes stay on the ragdoll,
  unseen.
- Smoke test `hurt`, phase `dismember`: a 400 blow to the downed colonist's torso; the sim must take off the head and
  four limbs and leave 5 pieces, the body must show the same bits with 5 flesh caps, 5 pieces must be drawn and come
  to rest. The phase switches to the real, dressed bodies (`Pawns.MetaHumans 1`), where the pieces must have at least
  two meshes each (skin and clothes).

## Later (sneed, 2026-11-16)

- **Ragdolls**: unconscious pawns, corpses and severed limbs are physics ragdolls in Unreal (cosmetic: the sim keeps
  where the body is; a ragdoll that drifts is pulled back to the sim's spot).
- **Head trophies**: a severed head becomes an item that can be carried and placed as a prop (on a pike, a shelf).
  Dismemberment (part F) makes severed parts real items in the sim (def, the pawn they came from, its looks), so a
  trophy is that item plus a prop to put it on.
