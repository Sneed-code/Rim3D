# Wounds on bodies (task V7, sim half)

What the sim hands Unreal so injuries can be drawn on the skin: where each wound is, what kind it is, how big, whether
it is bandaged and whether it still bleeds. Everything here is read from `Health` when a snapshot is written. Nothing
is saved or hashed, no sim rule changes, and old saves work as they are.

## In the snapshot

Every pawn with a body has `ThingView::firstWound` and `woundCount`: its wounds are
`Snapshot::wounds[firstWound .. firstWound + woundCount)`. Its blood is `ThingView::bloodLoss` (0 to 1, 1 is death) and
`ThingView::bleedPerDay`. A `WoundView` (`ColonySim/Wounds.h`) is:

| Field | Meaning |
| --- | --- |
| `part`, `partDef` | the part's position in the pawn's Bodies def, and its BodyParts def (`Arm`, `Hand`, `Eye`...) |
| `region` | the coarse skin piece: `Torso`, `Head`, `LeftArm`, `RightArm`, `LeftLeg`, `RightLeg` |
| `kind` | `Cut`, `Bruise`, `Burn`, `Bite`, `Gunshot`, `Infection`, `Scar`, `Missing` |
| `severity` | the share of the part's hit points the wound took, 0 to 1 (a lost part is 1) |
| `tended`, `tendQuality` | bandaged, and how well (below 0 while untended) |
| `bleeding` | untended and the wound or lost part bleeds: draw it wet |

Order: the injuries in the order they happened, then the lost parts. A lost part's children are gone with it and are
not listed, so a lost hand is one `Missing` wound on the hand, not one per finger.

Wounds on parts inside the body (organs, bones: the BodyParts the Bodies def marks `Inside`) are left out. A hit that
goes in also hurts the outside part over it (HealthRules `innerHurtsOuter`), and that wound is the one that shows.

## Data

- `Hediffs` defs of class Injury or Scar carry `woundKind`: Cut, Stab and Scratch are `Cut`; Gunshot `Gunshot`; Bite and
  Shredded `Bite`; Bruise, Crush and Crack `Bruise`; Burn and Frostbite `Burn`; Scar and OldWound `Scar`. A def without it
  is a `Cut` (a `Scar` for class Scar). Test `Wounds: every injury and scar def names a kind`.
- `BodyParts` defs carry `skinRegion` (`Torso`, `Head`, `Arm`, `Leg`); a part without one takes its parent's. Neck, eyes,
  ears, nose, jaw and tongue are `Head`; shoulder, arm, hand and fingers `Arm`; leg, foot and toes `Leg`. The side comes
  from the first label going up the body that says "left" or "right" (`left big toe`, `right pinky`); an arm or leg with
  no side is left.
- `BodyPartInfo::def` (HealthSystem::bodyParts) now gives the part's BodyParts def name.

## What is not here yet

- **Infection.** `WoundKind::Infection` is in the list and any injury def with `"woundKind": "Infection"` shows as one,
  but nothing in the sim makes one until H2 (diseases) is merged. H2 stores an infection as a `Disease` with a wound's
  `part`; when it lands, `WoundViews::append` (`Private/WoundViews.cpp`) should show the wound it sits on as `Infection`.
- **Drawing.** Skin-material layers per region, bandages, the blood-soaked look and blackened burns are the PC half of V7.
  Dismemberment (V8) reads the `Missing` wounds and the `PawnPartLost` event.

## Where the code is

`Private/WoundViews.cpp` works out the regions of a body once per pawn def and then turns each pawn's injuries and lost
parts into `WoundView`s; `Sim::writeSnapshot` calls it only for pawns that have any. Tests: `Tests/WoundTests.cpp`.
