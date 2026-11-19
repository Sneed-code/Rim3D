"""Blender script: turn the Codex models (tools/codex_assets/) into game files for the GunArt commandlet.

Run from the repo root (Blender 5.x, no window):
  blender -b --factory-startup -P tools/codex_prep.py -- <Rim3D_Codex folder> [Name ...]
With no names it does every model listed in assets/codex_art.json ("prep").

For each model it reads <Rim3D_Codex>/<Name>/<Name>.glb and writes assets/raw/codex/<Name>/ (not in Git):
  <Name>_Game.fbx        the mesh, Blender metres; Unreal's FBX import makes it cm and turns Blender -Y into Unreal +Y
  T_<Slot>_BaseColor.png one set per material, the slot named <Name> (one material) or <Name>_<n>; also _Normal (OpenGL),
  T_<Slot>_ORM.png       _ORM (R occlusion, G roughness, B metallic) and _Emissive where the model has them
  <Name>_info.json       sizes, parts, sockets (empties) and material slots in Unreal's frame, cm (Y flipped)

Kinds ("kind" in the prep entry):
  building / item  everything joined into one object named <Name>; turned so the footprint lies in x/y with the height
                   in z (a model saved lying on its side is stood up), its footprint middle at x = y = 0 and its bottom
                   at z = 0. The front faces Blender -Y, which the import makes Unreal +Y: the sim's north, the way
                   building meshes face (docs/design/building-draw.md).
  gun              the origin is kept (the firing hand's palm on the grip), +X out of the muzzle; the named "parts" are
                   kept as their own objects and everything else is joined into "Body".
"drop" lists objects to leave out (a second copy, a stack). Empties are recorded as sockets and not exported.
"""
import json
import math
import os
import sys

import bpy
import mathutils

ROOT = os.getcwd()
OUT = os.path.join(ROOT, "assets", "raw", "codex")
CONFIG = os.path.join(ROOT, "assets", "codex_art.json")


def world_points(objs, m=None):
    """Every vertex in world space (bound_box goes stale after mesh.transform), as an N x 3 array."""
    import numpy as np
    out = []
    for o in objs:
        co = np.empty(len(o.data.vertices) * 3, dtype=np.float64)
        o.data.vertices.foreach_get("co", co)
        co = co.reshape(-1, 3)
        w = np.array((m or mathutils.Matrix.Identity(4)) @ o.matrix_world)
        out.append(co @ w[:3, :3].T + w[:3, 3])
    return np.concatenate(out) if out else np.zeros((1, 3))


def world_bounds(objs):
    pts = world_points(objs)
    return mathutils.Vector(pts.min(axis=0)), mathutils.Vector(pts.max(axis=0))


def unreal_cm(v):
    """A Blender point (m) in Unreal's frame (cm, Y flipped)."""
    return [round(v.x * 100.0, 2), round(-v.y * 100.0, 2), round(v.z * 100.0, 2)]


def unreal_box(lo, hi):
    return {"min": [round(lo.x * 100, 2), round(-hi.y * 100, 2), round(lo.z * 100, 2)],
            "max": [round(hi.x * 100, 2), round(-lo.y * 100, 2), round(hi.z * 100, 2)]}


def apply_all(meshes):
    """Unparent every mesh keeping where it is and bake its transform into the mesh."""
    worlds = {o.name: o.matrix_world.copy() for o in meshes}  # all first: unparenting changes the children's
    for o in meshes:
        world = worlds[o.name]
        o.parent = None
        o.data = o.data.copy() if o.data.users > 1 else o.data
        o.data.transform(world)
        o.matrix_world = mathutils.Matrix.Identity(4)


def transform_all(meshes, empties, m):
    for o in meshes:
        o.data.transform(m)
    for e in empties:
        e["pos"] = m @ e["pos"]


