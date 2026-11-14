# 0008: Lovin is in, abstract and adults only

Date 2026-11-14. Replaces the "no sexual content" line in `docs/game-design.md` and `BACKLOG.md`.

sneed asked for RimWorld's lovin, because relationships matter to mood, marriage and social events. It is in,
under these rules:

- **Adults only, always.** The sim checks a real age of at least `SocialRules.adultAge` (18) on both partners. The game
  has no children (sneed dropped them on 2026-11-14), and the check stays regardless.
- **Abstract.** The game runs in real time, so there is no fade or cut-scene. The couple stays lying in its bed under the
  covers for the duration, cuddling in one of three poses (side by side, spooning, embracing). A kiss is fine. Unreal
  shows a small hearts icon above them. No nudity in it, no sex animation and nothing explicit.
- Shared sleeping comes first: the double bed (two slots), and partners moving in together.

Design: `docs/design/lovin.md`.
