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


def closing_brace(text, start):
    """Index of the } that closes the object whose { is just before `start` (strings may hold braces)."""
    depth, i, in_string = 1, start, False
    while i < len(text):
        ch = text[i]
        if in_string:
            if ch == "\\":
                i += 1
            elif ch == '"':
                in_string = False
        elif ch == '"':
            in_string = True
        elif ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return len(text)


def set_defs(meshes):
    """Set "mesh": "<path>" in the def's visual object (first if it had none); a def without its own visual object gets one."""
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
            own = re.search(r'"visual":\s*\{', block)
            if own:
                start = found.end() + own.end()
                end = closing_brace(text, start)
                inner = text[start:end]
                mesh = re.search(r'"mesh":\s*"[^"]*"', inner)
                if mesh:
                    # Already has a mesh (anywhere in the object): point it at ours rather than adding a second key.
                    text = text[:start + mesh.start()] + '"mesh": "%s"' % asset + text[start + mesh.end():]
                else:
                    comma = "" if inner.strip() == "" else ","
                    text = text[:start] + ' "mesh": "%s"%s' % (asset, comma) + text[start:]
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
