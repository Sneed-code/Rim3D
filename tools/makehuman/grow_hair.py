# Grows strand hair (grooms) on a MakeHuman body, combed with gravity, and writes each style as an Alembic file that
# Unreal imports as a groom (the HairGrooms commandlet). docs/design/hair-physics.md, "Growing our own grooms".
#
#   blender.exe -b -noaudio --python tools/makehuman/grow_hair.py -- <body.fbx> <out folder> [--styles A,B] [--preview]
#                                                                   [--count-scale 0.25] [--seed 7]
#
# <body.fbx> is a character exported by mh_export.py or mh_creator.py (game/Saved/MakeHuman/<Name>/<Name>.fbx): the
# hair is grown on its body mesh in the rest pose, so it fits that head exactly. (A plain .obj also works with
# --joints <json> giving "head" and "neck" points; the cloud tests use MakeHuman's base mesh that way.)
#
# Out: <out>/<Style>.abc (centimetres, relative to the head bone, the body's axes) and <out>/grooms.json (the head and
# neck the hair was grown on, so the game can fit it to other heads). --preview also renders <out>/<Style>_preview.png
# (front, side, back) with Cycles.
#
# How a strand is grown: roots are scattered over the scalp (inside a hairline that is high at the forehead, lower at
# the temples, above and behind the ears, low at the nape). Each strand starts out of the skin, combed along the style's
# flow (away from the parting, or towards a ponytail's band), and with every step its direction turns further towards
# the ground; every point is pushed out of the body mesh to its own small distance (so the hair has volume and lies on
# the head, shoulders and back instead of passing through). Then strands are pulled into clumps towards their tips,
# waved, given a little frizz, cut to length (a soft U at the back), and checked against the body once more.
import argparse
import json
import math
import os
import sys
import time

import bpy
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree
from mathutils.kdtree import KDTree

HERE = os.path.dirname(os.path.abspath(__file__))
DOWN = np.array([0.0, 0.0, -1.0])
BACK = np.array([0.0, 1.0, 0.0])  # MakeHuman bodies face -Y in Blender


def log(*a):
    print("[grow_hair]", *a, flush=True)


# ---- the body ---------------------------------------------------------------------------------------------------------

# The arm bones whose joints are read from the rig (their heads: shoulder, elbow, wrist on each side).
ARM_BONES = ("upperarm_l", "lowerarm_l", "hand_l", "upperarm_r", "lowerarm_r", "hand_r")
# At most this many collision shapes: Unreal's hair simulation reads at most 100 shapes from a physics asset.
MAX_COLLISION_SHAPES = 90
# The collision shapes are picked until hair can sink at most about this far (m) into the skin anywhere below the head.
COLLISION_TARGET = 0.006


def load_body(path, joints_path):
    """The body mesh as (vertices in metres, triangles) and its head and neck joints, from an FBX (bones head, neck_01)
    or an OBJ with a joints json."""
    bpy.ops.wm.read_factory_settings(use_empty=True)
    if path.lower().endswith(".fbx"):
        bpy.ops.import_scene.fbx(filepath=path)
    else:
        bpy.ops.wm.obj_import(filepath=path, forward_axis="Y", up_axis="Z")
    meshes = [o for o in bpy.data.objects if o.type == "MESH"]
    # The body: the tallest mesh that is not hair, eyes, teeth or another add-on (the creator FBXs carry the pubic hair,
    # wigs, brows, lashes, eyes, teeth and tongue as their own meshes; the pubic hair has more vertices than the body).
    skip = ("hair", "brow", "lash", "eye", "teeth", "tooth", "tongue", "pubic", "wig", "ponytail", "braid", "bob0", "long0",
            "short0", "afro", "tusk", "horn", "cat")

    def mesh_height(o):
        zs = [(o.matrix_world @ v.co).z for v in o.data.vertices]
        return max(zs) - min(zs) if zs else 0.0

    candidates = [o for o in meshes if not any(k in o.name.lower() for k in skip)] or meshes
    body = max(candidates, key=lambda o: (round(mesh_height(o), 2), len(o.data.vertices)))
    log("meshes", sorted((o.name, len(o.data.vertices)) for o in meshes))
    mesh = body.data
    world = body.matrix_world
    verts = np.array([tuple(world @ v.co) for v in mesh.vertices])
    # The creator's sliders are shape keys (morph targets in Unreal): kept to measure how each changes the head.
    keys = []
    if mesh.shape_keys:
        for kb in mesh.shape_keys.key_blocks[1:]:
            keys.append((kb.name, np.array([tuple(world @ d.co) for d in kb.data])))
    tris = []
    for p in mesh.polygons:
        vs = list(p.vertices)
        for i in range(1, len(vs) - 1):
            tris.append((vs[0], vs[i], vs[i + 1]))
    tris = np.array(tris, dtype=np.int64)
    # Each vertex's main bone (its largest skin weight), so the collision shapes go on the bone that carries that skin.
    group_names = {g.index: g.name for g in body.vertex_groups}
    bone_of = [max(v.groups, key=lambda e: e.weight) if len(v.groups) else None for v in mesh.vertices]
    bone_of = [group_names.get(e.group, "") if e is not None else "" for e in bone_of]
    if not any(bone_of):
        bone_of = None
    head = neck = None
    arms = {}
    rig = next((o for o in bpy.data.objects if o.type == "ARMATURE"), None)
    if rig:
        bones = rig.data.bones
        if "head" in bones and "neck_01" in bones:
            head = np.array(tuple(rig.matrix_world @ bones["head"].head_local))
            neck = np.array(tuple(rig.matrix_world @ bones["neck_01"].head_local))
        # The arms' joints (shoulder, elbow, wrist), so the hair collides with the arms too, on the arm bones.
        for name in ARM_BONES:
            if name in bones:
                arms[name] = np.array(tuple(rig.matrix_world @ bones[name].head_local))
    if head is None and joints_path:
        joints = json.load(open(joints_path))
        head = np.array(joints["head"])
        neck = np.array(joints["neck"])
    if head is None:
        raise SystemExit("no head and neck_01 bones (or --joints) for " + path)
    # Metres: the creator FBX is in centimetres.
    height = verts[:, 2].max() - verts[:, 2].min()
    scale = 0.01 if height > 10.0 else 1.0
    log("body", body.name, len(verts), "vertices,", len(tris), "triangles, height", round(height * scale, 3), "m,", len(keys), "shape keys")
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o)
    return (verts * scale, tris, head * scale, neck * scale, [(n, k * scale) for n, k in keys], {n: j * scale for n, j in arms.items()},
            bone_of)


def fit_sphere(points):
    A = np.c_[2 * points, np.ones(len(points))]
    b = (points ** 2).sum(1)
    sol = np.linalg.lstsq(A, b, rcond=None)[0]
    return sol[:3], math.sqrt(sol[3] + (sol[:3] ** 2).sum())


