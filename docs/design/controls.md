# Controls

sneed, 2026-11-17. Every key the player uses when playing as a pawn can be changed, and the keys are saved with the
settings. Defaults below; **Settings > Controls (change keys)** shows them all, click one and press the new key (Esc
cancels; "Reset all keys to default" puts them back). Code reads keys only through `Rim3DKeys` in
`game/Source/Rim3D/KeyBindings.h` (saved in `GameUserSettings.ini`, section `[Rim3D.Keys]`); a new player key is a new
`ERim3DAction` with a row in `KeyBindings.cpp`, never a fixed `EKeys` value.

## Playing as a pawn (defaults)

| Key | Does |
| --- | --- |
| W A S D | walk, relative to the camera |
| Left Ctrl (hold) | walk slowly |
| Left Shift | **sprint on / off**. It stays on until pressed again, also when you take over another colonist |
| Space | jump |
| T | **roll** in third person, **dash** in first person (the same move in the sim; the first person view stays level, blurs and widens). Left Alt does nothing now |
| F (hold) | **interact**: pick up, mine, chop, harvest, build, sleep in a bed, and take a weapon lying in reach into your hands |
| X | put down what you hold |
| E | take the nearest weapon (within 8 m) |
| Q | switch weapon |
| R | reload |
| C / Z | crouch / prone, on and off |
| G | throw a grenade |
| left mouse | shoot or swing |
| right mouse (hold) | aim a gun, or block with a melee weapon or shield |
| Tab | focus the next enemy (lock-on focus, its own task) |
| Middle mouse | lock on / let go (lock-on focus; Tab shuffles targets). Unreal's F-key debug views are cleared in DefaultInput.ini |
| N | sleep here at night |
| I | inventory |
| B | build mode |
| V | first / third person |
| O | overseer view and back (in the overseer view it takes control of the selected colonist). Was Tab, which is now focus |
| Y | take over the colonist you look at. Was T, which is now roll / dash |
| H | patch a bleeding wound (body health) |
| J | free hand mode on / off (H was taken by patching); while on, left mouse grips, right mouse looks, the wheel sets the reach (its own task) |

The page also has the **Lock-on focus** on / off setting for the focus task (`Rim3DKeys::IsLockOnFocusEnabled`).

**Resources are picked up by walking past them**: no key. Every 10 game seconds (a third of a real second) any
resource stack within 1.5 m of your colonist goes into your bag, as much as fits under the carry limit
([inventory.md](inventory.md) "Picking up resources"). Things in a stockpile, stone chunks and food are left alone.

## AI pawns and sprinting

AI pawns jog (RimWorld's normal pace) and **sprint only in a fight, fleeing or an emergency**, at FighterRules
`sprintSpeedFactor` (RimWorld's Sprint, 4/3 of a jog). Fighters sprint when they flee or fall back from enemies they
know of, head for a shelter, or charge into melee from 3 to 30 m (`FighterSystem::sprinting`, [focus.md](focus.md));
they also sprint carrying a downed friend to cover, and colonists sprint to rescue a downed colonist or beat out a fire
(`Sim::pawnSprinting`). Visitors and traders never hurry; animals keep their own speeds. The player's
colonist follows the sprint toggle above instead.
