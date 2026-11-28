# Blender half of the scanned skin bake (docs/design/scanned-skin.md; tools/makehuman/scan_skin.py runs it).
#   blender -b --factory-startup -P tools/makehuman/scan_skin_blend.py -- render <base> <work folder>
#   blender -b --factory-startup -P tools/makehuman/scan_skin_blend.py -- bake <base> <work folder>
#
# Both modes first line the MetaHuman body and face (game/Saved/ScanSkin/mesh_*.json, from the ScanSkin commandlet) up
# with the MakeHuman base (game/Saved/MakeHuman/<base>/<base>.fbx): turned, placed and scaled by the two skeletons, then
# each MetaHuman bone moved onto the matching MakeHuman joint and the skin carried by its weights. "render" then draws
# both faces from the front for the landmark finder (scan_skin.py, OpenCV); "bake" moves the MetaHuman face onto the
# found landmarks, settles every MetaHuman point onto the MakeHuman skin, and bakes the scanned colour, normal,
# roughness and cavity maps onto the base's texture layout with Cycles (skin_scan*.png beside its FBX).
import json
import math
import os
import sys
import time

import bpy
import numpy as np
from mathutils import Matrix, Vector
from mathutils.bvhtree import BVHTree

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.environ.get("RIM3D_ROOT") or os.path.abspath(os.path.join(HERE, "..", ".."))
SCAN = os.environ.get("SCAN_SKIN_DIR") or os.path.join(REPO, "game", "Saved", "ScanSkin")
BASES = os.environ.get("SCAN_SKIN_BASES") or os.path.join(REPO, "game", "Saved", "MakeHuman")
CONFIG = json.load(open(os.environ.get("SCAN_SKIN_CONFIG") or os.path.join(REPO, "assets", "scan_skin.json"), encoding="utf-8"))
STARTED = time.time()

# The 33 inner landmarks of the 68-point layout (nose, eyes, outer lips): the jaw line and the brows are left out (the
# jaw is the outline seen from the front and MakeHuman's brows are separate meshes, not in the skin), and so are the
# inner lips (a ray there goes through the mouth's slit).
INNER_LANDMARKS = list(range(27, 60))

# Torso chains: a MetaHuman joint in one of them is placed at the same share of the chain's length on the MakeHuman
# skeleton (MetaHuman has five spine bones and two neck bones; MakeHuman's game rig three and one).
CHAINS = [["pelvis", "spine_01", "spine_02", "spine_03", "spine_04", "spine_05", "neck_01"],
          ["neck_01", "neck_02", "head"]]
FINGERS = ["thumb", "index", "middle", "ring", "pinky"]


def log(*a):
    print(f"[scan_skin {time.time() - STARTED:6.1f}s]", *a, flush=True)


def norm(v):
    n = np.linalg.norm(v)
    return v / n if n > 1e-9 else v


# ---- The MakeHuman base ------------------------------------------------------------------------------------------

def reset_scene():
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    for coll in (bpy.data.meshes, bpy.data.materials, bpy.data.images, bpy.data.armatures, bpy.data.cameras, bpy.data.lights):
        for block in list(coll):
            coll.remove(block)


def base_paths(base):
    folder = os.path.join(BASES, base)
    return folder, os.path.join(folder, base + ".fbx")


def load_target(base):
    """Imports the base's FBX and returns (rig, skin objects, eye objects, brow objects, joints by name in world space)."""
    folder, fbx = base_paths(base)
    bpy.ops.import_scene.fbx(filepath=fbx, use_anim=False, ignore_leaf_bones=False)
    rig = next(o for o in bpy.data.objects if o.type == "ARMATURE")
    rig.data.pose_position = "REST"
    manifest = {}
    mpath = os.path.join(folder, "materials.json")
    if os.path.exists(mpath):
        manifest = json.load(open(mpath, encoding="utf-8"))
    skin_names = {k for k, v in manifest.items() if v.get("skin")}
    meshes = [o for o in bpy.data.objects if o.type == "MESH"]

    def label(o):
        names = [o.name.lower()]
        for slot in o.material_slots:
            if slot.material:
                names += [slot.material.name.lower(), manifest.get(slot.material.name, {}).get("blender", "").lower()]
        return " ".join(names)

    skins = [o for o in meshes if any(s.material and s.material.name in skin_names for s in o.material_slots)]
    if not skins:
        skins = [max(meshes, key=lambda o: len(o.data.vertices))]
        log("no skin in materials.json; using the biggest mesh", skins[0].name)
    eyes = [o for o in meshes if o not in skins and "eye" in label(o) and "eyebrow" not in label(o) and "lash" not in label(o)]
    brows = [o for o in meshes if o not in skins and "eyebrow" in label(o)]
    joints = {b.name: np.array(rig.matrix_world @ b.head_local) for b in rig.data.bones}
    tails = {b.name: np.array(rig.matrix_world @ b.tail_local) for b in rig.data.bones}
    log("target", base, "rig", rig.name, len(joints), "bones; skin", [o.name for o in skins], "eyes", [o.name for o in eyes])
    return rig, skins, eyes, brows, joints, tails, skin_names


