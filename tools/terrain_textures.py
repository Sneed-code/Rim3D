#!/usr/bin/env python3
"""A2 terrain textures: download the free (CC0) ground, sand, gravel and rock textures listed in
assets/terrain_textures.json (Poly Haven or ambientCG) into game/Content/ThirdParty/TerrainTextures/.
The TerrainMaterials commandlet then imports them and makes the terrain material (assets/manifest.json).

  python tools/terrain_textures.py     download what is missing

A layer with "source": "local" is never downloaded: put <layer>_color.jpg and <layer>_normal.jpg there yourself.
"""
import io, json, shutil, sys, urllib.request, zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LIST = ROOT / "assets" / "terrain_textures.json"
DEST = ROOT / "game" / "Content" / "ThirdParty" / "TerrainTextures"
AGENT = {"User-Agent": "Rim3D-terrain-textures"}


def get(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers=AGENT), timeout=120) as r:
        return r.read()


def polyhaven(layer, tex_id):
    files = json.loads(get(f"https://api.polyhaven.com/files/{tex_id}"))
    for kind, name in (("Diffuse", "color"), ("nor_dx", "normal")):
        (DEST / f"{layer}_{name}.jpg").write_bytes(get(files[kind]["1k"]["jpg"]["url"]))


def ambientcg(layer, tex_id):
    z = zipfile.ZipFile(io.BytesIO(get(f"https://ambientcg.com/get?file={tex_id}_1K-JPG.zip")))
    for suffix, name in (("_Color.jpg", "color"), ("_NormalDX.jpg", "normal")):
        member = next(n for n in z.namelist() if n.endswith(suffix))
        (DEST / f"{layer}_{name}.jpg").write_bytes(z.read(member))


def main():
    layers = json.loads(LIST.read_text(encoding="utf-8"))["layers"]
    DEST.mkdir(parents=True, exist_ok=True)
    for layer, info in layers.items():
        have = [p for ext in ("jpg", "png") for p in [DEST / f"{layer}_color.{ext}", DEST / f"{layer}_normal.{ext}"] if p.exists()]
        if info["source"] == "local":
            print(f"{layer}: local files {'found' if len(have) >= 2 else 'MISSING'} in {DEST}")
            continue
        if (DEST / f"{layer}_color.jpg").exists() and (DEST / f"{layer}_normal.jpg").exists():
            continue
        {"polyhaven": polyhaven, "ambientcg": ambientcg}[info["source"]](layer, info["id"])
        print("downloaded", layer, info["id"])


if __name__ == "__main__":
    main()