def head_morphs(body, keys):
    """How each shape key (a creator slider at full weight) changes the head the hair sits on: the skull sphere's
    growth as a share and its centre's move, in Unreal's axes and centimetres (Blender x, -y, z), for the game to fit
    the groom to each person's sliders. Only keys that change the skull by more than a hair's breadth are kept."""
    out = {}
    for name, kv in keys:
        c, r = fit_sphere(kv[body.crown_ids])
        grow = r / body.skull_r - 1.0 if body.skull_r > 1e-6 else 0.0
        move = (c - body.skull) * 100.0
        if abs(grow) > 0.002 or np.abs(move).max() > 0.05:
            out[name] = [round(float(grow), 5), round(float(move[0]), 3), round(float(-move[1]), 3), round(float(move[2]), 3)]
    return out


class Body:
    def __init__(self, verts, tris, head, neck, arms=None, bone_of=None):
        self.verts, self.tris, self.head, self.neck = verts, tris, head, neck
        self.arms = arms or {}
        self.bone_of = bone_of if bone_of and len(bone_of) == len(verts) else None
        self.bvh = BVHTree.FromPolygons([tuple(v) for v in verts], [tuple(t) for t in tris])
        self.top = verts[:, 2].max()
        self.bottom = verts[:, 2].min()
        self.height = self.top - self.bottom
        # The skull: a sphere fitted to the crown (the head above the ears).
        cut = head[2] + 0.55 * (self.top - head[2])
        self.crown_ids = np.nonzero((verts[:, 2] > cut) & (np.abs(verts[:, 0] - head[0]) < 0.12))[0]
        self.skull, self.skull_r = fit_sphere(verts[self.crown_ids])
        # Heights the long styles reach, as shares of the body's height above the soles (an adult's proportions).
        h = self.height
        self.heights = {
            "waist": self.bottom + 0.60 * h,
            "mid_back": self.bottom + 0.70 * h,
            "shoulder": self.bottom + 0.815 * h,
            "chin": self.bottom + 0.855 * h,
            "ear": self.bottom + 0.90 * h,
        }
        self.shoulder_hair = 0.11 * self.height / 1.67  # long hair keeps within this of the middle below the neck
        log("skull centre", np.round(self.skull, 3), "radius", round(self.skull_r, 3), "head bone", np.round(head, 3))

    def push_out(self, p, clearance, reach):
        """p pushed out of the body to at least `clearance` from its surface (looked for within `reach`)."""
        loc, nrm, _, dist = self.bvh.find_nearest(Vector(p), reach)
        if loc is None:
            return p
        loc = np.array(loc)
        nrm = np.array(nrm)
        d = np.dot(p - loc, nrm)
        if d < clearance:
            return loc + nrm * clearance
        return p

    def press(self, p, low, high, reach):
        """p kept between `low` and `high` off the body's surface: hair pressed flat under a hat."""
        loc, nrm, _, dist = self.bvh.find_nearest(Vector(p), reach)
        if loc is None:
            return p
        loc = np.array(loc)
        nrm = np.array(nrm)
        d = np.dot(p - loc, nrm)
        if d < low or d > high:
            return loc + nrm * min(max(d, low), high)
        return p

    def keep(self, p, clearance, reach, hat_z, gap):
        """Out of the body to the strand's clearance; under a hat (above hat_z) pressed within `gap` of the scalp."""
        if hat_z is not None and p[2] > hat_z:
            return self.press(p, 0.0012, gap, max(reach, gap + 0.03))
        return self.push_out(p, clearance, reach)


def collision_part(bone):
    """The collision part for skin carried by `bone` (its largest skin weight), or None for skin the hair never reaches
    below the head (hands, legs) or the head itself (the scalp keeps its own spheres)."""
    for side in ("_l", "_r"):
        if bone.endswith(side):
            if bone.startswith(("upperarm", "arm_soft")):
                return "upperarm" + side
            if bone.startswith("lowerarm"):
                return "lowerarm" + side
            if bone.startswith("clavicle"):
                return "clavicle" + side
            if bone.startswith("breast"):
                return "torso"
            return None
    if bone.startswith("neck"):
        return "neck"
    if bone.startswith(("spine", "pelvis", "belly")):
        return "torso"
    return None