def make_target_object(skins, skin_names):
    """One mesh of the base's skin (only the skin material's faces), in world space, without shape keys or modifiers:
    the bake's target."""
    copies = []
    for obj in skins:
        mesh = obj.data.copy()
        if mesh.shape_keys:
            # Drop the morphs: only the base shape is baked.
            tmp = bpy.data.objects.new("tmp", mesh)
            bpy.context.scene.collection.objects.link(tmp)
            bpy.context.view_layer.objects.active = tmp
            tmp.shape_key_clear()
            bpy.data.objects.remove(tmp, do_unlink=True)
        mesh.transform(obj.matrix_world)
        copy = bpy.data.objects.new(obj.name + "_target", mesh)
        bpy.context.scene.collection.objects.link(copy)
        copies.append(copy)
        # Only the skin's faces.
        if skin_names:
            skin_slots = {i for i, s in enumerate(obj.material_slots) if s.material and s.material.name in skin_names}
            import bmesh
            bm = bmesh.new()
            bm.from_mesh(mesh)
            gone = [f for f in bm.faces if f.material_index not in skin_slots]
            bmesh.ops.delete(bm, geom=gone, context="FACES")
            bm.to_mesh(mesh)
            bm.free()
    if len(copies) > 1:
        bpy.ops.object.select_all(action="DESELECT")
        for c in copies:
            c.select_set(True)
        bpy.context.view_layer.objects.active = copies[0]
        bpy.ops.object.join()
    target = copies[0]
    target.name = "Target"
    mesh = target.data
    if mesh.uv_layers:
        mesh.uv_layers.active_index = 0
        mesh.uv_layers[0].active_render = True
    log("target mesh", len(mesh.vertices), "points", len(mesh.polygons), "faces, UV", mesh.uv_layers[0].name if mesh.uv_layers else None)
    return target


def mesh_world_points(obj):
    mw = obj.matrix_world
    return np.array([mw @ v.co for v in obj.data.vertices]) if obj.data.vertices else np.zeros((0, 3))


# ---- The MetaHuman parts -----------------------------------------------------------------------------------------

class Part:
    """One MetaHuman mesh from the commandlet's JSON, in Blender's axes and metres (points, triangles, UVs, slots,
    skin weights, skeleton). Only the triangles of the slots kept stay."""

    def __init__(self, name, keep=None):
        data = json.load(open(os.path.join(SCAN, f"mesh_{name}.json"), encoding="utf-8"))
        self.name = name
        self.slots = data["slots"]
        p = np.array(data["positions"], dtype=np.float64).reshape(-1, 3)
        self.points = np.stack([p[:, 0], -p[:, 1], p[:, 2]], axis=1) * 0.01      # Unreal (cm, left-handed) -> Blender
        tris = np.array(data["triangles"], dtype=np.int64).reshape(-1, 3)
        uvs = np.array(data["uvs"], dtype=np.float64).reshape(-1, 3, 2)
        tslots = np.array(data["triangleSlots"], dtype=np.int64)
        # Mirroring Y flips the winding: swap two corners so the faces still point out.
        tris = tris[:, [0, 2, 1]]
        uvs = uvs[:, [0, 2, 1]]
        self.bones = data["bones"]
        self.parents = data["parents"]
        bp = np.array(data["bonePositions"], dtype=np.float64).reshape(-1, 3)
        self.joints = {n: np.array([q[0], -q[1], q[2]]) * 0.01 for n, q in zip(self.bones, bp)}
        offsets = data["skinOffsets"]
        sb, sw = data["skinBones"], data["skinWeights"]
        self.skin = [list(zip(sb[offsets[i]:offsets[i + 1]], sw[offsets[i]:offsets[i + 1]])) for i in range(len(offsets) - 1)]
        self.all_tris, self.all_uvs, self.all_slots = tris, uvs, tslots
        if keep is not None:
            wanted = [i for i, s in enumerate(self.slots) if any(k.lower() in s.lower() for k in keep)]
            mask = np.isin(tslots, wanted)
            tris, uvs, tslots = tris[mask], uvs[mask], tslots[mask]
        self.tris, self.uvs, self.tslots = tris, uvs, tslots
        log(f"part {name}: {len(self.points)} points, {len(tris)} of {len(self.all_tris)} triangles kept, slots {self.slots}, {len(self.bones)} bones")

    def slot_points(self, keyword):
        """The points of the triangles in slots whose name holds the keyword (an eye's points)."""
        wanted = [i for i, s in enumerate(self.slots) if keyword.lower() in s.lower()]
        idx = np.unique(self.all_tris[np.isin(self.all_slots, wanted)])
        return self.points[idx]


def skeleton_frame(joints):
    """Origin (pelvis), scale (pelvis to head) and axes (left, forward, up) of a skeleton, from its joints alone."""
    up = norm(joints["head"] - joints["pelvis"])
    left = joints["thigh_l"] - joints["thigh_r"]
    left = norm(left - up * np.dot(left, up))
    forward = np.cross(left, up)
    return joints["pelvis"], np.linalg.norm(joints["head"] - joints["pelvis"]), np.stack([left, forward, up], axis=1)


def toes_forward(joints):
    d = (joints["ball_l"] - joints["foot_l"]) + (joints["ball_r"] - joints["foot_r"])
    return norm(d)


def place_sources(parts, target_joints):
    """Turns, moves and scales the MetaHuman parts (one rigid move for all, from the body's skeleton) onto the base. A
    skeleton whose left is on its right after the change of axes is mirrored first (checked by the toes)."""
    body = parts["body"]
    o_s, s_s, f_s = skeleton_frame(body.joints)
    if np.dot(toes_forward(body.joints), f_s[:, 1]) < 0:
        log("MetaHuman skeleton came out mirrored; flipping X")
        for part in parts.values():
            part.points[:, 0] *= -1
            part.tris = part.tris[:, [0, 2, 1]]
            part.uvs = part.uvs[:, [0, 2, 1]]
            part.all_tris = part.all_tris[:, [0, 2, 1]]
            part.all_uvs = part.all_uvs[:, [0, 2, 1]]
            for n in part.joints:
                part.joints[n] = part.joints[n] * np.array([-1, 1, 1])
        o_s, s_s, f_s = skeleton_frame(body.joints)
    o_t, s_t, f_t = skeleton_frame(target_joints)
    if np.dot(toes_forward(target_joints), f_t[:, 1]) < 0:
        log("WARNING: the base's toes point backwards from its skeleton frame")
    rot = f_t @ f_s.T
    scale = s_t / s_s

    def move(p):
        return o_t + scale * (p - o_s) @ rot.T

    for part in parts.values():
        part.points = move(part.points)
        part.joints = {n: move(v) for n, v in part.joints.items()}
    log(f"placed MetaHuman on the base: scale {scale:.4f}, turn {math.degrees(math.acos(np.clip((np.trace(rot) - 1) / 2, -1, 1))):.1f} deg")
    return scale


