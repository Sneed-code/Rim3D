"""Joins the skinned meshes of a glb into one mesh, for the animal imports in assets/animal_art.json that name a
`mergeFrom` file.

Why: Sketchfab's FBX conversions split some animals into several skinned meshes on one skin (the WildMesh bear: body,
head, eyes). Unreal imports each as its own skeletal mesh, and the game draws the extra ones on the body's pose, but
these files keep each part in a different unit, so the bear's head came out as a giant grey shape over the map and the
body headless. One mesh with one primitive per part imports as one skeletal mesh with a section per material.

Only the JSON chunk changes: every skinned mesh node on the first skin hands its primitives to the first one and drops
its mesh. The binary chunk is copied as it is.

  python tools/animals/glb_merge.py            writes each `file` from its `mergeFrom` when missing or older
"""
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LIST = ROOT / "assets" / "animal_art.json"


def merge(source: Path, target: Path) -> str:
    data = source.read_bytes()
    magic, version, _length = struct.unpack_from("<III", data, 0)
    if magic != 0x46546C67:
        return f"{source.name}: not a glb"
    json_len, json_type = struct.unpack_from("<II", data, 12)
    gltf = json.loads(data[20:20 + json_len].decode("utf-8"))
    rest = data[20 + json_len:]

    skinned = [node for node in gltf.get("nodes", []) if "mesh" in node and node.get("skin") == 0]
    if len(skinned) < 2:
        return f"{source.name}: {len(skinned)} skinned mesh, nothing to join"
    first = gltf["meshes"][skinned[0]["mesh"]]
    for node in skinned[1:]:
        first["primitives"].extend(gltf["meshes"][node["mesh"]]["primitives"])
        del node["mesh"]
        del node["skin"]
    # Morph target weights no longer match the joined primitives.
    first.pop("weights", None)
    for primitive in first["primitives"]:
        primitive.pop("targets", None)

    text = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    text += b" " * ((4 - len(text) % 4) % 4)
    body = struct.pack("<II", len(text), json_type) + text + rest
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(struct.pack("<III", magic, version, 12 + len(body)) + body)
    return f"{source.name}: joined {len(skinned)} skinned meshes into {target.name}"


def main() -> int:
    entries = json.loads(LIST.read_text(encoding="utf-8")).get("imports", [])
    failed = 0
    for entry in entries:
        if "mergeFrom" not in entry:
            continue
        source, target = ROOT / entry["mergeFrom"], ROOT / entry["file"]
        if not source.exists():
            print(f"{entry['model']}: {source} is missing (download it again, assets/manifest.json)")
            failed += 1
            continue
        if target.exists() and target.stat().st_mtime >= source.stat().st_mtime:
            print(f"{entry['model']}: {target.name} is up to date")
            continue
        print(f"{entry['model']}: {merge(source, target)}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
