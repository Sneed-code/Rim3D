# Inventory and carry weight

sneed, 2026-11-14 (not a plan task). Every pawn has an inventory: what it wears, the weapons it carries and a
bag of items. Everything has a weight, a pawn can carry only so much, and a heavy load slows it a little.
Unreal opens the pawn's inventory with **I** (later, a PC task).

## What a pawn carries

- **Worn**: its apparel (`Gear::apparel`), shown by body slot (head, torso layers, legs, feet, accessories) as the
  game knows it from the apparel's groups and layers. Taking a piece off is the existing strip and wear commands.
- **Weapons**: the one in its hands and the ones on its body (`Gear::weapon`, `Gear::stowed`, one per slot).
- **Bag**: any items (`Gear::inventory`), whole stacks or part of one. Items in the bag are off the map, belong to
  the pawn (`EquippedBy`) and go with it wherever it goes (caravans, other maps, pods). When the pawn drops its
  gear (stripped, destroyed, a corpse taken apart) the bag falls out with it. RimWorld's inventory is a short list;
  ours has no slot count, only the weight limit, so the screen can show a big grid.
- **In the hands while hauling**: the stack it is carrying (`Carrying`), as before.

`InventoryCommand` moves items: `Take` puts an item lying within reach (2 cells) into the bag, all of the stack or
`count` of it, as much as fits under the weight limit; `Drop` drops `count` (0 for all) of a bag item where the
pawn stands; `DropAll` empties the bag. Colonists don't fill their bags by themselves yet (RimWorld's "take to
inventory" for drugs and meals is a later task).

## Weight

Every item's weight is its def's `stats.mass` (RimWorld's kg) times the stack count, unchanged. A pawn's **load**
is everything it carries: worn apparel, weapons, the bag and a hauled stack.

**Capacity** (how much it can carry):

- Colonists and other people: **50 kg** (sneed's baseline; RimWorld's is 75). Data: the Human def keeps RimWorld's 75
  and `data/tuning.json` multiplies `stats.carryingCapacity` by 0.6667.
- The protagonist: **100 kg**, twice everyone else (CarryRules `protagonistFactor` 2).
- Traits add to it (CarryRules `traitKg`): tough +15 kg, brawler +10 kg. The talent tree's pack mule adds 20% a rank
  as before.
- Animals keep RimWorld's 75 kg (no def gives them another).

**Skills make things lighter** for the pawn carrying them (CarryRules `lighter`), as sneed proposed:

| Skill  | What gets lighter                                             | Per level | At level 20 |
|--------|---------------------------------------------------------------|-----------|-------------|
| Mining | what mining gives: stone chunks, steel, silver, gold, uranium, plasteel, jade, components | 2.5%      | half        |
| Plants | what plants give: wood, crops, berries, leaves, hay, cotton cloth, herbal medicine, chocolate | 2.5%      | half        |
| Melee  | everything (strength)                                         | 1%        | 80%         |

sneed's message said "5% a level, so half at 20"; 5% a level would be nothing at 20, so this uses 2.5% a level to
reach his half at 20. The factors multiply (a level 20 miner and level 20 brawler carries granite at 0.5 x 0.8 =
0.4 of its weight) and never go under `minFactor` 0.3. Which items count as mined or grown is worked out from the
defs (anything a mineable drops or a deep drill brings up; anything a plant is harvested for), so new content joins
by itself. sneed was not sure about these numbers: they are all in CarryRules.

**Slowdown**: the first fifth of the capacity costs nothing (CarryRules `freeFraction` 0.2: 10 kg for a colonist,
enough for clothes and a rifle, 20 kg for the protagonist). Past it walking speed falls evenly to 75% at a full load
(`carrySlowdown` 0.25, as before for hauling). Over capacity (a wound, a trait lost) it slows a further 1% for each 1%
over, to no less than 20% of its speed (`overloadSlowdown`, `overloadMinFactor`). A pawn never picks up more than
fits: hauling takes only what fits next to its gear, and `Take` stops at the limit. With CarryRules missing (the
test defs), the load is the old one: weapons on the body plus the hauled stack, slowing from the first kg.

## Snapshot

Pawns: `loadKg` and `capacityKg`, and `inventoryCount` items from `Snapshot::inventory[firstInventory]` (item,
def, stack count, the weight it has for this pawn). The worn apparel (`Snapshot::apparel`) and weapons
(`carried`) were already there. Save section `inventory` 1 keeps the bag; older saves load with empty bags.

## Not yet

- Unreal's inventory screen on **I** (PC task): a paper-doll of what is worn and the weapons, a big grid of the bag,
  the load bar, drag to drop or wear.
- Colonists taking food, medicine and drugs into their bags by policy; a protagonist key to pick things up.
- Caravans count capacity by RimWorld's 35 kg a body size still; they could use this capacity later.