# ---- Bones onto bones --------------------------------------------------------------------------------------------

def chain_targets(source, target):
    """Where each MetaHuman torso and neck joint goes: the same share of its chain's length on the base."""
    out = {}
    for chain in CHAINS:
        s_chain = [j for j in chain if j in source]
        t_chain = [j for j in chain if j in target]
        if len(s_chain) < 2 or len(t_chain) < 2 or s_chain[0] != t_chain[0] or s_chain[-1] != t_chain[-1]:
            continue
        s_len = np.cumsum([0] + [np.linalg.norm(source[b] - source[a]) for a, b in zip(s_chain, s_chain[1:])])
        t_pts = [target[j] for j in t_chain]
        t_len = np.cumsum([0] + [np.linalg.norm(b - a) for a, b in zip(t_pts, t_pts[1:])])
        for j, s in zip(s_chain, s_len):
            share = s / s_len[-1] * t_len[-1]
            k = min(np.searchsorted(t_len, share, side="right") - 1, len(t_pts) - 2)
            seg = (share - t_len[k]) / max(t_len[k + 1] - t_len[k], 1e-9)
            out[j] = t_pts[k] + (t_pts[k + 1] - t_pts[k]) * np.clip(seg, 0, 1)
    return out


def main_child(name):
    """The joint a joint's bone points to, for the joints we move."""
    side = name[-2:] if name.endswith(("_l", "_r")) else ""
    stem = name[:-2] if side else name
    nxt = {"thigh": "calf", "calf": "foot", "foot": "ball", "clavicle": "upperarm", "upperarm": "lowerarm", "lowerarm": "hand",
           "hand": "middle_01"}
    if stem in nxt:
        return nxt[stem] + side
    for f in FINGERS:
        for a, b in (("01", "02"), ("02", "03")):
            if stem == f"{f}_{a}":
                return f"{f}_{b}" + side
    for chain in CHAINS:
        if name in chain[:-1]:
            return chain[chain.index(name) + 1]
    return None


def secondary(name, joints):
    """A second direction that fixes a bone's roll: across the hips for the pelvis, across the shoulders up the spine
    and neck, across the knuckles for the forearm and hand."""
    side = name[-2:] if name.endswith(("_l", "_r")) else ""
    stem = name[:-2] if side else name
    if stem in ("lowerarm", "hand") and f"index_01{side}" in joints and f"pinky_01{side}" in joints:
        return joints[f"index_01{side}"] - joints[f"pinky_01{side}"]
    if name == "pelvis":
        return joints["thigh_l"] - joints["thigh_r"]
    if name.startswith(("spine", "neck", "head")) and "clavicle_l" in joints and "clavicle_r" in joints:
        return joints["clavicle_l"] - joints["clavicle_r"]
    return None


def turn_between(a, b, a2=None, b2=None):
    """The rotation taking direction a to b (and, given, a2 towards b2 about it)."""
    a, b = norm(a), norm(b)
    if a2 is not None and b2 is not None:
        def frame(z, x):
            x = norm(x - z * np.dot(x, z))
            return np.stack([x, np.cross(z, x), z], axis=1)
        fa, fb = frame(a, a2), frame(b, b2)
        if np.isfinite(fa).all() and np.isfinite(fb).all():
            return fb @ fa.T
    v = np.cross(a, b)
    c = np.dot(a, b)
    if np.linalg.norm(v) < 1e-9:
        return np.eye(3) if c > 0 else -np.eye(3)
    vx = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + vx + vx @ vx * (1 / (1 + c))


def bone_moves(part, target_joints):
    """For each MetaHuman bone, the move (3x3 matrix and its fixed point) that puts it on the base's skeleton: a bone
    with a matching joint goes there, turned to point at its child's match and stretched to its length; any other bone
    (twist, metacarpal, face, corrective) moves with its nearest moved parent."""
    source = part.joints
    goal = chain_targets(source, target_joints)
    for name in source:
        if name in target_joints and name not in goal:
            goal[name] = target_joints[name]
    moves = {}
    order = list(range(len(part.bones)))  # parents come first in an Unreal skeleton
    for i in order:
        name = part.bones[i]
        parent = part.parents[i]
        if name in goal:
            child = main_child(name)
            if child and child in goal and child in source:
                d_s = source[child] - source[name]
                d_t = goal[child] - goal[name]
                s2, t2 = secondary(name, source), secondary(name, goal)
                rot = turn_between(d_s, d_t, s2, t2)
                k = np.linalg.norm(d_t) / max(np.linalg.norm(d_s), 1e-9)
                d = norm(d_s)
                stretch = np.eye(3) + (k - 1) * np.outer(d, d)
                mat = rot @ stretch
            elif parent >= 0 and part.bones[parent] in moves:
                mat = moves[part.bones[parent]][0]
                # keep the parent's turn but not its stretch
                u, _, vt = np.linalg.svd(mat)
                mat = u @ vt
            else:
                mat = np.eye(3)
            moves[name] = (mat, source[name], goal[name])
        elif parent >= 0 and part.bones[parent] in moves:
            mat, src, dst = moves[part.bones[parent]]
            moves[name] = (mat, src, dst)
        else:
            moves[name] = (np.eye(3), np.zeros(3), np.zeros(3))
    return moves


def skin_part(part, target_joints):
    """Carries every point of a MetaHuman part by its skin weights through the bone moves."""
    moves = bone_moves(part, target_joints)
    mats = np.array([moves[b][0] for b in part.bones])
    src = np.array([moves[b][1] for b in part.bones])
    dst = np.array([moves[b][2] for b in part.bones])
    out = np.zeros_like(part.points)
    for i, p in enumerate(part.points):
        total = 0.0
        acc = np.zeros(3)
        for b, w in part.skin[i]:
            if 0 <= b < len(mats):
                acc += w * (dst[b] + mats[b] @ (p - src[b]))
                total += w
        out[i] = acc / total if total > 0 else p
    moved = np.linalg.norm(out - part.points, axis=1)
    part.points = out
    part.joints = {n: moves[n][2] + moves[n][0] @ (v - moves[n][1]) for n, v in part.joints.items()}
    log(f"skinned {part.name}: points moved {np.median(moved) * 100:.1f} cm median, {moved.max() * 100:.1f} cm most")


