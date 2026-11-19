# Kill feed

sneed, 2026-11-17. Every death flashes up in the top left corner of the screen, in both the pawn view and the
overseer's view.

## What it shows

- **"Killer ➜ Target"**: each name on a coloured band that is full on the left and fades out to the right, white
  text with a shadow.
  - **Blue**: the colony's own people and tame animals (and the player's pawn).
  - **Red**: enemies: raiders and other hostile sides, and an animal that is attacking (manhunter, hunting a person,
    avenging a hurt).
  - **Yellow**: neutrals: wild animals going about their life, visitors and traders, prisoners.
- **"Name killed"**: a death that was nobody's doing (hunger, cold, heat, illness, drugs, a failed operation,
  slaughter, an execution, or bleeding out with no attacker on record).
- A line stays **5 real seconds** and fades out over the last 0.7 s. More deaths stack **under** the newest line and
  slide up smoothly as the ones above fade. At most 6 lines; with more, the oldest go first.
- When **the pawn the player controls** makes the kill, a short kill-confirm sound plays (cue `Ui_KillConfirm`: a low
  punch and a rising two-note metal ding, heard everywhere, not placed in the world) and the line gets a white edge.
- In the overseer's view the feed starts under the menu and colonist bar. In the combat range the range's gun list
  moved down to make room.

## Who gets the kill (sim)

`CombatSystem::noteHarm(victim, by)` runs whenever someone's bullet, melee blow or blast hurts a pawn (bullets and
blows through `noteHurt`, blasts with the thrower or planter as `by`). The sim keeps the last one on the pawn
(`SimKillCredit`, not saved: it changes nothing in the game). At the end of every step `Sim::creditKills` names the
killer in each `PawnDied` event's `target`, if that harm came within `Sim::kKillCreditSteps` (one game hour of steps,
long enough to bleed out) and the death was from wounds (blood loss, a vital part destroyed, organ failure, wounds).
Hurting yourself (your own grenade) credits nobody.

`Sim::killSideOf(pawn)` gives the colour (`KillSide` Friendly, Enemy, Neutral) and `Sim::feedName(pawn)` the name: a
person's nickname or first name, a tame animal's name, else the kind of creature ("Deer"). Tests: `KillFeedTests`.

## Unreal

`UKillFeedSubsystem` (ImGui foreground drawing, like the damage numbers). Console: `KillFeed.Enable 0` hides it,
`KillFeed.Test` puts up three sample lines. The `gunrange` smoke test reports `killfeed_lines`, `killfeed_mine` and
`killfeed_frames`. The sound is made by `tools/sounds/kill_confirm.py`.
