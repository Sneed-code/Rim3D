# Colony panels

Code: `game/Source/Rim3D/PanelsSubsystem.*`. Smoke test: `panels` (`tools/smoke.ps1 -Test panels`).
Task U8. Unreal side only; the sim is untouched. Everything is ImGui, drawn only while the overseer
view is the one in use ([overseer.md](overseer.md)); `Panels.UI 0` (or `Overseer.UI 0`) hides it.

`UPanelsSubsystem` reads the sim only through `USimSubsystem` (snapshot, `GetSim()` for the colonist
record, the work and schedule rows, mood thoughts and health report) and changes it only by commands.
While the game is paused a command makes the sim step once, so a click shows at once.

## What is on screen

| Where | What |
| --- | --- |
| Top left | **Work**, **Schedule** and **Log** buttons, and a bar with one button per colonist, coloured by mood band (green, yellow, orange, red; grey when down). Click selects, double click centres the camera, hover shows mood, food and rest |
| Middle | **Work tab**: a row per colonist, a column per kind of work that has a scanner in the sim. A cell shows 1 (done first) to 4 (last), `-` for never, `X` for work the colonist can never do. Left click goes `0 > 1 > 2 > 3 > 4 > 0`, right click goes back. Hover shows the work and the colonist's level in its skill. Sends `SetWorkPriorityCommand` |
| Middle | **Schedule tab**: a row per colonist, 24 hour columns, each A (anything), W (work), J (joy) or S (sleep); the current hour is marked `>h<`. Left click goes forward, right click back. Sends `SetScheduleCommand` |
| Left, under the bar | With a **colonist** selected: name, age, backstories, traits, what they are doing, food and rest bars, the mood bar with the three break levels and every thought with its offset (memories show how long they last), their bed (button to free it, list to assign another: `AssignBedCommand`), skills with passions (`*` minor, `**` major) and the health report (pain, bleeding, blood lost, injuries, missing parts, abilities). With a **stove** (any finished building with recipes) selected: its bills in order, each with mode, count (`-10 -1 +1 +10`), Pause/Resume and Remove, and a form to add one. Sends `AddBillCommand`, `SetBillCommand`, `RemoveBillCommand` |
| Left, under the bar | With a **heater or cooler** selected (task U19): the thermostat panel, a slider for its target (-50 to 50 C, sent as `SetThermostatCommand` when the mouse is let go), `-5 -1 +1 +5` buttons, the room's temperature now and **Reset** (back to the building's own limit). It replaces the bills panel for these buildings |
| Right, under the alarm | **Message log**: the last 5 lines, or 40 with the Log button on. Colonist deaths, going down and getting up, wounds, lost parts, mood trouble (crossing a break threshold, calming down), finished bills, and orders the sim refused. Time stamped; a line about a colonist selects and centres them when clicked. 300 lines kept, cleared when the sim is replaced |

## Notes

- Names: labels come from the defs (`label`), so a rename in `data/defs/` or `naming.md` shows here with no code change.
- The Work tab hides work types with no scanner yet (nothing happens for them), so it grows as C-tasks add work.
- The alarm is still U7's button; the log is placed under it.
- Not here yet: priorities for many colonists at once (drag, column header click), stockpile filter and priority panels,
  per-bill ingredient and worker filters, a bill's "store in" and "pause when" options, JOY choices (no recreation exists yet), and
  clicking a log line about a thing that is not a pawn.

## Smoke test `panels` (about 20 s)

Driven by the calls the buttons make, on the Crashlanded start (paused).

1. `panels_setup`: the panels list at least three colonists and drew.
2. `panels_work`: a click moves a priority one step and a right click undoes it; the colonist's mining ends at priority 1 for the
   picture; a click on work the colonist can never do sends nothing. The Work tab drew.
3. `panels_schedule`: a click changes hour 10 and a right click undoes it, three evening hours are Joy for the picture, hour 24 is refused. The Schedule tab drew.
4. `panels_colonist`: a colonist is cut; the panel drew, the health report has the injury and the log says they were hurt.
5. `panels_bills`: a stove is placed; three bills are added (a number of times, until have, forever), the first is paused, the second changed to
   "5 times", the third removed; the sim agrees with each; the bills panel drew.
6. `panels_log`: a bill on a colonist is refused by the sim and the log says "Refused"; the open log drew.

Screenshots are in `game/Saved/Smoke/panels/`. Not covered (needs a person): the mouse itself, the combo boxes, and layout at 4K.

## Trade window, gifts and factions (task U19)

Code: `game/Source/Rim3D/TradePanelSubsystem.*`. Smoke test: `trade`. Sim side: [trade.md](trade.md). Also ImGui, overseer view only; `Trade.UI 0` hides it, `Trade.Factions` toggles the factions window.