def make_object(part, name, materials=None):
    """A Blender mesh of a part's kept triangles (unused points dropped), with its UVs (Unreal's V down -> Blender's
    V up) and, given materials, slot per triangle."""
    used, inverse = np.unique(part.tris.reshape(-1), return_inverse=True)
    pts = part.points[used]
    tris = inverse.reshape(-1, 3)
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(pts.tolist(), [], tris.tolist())
    uv = mesh.uv_layers.new(name="UVMap")
    flat = part.uvs.reshape(-1, 2).copy()
    flat[:, 1] = 1.0 - flat[:, 1]
    uv.data.foreach_set("uv", flat.reshape(-1))
    for m in materials or []:
        mesh.materials.append(m)
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(obj)
    return obj, used


def prepare(base):
    reset_scene()
    rig, skins, eyes, brows, tjoints, ttails, skin_names = load_target(base)
    head_keep = [CONFIG.get("headSlot", "head")]
    parts = {"body": Part("body"), "face": Part("face", keep=head_keep)}
    unit = place_sources(parts, tjoints)
    for part in parts.values():
        skin_part(part, tjoints)
    return {"rig": rig, "skins": skins, "eyes": eyes, "brows": brows, "joints": tjoints, "skin_names": skin_names,
            "parts": parts, "height": float(np.linalg.norm(tjoints["head"] - tjoints["pelvis"])) / 0.62}


# ---- Rendering the faces ------------------------------------------------------------------------------------------

def flat_material(name, image_path=None, colour=(0.6, 0.45, 0.4)):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = nt.nodes.get("Principled BSDF")
    bsdf.inputs["Roughness"].default_value = 0.6
    if image_path and os.path.exists(image_path):
        tex = nt.nodes.new("ShaderNodeTexImage")
        tex.image = bpy.data.images.load(image_path, check_existing=True)
        nt.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    else:
        bsdf.inputs["Base Color"].default_value = (*colour, 1)
    return mat


def texture_path(entry):
    """A texture named in the config: an exported one (by asset name) or a synthesized face (synth:<Preset>:albedo)."""
    if not entry:
        return None
    if entry.startswith("synth:"):
        _, preset, kind = entry.split(":")
        return os.path.join(SCAN, f"face_{preset}_{kind}.png")
    path = os.path.join(SCAN, "textures", entry + ".png")
    return path


def face_frame(info):
    """Centre, axes and size of the base's face for the cameras: between the eyes, facing the skeleton's forward."""
    j = info["joints"]
    _, _, f = skeleton_frame(j)
    eyes = info["eyes"]
    unit = info["height"] / 1.75
    if eyes:
        centre = np.mean(np.concatenate([mesh_world_points(o) for o in eyes]), axis=0)
    else:
        centre = j["head"] + f[:, 2] * 0.09 * unit + f[:, 1] * 0.08 * unit
    # Halfway down to the mouth, so the chin is in the picture.
    return centre - f[:, 2] * 0.045 * unit, f, 0.30 * unit


def render_face(objects, centre, frame, size, path, res=800):
    scene = bpy.context.scene
    for o in objects:
        o.data.polygons.foreach_set("use_smooth", np.ones(len(o.data.polygons), dtype=bool))
        o.data.update()
    for o in bpy.data.objects:
        if o.type == "MESH":
            o.hide_render = o not in objects
    cam_data = bpy.data.cameras.get("FaceCam") or bpy.data.cameras.new("FaceCam")
    cam_data.type = "ORTHO"
    cam_data.ortho_scale = size
    cam_data.clip_start = 0.01 * size
    cam_data.clip_end = 20 * size
    cam = bpy.data.objects.get("FaceCam") or bpy.data.objects.new("FaceCam", cam_data)
    if cam.name not in scene.collection.objects:
        scene.collection.objects.link(cam)
    left, forward, up = frame[:, 0], frame[:, 1], frame[:, 2]
    # Camera looks down its -Z with +Y up: put it in front of the face looking back.
    z = forward
    y = up
    x = np.cross(y, z)
    m = Matrix(((x[0], y[0], z[0], 0), (x[1], y[1], z[1], 0), (x[2], y[2], z[2], 0), (0, 0, 0, 1)))
    m.translation = Vector(centre + forward * size * 3)
    cam.matrix_world = m
    scene.camera = cam
    sun_data = bpy.data.lights.get("FaceSun") or bpy.data.lights.new("FaceSun", "SUN")
    sun_data.energy = 2.5
    sun = bpy.data.objects.get("FaceSun") or bpy.data.objects.new("FaceSun", sun_data)
    if sun.name not in scene.collection.objects:
        scene.collection.objects.link(sun)
    sun.matrix_world = m @ Matrix.Rotation(math.radians(-20), 4, "X")
    scene.world = scene.world or bpy.data.worlds.new("World")
    scene.world.use_nodes = True
    bg = scene.world.node_tree.nodes.get("Background")
    bg.inputs["Color"].default_value = (0.35, 0.35, 0.35, 1)
    bg.inputs["Strength"].default_value = 1.0
    scene.render.engine = "CYCLES"
    scene.cycles.samples = 24
    scene.cycles.use_denoising = False
    scene.cycles.device = "CPU"
    scene.render.resolution_x = scene.render.resolution_y = res
    scene.render.resolution_percentage = 100
    scene.render.film_transparent = False
    scene.view_settings.view_transform = "Standard"
    scene.render.image_settings.file_format = "PNG"
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    return {"matrix": [list(r) for r in cam.matrix_world], "ortho": size, "res": res}


