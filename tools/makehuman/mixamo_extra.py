# Adds Mixamo clips that were prepared on another copy of the game to this one (docs/design/makehuman.md, "Animation
# clips"; sneed 2026-11-22). The punches (Lead Jab, Cross Punch, Hook Punch) and the downward blows at a body on the
# ground (Stabbing, Bayonet Stab, Sword And Shield Slash, Great Sword Slash, Stomping) went through
# tools/makehuman/mixamo_prep.py in a test copy (Rim3D-motion) and never reached the one game: the moves that use them
# had nothing to play. Their prepared FBX files (already on the shared rig) live in assets/raw/mixamo/extra/ (not in
# Git: Mixamo's files). This copies each into game/Saved/Mocap/Mixamo/ and lists it in that folder's mixamo.json, so
# `MakeHumanArt -Clips` (all of them, or -Only=<names>) imports it like any other clip.
#   python tools/makehuman/mixamo_extra.py            (from the repository root)
import json
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "assets", "raw", "mixamo", "extra")
DST = os.path.join(ROOT, "game", "Saved", "Mocap", "Mixamo")
# Name (the FBX's file name) -> category, the folder under Animations/Mixamo/ the clip lands in.
CATEGORIES = {
    "LeadJab": "Fighting", "CrossPunch": "Fighting", "HookPunch": "Fighting",
    "KnifeStabLow": "Fighting", "SpearStabLow": "Fighting", "OneHandHackLow": "Fighting",
    "TwoHandHackLow": "Fighting", "UnarmedStomp": "Fighting",
}


def main():
    manifest_path = os.path.join(DST, "mixamo.json")
    if not os.path.isfile(manifest_path):
        print(f"mixamo_extra: no {manifest_path} (run mixamo_prep.py first); nothing done")
        return 0
    if not os.path.isdir(SRC):
        print(f"mixamo_extra: no {SRC}; nothing to add")
        return 0
    with open(manifest_path, encoding="utf-8") as f:
        manifest = json.load(f)
    clips = manifest.setdefault("clips", [])
    by_name = {c.get("name"): c for c in clips}
    added = []
    for file in sorted(os.listdir(SRC)):
        name, ext = os.path.splitext(file)
        if ext.lower() != ".fbx":
            continue
        target = os.path.join(DST, file)
        shutil.copy2(os.path.join(SRC, file), target)
        entry = {"name": name, "category": CATEGORIES.get(name, "Other"), "fbx": target.replace("\\", "/")}
        if by_name.get(name) != entry:
            if name in by_name:
                by_name[name].update(entry)
            else:
                clips.append(entry)
            added.append(name)
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1)
    print(f"mixamo_extra: {len(added)} clip(s) listed: {', '.join(added) if added else 'none new'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
