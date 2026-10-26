# Backlog

Ideas parked for later. Newest first. Move an item into a stage plan when it's picked up.

- Crafting (found in C14; bill skill experience, tailoring experience, the electric tailor bench's half speed and "count only this stockpile" are done): inspirations that shift quality up, melee weapon damage and hit points from stuff and quality (P4 repair and upgrades fit), sculpting and smelting benches, and the fuel rule: a stove burns `fuel.perUse` per product, so one batch of 50 kibble would burn 50 wood (RimWorld burns by time worked; found in C11, still open).
- Social life (found in C20; cheating, jealousy, wedding thoughts and Social skill effects done): orientations and attractiveness for romance (now anyone may love anyone), a wedding party job where guests walk to a spot, the colonist panel's relations tab, and mood effects of seeing a friend die in front of you.
- Breeding (found in N4; switch, young health and egg warmth are done): young animals following their mother; gestation needing warmth; a hatchery for the fertilized eggs; tailoring from wool (C14 bills); milk pasteurizing is not in RimWorld either.
- Names for tamed animals (RimWorld lets you name them); shown in the animals tab (U19). Found in N3.
- Effects of the trained tricks: Obedience (come when called, follow when drafted), Release (go wild on a command), Rescue (carry a downed colonist to a bed). N3 only counts the steps.
- Animals and doors: tame animals walk through doors now; RimWorld's animals can't open them, so a door is a gate. Needs a door check in the animal's path (N3 only treats doors as walls in the pen test).
- Lead a tamed animal home by rope, and a "tame all marked animals nearby" button.
- Floor-building job and architect tool (floors are placed by `PlaceFloor` command only; found in C17, no plan task covers it). Needs a Construction-style job: pick floor, mark area, colonists lay it.
- Cleaning outdoors needs the home area (C17 cleans indoors only); vomit, ash and animal filth have no sources yet (E4 fire, animals).
- Colonists that seek a safe temperature (go indoors or toward a heater when outside their safe range, like RimWorld's `SeekSafeTemperature`): E2 only makes them sick (`docs/design/temperature.md` "Not yet"). Needed before the year scenario C25 can say nobody freezes; fits H-tasks or C25 itself.
- Per-building thermostat targets for heaters and coolers (U19 panels).

- Cooking leftovers and shooter feel: picked up in `docs/plans/stage-2.md` (C11 fuel done, C14 bills, C16 food poisoning; P2 and U21 shooter feel).
- Show mental breaks on screen (a U task): log lines for `PawnMentalBreak`, `PawnMentalBreakEnded`, `PawnInsulted`, `PawnLeftColony`; a ring or icon from `ThingView::mentalBreak`; a punch animation for `PawnAction::Punch` (now only for smashing things) and the melee swing animation for berserk. The sim side is done (C19).
- Protagonist needs sleep or not (open question in `docs/game-design.md`).
- Elevators for multi-storey buildings (after stairs and ladders).
- Carry on as another colonist after the protagonist dies (currently game over).
- Children and sexual content: out of scope for now; sneed may revisit.
- Rearm job for sprung spike traps (a Construction-style colonist job; F11 has only `RearmTrapCommand`) and a "rearm all" button (U19 panels).
- Raiders shooting back at turrets (the fighter AI targets pawns only); armor for spike-trap stabs; the other turrets (sentry, autocannon) and trap kinds (found in F11).
- Fix what the F7 raid soak found (`docs/design/raid-soak.md`): fighters stuck while walking to their spot (about 0.4 per seed in raids), fighters waiting in the open 13.6% of the time against the 10% rule, and some raids still going after a quarter game day. Then tighten the soak's limits.
- Fire (found in E4): heat armor of apparel against Burn damage, an allowed area per colonist (the home area is one shared area), a home-area overlay and painting tool (U19), burnt ground slowing plant regrowth, wood-fired buildings (stoves, generators, campfires) catching fire from their fuel, a "fire danger" thought for sleeping next to fuel. Tune `spreadPerTick`, `plantDamageFactor` and `beatPerTick` in a playtest (`data/defs/Rules/FireRules.json`).
- Hunting (found in N2, `docs/design/hunting.md`): hunters should skip animals that are too dangerous for them (bears, thrumbos), a butcher spot (70% yield) and tool cabinets by the butcher table, butchering and eating people and their mood effects, damaged and rotting corpses giving less, and Shooting experience for hunters.
