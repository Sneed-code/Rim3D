# Overseer screen layout

sneed's layout for the overseer (RTS) view, 2026-11-14, from 15 RimWorld screenshots: "change this
layout methodology first". It replaces the U7/U8 ImGui placement ([orders.md](orders.md),
[panels.md](panels.md)) when the UI is rebuilt. Mock-up (click the tabs, a tile, the rock):
https://claude.ai/artifact/QXsccQCbcwoBLcugZisa6N. sneed will add more as they see it.

## Rules for every box

- **Two text sizes only**: large for titles (a thing's name, a window title) and buttons; medium for
  everything else.
- Every fixed box looks the same: one panel colour, one border, one corner radius, one padding.
- **Tooltips** after the mouse rests about **1.2 s** on a button, like RimWorld's.
- Menus use ordinary buttons. No hidden gestures.

## The bottom bar

Full width along the bottom, like RimWorld. Left to right:

**Build** (RimWorld's Architect; the word is Build) · Work · Schedule · Assign · Animals · Mechs ·
Wildlife · Research · Quests · World · **Inventory** (new). Far right: the speed buttons (pause, 1, 2,
3), then the learning helper (book), Factions and Menu icons. The date, time and weather sit just
above the right end of the bar.

Clicking a tab opens its window above the bar; clicking it again closes it. The open tab stays lit.

## Build

- A **small submenu on the left**, above the bar: a search box, then a 2-column grid of categories:
  Orders, Zone, Structure, Production, Furniture, Power, Security, Misc, Floors, Recreation, Ship,
  Temperature, and spare slots for later ones. The clicked category stays highlighted.
- The category's things show as **icon tiles** in a row above the bar, beside the submenu: a clear
  pixel-style icon, the label under it and the hotkey letter in the corner. **Cancel and
  Deconstruct come first** in every building category, as in RimWorld. Things that need research
  show greyed out; the tooltip says which research.
- Picking a tile shows a slim strip above the tiles: name, one line on what it does, material
  choices with how many are on the map, the drag shape (Single, Line, Outline, Filled) and what the
  drag plans and costs. Sim side: `BuildPlanner` ([build-menu.md](build-menu.md)).

## Work

RimWorld's priority grid (a row per person, a column per kind of work, 1 to 4 or blank), plus a
**work group** column on the left: blank to start, click to put the person in a group (Fighters,
Growers, Miners, any name). Group buttons along the top filter the grid to that group. Slaves and
prisoners have rows too, marked as such.

## The other tabs

| Tab | What |
| --- | --- |
| Schedule | RimWorld's hour grid: Anything, Work, Recreation, Sleep, Meditate; allowed areas |
| Assign | outfits, drug and food rules, allowed areas; tooltips on headers |
| Animals | tame animals |
| Mechs | robots |
| Wildlife | wild animals on the map |
| Research | RimWorld's format (tree, info panel on the left, active project bar), in our own look |
| Quests | Available / Active / Historical, made to look better |
| World | the world map |
| Inventory | a neat box of everything the colony has, instead of RimWorld's always-on resource list. A star pins an item to the top left; **silver is pinned by default**. Later a setting picks which resources stay on screen |
| Factions (icon) | a better window than RimWorld's: goodwill, relation, what they are |

## Bottom left

- **Nothing selected, no window open**: what the cursor is over: terrain, walk speed, fertility
  (if any) and light, just above the bar.
- **Something selected**: its panel (name large, hit points, its buttons) with an **"i"** button
  that opens the **info window**: RimWorld's layout (Basics, Building stats and so on on the left,
  the description on the right, Close at the bottom).

## Colonists

The colonist bar at the top left uses **mugshots**: a picture of each pawn's face in their current
clothes, made once and remade when the window closes or their look changes (not live).

## Not decided yet

  Decide in the first U-thread; the sim side does not change either way.
- Which resources a player can pin, and where pinned ones sit when the colonist bar is long.

## As built (2026-11-17)

Decided: the layout **stays ImGui**, with one shared look (`ColonyHudStyle.h`: the mock-up's colours,
one border and radius, two text sizes, 1.2 s tooltips). Every subsystem that draws ImGui calls
`ColonyHud::ApplyFrameStyle`. `UColonyHudSubsystem` owns the layout:

- **Bottom bar** (`ColonyHudSubsystem.cpp`): the 11 tabs at equal width, Quests shows unseen offers
  in amber; at the right Ship (once a reactor exists), Pause/1x/2x/3x, Keys (the hint box), Factions
  and Menu (Settings, Message log, Key list, Quit). One click opens a tab, a second closes it. Build,
  Work, Schedule, Quests and World are still drawn by Orders, Panels, QuestPanel and WorldScreen; the
  bar opens and closes them. Console `Hud.Tab <name>`, `Hud.UI 0|1`.
- **Date box** above the bar's right end: time, day, season, temperature, weather, Step while paused,
  the level looked at with Up/Down. The old top-middle speed box is gone. Key hints sit above it.
- **Build** (`OrdersSubsystem::DrawBuildMenu`): left window with a search box and the categories in
  two columns (Orders first), a tile window with hotkey letters (Z X C V N M ...; not B, I, O, R or Y, which other keys use), Cancel and
  Deconstruct first in Orders, locked research greyed with a tooltip saying what to research, and a
  strip with name, description, material choice, drag shape (single, line, outline, filled), Turn (R)
  and the plan's cost. Uses the sim's `BuildPlanner`. Remove floor is a new order.
- **Work**: a Group column (sim `SetWorkGroupCommand`, saved and replayed; up to 24 letters) and
  filter buttons Everyone / each group above the table; prisoners get rows that say they don't work.
- **Assign** (outfit, drug policy, medical care, allowed area), **Animals** (name, kind, age and sex,
  health, the four trainings, zone or pen, breeding tick, pregnancy and milk / wool fullness, animal
  bed; right click renames, marks for slaughter or releases to the wild; below it a list of zones and
  pens with a breeding tick each), **Wildlife** (tame, hunt), **Mechs** (colony robots; none yet), **Research** (tree by depth with lines, current
  project bar, an info panel with Research this / Stop), **Inventory** (colony stock by category,
  search, a star pins a resource under the colonist bar; silver pinned by default).
- **Bottom left**: the hover readout (ground, walk speed, fertility, light) when nothing is selected
  and no tab is open; the selected panel with the name large, **i** (the info window: Basics, the
  thing's own table, its description) and x.
- **Colonist bar**: a face picture per colonist (scene capture, remade when clothes or looks change),
  name under it coloured by mood, a red edge while drafted, pinned resources below.
- **Settings** (Menu): pages down the left, Game (day length, mouse speed, text size Normal/Bigger,
  saved as `UI.TextSize`), Display, Sound, Feel and Controls. Other code adds pages with
  `AddSettingsPage`. Controls lists the overseer camera keys and holds the key rebind menu
  (`Rim3DKeys::DrawControlsPage`, docs/design/controls.md).
- Brought up to date with main on 2026-11-19 (lock-on, grenades, rebindable keys, earthworks, landing
  zone): Zone has Landing zone and Remove landing zone tiles, Structure has Level earth and Cancel
  earthworks tiles, and the strip shows Level earth's height with Lower / Raise.
- The **I inventory window** (playing as a pawn) now has a fixed place: centred, under the clock,
  above the bottom edge, so the clock and the key hints stay readable.

## Screens (2026-11-19)

`ColonyHudScreens.cpp`. The Assign tab has four buttons that swap the tab for a manager window
(Close brings the tab back; opening any tab closes the window):

- **Manage outfits**: the outfits down the left (Add outfit with a name, Remove for any but the
  first), and for the picked one the condition range (two sliders, sent when let go), the quality
  range, "keep armour in the bag", Allow all / none and a tick per apparel def grouped by its outer
  layer (headgear, outer, middle, on the skin, belts). Sends `EditOutfitCommand` and friends.
- **Manage areas**: each allowed area with its cell count and how many pawns are limited to it,
  Paint / Erase (the Orders tools `PaintArea` / `EraseArea`, drag rectangles; the area's cells show
  as blue rings while one is in hand), Delete with a yes/no, and New area with a name.
- **Medical**: colonists and prisoners with their state and medical care; for the picked one the
  health report (injuries, missing and added parts, diseases), the planned operations with Remove,
  and "Add an operation" listing every recipe and part `SurgerySystem::targets` allows now.
- **Prisoners**: each prisoner's resistance and will and mode (no interaction, reduce resistance,
  recruit, release, execute, enslave, reduce will), a Slaves list (suppression bar, Emancipate,
  Release), and every built bed with a "for prisoners" tick.
- **Build > Zone > Animal zone** paints animal zones (drawn as tan rings; Remove zone takes them off
  too); **Build > Orders** has Tame, Hunt and Slaughter drag tools (`MarkAnimalsInAreaCommand`).

**Main menu**: the title screen and the new game (scenario, landing site, people screen) are in [new-game.md](new-game.md). Load a game lists `Saved/Saves/*.save`, newest first. Settings opens over it.

**Factions** (2026-11-20): one block per faction with its relation in colour, tech level, what it
does (raids, trades, visits), "permanent enemy" where it is, and a goodwill bar from -100 to 100 filled
from the middle with lines at the hostile (-75) and ally (75) marks (hover: the numbers and the daily
drift). A faction that sends caravans has **Call for a caravan** (the comms console's
`CommsCommand` "RequestCaravan", -19 goodwill) with a trader kind picker; it is greyed with the
reason when the faction is no ally, was called in the last 4 days, a caravan is already coming, or
there is no powered comms console. The best talker (highest price improvement) makes the call.

**Tile pictures** (2026-11-20): a building's Build tile shows its own mesh (made of the best stuff in
stock), seen from the south-west and above, taken once per def by a show-only scene capture on a stage
4 km under the map (one picture a frame, 128 px). Unlit by default so it reads the same day and
night; `Orders.TileIconsLit 1` lights it by the world instead, `Orders.TileIcons 0` goes back to the
drawn footprints. Floors and orders keep their drawn icons.

