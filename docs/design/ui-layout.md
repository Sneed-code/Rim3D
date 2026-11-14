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

- Whether the new layout stays ImGui or moves to Unreal's own UI (Slate/UMG) for the look sneed wants.
  Decide in the first U-thread; the sim side does not change either way.
- Which resources a player can pin, and where pinned ones sit when the colonist bar is long.