def render_mode(base, work):
    info = prepare(base)
    folder, _ = base_paths(base)
    cfg = CONFIG["bases"][base]
    centre, frame, size = face_frame(info)
    target_mat = flat_material("TargetSkin", os.path.join(folder, "skin.png"))
    eye_mat = flat_material("EyeDark", colour=(0.05, 0.04, 0.04))
    brow_mat = flat_material("Brow", colour=(0.08, 0.06, 0.05))
    skin_copies = []
    for o in info["skins"]:
        o.data.materials.clear()
        o.data.materials.append(target_mat)
        skin_copies.append(o)
    for o in info["eyes"]:
        o.data.materials.clear()
        o.data.materials.append(eye_mat)
    for o in info["brows"]:
        o.data.materials.clear()
        o.data.materials.append(brow_mat)
    cams = {"target": render_face(skin_copies + info["eyes"] + info["brows"], centre, frame, size, os.path.join(work, f"{base}_target_face.png"))}
    face = info["parts"]["face"]
    face_mat = flat_material("SourceFace", texture_path(cfg["head"]["color"]))
    src_obj, _ = make_object(face, "SourceFace", [face_mat])
    eye_objs = []
    for k in CONFIG.get("eyeSlots", ["eyeLeft", "eyeRight"]):
        pts = face.slot_points(k)
        if len(pts):
            wanted = [i for i, s in enumerate(face.slots) if k.lower() in s.lower()]
            sub = Part.__new__(Part)
            sub.name, sub.points, sub.slots = k, face.points, face.slots
            mask = np.isin(face.all_slots, wanted)
            sub.tris, sub.uvs, sub.tslots = face.all_tris[mask], face.all_uvs[mask], face.all_slots[mask]
            eo, _ = make_object(sub, "SourceEye_" + k, [eye_mat])
            eye_objs.append(eo)
    cams["source"] = render_face([src_obj] + eye_objs, centre, frame, size, os.path.join(work, f"{base}_source_face.png"))
    json.dump(cams, open(os.path.join(work, f"{base}_cameras.json"), "w"), indent=1)
    log("rendered both faces for", base)


# ---- Landmarks onto the meshes -------------------------------------------------------------------------------------

def bvh_of(obj):
    pts = mesh_world_points(obj)
    return BVHTree.FromPolygons([Vector(p) for p in pts], [tuple(p.vertices) for p in obj.data.polygons])


def landmarks_3d(cam, pts2d, bvh):
    m = np.array(cam["matrix"])
    x, y, z, o = m[:3, 0], m[:3, 1], m[:3, 2], m[:3, 3]
    out = []
    for px, py in pts2d:
        u = (px + 0.5) / cam["res"] - 0.5
        v = 0.5 - (py + 0.5) / cam["res"]
        origin = o + x * u * cam["ortho"] + y * v * cam["ortho"]
        hit, normal, _, _ = bvh.ray_cast(Vector(origin), Vector(-z))
        out.append(np.array(hit) if hit is not None else None)
    return out


def umeyama(src, dst):
    """Similarity (scale, rotation, translation) taking the source points onto the target ones in least squares."""
    mu_s, mu_d = src.mean(0), dst.mean(0)
    a, b = src - mu_s, dst - mu_d
    cov = b.T @ a / len(src)
    u, d, vt = np.linalg.svd(cov)
    s = np.eye(3)
    if np.linalg.det(u) * np.linalg.det(vt) < 0:
        s[2, 2] = -1
    rot = u @ s @ vt
    scale = np.trace(np.diag(d) @ s) / max((a ** 2).sum() / len(src), 1e-12)
    return scale, rot, mu_d - scale * rot @ mu_s


def tps_fit(src, dst, smooth):
    """A 3D thin-plate spline (kernel r) through the landmark moves, with some smoothing."""
    n = len(src)
    k = np.linalg.norm(src[:, None] - src[None], axis=2)
    p = np.hstack([np.ones((n, 1)), src])
    a = np.zeros((n + 4, n + 4))
    a[:n, :n] = k + smooth * np.eye(n)
    a[:n, n:] = p
    a[n:, :n] = p.T
    rhs = np.zeros((n + 4, 3))
    rhs[:n] = dst - src
    sol = np.linalg.solve(a, rhs)
    return src, sol


def tps_eval(model, pts):
    src, sol = model
    n = len(src)
    out = np.zeros_like(pts)
    for start in range(0, len(pts), 20000):
        q = pts[start:start + 20000]
        k = np.linalg.norm(q[:, None] - src[None], axis=2)
        out[start:start + 20000] = k @ sol[:n] + np.hstack([np.ones((len(q), 1)), q]) @ sol[n:]
    return out


