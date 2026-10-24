# Third-party assets

Big downloaded packs (Megascans, MetaHumans, Fab packs) are not stored in Git. They live in
`game/Content/Megascans/`, `game/Content/MetaHumans/`, `game/Content/Fab/` or
`game/Content/ThirdParty/`, which `.gitignore` excludes.

Every pack we use gets an entry in `manifest.json` so it can be downloaded again:

```json
{ "name": "Game Animation Sample", "source": "https://www.fab.com/...", "licence": "Fab standard licence",
  "version": "5.x", "installedTo": "game/Content/ThirdParty/GameAnimationSample", "usedFor": "protagonist locomotion" }
```

sneed keeps one backup copy of `game/Content/` on another drive.
