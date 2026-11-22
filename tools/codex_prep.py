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

Moving parts (Neon models, docs/design/neon-assets.md): a building or item with "parts" keeps each named part as its own
object, <Name>_<Part>, exported to its own <Name>_<Part>_Game.fbx next to the body's <Name>_Game.fbx (the rest of the
model), all in the same frame, so a part drawn with no motion sits where it belongs. Every glTF action in the GLB is
sampled frame by frame and written as tracks of each part's motion away from that rest pose, in Unreal's frame (cm),
to <Name>_anims.json and to game/Content/Rim3D/Generated/PropAnims/<Name>.json (not in Git), which the game reads
(PropAnims.h). Guns get the tracks too (their parts stay in the one FBX as before). Actions that move no mesh (a
material that pulses) are listed with no parts.
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
ANIM_OUT = os.path.join(ROOT, "game", "Content", "Rim3D", "Generated", "PropAnims")
# Everything transform_all did to the model since import (stand up, centre), so animation deltas can follow it.
FRAME = [mathutils.Matrix.Identity(4)]


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
    FRAME[0] = m @ FRAME[0]


def part_of(obj_name, parts):
    """The named part an object belongs to (its name starts with the part's name), or None."""
    return next((p for p in parts if obj_name.startswith(p)), None)


def sample_actions(parts):
    """Every action, sampled each frame: {action: {"seconds", "fps", "frames": {part: [Matrix world, ...]}}} plus the
    rest matrices {part: Matrix} (the pose the meshes are baked in). Run straight after the import, before anything is
    joined or removed: a part's motion can come from an animated parent (an empty at its pivot)."""
    scene = bpy.context.scene
    objs = list(scene.objects)
    saved = {}
    for o in objs:
        saved[o.name] = (o.location.copy(), o.rotation_mode, o.rotation_quaternion.copy(), o.rotation_euler.copy(),
                         o.scale.copy())
    tracked = {}
    for o in objs:
        p = part_of(o.name, parts) if o.type == "MESH" else None
        if p and (p not in tracked or o.name == p):
            tracked[p] = o

    def reset():
        for o in objs:
            ad = o.animation_data
            if ad:
                ad.action = None
                ad.use_nla = False
            loc, mode, quat, eul, scl = saved[o.name]
            o.location = loc
            o.rotation_mode = mode
            o.rotation_quaternion = quat
            o.rotation_euler = eul
            o.scale = scl

    fps = scene.render.fps / (scene.render.fps_base or 1.0)
    out = {}
    for act in list(bpy.data.actions):
        reset()
        users = []
        for o in objs:
            slot = None
            if hasattr(act, "slots"):
                slot = next((s for s in act.slots if s.identifier[2:] == o.name), None)
                if slot is None:
                    continue
            elif not any(fc.data_path for fc in act.fcurves):
                continue
            if not o.animation_data:
                o.animation_data_create()
            o.animation_data.action = act
            if slot is not None:
                o.animation_data.action_slot = slot
            users.append(o.name)
        if not users:
            continue
        start, end = int(math.floor(act.frame_range[0])), int(math.ceil(act.frame_range[1]))
        frames = {p: [] for p in tracked}
        for f in range(start, end + 1):
            scene.frame_set(f)
            for p, o in tracked.items():
                frames[p].append(o.matrix_world.copy())
        out[act.name] = {"seconds": (end - start) / fps, "fps": fps, "frames": frames, "users": users}
    reset()
    scene.frame_set(scene.frame_start)
    bpy.context.view_layer.update()
    rest = {p: o.matrix_world.copy() for p, o in tracked.items()}
    return out, rest


def to_unreal(m):
    """A Blender-frame transform (m) as one in Unreal's frame (cm, Y flipped): S M S, translation x 100."""
    s = mathutils.Matrix.Diagonal((1.0, -1.0, 1.0, 1.0))
    u = s @ m @ s
    u.translation = u.translation * 100.0
    return u


