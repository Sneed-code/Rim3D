#!/usr/bin/env python3
"""A5 clothes art: download the free (CC0) Poly Haven props listed in assets/clothes_art.json and set
`visual.worn` in each apparel def. The ClothesArt commandlet then makes the meshes (assets/manifest.json).

  python tools/clothes_art.py            download what is missing, set the defs
  python tools/clothes_art.py --defs     only set the defs (no network)
"""
import json, re, sys, urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LIST = ROOT / "assets" / "clothes_art.json"
DEST = ROOT / "game" / "Content" / "ThirdParty" / "ClothesArt"
GAME_FOLDER = "/Game/Rim3D/Generated/Clothes"
HEADERS = {"User-Agent": "Rim3D-clothes-art"}


def fetch(url, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    with urllib.request.urlopen(urllib.request.Request(url, headers=HEADERS), timeout=120) as r:
        path.write_bytes(r.read())


def download(mesh_id):
    folder = DEST / mesh_id
    if (folder / f"{mesh_id}_1k.gltf").exists():
        return
    req = urllib.request.Request(f"https://api.polyhaven.com/files/{mesh_id}", headers=HEADERS)
    with urllib.request.urlopen(req, timeout=60) as r:
        files = json.load(r)["gltf"]["1k"]["gltf"]
    for rel, info in files.get("include", {}).items():
        fetch(info["url"], folder / rel)
    fetch(files["url"], folder / f"{mesh_id}_1k.gltf")
    print("downloaded", mesh_id)


def worn_json(spec):
    if "parts" in spec:
        parts = ", ".join('{ "mesh": "%s/SK_%s_%d.SK_%s_%d" }' % (GAME_FOLDER, spec["def"], i, spec["def"], i)
                          for i in range(len(spec["parts"])))
    else:
        parts = '{ "mesh": "%s/SM_%s.SM_%s", "bone": "head" }' % (GAME_FOLDER, spec["def"], spec["def"])
    return '"visual": { "worn": [%s] }' % parts


def set_defs(specs):
    """Put visual.worn on each apparel def (a def with a visual object of its own is left alone)."""
    for path in sorted((ROOT / "data" / "defs" / "Things").glob("*.json")):
        text = path.read_text(encoding="utf-8")
        original = text
        for spec in specs:
            found = re.search(r'"defName":\s*"%s"\s*,' % re.escape(spec["def"]), text)
            if not found:
                continue
            nxt = text.find('"defName"', found.end())
            block = text[found.end():nxt if nxt >= 0 else len(text)]
            if '"visual"' in block:
                text = re.sub(r'("defName":\s*"%s"\s*,\s*)"visual":\s*\{\s*"worn":\s*\[.*?\]\s*\},?\s*' % re.escape(spec["def"]),
                              lambda m: m.group(1), text, count=1, flags=re.S)
            found = re.search(r'"defName":\s*"%s"\s*,' % re.escape(spec["def"]), text)
            text = text[:found.end()] + " " + worn_json(spec) + "," + text[found.end():]
        if text != original:
            path.write_text(text, encoding="utf-8", newline="")
            print("updated", path.name)


def main():
    data = json.loads(LIST.read_text(encoding="utf-8"))
    specs = data["garments"] + data["props"]
    if "--defs" not in sys.argv:
        for mesh_id in sorted({p["id"] for p in data["props"] if "id" in p}):
            download(mesh_id)
    set_defs(specs)


if __name__ == "__main__":
    main()
