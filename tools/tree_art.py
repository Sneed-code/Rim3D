#!/usr/bin/env python3
"""L6 trees: download the free (CC0) Poly Haven tree models listed in assets/tree_art.json as glTF at the size the
list asks (2k or 1k) into game/Content/ThirdParty/Trees/<id>/ (gitignored). The TreeArt commandlet imports them
whole (full materials, Nanite) into /Game/Rim3D/Generated/Trees/ (assets/manifest.json, docs/design/terrain.md).

  python tools/tree_art.py
"""
import json
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LIST = ROOT / "assets" / "tree_art.json"
DEST = ROOT / "game" / "Content" / "ThirdParty" / "Trees"
AGENT = {"User-Agent": "Rim3D-tree-art"}


def fetch(url, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    with urllib.request.urlopen(urllib.request.Request(url, headers=AGENT), timeout=300) as r:
        path.write_bytes(r.read())


def main():
    trees = json.loads(LIST.read_text(encoding="utf-8"))["trees"]
    for tree in trees:
        tid, res = tree["id"], tree.get("res", "2k")
        folder = DEST / tid
        gltf = folder / f"{tid}.gltf"
        if gltf.exists():
            continue
        with urllib.request.urlopen(urllib.request.Request(f"https://api.polyhaven.com/files/{tid}", headers=AGENT), timeout=60) as r:
            files = json.load(r)["gltf"][res]["gltf"]
        for rel, info in files.get("include", {}).items():
            fetch(info["url"], folder / rel)
        fetch(files["url"], gltf)
        print("downloaded", tid, res)


if __name__ == "__main__":
    main()