def stand_up(meshes, empties, size):
    """Pick the turn (none, or 90 degrees about X either way) that makes the bounds best match the expected size."""
    if not size:
        return "none"
    want = mathutils.Vector(size)
    best, best_err = None, 1e9
    for label, rot in (("none", mathutils.Matrix.Identity(4)),
                       ("x-90", mathutils.Matrix.Rotation(math.radians(-90), 4, "X")),
                       ("x+90", mathutils.Matrix.Rotation(math.radians(90), 4, "X"))):
        pts = world_points(meshes, rot)
        ext = list(pts.max(axis=0) - pts.min(axis=0))
        # Footprint may be either way round in x/y; the height must match.
        err = abs(ext[2] - want.z) * 2 + min(abs(ext[0] - want.x) + abs(ext[1] - want.y),
                                               abs(ext[0] - want.y) + abs(ext[1] - want.x))
        if err < best_err - 1e-3:
            best, best_err = (label, rot), err
    if best[0] != "none":
        transform_all(meshes, empties, best[1])
        # x-90 (tried first, so it wins a tie) is the usual case: a model written Z-up into glTF, which is Y-up, comes in
        # with its top towards -Y; turning -90 degrees about X gives back exactly the model as made, front at -Y.
    return best[0]


def join(objs, name):
    bpy.ops.object.select_all(action="DESELECT")
    for o in objs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]
    if len(objs) > 1:
        bpy.ops.object.join()
    j = bpy.context.view_layer.objects.active
    j.name = name
    j.data.name = name
    return j


def image_of(socket):
    """The image feeding a shader input (through normal map / separate colour nodes), or None."""
    seen = set()
    stack = [socket]
    while stack:
        s = stack.pop()
        for link in s.links:
            n = link.from_node
            if n in seen:
                continue
            seen.add(n)
            if n.type == "TEX_IMAGE" and n.image:
                return n.image
            stack.extend(i for i in n.inputs if i.is_linked)
    return None


def pixels(img, size=None):
    import numpy as np
    if size and (img.size[0], img.size[1]) != size:
        img = img.copy()
        img.scale(size[0], size[1])
    a = np.empty(img.size[0] * img.size[1] * 4, dtype=np.float32)
    img.pixels.foreach_get(a)
    return a.reshape(img.size[1], img.size[0], 4)


def save_png(arr, path, colorspace):
    import numpy as np
    h, w = arr.shape[:2]
    img = bpy.data.images.new(os.path.basename(path), w, h, alpha=False, float_buffer=False)
    img.colorspace_settings.name = colorspace
    img.pixels.foreach_set(np.ascontiguousarray(arr, dtype=np.float32).ravel())
    img.filepath_raw = path
    img.file_format = "PNG"
    img.save()
    bpy.data.images.remove(img)