def anim_tracks(sampled, rest):
    """Per action, per part that moves: times and the part's motion away from its rest pose (delta = now x rest^-1, in
    the model's final frame, then Unreal's), as location cm, rotation quaternion [x, y, z, w] and scale. Samples a
    straight blend of their neighbours can rebuild are dropped."""
    frame, inv = FRAME[0], FRAME[0].inverted()
    anims = {}
    for name, a in sampled.items():
        parts = {}
        for p, mats in a["frames"].items():
            r_inv = rest[p].inverted()
            keys = []
            for i, m in enumerate(mats):
                d = to_unreal(frame @ (m @ r_inv) @ inv)
                loc, rot, scl = d.decompose()
                keys.append((i / a["fps"], loc, rot, scl))
            moves = any((k[1].length > 0.05 or k[2].angle > math.radians(0.1) or (k[3] - mathutils.Vector((1, 1, 1))).length > 1e-3)
                        for k in keys)
            if not moves:
                continue
            kept = [keys[0]]
            for i in range(1, len(keys) - 1):
                prev, nxt = kept[-1], keys[i + 1]
                u = (keys[i][0] - prev[0]) / max(nxt[0] - prev[0], 1e-6)
                lerp_loc = prev[1].lerp(nxt[1], u)
                q = prev[2].slerp(nxt[2], u)
                ok = ((lerp_loc - keys[i][1]).length < 0.02 and q.rotation_difference(keys[i][2]).angle < math.radians(0.05)
                      and (prev[3].lerp(nxt[3], u) - keys[i][3]).length < 1e-3
                      and prev[2].rotation_difference(nxt[2]).angle < math.radians(90))
                if not ok:
                    kept.append(keys[i])
            kept.append(keys[-1])
            parts[p] = {
                "t": [round(k[0], 4) for k in kept],
                "loc": [[round(v, 3) for v in k[1]] for k in kept],
                "rot": [[round(k[2].x, 5), round(k[2].y, 5), round(k[2].z, 5), round(k[2].w, 5)] for k in kept],
                "scale": [[round(v, 4) for v in k[3]] for k in kept],
            }
        anims[name] = {"seconds": round(a["seconds"], 4), "parts": parts}
    return anims


def describe_track(tr):
    """One line on how far a part goes: the biggest turn (degrees, about which Unreal axis) and shift (cm)."""
    best_angle, best_axis = 0.0, (0, 0, 0)
    for r in tr["rot"]:
        q = mathutils.Quaternion((r[3], r[0], r[1], r[2]))
        if q.angle > best_angle:
            best_angle, best_axis = q.angle, tuple(round(v, 2) for v in q.axis)
    shift = max(mathutils.Vector(l).length for l in tr["loc"])
    scale = max(max(abs(v - 1.0) for v in s) for s in tr["scale"])
    return f"turn {math.degrees(best_angle):.0f} deg about {best_axis}, shift {shift:.1f} cm, scale {scale:+.2f}, {len(tr['t'])} keys"


def stand_up(meshes, empties, size, force=None):
    """Pick the turn (none, or 90 degrees about X either way) that makes the bounds best match the expected size.
    force ("none", "x-90" or "x+90", the prep entry's "turn") skips the guess, for a model whose height and depth are
    about the same (the hi-tech research bench: 2.0 m tall, 2.2 m deep, so it came in lying on its back)."""
    turns = (("none", mathutils.Matrix.Identity(4)),
             ("x-90", mathutils.Matrix.Rotation(math.radians(-90), 4, "X")),
             ("x+90", mathutils.Matrix.Rotation(math.radians(90), 4, "X")))
    if force:
        rot = dict(turns)[force]
        if force != "none":
            transform_all(meshes, empties, rot)
        return force
    if not size:
        return "none"
    want = mathutils.Vector(size)
    best, best_err = None, 1e9
    for label, rot in turns:
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


def pivots_by_part(pivots, part):
    """The rest origin of a part's own object (the exact name, else the first that starts with it)."""
    if part in pivots:
        return {part: pivots[part]}
    hit = next((k for k in pivots if k.startswith(part)), None)
    return {part: pivots[hit]} if hit else {}


