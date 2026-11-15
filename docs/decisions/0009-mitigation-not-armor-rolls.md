# 0009: Armour divides damage; RimWorld's armor rolls are gone

Date 2026-11-15. Replaces RimWorld's armor rule in `docs/design/combat.md` ("Armor") and the armor numbers of
`docs/design/apparel.md`.

sneed set a new damage model: three damage classes (blunt, sharp, elemental with subtypes), and every worn piece
and creature hide divides damage by (1 + %), multiplied together, wherever the hit lands. No deflection rolls, no
cap, no sharp-to-blunt turn. Quality scales the %s from half (awful) to double (legendary); worn-down pieces work
at 67%. Clothing (shirt, pants, hat) sits under armour (head, chest, legs); the better of hat and helmet counts.

RimWorld's armor ratings no longer drive anything. Mitigation numbers are sneed's, or were drafted from RimWorld's
ratios and reviewed by sneed (the draft doc in the thread "Rework armour, damage types and clothing quality").
Weapons keep RimWorld's damage and armour penetration; penetration now takes its share off each %.

Design: `docs/design/armour.md`.