def fit_face(info, work, base):
    """Moves the MetaHuman points by the face landmarks: a similarity, then a smooth spline through the remaining
    misses, both fading out below the chin so the neck and body keep their skeleton fit."""
    path = os.path.join(work, f"{base}_landmarks.json")
    cams = json.load(open(os.path.join(work, f"{base}_cameras.json"), encoding="utf-8"))
    if not os.path.exists(path):
        log("no landmarks; the face keeps its skeleton fit")
        return
    found = json.load(open(path, encoding="utf-8"))
    if not found.get("target") or not found.get("source"):
        log("landmarks missing on one face; the face keeps its skeleton fit")
        return
    target_obj = info["target_obj"]
    face = info["parts"]["face"]
    tmp_obj, _ = make_object(face, "FaceTmp")
    t3 = landmarks_3d(cams["target"], [found["target"][i] for i in INNER_LANDMARKS], bvh_of(target_obj))
    s3 = landmarks_3d(cams["source"], [found["source"][i] for i in INNER_LANDMARKS], bvh_of(tmp_obj))
    bpy.data.objects.remove(tmp_obj, do_unlink=True)
    pairs = [(s, t) for s, t in zip(s3, t3) if s is not None and t is not None]
    if len(pairs) < 20:
        log(f"only {len(pairs)} landmarks hit both faces; the face keeps its skeleton fit")
        return
    src = np.array([p[0] for p in pairs])
    dst = np.array([p[1] for p in pairs])
    unit = info["height"] / 1.75
    # A similarity fitted again without the pairs it misses by far (a landmark the finder put on the wrong spot, or a ray
    # that slipped past an edge), three rounds.
    keep = np.ones(len(src), dtype=bool)
    for _ in range(3):
        scale, rot, trans = umeyama(src[keep], dst[keep])
        miss = np.linalg.norm(scale * src @ rot.T + trans - dst, axis=1)
        keep = miss <= max(3 * np.median(miss[keep]), 0.006 * unit)
    if not 0.85 < scale < 1.15 or keep.sum() < 15:
        log(f"face landmarks don't agree (scale {scale:.3f}, {keep.sum()} kept); the face keeps its skeleton fit")
        return
    before = np.linalg.norm(src - dst, axis=1)[keep].mean()
    src, dst = src[keep], dst[keep]
    moved = scale * src @ rot.T + trans
    log(f"face landmarks: {len(pairs)} pairs, {len(src)} kept, before {before * 100:.2f} cm, after similarity "
        f"{np.linalg.norm(moved - dst, axis=1).mean() * 100:.2f} cm (scale {scale:.3f})")
    model = tps_fit(moved, dst, smooth=0.0005 * unit)
    _, _, f = skeleton_frame(info["joints"])
    up = f[:, 2]
    chin = min(np.dot(p, up) for p in dst) - 0.03 * info["height"] / 1.75
    fade = 0.08 * info["height"] / 1.75
    for part in info["parts"].values():
        h = part.points @ up
        w = np.clip((h - (chin - fade)) / fade, 0, 1)
        w = w * w * (3 - 2 * w)
        sim = scale * part.points @ rot.T + trans
        warped = sim + tps_eval(model, sim)
        part.points = part.points + (warped - part.points) * w[:, None]
    check = tps_eval(model, moved) + moved
    log(f"face landmarks after the spline: {np.linalg.norm(check - dst, axis=1).mean() * 100:.2f} cm")
    json.dump({"pairs": len(pairs), "source": src.tolist(), "target": dst.tolist()}, open(os.path.join(work, f"{base}_landmarks3d.json"), "w"))


def neighbours(tris, n):
    e = np.concatenate([tris[:, [0, 1]], tris[:, [1, 2]], tris[:, [2, 0]]])
    e = np.concatenate([e, e[:, ::-1]])
    return e[:, 0], e[:, 1], np.bincount(e[:, 0], minlength=n)


def settle(info):
    """Settles every MetaHuman point onto the base's skin: the nearest skin point (facing the same way, within reach),
    the moves smoothed over the MetaHuman surface so fingers and folds don't tear."""
    bvh = bvh_of(info["target_obj"])
    reach = 0.07 * info["height"] / 1.75
    for part in info["parts"].values():
        pts = part.points
        n = len(pts)
        normals = vertex_normals(pts, part.tris)
        move = np.zeros_like(pts)
        good = np.zeros(n)
        for i in np.unique(part.tris):
            loc, nrm, _, dist = bvh.find_nearest(Vector(pts[i]), reach)
            if loc is None:
                continue
            if np.dot(np.array(nrm), normals[i]) < 0.2:
                continue
            move[i] = np.array(loc) - pts[i]
            good[i] = 1
        a, b, count = neighbours(part.tris, n)
        count = np.maximum(count, 1)
        smoothed = move.copy()
        weight = good.copy()
        for _ in range(30):
            acc = np.zeros_like(smoothed)
            np.add.at(acc, a, smoothed[b] * weight[b][:, None])
            wacc = np.zeros(n)
            np.add.at(wacc, a, weight[b])
            avg = np.where(wacc[:, None] > 0, acc / np.maximum(wacc, 1e-9)[:, None], 0)
            smoothed = np.where(good[:, None] > 0, 0.6 * move + 0.4 * avg, avg)
            weight = np.maximum(good, (wacc > 0).astype(float) * 0.5)
        part.points = pts + smoothed
        used = np.unique(part.tris)
        log(f"settled {part.name}: {int(good[used].sum())} of {len(used)} points found the skin, "
            f"median move {np.median(np.linalg.norm(smoothed[used], axis=1)) * 100:.2f} cm")


def vertex_normals(pts, tris):
    fn = np.cross(pts[tris[:, 1]] - pts[tris[:, 0]], pts[tris[:, 2]] - pts[tris[:, 0]])
    vn = np.zeros_like(pts)
    for k in range(3):
        np.add.at(vn, tris[:, k], fn)
    return vn / np.maximum(np.linalg.norm(vn, axis=1, keepdims=True), 1e-12)


# ---- Baking ------------------------------------------------------------------------------------------------------

