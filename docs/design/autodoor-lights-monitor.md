# Autodoor, flood light, vitals monitor

Three buildings that gave their research projects (`Autodoor`, `AdvancedLights`, `VitalsMonitor`) something to unlock.
Numbers from the RimWorld wiki, checked 2026-10-28; looks and the tie to our systems are ours.

| Def | Numbers |
|---|---|
| `Autodoor` | A door (`Door`) that also costs 40 steel + 2 components, 1100 ticks (1584 s), 160 HP, 50 W. `door.openSpeedFactor` 4. |
| `FloodLight` | 50 steel, 600 ticks (864 s), 120 HP, not flammable, 50 W, light radius 17.45 (a standing lamp's is 10). |
| `VitalsMonitor` | 50 steel + 3 components, 6000 ticks (8640 s), 100 HP, 70% flammable, 80 W. `building.vitalsMonitor`. |

- **Autodoor** (`MovementSystem::updateDoors`, set up in `Sim.cpp`): while it has power, the opening counts four steps for
  every one a plain door counts (RimWorld: 400%; the wiki's 480% for wood and 180% for stone are not modelled). Without
  power (no net, switch off, not enough power) it is a plain door. Closing is unchanged.
- **Flood light**: a lamp like the others. The wiki's 100 W is for the time before Advanced lights; it is 50 W once
  researched, which is the only time it can be built.
- **Vitals monitor** (`JobSystem::bedMonitored`): a powered monitor in any of the 8 cells around a bed's footprint adds +7%
  tend quality (`MedicineJobs`), +5% surgery success and +7% to the bed's tend-quality offset used in surgery (`SurgeryJobs`)
  and +0.02 immunity gain speed while the patient rests in a hospital bed (`HealthSystem::setImmunityBonus`). One
  monitor counts per bed; extra ones do nothing. A monitor can serve up to 8 beds. Any bed counts here, as the code reads
  the numbers off the bed it finds; only a hospital bed has numbers worth adding to.