**Bar icons** (2026-11-20): the speed buttons (pause bars, one to three play arrows), Keys (a
keyboard), the learning helper (an open book), Factions (a flag) and Menu (three lines) are clean line icons drawn by ImGui on
square-ish buttons, amber when on; their names are in the tooltips. Ship stays a word.

**Icons** (2026-11-24): painted icons, one style (hand-painted, semi-realistic, bold silhouette, thin dark outline, no
text), made with ChatGPT as 3x3 sheets in one chat and cut into 256 px PNGs in `assets/ui/icons/` by
`tools/slice_icon_sheet.py` (prompts and names in `assets/ui/icons/sheets.json`). `HudIcons.cpp` loads them at run time
with 128/64/32 px copies so they stay smooth at any scale (`UI.HudIcons 0` turns them off). They show on the bottom bar
tabs (icon left of the label), the Build categories, the order and zone tiles (in place of the coloured rings), Draft /
Stand down, the alarm button and the area Paint / Erase buttons. A missing file falls back to the text or drawn icon.
The speed, Keys, helper, Factions and Menu buttons keep their line icons. Building tiles keep their mesh pictures.

Each faction also shows its last three goodwill changes with why and the day ("Lately: -19 a caravan called (day 3)"),
from the sim's goodwill log (`comms-console.md`). Build tiles fill their rows evenly (27 tiles: 9, 9, 9, not 13, 13, 1).

