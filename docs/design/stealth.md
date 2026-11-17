# Stealth: sight, hearing and alerts

sneed, 2026-11-17 (thread "Add stealth, sight and alerts"). Code: `ColonySim/Stealth.h`, `Private/Stealth.cpp`, and
`FighterSystem::see` in `Private/Fighters.cpp`. Tests: `Tests/StealthTests.cpp`. Data:
`data/defs/Rules/StealthRules.json` (all ours: RimWorld has no stealth). It stands on the fighter AI
([fighters.md](fighters.md)), cover and line of sight ([cover.md](cover.md)) and light ([light.md](light.md)).

## What sneed asked for

- Pawns have to **see** an enemy before they know it is there and act (fight, flee or anything else).
- An **alert system**: whoever spots you tells allies within **20 m** where you are; the allies it called can't call
  others in turn.
- **Noise**: loud, unsilenced guns carry far; melee is short range, **8 m** ("someone with their back turned, with an
  ally attacked, notices if they are 8 m or less away"); stealth weapons less. The radius varies by weapon.
- **Missions**: once you are detected in such a zone they know there is a threat, even if you hide again.
- **Raids on your base** always assume a threat, and otherwise behave as they do now.
- Pawns set to flee do so on sight (built in the thread "Add focus lock and smart melee AI"; it keys off what this
  page calls a known enemy).
- To be expanded later.

## Alert states

Every fighter has an `Alert` state (`Fighter::alert`):

| State | Means | Sees |
| --- | --- | --- |
| Unaware | at ease at its post | only inside its sight cone (120 degrees round its facing), less far in the dark and at crouched targets |
| Suspicious | it noticed something: a shape far off, footsteps | the same; it has turned to look at where it noticed it |
| Alerted | it knows there is a threat | all round, at the full sight range, as fighters always did |

Who starts how:

- **Alerted**: anyone *sent to fight*: a drafted colonist, one the colony alarm called up, raiders (so **a raid always
  assumes a threat** and fights exactly as before), and any pawn put on another side with `SetFactionCommand`. These
  never calm down while that lasts (`staysAlert`: drafted, defending, in a raid squad, or holding an attack order).
- **Unaware**: a settlement's guards at their posts (`Trade.cpp`), and any fighter a scenario or the practice range
  sets so with `SetAlertCommand{pawn, 0}`.
- Anyone else left alone **calms down**: alerted with no enemy known for `searchSeconds` (900 steps, 30 real seconds)
  it is suspicious again, and after `suspiciousSeconds` more (600, 20 real seconds) unaware.

## Seeing (`FighterSystem::see`, at every plan, twice a real second)

An enemy is seen when all of these hold:

1. It is within the **sight range** (`FighterRules.sightRange` 60 m, less in fog) times, for a watcher that isn't
   alerted, a share for the **light** on the target's cell (`darkSight` 0.3 in pitch dark, rising to 1 at `fullLight`
   0.5) and for its **stance** (`crouchSight` 0.6, `proneSight` 0.4). So at night an unaware guard sees a standing
   intruder at about 18 m and a crouched one at about 11 m; under a lamp the intruder is seen as by day.
2. It is inside the **sight cone** (`coneDegrees` 120 for unaware and suspicious, `alertedConeDegrees` 360 for
   alerted), or within `closeSense` (1.5 m: you can't stand at someone's shoulder unnoticed).
3. The **line of sight** is clear (walls, rock, the ground; a crouched pawn behind sandbags is hidden, as before).

What seeing does:

- An alerted watcher, or any watcher with the enemy within `instantRange` (8 m): it **knows** the enemy at once
  (`Fighter::known`, firsthand) and is alerted.
- Further off and not alerted: it is **suspicious** first, turns to look (`Fighter::suspect`), and if the same enemy
  is still in view `noticeSteps` later (30, one real second) it is sure. Step out of view in that second and it stays
  suspicious, then calms down.
- A fighter caught **unaware** waits `surpriseSteps` (30) longer before its first shot or blow.

An alerted fighter ignores light and stance on purpose: a fight, once on, goes exactly as it did before stealth
(the year run and every fight test were tuned that way), and the test data starts at midnight.

## Hearing

- **Footsteps** (in `see`): an enemy the watcher doesn't see but that moves within earshot makes it suspicious and
  turn to look: `walkNoise` 3 m upright, `runNoise` 7 m at `runSpeed` (6 m a real second) or more, `crouchNoise` 0. So
  you can creep up behind a guard crouched, not walking.