def bake_material(name, image_path, kind):
    """A source material that shows one map: emission of the image (colour, roughness, cavity, white for coverage), or
    for the normal pass a surface with the image as a tangent-space normal map (Unreal's green flipped to Blender's)."""
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nt = mat.node_tree
    for node in list(nt.nodes):
        nt.nodes.remove(node)
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    uv = nt.nodes.new("ShaderNodeUVMap")
    uv.uv_map = "UVMap"
    if kind in ("coverage", "black") or not image_path:
        em = nt.nodes.new("ShaderNodeEmission")
        em.inputs["Color"].default_value = (1, 1, 1, 1) if kind == "coverage" else (0, 0, 0, 1) if kind == "black" else (0.5, 0.5, 0.5, 1)
        nt.links.new(em.outputs["Emission"], out.inputs["Surface"])
        return mat
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = bpy.data.images.load(image_path, check_existing=True)
    tex.image.colorspace_settings.name = "sRGB" if kind == "color" else "Non-Color"
    nt.links.new(uv.outputs["UV"], tex.inputs["Vector"])
    if kind == "normal":
        sep = nt.nodes.new("ShaderNodeSeparateColor")
        inv = nt.nodes.new("ShaderNodeMath")
        inv.operation = "SUBTRACT"
        inv.inputs[0].default_value = 1.0
        comb = nt.nodes.new("ShaderNodeCombineColor")
        nmap = nt.nodes.new("ShaderNodeNormalMap")
        nmap.space = "TANGENT"
        nmap.uv_map = "UVMap"
        bsdf = nt.nodes.new("ShaderNodeBsdfDiffuse")
        nt.links.new(tex.outputs["Color"], sep.inputs["Color"])
        nt.links.new(sep.outputs["Red"], comb.inputs["Red"])
        nt.links.new(sep.outputs["Green"], inv.inputs[1])
        nt.links.new(inv.outputs["Value"], comb.inputs["Green"])
        nt.links.new(sep.outputs["Blue"], comb.inputs["Blue"])
        nt.links.new(comb.outputs["Color"], nmap.inputs["Color"])
        nt.links.new(nmap.outputs["Normal"], bsdf.inputs["Normal"])
        nt.links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])
    else:
        em = nt.nodes.new("ShaderNodeEmission")
        nt.links.new(tex.outputs["Color"], em.inputs["Color"])
        nt.links.new(em.outputs["Emission"], out.inputs["Surface"])
    return mat


def bake_pass(target, source, mats, kind, size, info, colour_space):
    """Bakes one map from the MetaHuman source (its body and head materials set to show it) onto the target's UVs."""
    for i, m in enumerate(mats):
        source.data.materials[i] = m
    img = bpy.data.images.new(f"bake_{kind}", size, size, alpha=False, float_buffer=(kind != "color"))
    img.colorspace_settings.name = colour_space
    img.generated_color = (0, 0, 0, 1)
    for slot in target.material_slots:
        mat = slot.material
        if mat is None:
            mat = bpy.data.materials.new("TargetBake")
            slot.material = mat
        mat.use_nodes = True
        node = mat.node_tree.nodes.get("BakeImage") or mat.node_tree.nodes.new("ShaderNodeTexImage")
        node.name = "BakeImage"
        node.image = img
        mat.node_tree.nodes.active = node
    if not target.material_slots:
        mat = bpy.data.materials.new("TargetBake")
        mat.use_nodes = True
        node = mat.node_tree.nodes.new("ShaderNodeTexImage")
        node.name = "BakeImage"
        node.image = img
        mat.node_tree.nodes.active = node
        target.data.materials.append(mat)
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.cycles.samples = 1
    scene.render.bake.use_selected_to_active = True
    scene.render.bake.use_cage = False
    unit = info["height"] / 1.75
    scene.render.bake.cage_extrusion = CONFIG.get("rayCm", 2.5) * 0.01 * unit
    scene.render.bake.max_ray_distance = 2 * CONFIG.get("rayCm", 2.5) * 0.01 * unit
    scene.render.bake.margin = 16
    scene.render.bake.margin_type = "EXTEND"
    scene.render.bake.target = "IMAGE_TEXTURES"
    bpy.ops.object.select_all(action="DESELECT")
    source.select_set(True)
    target.select_set(True)
    bpy.context.view_layer.objects.active = target
    started = time.time()
    if kind == "normal":
        scene.render.bake.normal_space = "TANGENT"
        bpy.ops.object.bake(type="NORMAL")
    else:
        bpy.ops.object.bake(type="EMIT")
    px = np.empty(size * size * 4, dtype=np.float32)
    img.pixels.foreach_get(px)
    log(f"baked {kind} in {time.time() - started:.0f} s")
    return px.reshape(size, size, 4)[..., :3].copy()


def load_png(path, size, colour=True):
    if not path or not os.path.exists(path):
        return None
    img = bpy.data.images.load(path, check_existing=False)
    img.colorspace_settings.name = "sRGB" if colour else "Non-Color"
    if img.size[0] != size:
        img.scale(size, size)
    px = np.empty(size * size * 4, dtype=np.float32)
    img.pixels.foreach_get(px)
    return px.reshape(size, size, 4)[..., :3].copy()


def save_png(pixels, path, colour=True):
    size = pixels.shape[0]
    img = bpy.data.images.new(os.path.basename(path), size, size, alpha=False, float_buffer=False)
    img.colorspace_settings.name = "sRGB" if colour else "Non-Color"
    rgba = np.concatenate([np.clip(pixels, 0, 1), np.ones((size, size, 1), dtype=np.float32)], axis=2)
    img.pixels.foreach_set(rgba.reshape(-1).astype(np.float32))
    img.filepath_raw = path
    img.file_format = "PNG"
    img.save()
    log("wrote", path)


def blur(img, radius):
    """A cheap box blur (three passes ~ Gaussian), wrapping at the edges."""
    out = img.copy()
    r = max(1, int(radius))
    for _ in range(3):
        for axis in (0, 1):
            c = np.cumsum(out, axis=axis)
            c = np.concatenate([np.zeros_like(np.take(c, [0], axis=axis)), c], axis=axis)
            n = out.shape[axis]
            idx_hi = np.clip(np.arange(n) + r + 1, 0, n)
            idx_lo = np.clip(np.arange(n) - r, 0, n)
            out = (np.take(c, idx_hi, axis=axis) - np.take(c, idx_lo, axis=axis)) / (idx_hi - idx_lo).reshape([-1 if a == axis else 1 for a in range(out.ndim)])
    return out


def srgb_to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(c):
    c = np.clip(c, 0, 1)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055)