def write_textures(mat, slot, folder):
    """T_<slot>_BaseColor / _Normal / _ORM / _Emissive from a glTF-imported material. Returns the files written."""
    import numpy as np
    bsdf = next((n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED"), None) if mat and mat.use_nodes else None
    files = {}
    if not bsdf:
        return files
    base = image_of(bsdf.inputs["Base Color"])
    normal = image_of(bsdf.inputs["Normal"])
    rough = image_of(bsdf.inputs["Roughness"])
    metal = image_of(bsdf.inputs["Metallic"])
    emit = image_of(bsdf.inputs["Emission Color"]) if "Emission Color" in bsdf.inputs else None
    occl = None
    for n in mat.node_tree.nodes:  # the glTF importer keeps occlusion in a "glTF Material Output"/"glTF Settings" group
        if n.type == "GROUP" and n.node_tree and "glTF" in n.node_tree.name and "Occlusion" in n.inputs:
            occl = image_of(n.inputs["Occlusion"])
    stem = os.path.join(folder, f"T_{slot}_")
    if base:
        save_png(pixels(base), stem + "BaseColor.png", "sRGB")
        files["baseColor"] = stem + "BaseColor.png"
    else:
        c = list(bsdf.inputs["Base Color"].default_value)
        save_png(np.tile(np.array(c, dtype=np.float32), (4, 4, 1)), stem + "BaseColor.png", "sRGB")
        files["baseColor"] = stem + "BaseColor.png"
    if normal:
        save_png(pixels(normal), stem + "Normal.png", "Non-Color")
        files["normal"] = stem + "Normal.png"
    ref = rough or metal or occl
    if ref:
        size = (ref.size[0], ref.size[1])
        h, w = size[1], size[0]
        orm = np.ones((h, w, 4), dtype=np.float32)
        orm[..., 1] = pixels(rough, size)[..., 1] if rough else bsdf.inputs["Roughness"].default_value
        orm[..., 2] = pixels(metal, size)[..., 2] if metal else bsdf.inputs["Metallic"].default_value
        orm[..., 0] = pixels(occl, size)[..., 0] if occl else 1.0
        save_png(orm, stem + "ORM.png", "Non-Color")
        files["orm"] = stem + "ORM.png"
    if emit:
        save_png(pixels(emit), stem + "Emissive.png", "sRGB")
        files["emissive"] = stem + "Emissive.png"
    return files


def prep(src_root, name, entry):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    glb = os.path.join(src_root, name, f"{name}.glb")
    if not os.path.exists(glb):
        print(f"CODEXPREP {name}: no {glb}")
        return False
    bpy.ops.import_scene.gltf(filepath=glb)
    bpy.context.view_layer.update()  # matrix_world is stale until then
    kind = entry["kind"]
    drop = set(entry.get("drop", []))
    empties = [{"name": o.name, "pos": o.matrix_world.translation.copy()} for o in bpy.context.scene.objects if o.type == "EMPTY"]
    meshes = [o for o in bpy.context.scene.objects if o.type == "MESH" and not any(o.name.startswith(d) for d in drop)]
    for o in list(bpy.context.scene.objects):
        if o not in meshes:
            bpy.data.objects.remove(o, do_unlink=True)
    apply_all(meshes)
    turn = "none"
    if kind in ("building", "item"):
        turn = stand_up(meshes, empties, entry.get("size"))
        # "centerOn": the footprint is the bounds of the objects whose name starts with it (a turret's base), so an
        # overhanging barrel doesn't move the model off its cells.
        base = [o for o in meshes if entry.get("centerOn") and o.name.startswith(entry["centerOn"])]
        lo, hi = world_bounds(base or meshes)
        lo.z = world_bounds(meshes)[0].z
        transform_all(meshes, empties, mathutils.Matrix.Translation((-(lo.x + hi.x) / 2, -(lo.y + hi.y) / 2, -lo.z)))
        kept = [join(meshes, name)]
    else:
        parts = entry.get("parts", [])
        kept = []
        for part in parts:
            objs = [o for o in meshes if o.name.startswith(part)]
            if objs:
                kept.append(join(objs, part))
                meshes = [o for o in meshes if o not in objs]
            else:
                print(f"CODEXPREP {name}: no object for part {part}")
        if meshes:
            kept.insert(0, join(meshes, "Body"))
    bpy.context.view_layer.update()

    # One material slot name per material: <Name> or <Name>_<n>, so the GunArt slotTextureDir finds T_<slot>_*.png.
    folder = os.path.join(OUT, name)
    os.makedirs(folder, exist_ok=True)
    mats = []
    for o in kept:
        for s in o.material_slots:
            if s.material and s.material not in mats:
                mats.append(s.material)
    slots = {}
    for i, m in enumerate(mats):
        slot = name if len(mats) == 1 else f"{name}_{i}"
        files = write_textures(m, slot, folder)
        slots[slot] = {"from": m.name, "maps": sorted(files)}
        m.name = slot
    lo, hi = world_bounds(kept)
    info = {
        "name": name, "kind": kind, "turned": turn,
        "boundsCm": unreal_box(lo, hi),
        "sizeM": [round(hi.x - lo.x, 3), round(hi.y - lo.y, 3), round(hi.z - lo.z, 3)],
        "parts": {o.name: {"boundsCm": unreal_box(*world_bounds([o])), "tris": sum(len(p.vertices) - 2 for p in o.data.polygons)} for o in kept},
        "sockets": {e["name"]: unreal_cm(e["pos"]) for e in empties},
        "slots": slots,
    }
    with open(os.path.join(folder, f"{name}_info.json"), "w") as f:
        json.dump(info, f, indent=1)
    bpy.ops.object.select_all(action="DESELECT")
    out = os.path.join(folder, f"{name}_Game.fbx")
    bpy.ops.export_scene.fbx(filepath=out, use_selection=False, object_types={"MESH"}, mesh_smooth_type="FACE",
                             add_leaf_bones=False, bake_anim=False, path_mode="STRIP", use_tspace=True)
    print(f"CODEXPREP {name}: {kind}, turned {turn}, {info['sizeM']} m, parts {list(info['parts'])}, slots {list(slots)} -> {out}")
    return True


def main():
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if not args:
        print("usage: blender -b --factory-startup -P tools/codex_prep.py -- <Rim3D_Codex folder> [Name ...]")
        return
    src, names = args[0], args[1:]
    with open(CONFIG) as f:
        table = json.load(f)["prep"]
    ok = 0
    for name in names or list(table):
        ok += 1 if prep(src, name, table[name]) else 0
    print(f"CODEXPREP done: {ok} of {len(names or table)}")


main()
