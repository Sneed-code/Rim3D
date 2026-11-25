#!/usr/bin/env python3
"""L4 stuff textures: download the free (CC0) wood, stone block and metal plate textures listed in
assets/stuff_textures.json (ambientCG or Poly Haven) into game/Content/ThirdParty/StuffTextures/. The SurfaceMaterials
commandlet then imports them and makes one material per surface (assets/manifest.json).

  python tools/stuff_textures.py     download what is missing or changed

A surface with "source": "local" is never downloaded: put <surface>_color, _normal and _rough (.jpg or .png) there yourself.
Each download leaves <surface>.source beside the files naming where it came from, so a surface whose source or id
changes in the list is downloaded again (wood went from ambientCG's WoodSiding008 to Poly Haven planks, 2026-11-25).
A Poly Haven surface may list "ids": the first that downloads is used.
"""
import io, json, urllib.request, zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LIST = ROOT / "assets" / "stuff_textures.json"
DEST = ROOT / "game" / "Content" / "ThirdParty" / "StuffTextures"
AGENT = {"User-Agent": "Rim3D-stuff-textures"}
KINDS = (("_Color.jpg", "color"), ("_NormalDX.jpg", "normal"), ("_Roughness.jpg", "rough"))
POLYHAVEN_KINDS = (("Diffuse", "color"), ("nor_dx", "normal"), ("Rough", "rough"))


def get(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers=AGENT), timeout=300) as r:
        return r.read()


def ambientcg(surface, info):
    tex_id = info["id"]
    z = zipfile.ZipFile(io.BytesIO(get(f"https://ambientcg.com/get?file={tex_id}_2K-JPG.zip")))
    for suffix, name in KINDS:
        member = next(n for n in z.namelist() if n.endswith(suffix))
        (DEST / f"{surface}_{name}.jpg").write_bytes(z.read(member))
    return tex_id


def polyhaven(surface, info):
    errors = []
    for tex_id in info.get("ids", [info["id"]]):
        try:
            files = json.loads(get(f"https://api.polyhaven.com/files/{tex_id}"))
            maps = {name: get(files[kind]["2k"]["jpg"]["url"]) for kind, name in POLYHAVEN_KINDS}
        except Exception as e:  # a name that isn't on Poly Haven: try the next one
            errors.append(f"{tex_id}: {e}")
            continue
        for name, data in maps.items():
            (DEST / f"{surface}_{name}.jpg").write_bytes(data)
        return tex_id
    raise RuntimeError(f"no Poly Haven texture for {surface}: " + "; ".join(errors))


def stamp_of(info):
    return f"{info['source']}:{info['id']}"


def main():
    surfaces = json.loads(LIST.read_text(encoding="utf-8"))["surfaces"]
    DEST.mkdir(parents=True, exist_ok=True)
    for surface, info in surfaces.items():
        have = all(any((DEST / f"{surface}_{name}.{ext}").exists() for ext in ("jpg", "png")) for _, name in KINDS)
        if info["source"] == "local":
            print(f"{surface}: local files {'found' if have else 'MISSING'} in {DEST}")
            continue
        stamp = DEST / f"{surface}.source"
        # Files from before the stamps were written count as the list's, except a surface whose source moved.
        old = stamp.read_text(encoding="utf-8").strip() if stamp.exists() else ("ambientcg:" + info["id"] if have else "")
        if have and old == stamp_of(info):
            continue
        got = {"ambientcg": ambientcg, "polyhaven": polyhaven}[info["source"]](surface, info)
        stamp.write_text(stamp_of(info), encoding="utf-8")
        print("downloaded", surface, info["source"], got)


if __name__ == "__main__":
    main()