def collision_shapes(body, budget=MAX_COLLISION_SHAPES, target=COLLISION_TARGET, seed=3):
    """Shapes for the groom's physics to collide with, all inside the skin, so hair grown lying on the skin starts clear
    of them and only meets them when it swings or the body moves into it. The body's own physics asset wraps each bone
    in a capsule standing centimetres proud of the scalp and back, so the hair started inside it and was thrown out into
    a puffball (PC, 2026-11-21).

    The head keeps 12 spheres filling the cranium. Below it (neck, shoulders, chest, back, both arms, down to the waist)
    the shapes are picked to leave no gaps (sneed 2026-11-22: hair still through the body): the old spheres in a few
    columns let a strand sink 2 cm (up to 5 cm) into the back before anything stopped it. Candidates are spheres and
    capsules a little under the skin (1.5 to 5 cm in, along the skin's normal), each as big as stays `margin` inside the
    surface; then the one that most cuts how deep hair could sink into the skin (measured at the skin's vertices, the
    back and shoulder tops counting double) is taken, again and again, up to `budget` shapes in all. Each shape belongs
    to one bone's skin (its vertices' largest skin weight: torso, neck, a clavicle, an upper arm or a forearm), so it
    moves with that bone, and only counts for that skin.
    Returns [(kind, part, a, b, radius, out)] in metres: kind "sphere" (a == b) or "capsule" (a to b); out is the outward
    normal of the skin it was placed under (zero for the head's), which the game uses to grow it out to worn clothes."""
    margin = 0.004
    rng = np.random.default_rng(seed)
    bvh = body.bvh
    shapes = []

    def inside(p):
        """Distance from p to the skin if p is inside the body, else -1."""
        loc, nrm, _, dist = bvh.find_nearest(Vector(p))
        if loc is None or np.dot(np.asarray(p) - np.array(loc), np.array(nrm)) >= 0.0:
            return -1.0
        return dist

    # The head: the skull centre and points towards the crown, back and sides (MakeHuman faces -y).
    for d in [(0, 0, 0), (0, 0, 1), (0, 1, 0), (0, 0.7, 0.7), (0, 0.8, -0.5), (0, -0.5, 0.7), (1, 0, 0.2), (-1, 0, 0.2),
              (0.6, 0.6, 0.4), (-0.6, 0.6, 0.4), (0.6, 0.6, -0.4), (-0.6, 0.6, -0.4)]:
        d = np.array(d, dtype=float)
        n = np.linalg.norm(d)
        c = body.skull + (d / n * 0.45 * body.skull_r if n > 0 else d)
        r = inside(c) - margin
        if r > 0.012:
            shapes.append(("sphere", "head", c, c, float(r), np.zeros(3)))

    # The skin below the head that hair can reach, each vertex with its part and normal.
    v = body.verts
    normals = np.zeros_like(v)
    t = body.tris
    face = np.cross(v[t[:, 1]] - v[t[:, 0]], v[t[:, 2]] - v[t[:, 0]])
    for k in range(3):
        np.add.at(normals, t[:, k], face)
    normals /= np.linalg.norm(normals, axis=1, keepdims=True) + 1e-12
    if body.bone_of is not None:
        parts = [collision_part(b) for b in body.bone_of]
    else:
        # No skin weights (the cloud's OBJ): by the nearest joint line.
        lines = [("neck", body.neck, body.head)]
        for side in ("_l", "_r"):
            j = [body.arms.get(n + side) for n in ("upperarm", "lowerarm", "hand")]
            if all(x is not None for x in j):
                lines += [("upperarm" + side, j[0], j[1]), ("lowerarm" + side, j[1], j[2])]

        def nearest_line(p):
            best, part = 0.12, "torso"
            for name, a, b in lines:
                ab = b - a
                tt = np.clip(np.dot(p - a, ab) / max(np.dot(ab, ab), 1e-12), 0.0, 1.0)
                dist = np.linalg.norm(p - (a + tt * ab))
                if dist < best:
                    best, part = dist, name
            return part
        parts = [nearest_line(p) for p in v]
    low = body.heights["waist"] - 0.03
    high = body.skull[2] - 0.5 * body.skull_r  # the neck up to under the skull
    skin = [i for i, part in enumerate(parts) if part and low <= v[i, 2] <= high]
    if not skin:
        return shapes
    names = sorted({parts[i] for i in skin})
    skin = np.array(skin)
    skin_part = np.array([names.index(parts[i]) for i in skin])
    weight = np.where((normals[skin, 1] > 0.3) | (normals[skin, 2] > 0.4), 2.0, 1.0)  # the back and shoulder tops

    # Candidates: points 1.5 to 5 cm under the skin, as spheres and as capsules joining two of them 3 to 16 cm apart.
    # Left and right match: candidates are made on the left half (and the middle) and each is taken with its mirror image.
    mid_x = body.head[0]
    stride = max(1, len(skin) // 900)
    seeds = [i for i in skin[::max(1, stride // 2)] if v[i, 0] <= mid_x + 0.01]
    cands = []  # (part index, a, b, r, the skin's outward normal where it was seeded)
    for depth in (0.015, 0.025, 0.035, 0.05):
        pts, owner, outs = [], [], []
        for i in seeds:
            q = v[i] - normals[i] * depth
            if inside(q) >= 0.6 * depth:
                pts.append(q)
                owner.append(names.index(parts[i]))
                outs.append(normals[i])
        if not pts:
            continue
        tree = KDTree(len(pts))
        for k, q in enumerate(pts):
            tree.insert(Vector(q), k)
        tree.balance()
        for k, q in enumerate(pts):
            cands.append((owner[k], q, q, inside(q) - margin, outs[k]))
            near = [j for (_, j, dist) in tree.find_range(Vector(q), 0.16) if j > k and dist >= 0.03 and owner[j] == owner[k]]
            for j in (rng.choice(near, size=min(6, len(near)), replace=False) if near else []):
                a, b = q, pts[j]
                r = min(inside(a + s * (b - a)) for s in np.linspace(0.0, 1.0, 7))
                if r > 0.0:
                    out = outs[k] + outs[j]
                    cands.append((owner[k], a, b, r - margin, out / max(np.linalg.norm(out), 1e-9)))
    cands = [c for c in cands if c[3] > 0.008]

    # How deep (m) hair could sink into each skin vertex before meeting each candidate: its distance to the shape, less
    # the shape's radius, less how far the shape is under that skin. Only the candidate's own part counts.
    sample = skin[:: max(1, len(skin) // 2500)]
    sample_part = np.array([names.index(parts[i]) for i in sample])
    sample_w = np.where((normals[sample, 1] > 0.3) | (normals[sample, 2] > 0.4), 2.0, 1.0)
    P = v[sample]
    cap = 0.05
    D = np.full((len(cands), len(sample)), cap, dtype=np.float32)
    def mirror(p):
        return np.array([2.0 * mid_x - p[0], p[1], p[2]])

    def mirror_part(name):
        return name[:-2] + ("_r" if name.endswith("_l") else "_l") if name.endswith(("_l", "_r")) else name

    def pair(cand):
        """The candidate and its mirror image (one shape if it is its own mirror)."""
        owner, a, b, r, out = cand
        twin = (names.index(mirror_part(names[owner])) if mirror_part(names[owner]) in names else owner, mirror(a), mirror(b), r,
                np.array([-out[0], out[1], out[2]]))
        same = np.allclose(twin[1], a, atol=0.004) and np.allclose(twin[2], b, atol=0.004) or (
            np.allclose(twin[1], b, atol=0.004) and np.allclose(twin[2], a, atol=0.004))
        return [cand] if same else [cand, twin]

    for k, cand in enumerate(cands):
        for owner, a, b, r, _ in pair(cand):
            mine = sample_part == owner
            ab = b - a
            tt = np.clip(((P[mine] - a) @ ab) / max(float(ab @ ab), 1e-12), 0.0, 1.0)
            D[k, mine] = np.minimum(D[k, mine], np.linalg.norm(P[mine] - (a + tt[:, None] * ab), axis=1) - r)
    current = np.full(len(sample), cap, dtype=np.float32)

    def cost(d):
        return (np.clip(d - target, 0.0, None) * sample_w).sum(axis=-1)

    base = cost(current)
    count = np.array([len(pair(c)) for c in cands])
    while len(cands):
        totals = cost(np.minimum(D, current))
        gain = (base - totals) / count  # per shape spent
        gain[count > budget - len(shapes)] = -1.0
        best = int(np.argmax(gain))
        if gain[best] < 5e-5:
            break
        current = np.minimum(current, D[best])
        base = totals[best]
        for owner, a, b, r, out in pair(cands[best]):
            shapes.append(("sphere" if np.allclose(a, b) else "capsule", names[owner], np.array(a), np.array(b), float(r), np.array(out)))
    deep = current - 0.0
    log("collision: %d shapes; hair can sink into the skin (cm) median %.2f, 90%% %.2f, worst %.2f" % (
        len(shapes), np.median(deep) * 100, np.percentile(deep, 90) * 100, deep.max() * 100))
    return shapes


def collision_entry(shape, head):
    """grooms.json's form of a collision shape: centimetres from the head bone in Unreal's axes (Blender x, -y, z)."""
    kind, part, a, b, r, out = shape
    cm = lambda p: [round(float(x), 2) for x in ((p - head) * 100.0 * np.array([1, -1, 1]))]
    entry = {"part": part, "centre_cm": cm((a + b) / 2), "radius_cm": round(r * 100.0, 2)}
    if np.linalg.norm(out) > 0.5:
        entry["out"] = [round(float(x), 3) for x in out * np.array([1, -1, 1])]
    if kind == "capsule":
        entry["a_cm"], entry["b_cm"] = cm(a), cm(b)
    return entry


# ---- the scalp ----------------------------------------------------------------------------------------------------------

# Hairline: (degrees round the head from the front, lowest elevation above the skull centre's height that grows hair).
HAIRLINE = [(0, 22), (30, 20), (55, 15), (75, 4), (90, 6), (105, -4), (125, -22), (150, -36), (180, -42)]


def hairline_elevation(az_deg):
    a = np.abs(az_deg)
    xs, ys = zip(*HAIRLINE)
    return np.interp(a, xs, ys)


def scalp_frame(body, p):
    d = p - body.skull
    r = np.linalg.norm(d, axis=-1, keepdims=True)
    d = d / np.maximum(r, 1e-9)
    az = np.degrees(np.arctan2(d[..., 0], -d[..., 1]))
    el = np.degrees(np.arcsin(np.clip(d[..., 2], -1, 1)))
    return az, el, r[..., 0]


def scatter_roots(body, count, rng):
    """Roots spread evenly over the scalp: points on the triangles inside the hairline, with their normals."""
    v = body.verts
    t = body.tris
    a, b, c = v[t[:, 0]], v[t[:, 1]], v[t[:, 2]]
    centre = (a + b + c) / 3.0
    az, el, r = scalp_frame(body, centre)
    lateral = np.abs(centre[:, 0] - body.skull[0])
    inside = (el > hairline_elevation(az)) & (r < body.skull_r * 1.35) & (centre[:, 2] > body.head[2] - 0.01)
    # Not the ears: anything standing out sideways from the skull low down.
    inside &= ~((lateral > body.skull_r * 0.93) & (el < 22))
    cross = np.cross(b - a, c - a)
    area = np.linalg.norm(cross, axis=1) * 0.5
    area[~inside] = 0.0
    log("scalp", int(inside.sum()), "triangles,", round(area.sum() * 1e4, 1), "cm2")
    pick = rng.choice(len(t), size=count, p=area / area.sum())
    u = rng.random(count)
    w = rng.random(count)
    flip = u + w > 1
    u[flip], w[flip] = 1 - u[flip], 1 - w[flip]
    roots = a[pick] + (b[pick] - a[pick]) * u[:, None] + (c[pick] - a[pick]) * w[:, None]
    normals = cross[pick] / np.linalg.norm(cross[pick], axis=1, keepdims=True)
    outward = np.einsum("ij,ij->i", normals, roots - body.skull) < 0
    normals[outward] *= -1
    return roots, normals


# ---- growing --------------------------------------------------------------------------------------------------------------

def unit(x):
    return x / np.maximum(np.linalg.norm(x, axis=-1, keepdims=True), 1e-9)


def tangent(dirs, normals):
    t = dirs - normals * np.einsum("ij,ij->i", dirs, normals)[:, None]
    return unit(t)


def flow_directions(body, style, roots, rng):
    """Where each strand is combed at its root: away from the parting (and a little back), or straight back."""
    part = style["part"]
    x = roots[:, 0] - body.skull[0]
    if part == "side":
        x = x - body.skull_r * 0.38
    side = np.sign(x)
    side[side == 0] = 1
    if part in ("middle", "side"):
        # A soft parting (sneed, 2026-11-22: "a central hair part that doesn't look good, more hair in the middle line"):
        # strands rooted within `parting_width` of the line are combed less to the side the nearer they are to it, and
        # some cross over onto the other side, so the line is covered and reads as a natural parting, not a bald stripe.
        width = style.get("parting_width", 0.014)
        side = np.clip(x / width + rng.normal(0, style.get("parting_cross", 0.6), len(x)), -1.0, 1.0)
    az, el, _ = scalp_frame(body, roots)
    front = np.clip(1.0 - np.abs(az) / 90.0, 0.0, 1.0)  # 1 at the forehead, 0 from the ears back
    top = np.clip((el - 10.0) / 50.0, 0.0, 1.0)          # 1 on the crown, 0 low on the head
    if part in ("middle", "side"):
        # Away from the parting over the top of the head (strands at the front swing back round the temples); lower
        # down the back and sides of the head, just down.
        lateral = (0.25 + 0.95 * front) * top * np.clip(1.4 - np.abs(az) / 130.0, 0.0, 1.0)
        f = side[:, None] * np.array([1.0, 0.0, 0.0]) * lateral[:, None] + BACK * (0.25 + 0.9 * front[:, None]) \
            + DOWN * (0.45 + 0.5 * (1 - top[:, None]))
    elif part == "radial":
        # Straight out from a little below the skull centre (an afro): up on the crown, out at the sides and back.
        f = roots - radial_origin(body)
    else:
        f = np.tile(BACK, (len(roots), 1)) + DOWN * 0.6
    f += rng.normal(0, 0.06, f.shape)
    messy = style.get("messy", 0.0)
    if messy:
        # Tousled: each patch of hair a few centimetres across combed its own way (a smooth random field over the head,
        # so neighbouring strands agree and the hair goes in locks, not fuzz).
        f = unit(f) + messy * tousle_field(roots, rng, style.get("messy_size", 0.04))
    return unit(f)


def radial_origin(body):
    return body.skull - np.array([0.0, 0.0, 0.1 * body.skull_r])


def tousle_field(p, rng, size, waves=8):
    """A smooth random direction field: a sum of a few plane waves about `size` across, for each axis."""
    out = np.zeros_like(p)
    for axis in range(3):
        for _ in range(waves):
            k = unit(rng.normal(0, 1, 3)) * (2 * math.pi / size) * (0.7 + 0.6 * rng.random())
            out[:, axis] += np.sin(p @ k + rng.random() * 2 * math.pi)
    return out / math.sqrt(waves / 2)


def braid_offsets(t, bundle, braid, length, w_scale):
    """Where a strand sits across a three-strand plait at depth t (metres below the band): (sideways, back) offsets of its
    bundle's centre and the bundle's radius. The bundles weave as x = sin(a), y = sin(2a) / 2, a third of a turn apart,
    so each crosses over the middle in turn; they start together at the band, narrow towards the tie, and below the
    tie the ends come loose in a small tuft."""
    tie = max(length - braid["tuft"], 0.05)
    w = braid["width"] + (braid["width_tip"] - braid["width"]) * np.clip(t / tie, 0, 1)
    w = w * np.clip((t + 0.012) / 0.05, 0, 1) * w_scale                 # out of the band
    into_tie = np.clip((tie - t) / 0.025, 0, 1)                         # bunched at the tie
    a = 2 * math.pi * t / braid["period"] + 2 * math.pi * bundle / 3.0
    side = w * np.sin(a) * into_tie
    back = 0.55 * w * np.sin(2 * a) * into_tie
    radius = np.where(t < tie, np.maximum(0.5 * w, braid["tie_radius"] * (1 - into_tie) + 0.5 * w * into_tie),
                      braid["tie_radius"] + (braid["tuft_radius"] - braid["tie_radius"]) * np.clip((t - tie) / braid["tuft"], 0, 1))
    return side, back, radius


def braid_band(body, gather):
    """A braid's band at the nape: `up` from the skull centre's height, just off the back of the head as it is there."""
    z = body.skull[2] + gather["up"]
    loc, _, _, _ = body.bvh.ray_cast(Vector((body.skull[0], body.skull[1] + 0.5, z)), Vector((0.0, -1.0, 0.0)), 1.0)
    y = loc[1] if loc is not None else body.skull[1] + body.skull_r
    return np.array([body.skull[0], y + gather["band"] + gather["back"], z])


def braid_line(body, start, z_end, clearance, step=0.004):
    """The braid's middle from the band down to z_end: straight down, kept `clearance` off the back (it lies on the
    shoulder blades and hangs free where the back curves in). Returns points every `step` of height."""
    pts = [start]
    p = start.copy()
    while p[2] > z_end - 0.1:
        p = body.push_out(p + DOWN * step, clearance, clearance + 0.05)
        p[2] = pts[-1][2] - step  # height goes down evenly, so depth indexes it
        pts.append(p.copy())
    return np.array(pts), step


def grow(body, style, rng, count_scale):
    count = max(200, int(style["count"] * count_scale))
    roots, normals = scatter_roots(body, count, rng)
    n = len(roots)
    segs = int(style["segments"])
    az, el, _ = scalp_frame(body, roots)
    side_dist = np.abs(roots[:, 0] - body.skull[0]) / 0.18
    hat = style.get("_hat")
    hat_z = body.skull[2] + hat["brim"] if hat else None
    hat_gap = hat["gap"] if hat else 0.0
    radial = style["part"] == "radial"

    # Length: down to a height on the body (a soft U: longer in the middle of the back), or a set length, or (an afro) out
    # to a rounded shell round the head.
    if radial:
        shape = style["shape"]
        shell_c = body.skull + np.array([0.0, shape.get("back", 0.0), shape.get("up", 0.0)])
        shell_r = (body.skull_r + style["puff"]) * np.array(shape.get("scale", [1.0, 1.0, 1.0]))
        shell_cut = 1.0 + rng.normal(0, shape.get("rough", 0.03), n)
        z_end = np.full(n, -1e9)
        max_len = style["puff"] * 1.7 + 0.02 + (body.skull_r * 1.8 if hat else 0.0)  # under a hat: down the scalp first
        lengths = np.full(n, max_len)
        segs = int(math.ceil(max_len / style["seg_len"]))
    elif "to" in style:
        z_end = body.heights[style["to"]] + style.get("u_cut", 0.0) * np.clip(side_dist, 0, 1.5) ** 2
        z_end += rng.normal(0, 0.006, n)
        max_len = (body.top - z_end.min()) * 1.55 + 0.12
        lengths = np.full(n, max_len)
    else:
        z_end = np.full(n, -1e9)
        lengths = np.full(n, style["length"])
        short_sides = style.get("shorter_sides", 0.0)
        lengths *= 1.0 - short_sides * np.clip((40 - el) / 60.0, 0, 1)  # sides and nape cut shorter
        lengths *= 1.0 + rng.normal(0, style.get("length_spread", 0.06), n)
        max_len = lengths.max()
    seg_len = max_len / segs

    # Each strand stands off the body by its own small distance: the layers that give the hair its volume.
    clearance = style["lift"] * 0.25 + (style["lift"] * 0.75 + style["volume"]) * np.sqrt(rng.random(n))
    clearance = np.maximum(clearance, 0.0015)

    flow = flow_directions(body, style, roots, rng)
    gather = style.get("gather")
    braid = gather.get("braid") if gather else None
    if gather:
        if braid:
            # A braid gathers low at the nape; each strand goes to the part of the band on its own side of the head
            # (crown hair to the top, the left side to the left), so the three bundles take neighbouring hair.
            g_centre = braid_band(body, gather)
            rel = roots - g_centre
            ang = np.arctan2(rel[:, 2], rel[:, 0])
            rad = np.sqrt(rng.random(n))
            disc = np.c_[np.cos(ang) * rad, np.sin(ang) * rad]
            bundle = np.floor(((ang + math.pi / 2) % (2 * math.pi)) / (2 * math.pi / 3)).astype(int)  # 0 top, then round
            local = rng.normal(0, 1, (n, 2))
            local = local / np.linalg.norm(local, axis=1, keepdims=True) * np.sqrt(rng.random(n))[:, None]
            braid_len = g_centre[2] - float(np.median(z_end))
            line, line_step = braid_line(body, g_centre, float(z_end.min()), braid["width"] * 1.1 + 0.006)
            w_scale = 1.0 + rng.normal(0, 0.02)
        else:
            g_centre = body.skull + np.array([0.0, body.skull_r + gather["back"], gather["up"]])
            disc = rng.normal(0, 1, (n, 2))
            disc = disc / np.linalg.norm(disc, axis=1, keepdims=True) * np.sqrt(rng.random(n))[:, None]
        band_target = g_centre + np.c_[disc[:, 0] * gather["band"], np.zeros(n), disc[:, 1] * gather["band"]]
        tail_offset = np.c_[disc[:, 0], disc[:, 1] * 0.6, np.zeros(n)] * gather["tail_spread"]
        in_tail = np.zeros(n, dtype=bool)
        tail_depth = np.zeros(n)

    # Which side of the shoulders a long strand falls: in front only from the front of the sides of the head.
    in_front = (np.abs(az) > 35) & (np.abs(az) < 80) & (el < 25) & (rng.random(n) < style.get("front_share", 0.6))
    pts = np.zeros((n, segs + 1, 3))
    pts[:, 0] = roots + normals * 0.0005
    d = unit(normals * style.get("rise", 0.25) + tangent(flow, normals))
    if gather:
        d = unit(normals * 0.3 + tangent(unit(band_target - roots), normals))
    arc = np.zeros(n)
    end_idx = np.full(n, segs)
    alive = np.ones(n, dtype=bool)
    stiff = style["stiffness"] ** (seg_len / 0.01)  # the style's stiffness is per centimetre of strand
    grav = style["gravity"]
    back = style.get("back", 0.0)
    hug_w = style.get("hug", 1.0)  # how flat the hair lies along the scalp (1 flat; less stands up, a messy cut)
    reach = float(clearance.max() + seg_len * 2.5)
    # Under a hat: everything above the brim lies pressed within a few millimetres of the scalp, then hangs below it.
    t0 = time.time()
    for k in range(1, segs + 1):
        p = pts[:, k - 1]
        az_p, el_p, r_p = scalp_frame(body, p)
        n_here = unit(p - body.skull)
        on_head = r_p < body.skull_r * 1.6
        w_flow = np.exp(-arc / 0.11)
        hug = (r_p < body.skull_r * 1.3) & (p[:, 2] > body.neck[2])
        if gather:
            to_band = band_target - p
            reached = np.linalg.norm(to_band, axis=1) < seg_len * 1.2
            if braid:
                # Into the plait at the band even if not at its own spot there, and anything still wandering after
                # 40 cm (a strand caught round an ear) joins it too, so no loose strand hangs beside the braid.
                reached |= (np.linalg.norm(g_centre - p, axis=1) < gather["band"] + seg_len * 1.5) | (arc > 0.4)
            newly = reached & ~in_tail
            in_tail |= reached
            desired = np.where(in_tail[:, None], 0, tangent(unit(to_band), n_here))
            # In the tail: down from the band, spreading out a little, then narrowing towards the ends.
            tail_depth = np.where(in_tail, tail_depth + np.where(newly, 0, seg_len), 0)
            spread = np.minimum(1.0, (tail_depth + seg_len) / 0.06) * (1.0 - 0.35 * np.minimum(1.0, tail_depth / 0.5))
            target = g_centre + DOWN[None, :] * (tail_depth + seg_len)[:, None] + tail_offset * spread[:, None] + BACK * 0.01
            if braid:
                t = tail_depth + seg_len
                side, bk, radius = braid_offsets(t, bundle, braid, braid_len, w_scale)
                mid = line[np.minimum((t / line_step).astype(int), len(line) - 1)]
                # Round bundles: each strand's spot across its bundle is taken square to the bundle's own direction.
                ahead = unit(np.c_[(braid_offsets(t + 0.002, bundle, braid, braid_len, w_scale)[0] - side) / 0.002,
                                   (braid_offsets(t + 0.002, bundle, braid, braid_len, w_scale)[1] - bk) / 0.002, -np.ones(n)])
                e1 = unit(np.cross(ahead, BACK))
                e2 = np.cross(ahead, e1)
                target = mid + np.c_[side, bk, np.zeros(n)] + (e1 * local[:, :1] + e2 * local[:, 1:]) * radius[:, None]
            desired = np.where(in_tail[:, None], unit(target - p), desired)
            step_stiff = np.where(in_tail, 0.35, stiff)
        elif radial:
            # Out from the head, sagging a little; under a hat first combed down along the scalp to the brim.
            out = unit(p - radial_origin(body))
            desired = unit(out + DOWN * style.get("droop", 0.1))
            if hat_z is not None:
                under = p[:, 2] > hat_z
                desired = np.where(under[:, None], tangent(np.broadcast_to(DOWN + BACK * 0.2, n_here.shape), n_here), desired)
                hug = under
            else:
                hug = np.zeros(n, dtype=bool)
            step_stiff = np.full(n, stiff)
        else:
            flow_t = np.where(on_head[:, None], tangent(flow, n_here), flow)
            # Below the head: the ground, a little back (behind the shoulders), and hair behind the body leans onto it.
            behind = (p[:, 1] > body.neck[1] + 0.02) & (p[:, 2] < body.neck[2])
            g = DOWN + BACK * back * (1 - w_flow[:, None]) - BACK * (style.get("cling", 0.35) * behind)[:, None]
            desired = unit(flow_t * (w_flow[:, None] * 1.6) + unit(g) * (1 - w_flow[:, None]) * (0.4 + grav * 3))
            # Past the shoulders: each strand was given a side at its root (most go behind the shoulders, some from in
            # front of the ears fall in front); below the ears it is drawn that way and in towards the middle, so it
            # clears the shoulder instead of landing on it and running down the arm.
            low = np.clip((body.skull[2] - body.skull_r * 0.3 - p[:, 2]) / 0.06, 0, 1)
            lat = p[:, 0] - body.neck[0]
            wide = np.clip((np.abs(lat) - body.shoulder_hair * 0.55) / 0.06, 0, 1)
            clear = np.where(in_front, p[:, 1] > body.neck[1] - 0.09, p[:, 1] < body.neck[1] + 0.07)
            side_pull = np.where(in_front, -1.0, 1.0)[:, None] * BACK * (1.4 * clear)[:, None]
            inward = -np.sign(lat)[:, None] * np.array([1.0, 0.0, 0.0]) * np.clip((np.abs(lat) - body.shoulder_hair) / 0.05, 0, 1)[:, None]
            desired = unit(desired + (side_pull * wide[:, None] + inward) * low[:, None])
            step_stiff = np.full(n, stiff)
        # On the head the strand lies along the scalp (its distance off the skin gives the volume); below it, it bends
        # towards where it is pulled only as stiff as the style says.
        if hug_w >= 1.0 or radial:
            d = np.where(hug[:, None], tangent(d, n_here), d)
        else:
            d = np.where(hug[:, None], unit(d * (1 - hug_w) + tangent(d, n_here) * hug_w), d)
        step_stiff = np.where(hug, np.minimum(step_stiff, 0.55), step_stiff)
        d = unit(step_stiff[:, None] * d + (1 - step_stiff[:, None]) * desired)
        q = p + d * seg_len
        # Keep it out of the face: anything in front of the face is moved out to the side.
        rel = q - body.skull
        face = (rel[:, 1] < -body.skull_r * 0.55) & (np.abs(rel[:, 0]) < body.skull_r * 0.8) & \
               (q[:, 2] > body.heights["chin"] - 0.03) & (q[:, 2] < body.skull[2] + body.skull_r * 0.25) & (gather is None)
        if face.any():
            sgn = np.sign(rel[face, 0])
            sgn[sgn == 0] = 1
            q[face, 0] = body.skull[0] + sgn * body.skull_r * 0.8
        # Out of the body, to the strand's own distance off the skin.
        for i in np.nonzero(alive)[0]:
            q[i] = body.keep(q[i], clearance[i], reach, hat_z, hat_gap)
        d = unit(q - p)
        q = p + d * seg_len
        # Below the neck hair does not spread outwards over the shoulders (the push out of a sloping shoulder would
        # otherwise walk it down the arm): it may only widen slowly; it finds its way round in front or behind.
        lat_p = p[:, 0] - body.neck[0]
        lat_q = q[:, 0] - body.neck[0]
        cap = np.abs(lat_p) + 0.12 * seg_len
        spread = (p[:, 2] < body.neck[2]) & (np.abs(lat_q) > cap)
        q[spread, 0] = body.neck[0] + np.sign(lat_q[spread]) * cap[spread]
        for i in np.nonzero(alive)[0]:
            q[i] = body.keep(q[i], clearance[i], reach, hat_z, hat_gap)
        lat_q = q[:, 0] - body.neck[0]
        spread = (p[:, 2] < body.neck[2]) & (np.abs(lat_q) > cap)
        q[spread, 0] = body.neck[0] + np.sign(lat_q[spread]) * cap[spread]
        if braid:
            # In the plait every strand is placed exactly (its middle line is already kept off the back).
            q = np.where(in_tail[:, None], target, q)
        pts[:, k] = np.where(alive[:, None], q, p)
        arc += seg_len
        done = alive & ((q[:, 2] <= z_end) | (arc >= lengths - 1e-6))
        if radial:
            done |= alive & (np.sqrt((((q - shell_c) / shell_r) ** 2).sum(1)) >= shell_cut)
            if hat_z is not None:
                # Under a hat only the hair behind the ears puffs out below the brim; in front of them it ends there
                # (it would otherwise come down the temples and puff over the cheeks).
                done |= alive & (np.abs(az) < 100) & (q[:, 2] < hat_z - 0.004)
        end_idx[done] = k
        alive &= ~done
        if not alive.any():
            break
    log("grown", n, "strands,", segs, "segments of", round(seg_len * 100, 2), "cm in", round(time.time() - t0, 1), "s")
    return pts, end_idx, roots, seg_len


def comb(body, style, pts, end_idx, roots, rng, seg_len):
    """Clumps, waves and frizz, then once more out of the body."""
    n, k1, _ = pts.shape
    idx = np.arange(k1)[None, :]
    frac = np.clip(idx / np.maximum(end_idx[:, None], 1), 0, 1)
    # Clumps: a strand every clump_size or so is a clump's centre; the others are drawn towards it towards their tips.
    size = style["clump_size"]
    area = 4 * math.pi * body.skull_r ** 2 * 0.45
    centres_n = max(1, min(n, int(area / (size * size))))
    centre_ids = rng.choice(n, size=centres_n, replace=False)
    tree = KDTree(centres_n)
    for j, cid in enumerate(centre_ids):
        tree.insert(Vector(roots[cid]), j)
    tree.balance()
    clump_of = np.array([tree.find(Vector(r))[1] for r in roots])
    centre = centre_ids[clump_of]
    amount = style["clump"] * (0.7 + 0.6 * rng.random(n))
    pull = (amount[:, None] * frac ** 1.3)[..., None]
    cidx = np.minimum(np.broadcast_to(idx, (n, k1)), end_idx[centre][:, None])
    centre_pts = pts[centre[:, None], cidx]
    out = pts + pull * (centre_pts - pts)
    # Waves: the same for a whole clump, growing in from the first 12 cm.
    if style.get("wave", 0.0) > 0:
        period = style.get("wave_period", 0.09)
        phase = rng.random(centres_n)[clump_of] * 2 * math.pi
        s = idx * seg_len
        amp = style["wave"] * np.clip((s - 0.12) / 0.15, 0, 1)
        tang = np.gradient(out, axis=1)
        side = unit(np.cross(tang, np.array([0.0, 0.0, 1.0])) + 1e-6)
        wave = np.sin(phase[:, None] + 2 * math.pi * s / period) * amp
        out = out + side * wave[..., None] + np.cross(unit(tang), side) * (0.4 * np.cos(phase[:, None] + 2 * math.pi * s / period) * amp)[..., None]
    # Coils (an afro): each strand winds in a tight spiral of its own about its path, from just out of the skin.
    if style.get("coil", 0.0) > 0:
        period = style["coil_period"] * (0.75 + 0.5 * rng.random(n))
        phase = rng.random(n) * 2 * math.pi
        s = idx * seg_len
        amp = style["coil"] * (0.7 + 0.6 * rng.random(n))[:, None] * np.clip(s / 0.006, 0, 1)
        if style.get("_hat"):
            amp = amp * (out[..., 2] < body.skull[2] + style["_hat"]["brim"])  # no coils pressed under the hat
        tang = unit(np.gradient(out, axis=1))
        ref = np.where((np.abs(tang[..., 2]) > 0.9)[..., None], np.array([1.0, 0.0, 0.0]), np.array([0.0, 0.0, 1.0]))
        side = unit(np.cross(tang, ref))
        bi = np.cross(tang, side)
        ang = phase[:, None] + 2 * math.pi * s / period[:, None]
        out = out + side * (np.cos(ang) * amp)[..., None] + bi * (np.sin(ang) * amp)[..., None]
    # Frizz: a little random wander, more towards the tips.
    out = out + rng.normal(0, 1, out.shape) * (style["frizz"] * frac ** 1.5)[..., None]
    out[:, 0] = pts[:, 0]
    # Out of the body again (clumping and waves can pull a strand in).
    reach = 0.03
    hat = style.get("_hat")
    hat_z = body.skull[2] + hat["brim"] if hat else None
    hat_gap = hat["gap"] if hat else 0.0
    for i in range(n):
        for k in range(1, end_idx[i] + 1):
            out[i, k] = body.keep(out[i, k], 0.0015, reach, hat_z, hat_gap)
    return out


# ---- writing --------------------------------------------------------------------------------------------------------------

def make_curves(name, pts, end_idx, origin, style):
    curves = bpy.data.hair_curves.new(name)
    sizes = (end_idx + 1).tolist()
    curves.add_curves(sizes)
    # Straight segments between the points (Blender's hair curves are Catmull-Rom by default, which the Alembic file
    # carries as cubic curves with knots; Unreal's reader tripped over their knot counts, PC 2026-11-21).
    curves.set_types(type="POLY")
    flat = np.concatenate([pts[i, : end_idx[i] + 1] - origin for i in range(len(pts))])
    curves.position_data.foreach_set("vector", flat.astype(np.float32).ravel())
    radius = np.concatenate([np.linspace(style["width_root"], style["width_tip"], s) * 0.5 for s in sizes]).astype(np.float32)
    attr = curves.attributes.get("radius") or curves.attributes.new("radius", "FLOAT", "POINT")
    attr.data.foreach_set("value", radius)
    obj = bpy.data.objects.new(name, curves)
    bpy.context.scene.collection.objects.link(obj)
    return obj, len(flat)


def export_abc(obj, path):
    """Writes the curves in centimetres. The points and the widths are scaled here: Blender's own global_scale only puts a
    scale on the transform, which Unreal applies to the points but not to the widths, so the hair came in 100 times too
    thin (PC, 2026-11-21: "almost bald, a thin fuzz")."""
    curves = obj.data
    n = len(curves.position_data)
    pos = np.empty(n * 3, dtype=np.float32)
    curves.position_data.foreach_get("vector", pos)
    rad = np.empty(n, dtype=np.float32)
    curves.attributes["radius"].data.foreach_get("value", rad)
    curves.position_data.foreach_set("vector", pos * 100.0)
    curves.attributes["radius"].data.foreach_set("value", rad * 100.0)
    for o in bpy.context.scene.objects:
        o.select_set(o == obj)
    bpy.context.view_layer.objects.active = obj
    # Blender's own axes become Alembic's Y up; Unreal's default groom import turns them back.
    bpy.ops.wm.alembic_export(filepath=path, selected=True, start=1, end=1, global_scale=1.0, evaluation_mode="RENDER")
    curves.position_data.foreach_set("vector", pos)  # back to metres for the preview
    curves.attributes["radius"].data.foreach_set("value", rad)


def preview(body_obj_verts, tris, hair_obj, origin, path, head, colour=(0.09, 0.055, 0.03), cap=None):
    scene = bpy.context.scene
    extra = []
    if cap:
        # A plain cap on the head for the under-a-hat versions: a sphere just over the pressed hair, cut at the brim.
        centre, radius, brim = cap
        bpy.ops.mesh.primitive_uv_sphere_add(radius=radius, location=tuple(centre), segments=48, ring_count=24)
        hat = bpy.context.active_object
        for v in hat.data.vertices:
            if v.co.z < brim:
                v.co.z = brim
        felt = bpy.data.materials.new("Felt")
        felt.use_nodes = True
        felt.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = (0.12, 0.16, 0.10, 1)
        hat.data.materials.append(felt)
        extra.append(hat)
    mesh = bpy.data.meshes.new("PreviewBody")
    mesh.from_pydata([tuple(v - origin) for v in body_obj_verts], [], [tuple(t) for t in tris])
    mesh.shade_smooth() if hasattr(mesh, "shade_smooth") else None
    body = bpy.data.objects.new("PreviewBody", mesh)
    scene.collection.objects.link(body)
    skin = bpy.data.materials.new("Skin")
    skin.use_nodes = True
    skin.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = (0.8, 0.6, 0.5, 1)
    body.data.materials.append(skin)
    hair_mat = bpy.data.materials.new("Hair")
    hair_mat.use_nodes = True
    nodes = hair_mat.node_tree.nodes
    nodes.clear()
    out = nodes.new("ShaderNodeOutputMaterial")
    bsdf = nodes.new("ShaderNodeBsdfHairPrincipled")
    bsdf.parametrization = "MELANIN"
    bsdf.inputs["Melanin"].default_value = 0.65
    bsdf.inputs["Melanin Redness"].default_value = 0.45
    hair_mat.node_tree.links.new(bsdf.outputs[0], out.inputs["Surface"])
    hair_obj.data.materials.append(hair_mat)
    world = bpy.data.worlds.new("W")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.55, 0.6, 0.65, 1)
    world.node_tree.nodes["Background"].inputs["Strength"].default_value = 0.6
    scene.world = world
    sun = bpy.data.objects.new("Sun", bpy.data.lights.new("Sun", "SUN"))
    sun.data.energy = 3.5
    sun.rotation_euler = (math.radians(50), math.radians(10), math.radians(30))
    scene.collection.objects.link(sun)
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.cycles.samples = 24
    scene.cycles.use_denoising = True
    scene.render.resolution_x = 420
    scene.render.resolution_y = 640
    scene.render.film_transparent = False
    cam = bpy.data.objects.new("Cam", bpy.data.cameras.new("Cam"))
    cam.data.lens = 50
    scene.collection.objects.link(cam)
    scene.camera = cam
    shots = []
    # The head bone is the origin: close-ups of the head from the front, side and back, then the whole length behind.
    views = (("front", 25.0, 0.07, 0.75), ("side", 90.0, 0.03, 0.85), ("back", 160.0, 0.03, 0.85), ("full", 200.0, -0.30, 2.3))
    for name, ang, height, dist in views:
        a = math.radians(ang)
        target = Vector((0.0, 0.0, height))
        cam.location = target + Vector((math.sin(a) * dist, -math.cos(a) * dist, 0.08))
        cam.rotation_euler = (target - cam.location).to_track_quat("-Z", "Y").to_euler()
        f = path.replace(".png", "_%s.png" % name)
        scene.render.filepath = f
        bpy.ops.render.render(write_still=True)
        shots.append(f)
    bpy.data.objects.remove(body)
    for o in extra:
        bpy.data.objects.remove(o)
    bpy.data.objects.remove(sun)
    bpy.data.objects.remove(cam)
    return shots


def main():
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument("body")
    ap.add_argument("out")
    ap.add_argument("--styles", default="")
    ap.add_argument("--joints", default="")
    ap.add_argument("--preview", action="store_true")
    ap.add_argument("--no-hats", action="store_true", help="skip the under-a-hat version of each style")
    ap.add_argument("--count-scale", type=float, default=1.0)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--config", default=os.path.join(HERE, "hair_styles.json"))
    a = ap.parse_args(args)
    # Absolute paths: Blender reads relative ones against the drive root when no .blend is open (the previews went to
    # C:\game\Saved\... on the PC).
    a.body = os.path.abspath(a.body)
    a.out = os.path.abspath(a.out)
    a.joints = os.path.abspath(a.joints) if a.joints else ""
    styles = json.load(open(a.config))["styles"]
    wanted = [s for s in a.styles.split(",") if s] or list(styles)
    verts, tris, head, neck, keys, arms, bone_of = load_body(a.body, a.joints)
    body = Body(verts, tris, head, neck, arms, bone_of)
    morphs = head_morphs(body, keys)
    log("head-changing sliders:", len(morphs), "of", len(keys))
    os.makedirs(a.out, exist_ok=True)
    manifest = {"body": os.path.basename(a.body), "head": head.tolist(), "neck": neck.tolist(),
                "neck_to_head_cm": float(np.linalg.norm(head - neck) * 100),
                # For the game's fit (StrandHair.cpp): the skull centre from the head bone and each slider's change to it,
                # in Unreal's axes (Blender x, -y, z) and centimetres.
                "skull_from_head_cm": [round(float(v), 3) for v in ((body.skull - head) * 100.0 * np.array([1, -1, 1]))],
                "skull_radius_cm": round(body.skull_r * 100.0, 3),
                "head_morphs": morphs,
                # What the groom's physics collides with (StrandHair.cpp builds a physics asset from them): spheres and
                # capsules inside the skin, ends from the head bone in Unreal's axes and cm; part head, neck, torso or a
                # bone (clavicle_l, upperarm_l, lowerarm_r...) picks the bone. A sphere has only "centre_cm".
                "collision": [collision_entry(shape, head) for shape in collision_shapes(body)],
                "styles": {}}
    log("collision shapes:", len(manifest["collision"]), [(c["part"], c["radius_cm"]) for c in manifest["collision"]])
    hat = json.load(open(a.config)).get("hat")
    jobs = []
    for name in wanted:
        jobs.append((name, styles[name]))
        if hat and not a.no_hats and not styles[name].get("no_hat"):
            jobs.append((name + "Hat", dict(styles[name], _hat=hat)))  # the same style worn under a hat
    for name, style in jobs:
        rng = np.random.default_rng([a.seed, sum(map(ord, name.removesuffix("Hat")))])
        pts, end_idx, roots, seg_len = grow(body, style, rng, a.count_scale)
        pts = comb(body, style, pts, end_idx, roots, rng, seg_len)
        obj, npts = make_curves(name, pts, end_idx, head, style)
        path = os.path.join(a.out, name + ".abc")
        export_abc(obj, path)
        lengths = (end_idx * seg_len)
        manifest["styles"][name] = {"file": name + ".abc", "strands": int(len(pts)), "points": int(npts),
                                    "longest_cm": round(float(lengths.max()) * 100, 1),
                                    "lowest_tip_cm_below_head": round(float((head[2] - pts[np.arange(len(pts)), end_idx][:, 2]).max()) * 100, 1)}
        log(name, manifest["styles"][name])
        if a.preview:
            cap = None
            if style.get("_hat"):
                cap = (body.skull - head, body.skull_r + style["_hat"]["gap"] + 0.006, style["_hat"]["brim"])
            preview(verts, tris, obj, head, os.path.join(a.out, name + "_preview.png"), head, cap=cap)
        bpy.data.objects.remove(obj)
    with open(os.path.join(a.out, "grooms.json"), "w") as f:
        json.dump(manifest, f, indent=1)
    log("done", a.out)


if __name__ == "__main__":
    main()