**Learning helper** (2026-11-20, `ColonyHudLessons.cpp`): 16 short lessons in plain words, each shown
when it matters, one at a time, in a box at the top right under the alarm: the camera, selecting colonists,
a stockpile (until there is one), building, the Work tab, then on their moment: hungry colonists (food
under 30%), beds (rest under 30%), strangers attack (draft), someone is down, playing as a colonist and
research (from day 1), traders and visitors, prisoners, tame animals, cold (outdoors under 0 C) and heat
(over 35 C). The first unlearned lesson whose moment has come shows, with "Learning helper n / 16".
**Got it** marks it learned for good; **Hide the helper** or the book icon on the bar (between Keys and
Factions, amber while on) turns it off. Both are saved with the HUD's settings (`Rim3D.Hud` keys
`Lessons` and `LessonsOn`), not with the colony.

## Canopy in game (2026-11-22)

sneed: "make the actual game's UI ... high quality, well made". Every in-game box now uses **Canopy**, the look of the
title and new-game screens ([new-game.md](new-game.md)), instead of the mock-up's grey and amber. It's all in
`ColonyHudStyle.h`, so every subsystem that calls `ApplyFrameStyle` changes together:

- Dark green-black glass, **square corners**, a faint jade edge; bone text; **jade** for what is open, picked or a
  heading (it was amber); **ember** for costs, warnings and waiting quest offers.
- A short **jade bar on each box's top edge**, drawn by one ImGui end-of-frame hook (`InstallCanopyBars`), not by each
  window. Child windows, tooltips, popups, see-through and small boxes (key hints) and the bottom bar get none.
- The bottom bar has a jade hairline along its top, and the open tab a jade line on its top edge.
- Smoke `hud` checks that the hook draws bars.