def prep(src_root, name, entry):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    glb = os.path.join(src_root, name, f"{name}.glb")
    if not os.path.exists(glb):
        print(f"CODEXPREP {name}: no {glb}")
        return False
    bpy.ops.import_scene.gltf(filepath=glb)
    bpy.context.view_layer.update()  # matrix_world is stale until then
    kind = entry["kind"]
    FRAME[0] = mathutils.Matrix.Identity(4)
    parts = entry.get("parts", [])
    sampled, rest = sample_actions(parts) if parts else ({}, {})
    drop = set(entry.get("drop", []))
    empties = [{"name": o.name, "pos": o.matrix_world.translation.copy()} for o in bpy.context.scene.objects if o.type == "EMPTY"]
    meshes = [o for o in bpy.context.scene.objects if o.type == "MESH" and not any(o.name.startswith(d) for d in drop)]
    for o in list(bpy.context.scene.objects):
        if o not in meshes:
            bpy.data.objects.remove(o, do_unlink=True)
    pivots = {o.name: o.matrix_world.translation.copy() for o in meshes}  # each object's own origin, before baking
    apply_all(meshes)
    # "align": parts posed open in the model (a break-open barrel) are turned about their own origin, in the gun's
    # side plane, until their long axis runs along +X again.
    import numpy as np
    for o in meshes:
        if not any(o.name.startswith(a) for a in entry.get("align", [])):
            continue
        pts = world_points([o])
        c = pts - pts.mean(axis=0)
        axis = np.linalg.svd(c, full_matrices=False)[2][0]
        if axis[0] < 0:
            axis = -axis
        angle = math.atan2(axis[2], axis[0])
        piv = pivots[o.name]
        o.data.transform(mathutils.Matrix.Translation(piv) @ mathutils.Matrix.Rotation(angle, 4, "Y") @ mathutils.Matrix.Translation(-piv))
        print(f"CODEXPREP {name}: {o.name} turned {math.degrees(angle):.1f} degrees about its pivot {tuple(round(v, 3) for v in piv)}")
    turn = "none"
    if kind in ("building", "item"):
        turn = stand_up(meshes, empties, entry.get("size"), entry.get("turn"))
        # "centerOn": the footprint is the bounds of the objects whose name starts with it (a turret's base), so an
        # overhanging barrel doesn't move the model off its cells.
        base = [o for o in meshes if entry.get("centerOn") and o.name.startswith(entry["centerOn"])]
        lo, hi = world_bounds(base or meshes)
        lo.z = world_bounds(meshes)[0].z
        transform_all(meshes, empties, mathutils.Matrix.Translation((-(lo.x + hi.x) / 2, -(lo.y + hi.y) / 2, -lo.z)))
        kept = []
        for part in parts:
            objs = [o for o in meshes if part_of(o.name, [part])]
            if objs:
                kept.append(join(objs, f"{name}_{part}"))
                meshes = [o for o in meshes if o not in objs]
            else:
                print(f"CODEXPREP {name}: no object for part {part}")
        kept.insert(0, join(meshes, name))
    else:
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
    for p in parts:
        key = f"{name}_{p}" if kind in ("building", "item") else p
        if key in info["parts"] and p in pivots_by_part(pivots, p):
            info["parts"][key]["pivotCm"] = unreal_cm(FRAME[0] @ pivots_by_part(pivots, p)[p])
    anims = anim_tracks(sampled, rest) if sampled else {}
    if anims:
        info["anims"] = {a: {"seconds": v["seconds"], "parts": sorted(v["parts"])} for a, v in anims.items()}
        doc = {"name": name, "note": "Made by tools/codex_prep.py from the GLB's glTF actions: each part's motion away from its rest pose, Unreal frame, cm.",
               "anims": anims}
        for path in (os.path.join(folder, f"{name}_anims.json"), os.path.join(ANIM_OUT, f"{name}.json")):
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w") as f:
                json.dump(doc, f, separators=(",", ":"))
        for a, v in anims.items():
            if not v["parts"]:
                print(f"CODEXPREP {name}: action {a} ({v['seconds']} s) moves no mesh (a material change?)")
            for p, tr in v["parts"].items():
                print(f"CODEXPREP {name}: action {a} ({v['seconds']} s) {p}: {describe_track(tr)}")
    with open(os.path.join(folder, f"{name}_info.json"), "w") as f:
        json.dump(info, f, indent=1)
    def export(objs, path):
        bpy.ops.object.select_all(action="DESELECT")
        for o in objs:
            o.select_set(True)
        bpy.ops.export_scene.fbx(filepath=path, use_selection=True, object_types={"MESH"}, mesh_smooth_type="FACE",
                                 add_leaf_bones=False, bake_anim=False, path_mode="STRIP", use_tspace=True)
    out = os.path.join(folder, f"{name}_Game.fbx")
    if kind in ("building", "item") and len(kept) > 1:
        export(kept[:1], out)  # the body; each moving part to its own file, in the same frame
        for o in kept[1:]:
            export([o], os.path.join(folder, f"{o.name}_Game.fbx"))
    else:
        export(kept, out)
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