| Where | What |
| --- | --- |
| Top middle, under the clock | **Strip**: a **Factions** button and a line per visit here (faction, visitors / trade caravan / trade ship, hours left) with **Trade** (caravans and ships) and **Gift** buttons |
| Middle | **Trade window**: the negotiator (combo, default the best haggler who can act) and the price improvement; the colony's silver on the map and the trader's. Left: what the trader sells, each with its price and a `-10 -1 +1 +10` count. Right: what lies on the map that this trader takes, same. Under it what the colony pays and gets, who pays the difference, a warning when the colony is short of silver or the trader can't pay all it owes, and **Make the trade** (off when nothing is chosen or the colony can't pay; sends `TradeCommand`) and **Clear** |
| Middle | **Gift window**: silver and anything lying on the map with counts, the goodwill the gift earns (value / `giftSilverPerPoint`), **Give the gift** (`GiftCommand`) |
| Middle | **Factions window**: every faction's goodwill and relation (hostile red, ally green) |

Prices come from `TradeSystem::unitPrice` with the negotiator's `priceImprovement`, so the window and the sim agree. The window closes when the visit leaves. Not here yet: trading with a caravan trader by selecting the pawn, selling from a stockpile only, animals as trade lines (they are not items), a hostile faction's "make peace" gifts as a quest.

Smoke test `trade` (about 20 s): a trade ship starts; buying 40 steel pays silver and puts the steel on the map; selling 100 steel brings exactly the quoted silver; a buy of the whole stock is flagged unaffordable; a gift raises goodwill by the amount the window promised; the factions window lists the factions; the thermostat slider's calls set, clamp (500 becomes 50) and reset a heater's target, and a stove refuses one.

## Quests (quest panel, the Unreal side of Q1)

Code: `game/Source/Rim3D/QuestPanelSubsystem.*`. Smoke test: `quests`. Sim side: [quests.md](quests.md). Also ImGui, overseer view only; `Quests.UI 0` hides the window, `Quests.Open` opens or closes it.

| Where | What |
| --- | --- |
| Top left, in the menu row | **Quests** button after Log: `Quests (2)` while offers wait for an answer, amber until the window has been opened since the newest offer |
| Middle | **Quest window**, three parts. *Waiting for an answer*: each offer's label, the def's `text`, what comes of it (how many join, the raiders' faction and points, goods landing at once), the rewards and how long the offer stands, with **Accept** and **Decline** (`QuestCommand`). *Under way*: when the raiders come (the snapshot's `deadline` less the def's `deadlineHours`) and then how long is left to beat them. *Finished*, newest first: done, failed, declined or lapsed, with how it ended (the last quest event's message) |
| Right, the message log | A line when a quest is offered, accepted, declined, done, failed or lapsed |

A refused accept (too many under way, no room for the goods, nobody could reach the colony) shows as "Refused" in the log and the offer stays open. Not here yet: a letter that opens the window, jumping the camera to a quest's raid or its people, and the narrator's quests on the world map (Stage 3).

Smoke test `quests` (about 10 s), on the Crashlanded start (paused): a wanderer and a supply cache are offered (the button counts 2 unseen offers, opening the window marks them seen, the log says so); accepting both puts the cache's steel on the map and one more colonist in the colony, and a second answer to either is not sent; a second wanderer is declined (nobody joins) and a new cache is left waiting for the picture. Not covered: a quest under way (Refugees and Defend need day 5 and day 8).

## Ship (the Unreal side of the spaceship ending)

Code: `game/Source/Rim3D/ShipPanelSubsystem.*`. Smoke test: `ship`. Sim side: [ship.md](ship.md). ImGui, overseer view only; `Ship.UI 0` hides the window and the ending, `Ship.Open` opens or closes the window.

| Where | What |
| --- | --- |
| Top left, in the menu row | **Ship** button after Quests, once a finished ship part stands or the reactor runs: `Ship (12.5 days)` while the reactor warms up, green once the ship can launch |
| Middle | **Ship window**. *Parts*: reactor, engines (3, `ShipRules.enginesNeeded`), computer core, sensor cluster, beams and caskets, each green with enough. *Reactor*: **Start reactor** (`ShipCommand` StartReactor on the first finished reactor) with a warning about the raids, then a bar of the warm-up (`activationLeft` of `ShipRules.activationDays`) and the waves so far, then "active". *Cryptosleep caskets*: one row per finished casket with a list of the colonists to put in it, or empty (`ShipCommand` Board; the snapshot's `ship.aboard`). **Launch**, greyed with the sim's `problem` until `canLaunch` |
| Whole screen | **Ending screen** when the ship has launched (`ShipLaunched`): the world darkened, who left (their names as the last frame before the launch had them), and **Keep playing**: the sim runs on with those left behind |
| Right, the message log | A line when the reactor starts, each wave, the reactor is active or lost, and the launch |

The parts are built from the build menu's Ship category (their defs' `designationCategory`) once researched. Not here yet: prisoners and tame animals in the casket list (the sim takes them), which casket touches no beam, jumping the camera to the reactor, a letter for the ending, and a launch animation.

Smoke test `ship` (about 8 s), on a flat map with the Crashlanded colonists (paused): a whole ship is spawned (the Ship button shows only then, the window lists one casket, Launch is greyed and sends nothing); Start reactor warms it up (a second press sends nothing) and Board puts a colonist in the casket; the dev action `FinishWarmUp` makes the reactor active and Launch wakes up; Launch wins: the ending screen is drawn and names the one rider, and Keep playing closes it.
