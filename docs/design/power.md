# Power

Code: `ColonySim/Power.h`, `Private/Power.cpp`. Tests: `Tests/PowerTests.cpp`, `simrun --power` (the
`power_check` ctest). Defs: `data/defs/Things/BuildingsPowerClimate.json` (D11) and
`data/defs/Rules/PowerRules.json`. Task E5.

Power works as in RimWorld. Conduits join the buildings beside them into **nets**. Generators feed a
net, consumers (lamps, heaters, benches) draw from it, batteries buffer it. A net that cannot cover
what its consumers want goes dark as a whole.

## Units

Watts for output and draw, **watt-days (Wd)** for stored energy, as RimWorld. A game day is 86,400
steps, so one step moves `watts / 86400` Wd. A wood-fired generator (1000 W) charges a battery
(capacity 1000 Wd) with its whole output for 24 game hours if the battery kept all of it; it keeps
half (below).

## What a def says

A Things def with a `power` object gets a `PowerPart` component when it is made (`Things::make`). The
def's numbers are read once per def (`PowerGrid::info`):

| Def field | Meaning |
| --- | --- |
| `power.transmitter` | conduit or switch: joins what is beside it |
| `power.switchable` | a switch: open means it joins nothing |
| `power.output` | watts a generator makes |
| `power.scalesWithSun` | output follows the sun and the share of the footprint with no roof |
| `power.scalesWithWind`, `windBlockRadius` | output follows the wind and how clear the air is at both ends |
| `fuel` (on a generator) | needs fuel; see the hooks below |
| `power.consumption` | watts a consumer draws while on |
| `power.battery`, `power.idleDrain` | a battery's capacity (Wd) and the watts it costs its net |

`PowerRules` holds `batteryEfficiency` (0.5: a battery keeps half of what is put into it, recalled
from the wiki), `restartReserveSeconds` (60, ours), the default sun curve and the default wind.

## Nets

- A conduit or closed switch joins the conduits and switches on its four sides.
- Any other power building joins the conduits or switches on the four sides of its edge cells.
  Buildings next to each other do **not** join without a conduit. (RimWorld lets a conduit lie under
  a building; here the building layer holds one thing per cell, so the conduit goes beside.)
- An open switch joins nothing and is on no net.
- A group of joined buildings is a net only if it has a building that is not a conduit or switch.
  Net ids are 1, 2, 3... by the lowest entity id in each net. They are made again whenever any
  building is set or cleared or a switch is flicked (`Map::buildingChanges`), so an id is good only
  until the next change. A building on no net has `netOf == 0` and no power.

## Each step

`Sim::step` calls `power().step(time)` after rooms (the sun needs roofs). For every net:

1. **Generation:** each generator that is on makes `output` times its factors: the sun (`sun x open
   share of its footprint`), the wind (`wind x open share of its wind cells`) and fuel (0 if
   the fuel check says empty).
2. **Demand:** each consumer that is on wants its `draw` (the def's `consumption`, or what
   `setDraw` set), plus each battery's `idleDrain`.
3. **Served or dark.** Generation covers demand: served. Otherwise the batteries must cover the
   step's shortfall. A net that was already served stays served while they can. A net that was
   dark comes back only when its batteries hold `restartReserveSeconds` of the shortfall, so it
   does not flicker. A dark net serves nobody: every consumer in it is off, as RimWorld's.
4. **Energy.** A surplus charges batteries in entity order (each keeps `batteryEfficiency` of what
   it is given, up to its capacity); a shortfall comes out of them. A dark net still charges its
   batteries from what its generators make beyond the idle drain.
5. **Flags.** Every building's `powered` is: in a served net and switched on. A change sends a
   `PowerChanged` event (not for conduits and switches).

The sun is E3's sky glow. `PowerRules` still holds a fallback curve (none before 05:00, full 08:00 to 16:00,
none after 19:00; ours) for a grid with no sun source set; wind follows E1's weather (`weather().wind()`, see weather.md).

## Asking and doing

`sim.power()`:

| Call | Answer |
| --- | --- |
| `running(b)` | the building does its job: it draws no power, or it is on and served. **Buildings that draw power must be checked with this before they work.** `JobSystem::buildingRuns` wraps it; bill jobs use it (a bench with no power gets no new bills worked, and a running bill fails: "the work building has no power") |
| `powered(b)`, `stateOf(b)` | on and served; `PowerState` None / Off / Unpowered / Powered |
| `netOf(b)`, `net(id)`, `nets()`, `membersOf(id)` | nets and what is on them |
| `wattsOf(b)`, `storedIn(b)`, `capacityOf(b)` | power now (+ making, - drawing) and battery charge |
| `flick(b, on, error)` | a switch opens or closes; a consumer or generator is switched on or off. Batteries and plain conduits cannot be flicked |
| `setDraw(b, watts)` | a consumer asks for less than its full draw (a heater that reached its temperature: E2), or negative for the def's value |
| `shortCircuit(b, released, error)` | empties every battery on b's net, returns the energy in Wd, and sends a `ShortCircuit` event with it |

**Hooks** (set on the grid by `Sim` or by the system that owns the source):
`setSun(f(time) -> 0..1)`, wired to E3's `lights().skyGlow()`; `setWind(f(time) -> factor)`, wired to E1's `weather().wind()`
(a constant 1.0 with no climate defs); `setFuel(f(generator) -> bool)` for C11. **Until C11 sets it, a wood-fired
generator runs without fuel.** Whoever merges second wires it: C11's refuelable part should answer
the fuel check, and a generator burns `fuel.perDay` of its item while it makes power.
`setOnChange(f())` is called after a step in which any building gained or lost power; `Sim` uses it
to make the lamp layer again, and `Lights::setActive` makes a lamp that draws power shine only while
`running` (a flicked-off or unpowered lamp is dark). Lamps with no power part (torch lamp, campfire)
always shine.

## Commands, events, snapshot, save

- `FlickCommand{thing, on}`; `ShortCircuitCommand{thing}` (a dev command and T3's hook: empties the
  net's batteries and destroys the building if it is a conduit or switch; the explosion and fire
  that follow are F9's and E4's, which listen to the `ShortCircuit` event's `amount`).
- Events `PowerChanged` (thing, def, cell, message "on" or "off") and `ShortCircuit` (thing, def,
  cell, `amount` in Wd).
- `ThingView` has `power` (`PowerState`), `powerNet`, `powerWatts` and `batteryFill` (0 to 1, -1 for
  other buildings); `Snapshot::powerNets` lists the nets (id, generation, demand, stored, capacity,
  served, counts) for U19's overlay.
- Save section `power` (version 1): a building's `on`, `powered`, `stored` and `draw`. Nets, net ids
  and watts are made again on loading. Saves from before the section load with every power building
  on and unpowered until the first step. All four are in the state hash.

## Not here

Fuel use (C11), the wind source (E1), heaters and coolers pushing room temperature (E2),
the short-circuit event itself (T3), the overlay and power panel
(U19), solar flares, power drain from damaged batteries, and conduits under buildings.