- **Noises** (`FighterSystem::hear`, from the step's events, like `AnimalSystem::scare`): every fighter in the radius,
  on the same level, is **alerted**, whichever way it faces and walls or not:

| Noise | Radius | Hearers learn |
| --- | --- | --- |
| A shot (`ShotFired`) | the gun's `noiseRadius`, else `gunNoise` 90 m | where the shooter is, if it is their enemy |
| A melee blow that lands (`MeleeHit`) | the weapon's `noiseRadius`, else `meleeNoise` 8 m (sneed) | where the attacker is |
| A bullet striking a pawn (`ProjectileHit`) | `hitNoise` 8 m round the one hit | only that something is up |
| A blast (`Explosion`) | `explosionNoise` 150 m | only that something is up |

- **Being hit** (`FighterSystem::struck`, the combat hurt hook): the victim is alerted and knows its attacker.
- **Quiet weapons** are data: a Things def's `noiseRadius`. Bows are 15 m, pila 10 m. A silenced gun or a stealth
  blade is one more number in its def when we add them.
- Animals fighting and hunting alert nobody.

## Calling out (sneed's 20 m rule)

A fighter takes what **friends within `calloutRadius` (20 m)** know **firsthand** (saw or heard themselves). What it
gets that way is marked `firsthand = false` and is **not passed on**: a call reaches 20 m from the one who saw, not
further. A fighter that was *alerted by* a call (`Fighter::called`) calls nobody until it has calmed down, even about
what it then sees itself (`calledCanCall` false, sneed: "others called can't trigger it themselves"). Before this,
every fighter of a side knew at once what any one of them saw, anywhere on the map.

In practice a fight is loud: shots and blows alert others by their own noise. A quiet kill (a bow from 20 m, a blow
with nobody within 8 m) alerts nobody else.

The **colony alarm** is unchanged: it goes off when any colonist sees an enemy, calls everyone to their defense role
and gives them the threat's direction ([raids.md](raids.md)).

## Mission sites stay alerted

On another map (`world.visit`, [other-maps.md](other-maps.md): today the settlements; quest sites when they come),
`FighterSystem::setStaysAlerted(true)`: the first time a fighter of a side other than the player's is alerted by its
own eyes or ears, **its whole side on that map is alerted for good** (`Fighter::onGuard`, event "site raised"). They
know there is a threat, not where: hiding again breaks their line of sight, not their guard.

## For Unreal (not built yet: needs the PC)

- Snapshot `ThingView::alert` (0, 1, 2), `hasSuspect`, `suspectX`, `suspectY`: a "?" or "!" over the head, the head
  turning to the suspect point.
- Event `PawnAction::Alert`: `thing` the pawn, `amount` the new state, `message` why ("saw", "glimpsed", "heard
  steps", "heard a shot", "heard a blow", "a friend was hit", "heard a blast", "was hit", "called", "site raised",
  "lost them", "calm", "ordered"): a sound sting, a bark.
- `SetAlertCommand{pawn, 0..2}`: the practice range sets its targets unaware.

## Saves, hash, replays

Saved in the fighters section (version 5): the alert state, `onGuard`, `called`, `alertAt`, the suspect point and
pawn, and `firsthand` on each known enemy. Older saves load every fighter alerted, as it was. All of it is in the
state hash, but only when it differs from a new fighter's values, so replays recorded before stealth keep their
hashes. `SetAlert` is in replays.

## The tests

- The cone, the light share and footstep radii as plain numbers; the rules def is read.
- An unaware guard doesn't see an intruder 10 m behind it in daylight; turned round it is suspicious, then alerted,
  with two `Alert` events.
- Within 8 m in front it is alerted at once.
- A crouched intruder 45 m in front isn't seen; a standing one is.
- At midnight an unaware guard doesn't see 30 m; told to be alerted it does.
- Walking up behind a guard is heard within 3 m and it turns and is alerted; crouched, the intruder gets to 2 m unheard.
- A spots, B within 20 m is called (and knows the enemy secondhand), C 30 m from A but 15 m from B stays unaware.
- A melee noise alerts at 6 m, not at 12; a real pistol shot alerts at 50 m and gives the shooter away, not at 100 m.
- A mission site stays alerted half a game hour after the intruder is gone; elsewhere the guard is suspicious, then
  unaware.
- A drafted colonist never calms down; a hit alerts and names the attacker.
- A suspicious guard is the same after a save and load.

## Not yet

- Unreal: the indicators, the bark, crouch-sneak animation, the practice range's unaware targets (needs the PC).
- Guards **looking round or patrolling** (an unaware guard keeps the facing it has), and a suspicious or alerted one
  **walking over to look** at where it heard something.
- Plants and tall grass hiding a crouched pawn; walls muffling noise; hearing loss (the Hearing capacity).
- Silenced guns and stealth melee weapons as defs (the `noiseRadius` field is ready).
- Colonists at work noticing by cone and light too (today any colonist with a line of sight sounds the alarm).
- Stealth kills and bonus damage on an unaware target.
- Quest sites as mission zones once they are maps.