def bake_mode(base, work):
    info = prepare(base)
    folder, _ = base_paths(base)
    cfg = CONFIG["bases"][base]
    size = int(os.environ.get("SCAN_SKIN_SIZE") or CONFIG.get("size", 4096))
    target = make_target_object(info["skins"], info["skin_names"])
    info["target_obj"] = target
    # Everything else of the base out of the way (eyes, hair, clothes, the rig).
    for o in list(bpy.data.objects):
        if o.type == "MESH" and o is not target:
            bpy.data.objects.remove(o, do_unlink=True)
    fit_face(info, work, base)
    settle(info)

    # One source object: the body's triangles on material 0, the face's on material 1.
    body, face = info["parts"]["body"], info["parts"]["face"]
    merged = Part.__new__(Part)
    merged.name = "Source"
    offset = len(body.points)
    merged.points = np.concatenate([body.points, face.points])
    merged.tris = np.concatenate([body.tris, face.tris + offset])
    merged.uvs = np.concatenate([body.uvs, face.uvs])
    merged.tslots = np.concatenate([np.zeros(len(body.tris), dtype=np.int64), np.ones(len(face.tris), dtype=np.int64)])
    placeholder = [bpy.data.materials.new("SrcBody"), bpy.data.materials.new("SrcHead")]
    source, _ = make_object(merged, "Source", placeholder)
    source.data.polygons.foreach_set("material_index", merged.tslots.astype(np.int32))
    source.data.update()

    maps = {}
    maps["coverage"] = bake_pass(target, source, [bake_material("CovB", None, "coverage"), bake_material("CovH", None, "coverage")],
                                 "coverage", size, info, "Non-Color")[..., 0]
    log(f"coverage: {maps['coverage'].mean() * 100:.1f}% of the texture")
    # Which MetaHuman part each texel came from (white: the face), to match the face's tone to the body's at the neck.
    maps["part"] = bake_pass(target, source, [bake_material("PartB", None, "black"), bake_material("PartH", None, "coverage")],
                             "part", size, info, "Non-Color")[..., 0]
    for kind, space in (("color", "sRGB"), ("normal", "Non-Color"), ("rough", "Non-Color"), ("cavity", "Non-Color")):
        b, h = texture_path(cfg["body"].get(kind)), texture_path(cfg["head"].get(kind))
        missing = [p for p in (b, h) if p and not os.path.exists(p)]
        if missing:
            raise SystemExit(f"missing textures for {kind}: {missing} (see game/Saved/ScanSkin/textures.json)")
        if not b and not h:
            continue
        maps[kind] = bake_pass(target, source, [bake_material(f"{kind}B", b, kind), bake_material(f"{kind}H", h, kind)], kind, size, info, space)

    # Put together with the base's own 4K maps where the scan didn't reach (coverage feathered over a few texels).
    cover = np.clip(blur(maps["coverage"][..., None], 2)[..., 0] * 1.2 - 0.1, 0, 1)[..., None]
    own_colour = load_png(os.path.join(folder, "skin_4k.png"), size, True)
    own_normal = load_png(os.path.join(folder, "skin_normal_4k.png"), size, False)
    own_rough = load_png(os.path.join(folder, "skin_rough_4k.png"), size, False)
    covered = cover[..., 0] > 0.9
    if "color" in maps:
        scan = srgb_to_linear(maps["color"])
        # The face's colour map and the body's are two textures: the face is brought to the body's tone where they meet
        # (a band either side of the neck seam), so no ring shows; its own variation stays.
        part = maps["part"]
        soft = blur(part[..., None], 6 * size / 4096)[..., 0]
        band = covered & (soft > 0.05) & (soft < 0.95)
        head_band, body_band = band & (part > 0.5), band & (part < 0.5)
        if head_band.sum() > 50 and body_band.sum() > 50:
            ratio = scan[body_band].mean(0) / np.maximum(scan[head_band].mean(0), 1e-4)
            ratio = np.clip(ratio, 0.6, 1.6)
            scan = np.where((part > 0.5)[..., None], scan * ratio, scan)
            log(f"face tone matched to the body at the neck: x{ratio.round(3).tolist()} ({head_band.sum()} and {body_band.sum()} texels)")
        if own_colour is not None:
            own = srgb_to_linear(own_colour)
            ratio = scan[covered].mean(0) / np.maximum(own[covered].mean(0), 1e-4)
            own = own * ratio
            if CONFIG.get("colour") == "detail":
                detail = scan / np.maximum(blur(scan, 24 * size / 4096), 1e-4)
                scan = own * detail
            scan = scan * cover + own * (1 - cover)
        save_png(linear_to_srgb(scan), os.path.join(folder, "skin_scan.png"), True)
    if "normal" in maps:
        n = maps["normal"].copy()
        n[..., 1] = 1.0 - n[..., 1]          # Blender's green up -> Unreal's green down
        if own_normal is not None:
            n = n * cover + own_normal * (1 - cover)
        save_png(n, os.path.join(folder, "skin_scan_normal.png"), False)
    if "rough" in maps or "cavity" in maps:
        rough = np.full((size, size), 0.5, dtype=np.float32)
        if "rough" in maps:
            r = maps["rough"][..., 0]
            rough = 0.5 + (r - np.median(r[covered])) * 1.0
        if "cavity" in maps:
            cav = maps["cavity"][..., 0]
            rough = rough + (np.median(cav[covered]) - cav) * 0.3   # pores and creases a little rougher
            save_png(np.repeat(cav[..., None], 3, axis=2), os.path.join(folder, "skin_scan_cavity.png"), False)
        rough = rough[..., None].repeat(3, axis=2)
        if own_rough is not None:
            rough = rough * cover + own_rough * (1 - cover)
        save_png(rough, os.path.join(folder, "skin_scan_rough.png"), False)
    save_png(np.repeat(cover, 3, axis=2), os.path.join(work, f"{base}_coverage.png"), False)
    log("done", base)


if __name__ == "__main__":
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if len(argv) < 3 or argv[0] not in ("render", "bake"):
        raise SystemExit("usage: scan_skin_blend.py -- render|bake <base> <work folder>")
    mode, base_name, work_dir = argv[:3]
    os.makedirs(work_dir, exist_ok=True)
    (render_mode if mode == "render" else bake_mode)(base_name, work_dir)
