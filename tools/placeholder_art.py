#!/usr/bin/env python3
"""A1 placeholder art: download the free (CC0) Poly Haven meshes listed in assets/placeholder_art.json
and set `visual.mesh` in each def. The PlaceholderArt commandlet then imports them (assets/manifest.json).

  python tools/placeholder_art.py            download what is missing, set the defs
  python tools/placeholder_art.py --defs     only set the defs (no network)
"""
import json, re, sys, urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LIST = ROOT / "assets" / "placeholder_art.json"
DEST = ROOT / "game" / "Content" / "ThirdParty" / "PlaceholderArt"
GAME_FOLDER = "/Game/Rim3D/Generated/Art"


def fetch(url, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    req = urllib.request.Request(url, headers={"User-Agent": "Rim3D-placeholder-art"})
    with urllib.request.urlopen(req, timeout=120) as r:
        path.write_bytes(r.read())


def download(mesh_id):
    folder = DEST / mesh_id
    if (folder / f"{mesh_id}_1k.gltf").exists():
        return
    req = urllib.request.Request(f"https://api.polyhaven.com/files/{mesh_id}", headers={"User-Agent": "Rim3D-placeholder-art"})
    with urllib.request.urlopen(req, timeout=60) as r:
        files = json.load(r)["gltf"]["1k"]["gltf"]
    for rel, info in files.get("include", {}).items():
        fetch(info["url"], folder / rel)
    fetch(files["url"], folder / f"{mesh_id}_1k.gltf")
    print("downloaded", mesh_id)


def set_defs(meshes):
    """Put "mesh": "<path>" first in the def's visual object; a def without its own visual object gets one."""
    for path in sorted((ROOT / "data" / "defs" / "Things").glob("*.json")):
        text = path.read_text(encoding="utf-8")
        original = text
        for m in meshes:
            asset = f"{GAME_FOLDER}/SM_{m['def']}.SM_{m['def']}"
            head = re.compile(r'"defName":\s*"%s"\s*,' % re.escape(m["def"]))
            found = head.search(text)
            if not found:
                continue
            nxt = text.find('"defName"', found.end())
            block = text[found.end():nxt if nxt >= 0 else len(text)]
            own = re.search(r'"visual":\s*\{(\s*"mesh":\s*"[^"]*"\s*,?)?', block)
            if own:
                a, b = found.end() + own.start(), found.end() + own.end()
                rest = text[b:].lstrip()
                comma = "" if rest.startswith("}") else ","
                gap = "" if text[b:b + 1].isspace() else " "
                text = text[:a] + '"visual": { "mesh": "%s"%s' % (asset, comma) + gap + text[b:]
            else:
                text = text[:found.end()] + ' "visual": { "mesh": "%s" },' % asset + text[found.end():]
        if text != original:
            path.write_text(text, encoding="utf-8", newline="")
            print("updated", path.name)


def main():
    meshes = json.loads(LIST.read_text(encoding="utf-8"))["meshes"]
    if "--defs" not in sys.argv:
        for mesh_id in sorted({m["id"] for m in meshes}):
            download(mesh_id)
    set_defs(meshes)


if __name__ == "__main__":
    main()