Part 2 (tidy the overseer screen): the selected panel names a colonist ("Theo 'Rusty' Ito") and capitalises anything
else; the colonist panel shows backstory titles ("hunter's child", not `Child_HuntersKid`), what they do in plain words
("Wear apparel"), Rest only when the pawn has a rest need, and jade skill bars with ember for passions. Boxes that stack
over the date box (key hints, visitors) use where it was really drawn (`DateBoxTop`), the Power / Temperature ticks
sit left of it, and the visitors strip shows only while visitors are here (Factions is the bar's flag).

Next: Canopy cards for the colonist bar and the selected panel, then each tab's window.

## Bigger text, same widths (2026-11-24)

sneed: "text size is too small. gotta be 50% bigger ... universally ... make sure width of ui boxes etc don't expand
wider", and "stop looking basic, make it professional ... be consistent".

- **Text 1.5x everywhere**: `ColonyHud::TextGrow()` (console `UI.TextGrow`, default 1.5) multiplies the frame's text
  scale in `ApplyFrameStyle`, so every box, the new-game screens and the title's menu grow together. The Settings
  "bigger text" choice still adds its 1.2 on top.
- **Widths held**: boxes that size themselves call `ColonyHud::HoldWidth(name, Scale)` before `Begin`; it fixes the
  width they had before (table `HeldWidthOf`, pixels at 1080 lines) and their text wraps at the edge (`ColonyHud::Text`
  wraps in any box of fixed width, never in one that fits its text). The colonist panel keeps 360. Measure a box with
  `UI.TextGrow 1` and `UI.LogBoxes 1` (log lines `BOXWIDTH name width`); a box not in the table sizes itself as before.
  Label columns that were fixed pixels (skills, needs) grow with the text instead, inside the same width.
- **Glass**: every plain box gets a faint jade glow fading down from its top edge, a hairline of light inside the top
  border and a darker foot (the same end-of-frame hook as the jade bars), slimmer jade scrollbars and a little more
  padding, so all boxes read as one lit pane.

## Layout v2 (sneed, 2026-11-24)

`game/Source/Rim3D/HudLayout.h` holds the rules:

- **Bar windows** (Work, Schedule, Assign, the Build menu and every other tab) start at the screen's left edge and sit right
  on the bottom bar, no gap. They are only as wide as they need to be. The Build menu's list and tiles are equal height
  and touch. While a tab is open the terrain hover box and a selected cell's box are hidden.
- **The right column** stacks from the very top right: the inspect box (or the hover readout when nothing is selected),
  the alarm, the learning helper, then the alerts. Each box reports its height. The next frame stacks on those heights.
- The bottom bar's buttons are 1.35 frame heights tall (`ColonyHud::BarButtonHeight`). The text size comes from the
  shared Canopy style (`TextGrow`).
- Power and Temperature buttons are hidden (`Overlay.Toggles 1` shows them).
- Engine debug keys (F1 wireframe, F2 unlit, F3 lit...) are off: `DefaultInput.ini` clears
  `PlayerInput.DebugExecBindings`, and the HUD empties the list at run time too.

## Selecting (2026-11-24)

- A click selects one thing. Shift + click adds or removes a thing.
- **Drag a box** (more than 10 px) to select many. Your colonists come first. With none in the box, it picks other people,
  then animals. With none of those, it picks things of the same kind as the one nearest where the drag began.
  Shift + drag adds to the selection.
- The selection shows as a flat jade ring on the ground round each thing, not a box. Your pawns' routes are drawn flat.
- Draft, move and attack orders go to every selected colonist. A move spreads them over nearby cells.
- When the game starts, the camera goes to your character and selects them.

## Order marks (2026-11-24)

The tall posts are gone. A marked cell or thing gets a flat ring: green for chop, yellow for harvest, orange for mine,
red for take down (deconstruct or remove floor). Rings show only while that order's tool is held, for all of them while
Cancel is held, or for the selected thing or cell. Otherwise the order stays but nothing is drawn. Chop marks
only trees (`DesignateCommand.treesOnly`, RimWorld's "Chop wood"), not the grass and bushes in the drag.

## Alerts (2026-11-24)

The old event list is replaced by RimWorld-style alerts down the right side. Each has a short title of one to three
words, coloured by severity.

- **Left click** opens the full message in the middle of the screen. Reading it removes the alert. The pop-up can jump
  to the subject.
- **Right click** dismisses it.
- **Shift + right click** (or "Never show this kind" in the pop-up) turns that kind off for good. The list is saved in
  `GameUserSettings.ini` `[Rim3D.Alerts]`. Menu > Message log shows it with a "Turn on" button for each kind.
- An alert fades after `UPanelsSubsystem::AlertSeconds` (60 s for every severity for now) and dims in its last 8 s.
- The same alert repeated within 5 s counts up ("x3") instead of stacking.
- Menu > Message log keeps every message with its game time, including single finished crafts, which never become alerts.

## Colonist bar (2026-11-24)

The faces grow with the text. A health bar under each face shows the worst body part's health: green, then amber below
60%, then red below 30%.
