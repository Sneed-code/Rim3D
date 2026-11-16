# 0010: Real-time combat; no hit or dodge dice in melee

Date 2026-11-16. Replaces RimWorld's melee hit chance and dodge chance in `docs/design/melee.md` and adds to the
combat parts of `game-design.md`.

sneed: fights play like a real-time shooter and souls-like melee. A blow or bullet hits what it physically meets;
stats change how well a pawn fights (melee skill: damage and attack speed, -20% at 0 to +20% at 20), not whether a
roll succeeds. AI fighters defend by choice (shield block, parry, dodge) with timing, fight as groups (colonists
never charge alone; raiders can be more reckless by profile), take cover under fire, and routed raiders fight to
the death when cornered or caught.

RimWorld's melee hit and dodge chance curves no longer drive anything. Guns keep a skill-sized aim spread (a real
unsteady hand), with every bullet a traced line. The sim still decides everything (deterministic); Unreal shows it.

Design and phases: `docs/design/real-time-combat.md`.
