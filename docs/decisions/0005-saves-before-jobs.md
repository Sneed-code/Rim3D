# 0005: Saves from before the job system no longer load

_Decided 2026-10-25 in task C1 (Claude). Easy to undo: an older format can be read again later if a
save turns out to matter._

**What.** Save format 6 (C1) replaces the Stage 0 hauling placeholder with the job system: pawns
have a worker, a Work tab row and what they carry, and the map has stockpile zones. Loading a save
of format 1 to 5 fails with a plain message ("made before the job system") instead of converting it.

**Why.** The placeholder's state (a hauling stage, "reserved by", "stored") has no clean
counterpart in jobs and zones, and converting it would be code kept forever for saves that were
only ever test saves: the in-game quicksaves from U12 are a few hours old and were made to try the
save keys, not to keep a colony.

**What it replaces.** Rule 7 in `docs/design/saves.md` ("old saves keep loading until a decision
says otherwise") for formats 1 to 5. From format 6 on, the rule holds again: a layout change bumps
the version and keeps reading the old one.
