#!/usr/bin/env python3
"""L4 stuff textures: download the free (CC0) wood, stone block and metal plate textures listed in
assets/stuff_textures.json (ambientCG) into game/Content/ThirdParty/StuffTextures/. The SurfaceMaterials
commandlet then imports them and makes one material per surface (assets/manifest.json).

  python tools/stuff_textures.py     download what is missing

A surface with "source": "local" is never downloaded: put <surface>_color, _normal and _rough (.jpg or .png) there yourself.
"""
import io, json, urllib.request, zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LIST = ROOT / "assets" / "stuff_textures.json"
DEST = ROOT / "game" / "Content" / "ThirdParty" / "StuffTextures"
AGENT = {"User-Agent": "Rim3D-stuff-textures"}
KINDS = (("_Color.jpg", "color"), ("_NormalDX.jpg", "normal"), ("_Roughness.jpg", "rough"))


def get(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers=AGENT), timeout=300) as r:
        return r.read()


def ambientcg(surface, tex_id):
    z = zipfile.ZipFile(io.BytesIO(get(f"https://ambientcg.com/get?file={tex_id}_2K-JPG.zip")))
    for suffix, name in KINDS:
        member = next(n for n in z.namelist() if n.endswith(suffix))
        (DEST / f"{surface}_{name}.jpg").write_bytes(z.read(member))


def main():
    surfaces = json.loads(LIST.read_text(encoding="utf-8"))["surfaces"]
    DEST.mkdir(parents=True, exist_ok=True)
    for surface, info in surfaces.items():
        have = all(any((DEST / f"{surface}_{name}.{ext}").exists() for ext in ("jpg", "png")) for _, name in KINDS)
        if info["source"] == "local":
            print(f"{surface}: local files {'found' if have else 'MISSING'} in {DEST}")
            continue
        if have:
            continue
        ambientcg(surface, info["id"])
        print("downloaded", surface, info["id"])


if __name__ == "__main__":
    main()
