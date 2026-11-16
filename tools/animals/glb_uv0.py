#!/usr/bin/env python3
"""Rewrite a .glb so every material samples its textures from the first UV set (TEXCOORD_0).

Some Sketchfab models (the Akhal-Teke horse) bake their textures onto a later UV set (texCoord 2),
and Unreal's glTF materials read UV0, so the body imports grey. This moves the UV set each material
uses into TEXCOORD_0 on the primitives that use that material, and sets texCoord to 0. Only the
JSON chunk changes; the binary data is copied as it is.

  python3 tools/animals/glb_uv0.py IN.glb OUT.glb
"""
import json, struct, sys

TEX_KEYS = ("baseColorTexture", "metallicRoughnessTexture", "normalTexture", "occlusionTexture", "emissiveTexture")


def texcoord_of(material):
    found = set()
    pbr = material.get("pbrMetallicRoughness", {})
    for key in TEX_KEYS:
        info = pbr.get(key) or material.get(key)
        if info is not None:
            found.add(info.get("texCoord", 0))
    return found


def reset_texcoord(material):
    pbr = material.get("pbrMetallicRoughness", {})
    for key in TEX_KEYS:
        info = pbr.get(key) or material.get(key)
        if info is not None:
            info.pop("texCoord", None)


def main(src, dst):
    with open(src, "rb") as f:
        magic, version, _ = struct.unpack("<III", f.read(12))
        json_len, json_type = struct.unpack("<II", f.read(8))
        gltf = json.loads(f.read(json_len))
        rest = f.read()
    changed = 0
    for mesh in gltf.get("meshes", []):
        for prim in mesh["primitives"]:
            if "material" not in prim:
                continue
            sets = texcoord_of(gltf["materials"][prim["material"]])
            if len(sets) != 1 or 0 in sets:
                continue
            uv = sets.pop()
            attrs = prim["attributes"]
            if f"TEXCOORD_{uv}" in attrs:
                attrs["TEXCOORD_0"], attrs[f"TEXCOORD_{uv}"] = attrs[f"TEXCOORD_{uv}"], attrs.get("TEXCOORD_0", attrs[f"TEXCOORD_{uv}"])
                changed += 1
    for material in gltf.get("materials", []):
        sets = texcoord_of(material)
        if len(sets) == 1 and 0 not in sets:
            reset_texcoord(material)
    data = json.dumps(gltf, separators=(",", ":")).encode()
    data += b" " * ((4 - len(data) % 4) % 4)
    with open(dst, "wb") as f:
        f.write(struct.pack("<III", magic, version, 12 + 8 + len(data) + len(rest)))
        f.write(struct.pack("<II", len(data), json_type))
        f.write(data)
        f.write(rest)
    print(f"{dst}: {changed} primitive(s) moved to UV0")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
