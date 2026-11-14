# Shared by the MakeHuman export scripts (run inside Blender): Unreal-ready transforms and the materials manifest.
import json
import os
import re

import bpy
from mathutils import Matrix


def prepare_for_unreal(rig, meshes):
    """Unreal wants centimetres and an unscaled, unrotated root: every object's world transform and a scale of 100 are
    baked into its data (bones and vertices alike), and the objects are left at the identity."""
    bpy.context.scene.unit_settings.system = "METRIC"
    bpy.context.scene.unit_settings.scale_length = 0.01
    scale = Matrix.Scale(100.0, 4)
    objs = [rig] + [m for m in meshes if m != rig]
    worlds = {o.name: o.matrix_world.copy() for o in objs}
    done = set()
    for o in objs:
        if o.data.name in done:
            continue
        done.add(o.data.name)
        if o.type == "MESH":
            o.data.transform(scale @ worlds[o.name], shape_keys=True)  # morphs (the creator's sliders) too
        else:
            o.data.transform(scale @ worlds[o.name])
    for o in objs:
        if o != rig:
            o.matrix_parent_inverse = Matrix.Identity(4)
        o.matrix_basis = Matrix.Identity(4)
    bpy.context.view_layer.update()


def _image_path(image):
    path = bpy.path.abspath(image.filepath, library=image.library)
    return os.path.normpath(path) if path and os.path.exists(path) else ""


def _linked_image(node_tree, socket_names, depth=0):
    """The image feeding a Principled BSDF input (through a few nodes), or None."""
    for node in node_tree.nodes:
        if node.type != "BSDF_PRINCIPLED":
            continue
        for name in socket_names:
            socket = node.inputs.get(name)
            if not socket or not socket.is_linked:
                continue
            stack = [socket.links[0].from_node]
            seen = 0
            while stack and seen < 12:
                n = stack.pop(0)
                seen += 1
                if n.type == "TEX_IMAGE" and n.image:
                    return n.image
                for i in n.inputs:
                    if i.is_linked:
                        stack.append(i.links[0].from_node)
    return None


def write_materials(meshes, out_dir, skin_meshes=(), tints=None, overrides=None):
    """Renames every material on the exported meshes to a plain ASCII name and writes materials.json beside the FBX:
    slot name -> colour map, normal map, whether the colour map's alpha cuts it out, and whether it is skin (our skin
    material, tinted by the pawn's skin colour). The MakeHumanArt commandlet reads it."""
    manifest = {}
    tints = tints or {}
    # A skin mesh whose material sits elsewhere (MPFB can leave a proxy without its own slot) gets one to name.
    for obj in skin_meshes:
        if not obj.material_slots or not any(slot.material for slot in obj.material_slots):
            skin = bpy.data.materials.new("Skin")
            if obj.material_slots:
                for slot in obj.material_slots:
                    slot.link = "OBJECT"
                    slot.material = skin
            else:
                obj.data.materials.append(skin)
            print("[mh_common] gave", obj.name, "a skin material")
    done = set()
    for obj in meshes:
        for index, mat in enumerate([slot.material for slot in obj.material_slots]):
            if not mat or mat.as_pointer() in done:
                continue  # a material shared by several meshes is listed once
            done.add(mat.as_pointer())
            slug = re.sub(r"[^A-Za-z0-9]+", "_", mat.name).strip("_")[:40] or "Material"
            name = f"MH{len(manifest):02d}_{slug}"
            entry = {"blender": mat.name}
            color = None
            if mat.use_nodes:
                color = _linked_image(mat.node_tree, ["Base Color"])
                if not color:
                    images = [n.image for n in mat.node_tree.nodes if n.type == "TEX_IMAGE" and n.image]
                    color = images[0] if images else None
                normal = _linked_image(mat.node_tree, ["Normal"])
                alpha = _linked_image(mat.node_tree, ["Alpha"])
                if color is not None and _image_path(color):
                    entry["color"] = _image_path(color)
                if normal is not None and _image_path(normal) and normal != color:
                    entry["normal"] = _image_path(normal)
            # Hair cards, brows and lashes cut out by their colour map's alpha.
            cutout = any(k in (obj.name + mat.name).lower() for k in ("hair", "brow", "lash", "ponytail", "tail", "eye", "high-poly", "low-poly"))
            if cutout and color is not None and getattr(color, "channels", 4) == 4:
                entry["alpha"] = True
            if "color" not in entry and "tint" not in entry:
                entry["tint"] = [round(c, 4) for c in mat.diffuse_color[:3]]
            if obj in skin_meshes:
                entry["skin"] = True
                entry.pop("alpha", None)
            for key, tint in tints.items():
                if key.lower() in mat.name.lower() or key.lower() in obj.name.lower():
                    entry["tint"] = list(tint)
            for key, extra in (overrides or {}).items():
                if key.lower() in mat.name.lower() or key.lower() in obj.name.lower():
                    entry.update(extra)
            mat.name = name
            manifest[name] = entry
    with open(os.path.join(out_dir, "materials.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1)
    return manifest


def _deform_groups(mesh, rig):
    names = {b.name for b in rig.data.bones if b.use_deform}
    return [g for g in mesh.vertex_groups if g.name in names]


def add_soft_bones(rig, body, log=print):
    """Buttock bones for jiggle, made for any MakeHuman body (no hand work): for each side, a bone inside the
    buttock's fullest point behind the hip joint, parented to the pelvis, weighted onto the skin around it (fading out
    over 11 cm, back of the body only) with the other bones' weights scaled down to make room."""
    from mathutils import Vector
    world_to_rig = rig.matrix_world.inverted() @ body.matrix_world
    verts = [world_to_rig @ v.co for v in body.data.vertices]
    bones = rig.data.bones
    if "pelvis" not in bones or "thigh_l" not in bones or "butt_l" in bones:
        return
    # MakeHuman faces -Y: the back is +Y.
    made = {}
    for side in ("l", "r"):
        hip = bones["thigh_" + side].head_local
        near = [i for i, p in enumerate(verts) if abs(p.x - hip.x) < 0.07 and hip.z - 0.12 < p.z < hip.z + 0.06 and p.y > hip.y]
        if not near:
            log("no buttock found for side", side)
            continue
        back = max(near, key=lambda i: verts[i].y)
        surface = verts[back]
        head = Vector((surface.x, surface.y - 0.06, surface.z))
        made[side] = (head, head + Vector((0.0, 0.07, 0.0)))
    if not made:
        return
    if bpy.context.object and bpy.context.object.mode != "OBJECT":
        bpy.ops.object.mode_set(mode="OBJECT")
    rig.hide_set(False)
    rig.hide_viewport = False
    bpy.ops.object.select_all(action="DESELECT")
    rig.select_set(True)
    bpy.context.view_layer.objects.active = rig
    bpy.ops.object.mode_set(mode="EDIT")
    for side, (head, tail) in made.items():
        eb = rig.data.edit_bones.new("butt_" + side)
        eb.head, eb.tail = head, tail
        eb.parent = rig.data.edit_bones["pelvis"]
        eb.use_deform = True
    bpy.ops.object.mode_set(mode="OBJECT")
    groups = _deform_groups(body, rig)
    for side, (head, _) in made.items():
        group = body.vertex_groups.new(name="butt_" + side)
        count = 0
        for i, p in enumerate(verts):
            d = (p - head).length
            if d >= 0.11 or p.y < head.y - 0.02 or (p.x - head.x) * (1 if side == "l" else -1) < -0.06:
                continue
            w = 0.9 * (1.0 - d / 0.11) ** 2
            v = body.data.vertices[i]
            for g in v.groups:
                vg = body.vertex_groups[g.group]
                if vg in groups:
                    g.weight *= (1.0 - w)
            group.add([i], w, "REPLACE")
            count += 1
        log("buttock bone", "butt_" + side, "weights", count, "vertices")


def lower_pubic_hair(obj, body, top_share=0.6, above_crotch=0.025, width=1.0, depth=0.0, log=print):
    """MakeHuman's pubic hair sits too high on these bodies (it reaches up the belly and stops well above where the legs
    meet): squeezed down towards its lowest point (its top edge comes down to top_share of its height), narrowed a
    little at the top, moved down so its lowest point is above_crotch (m) over the crotch (the lowest point of the body
    on its middle line, where the legs part), then laid back onto the skin it sits on (the same distance off the body
    as before)."""
    from mathutils import kdtree
    to_local = obj.matrix_world.inverted() @ body.matrix_world
    body_pts = [to_local @ v.co for v in body.data.vertices]
    zs = [v.co.z for v in obj.data.vertices]
    xs = [v.co.x for v in obj.data.vertices]
    if not zs:
        return
    bottom, top = min(zs), max(zs)
    centre_x = (min(xs) + max(xs)) / 2.0
    ys = [v.co.y for v in obj.data.vertices]
    front_limit = max(ys) + 0.05
    # The skin in front, indexed by height and side.
    tree = kdtree.KDTree(len(body_pts))
    for i, p in enumerate(body_pts):
        tree.insert((p.x, 0.0, p.z), i)
    tree.balance()

    def skin_y(x, z):
        # The frontmost skin (MakeHuman faces -Y) among the nearest points at that height and side.
        best = None
        for (_, i, _) in tree.find_n((x, 0.0, z), 8):
            y = body_pts[i].y
            if y < front_limit and (best is None or y < best):
                best = y
        return best

    # The crotch: the lowest body point near the middle line above the knees (between the legs there is none lower).
    middle = _perineum_heights(body_pts, centre_x, bottom - 0.25, top)
    drop = 0.0
    if middle:
        crotch = min(middle)
        drop = max(0.0, bottom - (crotch + above_crotch))
        log("pubic hair: lowest point", round(bottom - crotch, 3), "m over the crotch, moved down", round(drop, 3))
    for v in obj.data.vertices:
        x, y, z = v.co
        old_skin = skin_y(x, z)
        t = (z - bottom) / max(top - bottom, 1e-6)
        nz = bottom + (z - bottom) * top_share - drop
        nx = centre_x + (x - centre_x) * (1.0 - 0.15 * t) * width
        new_skin = skin_y(nx, nz)
        ny = y if old_skin is None or new_skin is None else new_skin + (y - old_skin)
        ny += depth  # pushed in behind the skin (+Y is into the body): the creator's "none"
        v.co = (nx, ny, nz)
    obj.data.update()
    log("pubic hair lowered: top", round(top - bottom, 3), "->", round((top - bottom) * top_share, 3), "above its lowest point")


def make_skin_normal(color_path, out_path, strength=2.5, radius=12, log=print):
    """A normal map for a skin that has none (MakeHuman's skins are colour only): the colour map's fine detail (pores,
    creases, nipples, veins) as height, darker being deeper, high-passed against a blur of `radius` texels so the broad
    shading of the paint does not turn into bumps."""
    import numpy as np
    img = bpy.data.images.load(color_path, check_existing=False)
    w, h = img.size
    px = np.empty(w * h * 4, dtype=np.float32)
    img.pixels.foreach_get(px)
    px = px.reshape(h, w, 4)
    lum = px[:, :, 0] * 0.3 + px[:, :, 1] * 0.59 + px[:, :, 2] * 0.11

    def box(a, r):
        # A separable box blur by running sums, edges clamped; three passes are close to a Gaussian.
        for axis in (0, 1):
            pad = [(0, 0), (0, 0)]
            pad[axis] = (r + 1, r)
            c = np.cumsum(np.pad(a, pad, mode="edge"), axis=axis)
            hi = np.take(c, np.arange(2 * r + 1, c.shape[axis]), axis=axis)
            lo = np.take(c, np.arange(0, c.shape[axis] - 2 * r - 1), axis=axis)
            a = (hi - lo) / (2 * r + 1)
        return a

    blur = lum
    for _ in range(3):
        blur = box(blur, radius)
    height = (lum - blur) / np.maximum(blur, 0.05)  # relative, so pale and dark skins get the same depth
    height = box(height, 1)                          # a touch of smoothing against compression noise
    dx = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * 0.5 * strength
    dy = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * 0.5 * strength
    n = np.stack([-dx, -dy, np.ones_like(dx)], axis=-1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    out = np.concatenate([n * 0.5 + 0.5, np.ones((h, w, 1), dtype=np.float32)], axis=-1).astype(np.float32)
    normal = bpy.data.images.new("skin_normal", w, h, alpha=False, float_buffer=False)
    normal.colorspace_settings.name = "Non-Color"
    normal.pixels.foreach_set(out.ravel())
    normal.filepath_raw = out_path
    normal.file_format = "PNG"
    normal.save()
    log("skin normal map", out_path, w, "x", h)
    return out_path


def _tube(name, centres, radii, frames, segments=12, uv_rect=(0.0, 0.0, 1.0, 1.0)):
    """A closed tapered tube along `centres` (rig space); `frames` gives each ring's two side axes."""
    import math
    verts, faces = [], []
    for c, r, (a, b) in zip(centres, radii, frames):
        for s in range(segments):
            ang = 2.0 * math.pi * s / segments
            verts.append(c + (a * math.cos(ang) + b * math.sin(ang)) * r)
    n = len(centres)
    for i in range(n - 1):
        for s in range(segments):
            p0 = i * segments + s
            p1 = i * segments + (s + 1) % segments
            faces.append((p0, p1, p1 + segments, p0 + segments))
    tip = len(verts)
    verts.append(centres[-1] + (centres[-1] - centres[-2]).normalized() * radii[-1] * 0.5)
    for s in range(segments):
        faces.append(((n - 1) * segments + s, (n - 1) * segments + (s + 1) % segments, tip))
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata([tuple(v) for v in verts], [], faces)
    mesh.update()
    uv = mesh.uv_layers.new(name="UVMap")
    u0, v0, u1, v1 = uv_rect
    for poly in mesh.polygons:
        poly.use_smooth = True
        for li in poly.loop_indices:
            vi = mesh.loops[li].vertex_index
            ring = min(vi // segments, n - 1)
            seg = vi % segments if vi < tip else 0
            uv.data[li].uv = (u0 + (u1 - u0) * seg / segments, v0 + (v1 - v0) * ring / max(n - 1, 1))
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(obj)
    return obj


def _skin_to(obj, rig, weights_per_vertex):
    """Parents a mesh to the rig with an armature modifier and the given {vertex: {bone: weight}}."""
    obj.parent = rig
    mod = obj.modifiers.new("Armature", "ARMATURE")
    mod.object = rig
    groups = {}
    for vi, bw in weights_per_vertex.items():
        for bone, w in bw.items():
            if bone not in groups:
                groups[bone] = obj.vertex_groups.new(name=bone)
            groups[bone].add([vi], w, "REPLACE")


def _material(name, color):
    mat = bpy.data.materials.new(name)
    mat.diffuse_color = (color[0], color[1], color[2], 1.0)
    return mat


def add_horns(rig, body, length=0.11, radius=0.017, curl=1.0, color=(0.05, 0.04, 0.035), log=print):
    """Two horns out of the top of the forehead, sweeping up, back and out, for any MakeHuman head (placed from the
    skin, not by hand): tapered tubes weighted wholly to the head bone. Returns the horn objects."""
    from mathutils import Vector
    to_rig = rig.matrix_world.inverted() @ body.matrix_world
    head = rig.data.bones.get("head")
    if head is None:
        log("no head bone: no horns")
        return []
    hx, hz = head.head_local.x, head.head_local.z
    pts = [to_rig @ v.co for v in body.data.vertices]
    skull = [p for p in pts if p.z > hz + 0.02 and abs(p.x - hx) < 0.12]
    if not skull:
        return []
    top = max(p.z for p in skull)
    front = min(p.y for p in skull if p.z > top - 0.06)   # faces -Y
    mat = _material("Horn", color)
    objs = []
    for side in (1.0, -1.0):
        # The root: on the skull, a little in front of the crown and to the side.
        want = Vector((hx + side * 0.032, front + 0.035, top - 0.025))
        base = min(skull, key=lambda p: (p - want).length)
        normal = (base - Vector((hx, base.y + 0.03, top - 0.07))).normalized()
        up, back, out = Vector((0, 0, 1)), Vector((0, 1, 0)), Vector((side, 0, 0))
        rings = 12
        centres, radii = [], []
        for i in range(rings):
            t = i / (rings - 1)
            centres.append(base - normal * 0.012 + (normal * 0.6 + up * 0.4) * length * t + back * length * 0.75 * curl * t * t
                           + out * length * 0.25 * t)
            radii.append(max(radius * (1.0 - t) ** 0.85, 0.0015))
        frames = []
        for i in range(rings):
            d = (centres[min(i + 1, rings - 1)] - centres[max(i - 1, 0)]).normalized()
            a = d.cross(Vector((1, 0, 0)))
            if a.length < 0.1:
                a = d.cross(Vector((0, 1, 0)))
            a.normalize()
            frames.append((a, d.cross(a).normalized()))
        obj = _tube("Horn_" + ("l" if side > 0 else "r"), centres, radii, frames)
        obj.data.materials.append(mat)
        _skin_to(obj, rig, {i: {"head": 1.0} for i in range(len(obj.data.vertices))})
        objs.append(obj)
    log("horns", length, "m on the skull at z", round(top, 3))
    return objs


def add_tail(rig, body, length=0.75, radius=0.03, bones=7, log=print):
    """A tail from the base of the spine for any MakeHuman body: a chain of bones tail_01... under the pelvis (the game
    swings it like hair) and a tapered tube skinned along them, out and down from the lower back and up at the tip.
    Returns the tail object (its material is skin)."""
    from mathutils import Vector
    to_rig = rig.matrix_world.inverted() @ body.matrix_world
    pelvis = rig.data.bones.get("pelvis")
    if pelvis is None or "tail_01" in rig.data.bones:
        return None
    pz = pelvis.head_local.z
    pts = [to_rig @ v.co for v in body.data.vertices]
    back = [p for p in pts if abs(p.x - pelvis.head_local.x) < 0.02 and pz - 0.02 < p.z < pz + 0.08]
    if not back:
        return None
    root = max(back, key=lambda p: p.y) + Vector((0.0, -0.02, 0.0))   # just under the skin of the lower back
    n = 24
    path = [root + Vector((0.0, 0.85 * t, -0.55 * t + 0.6 * t ** 3)) * length for t in [i / (n - 1) for i in range(n)]]
    if bpy.context.object and bpy.context.object.mode != "OBJECT":
        bpy.ops.object.mode_set(mode="OBJECT")
    bpy.ops.object.select_all(action="DESELECT")
    rig.select_set(True)
    bpy.context.view_layer.objects.active = rig
    bpy.ops.object.mode_set(mode="EDIT")
    parent = rig.data.edit_bones["pelvis"]
    joints = [path[round(i * (n - 1) / bones)] for i in range(bones + 1)]
    names = []
    for i in range(bones):
        eb = rig.data.edit_bones.new("tail_%02d" % (i + 1))
        eb.head, eb.tail = joints[i], joints[i + 1]
        eb.parent = parent
        eb.use_connect = i > 0
        eb.use_deform = True
        parent = eb
        names.append(eb.name)
    bpy.ops.object.mode_set(mode="OBJECT")
    radii = [max(radius * (1.0 - 0.85 * i / (n - 1)), 0.004) for i in range(n)]
    frames = []
    for i in range(n):
        d = (path[min(i + 1, n - 1)] - path[max(i - 1, 0)]).normalized()
        a = Vector((1, 0, 0))
        frames.append((a, d.cross(a).normalized()))
    obj = _tube("Tail", path, radii, frames, uv_rect=(0.30, 0.50, 0.36, 0.60))
    segs = 12
    weights = {}
    for vi in range(len(obj.data.vertices)):
        ring = min(vi // segs, n - 1)
        f = ring / (n - 1) * bones                     # how far along the bones, 0 .. bones
        k = min(int(f), bones - 1)
        frac = f - k
        w = {names[k]: 1.0 - 0.5 * frac}
        if k + 1 < bones:
            w[names[k + 1]] = 0.5 * frac
        weights[vi] = w
    _skin_to(obj, rig, weights)
    log("tail", length, "m,", bones, "bones")
    return obj


def add_genital_bone(rig, body, log=print):
    """A soft bone for the male genitals of an anatomy body (jiggle), found from the shape for any body: the parts in
    front of the thighs just above and below the crotch on the middle line; the bone sits at their root under the pubic
    bone, parented to the pelvis, weighted more the further a point hangs from the root."""
    from mathutils import Vector
    if "genital" in rig.data.bones or "pelvis" not in rig.data.bones:
        return
    to_rig = rig.matrix_world.inverted() @ body.matrix_world
    pts = [to_rig @ v.co for v in body.data.vertices]
    cx = rig.data.bones["pelvis"].head_local.x
    hip_z = rig.data.bones["thigh_l"].head_local.z if "thigh_l" in rig.data.bones else rig.data.bones["pelvis"].head_local.z
    mid = [p.z for p in pts if abs(p.x - cx) < 0.012 and hip_z - 0.3 < p.z < hip_z]
    if not mid:
        log("no crotch found: no genital bone")
        return
    crotch = min(mid)
    # The front of the thighs at each height, to tell what stands out in front of them.
    def thigh_front(z):
        ys = [p.y for p in pts if 0.07 < abs(p.x - cx) < 0.13 and abs(p.z - z) < 0.02]
        return min(ys) if ys else None   # MakeHuman faces -Y
    region = []
    for i, p in enumerate(pts):
        if abs(p.x - cx) < 0.065 and crotch - 0.13 < p.z < crotch + 0.07:
            front = thigh_front(p.z)
            if front is not None and p.y < front - 0.008:
                region.append(i)
    if len(region) < 20:
        log("no genitals found (", len(region), "points): no genital bone")
        return
    top = max(pts[i].z for i in region)
    back = max(pts[i].y for i in region)
    root = Vector((cx, back - 0.01, top - 0.015))
    if bpy.context.object and bpy.context.object.mode != "OBJECT":
        bpy.ops.object.mode_set(mode="OBJECT")
    bpy.ops.object.select_all(action="DESELECT")
    rig.select_set(True)
    bpy.context.view_layer.objects.active = rig
    bpy.ops.object.mode_set(mode="EDIT")
    eb = rig.data.edit_bones.new("genital")
    eb.head, eb.tail = root, root + Vector((0.0, -0.03, -0.05))
    eb.parent = rig.data.edit_bones["pelvis"]
    eb.use_deform = True
    bpy.ops.object.mode_set(mode="OBJECT")
    groups = [g for g in body.vertex_groups if g.name in {b.name for b in rig.data.bones if b.use_deform}]
    group = body.vertex_groups.new(name="genital")
    for i in region:
        w = min(1.0, (pts[i] - root).length / 0.07) * 0.95
        if w <= 0.02:
            continue
        for g in body.data.vertices[i].groups:
            if body.vertex_groups[g.group] in groups:
                g.weight *= (1.0 - w)
        group.add([i], w, "REPLACE")
    log("genital bone weights", len(region), "vertices, root", round(top - crotch, 3), "m over the crotch")


def dump_vertices(meshes, path, log=print):
    """Every mesh's vertex positions (object space) by name, for the creator's morphs (mh_export.py "morphs")."""
    import numpy as np
    data = {}
    for o in meshes:
        co = np.empty(len(o.data.vertices) * 3, dtype=np.float32)
        o.data.vertices.foreach_get("co", co)
        data[o.name] = co
    np.savez(path, **data)
    log("vertices of", len(data), "meshes to", path)


def add_morphs(meshes, morphs, log=print):
    """Shape keys from other builds of the same character (dump_vertices files): {"Heavy": "heavy.npz", ...}. A mesh
    whose vertex count differs (a part that changed) gets no key."""
    import numpy as np
    for name, path in morphs.items():
        data = np.load(path)
        made = 0
        for o in meshes:
            if o.name not in data.files or len(data[o.name]) != len(o.data.vertices) * 3:
                continue
            if not o.data.shape_keys:
                o.shape_key_add(name="Basis", from_mix=False)
            key = o.shape_key_add(name=name, from_mix=False)
            key.data.foreach_set("co", data[o.name])
            made += 1
        log("morph", name, "on", made, "meshes")


def add_tusks(rig, body, length=0.03, radius=0.006, color=(0.78, 0.72, 0.58), log=print):
    """Two lower tusks out of the jaw at the corners of the mouth, curving up and out, for any MakeHuman face (found
    from the skin: the nose tip is the face's frontmost point, the mouth a little under it), weighted to the head."""
    from mathutils import Vector
    head = rig.data.bones.get("head")
    if head is None:
        return []
    to_rig = rig.matrix_world.inverted() @ body.matrix_world
    hx, hz = head.head_local.x, head.head_local.z
    face = [to_rig @ v.co for v in body.data.vertices]
    face = [p for p in face if hz - 0.02 < p.z < hz + 0.2 and abs(p.x - hx) < 0.09]
    if not face:
        return []
    nose = min(face, key=lambda p: p.y)          # faces -Y
    mat = _material("Tusk", color)
    objs = []
    for side in (1.0, -1.0):
        want = Vector((hx + side * 0.021, nose.y + 0.02, nose.z - 0.05))
        near = [p for p in face if abs(p.x - want.x) < 0.006 and abs(p.z - want.z) < 0.006]
        base = min(near, key=lambda p: p.y) if near else want   # the skin's front there
        base = base + Vector((0.0, 0.004, 0.0))                 # its root just inside the lip
        up, out, fwd = Vector((0, 0, 1)), Vector((side, 0, 0)), Vector((0, -1, 0))
        rings = 8
        centres = [base + (up * 0.85 + fwd * 0.35) * length * (i / (rings - 1)) + out * length * 0.25 * (i / (rings - 1)) ** 2
                   for i in range(rings)]
        radii = [max(radius * (1.0 - i / (rings - 1)) ** 0.7, 0.0008) for i in range(rings)]
        frames = []
        for i in range(rings):
            d = (centres[min(i + 1, rings - 1)] - centres[max(i - 1, 0)]).normalized()
            a = d.cross(Vector((1, 0, 0)))
            if a.length < 0.1:
                a = d.cross(Vector((0, 1, 0)))
            a.normalize()
            frames.append((a, d.cross(a).normalized()))
        obj = _tube("Tusk_" + ("l" if side > 0 else "r"), centres, radii, frames, segments=10)
        obj.data.materials.append(mat)
        _skin_to(obj, rig, {i: {"head": 1.0} for i in range(len(obj.data.vertices))})
        objs.append(obj)
    log("tusks", length, "m at the mouth, nose tip at z", round(nose.z, 3))
    return objs


def _crotch(rig, pts):
    cx = rig.data.bones["pelvis"].head_local.x
    hip_z = rig.data.bones["thigh_l"].head_local.z if "thigh_l" in rig.data.bones else rig.data.bones["pelvis"].head_local.z
    mid = _perineum_heights(pts, cx, hip_z - 0.3, hip_z)
    return cx, (min(mid) if mid else None)


def _vulva_weights(rig, pts, normals, radius_x=0.03, height=0.065):
    """Weights (0..1) of the vulva on a female anatomy body: front-facing skin on the middle line from the crotch up."""
    cx, crotch = _crotch(rig, pts)
    out = {}
    if crotch is None:
        return out
    for i, (p, n) in enumerate(zip(pts, normals)):
        dx = abs(p.x - cx)
        dz = p.z - crotch
        if dx < radius_x and -0.005 < dz < height and n.y < -0.15:   # faces -Y (front) or down-front
            w = (1.0 - dx / radius_x) ** 0.7 * min(1.0, (height - dz) / 0.02) * min(1.0, (dz + 0.005) / 0.01)
            if w > 0.01:
                out[i] = min(1.0, w)
    return out


def _male_genital_weights(rig, pts):
    """The male genitals: what stands in front of the thighs at the crotch (as add_genital_bone finds them)."""
    cx, crotch = _crotch(rig, pts)
    out = {}
    if crotch is None:
        return out
    def thigh_front(z):
        ys = [p.y for p in pts if 0.07 < abs(p.x - cx) < 0.13 and abs(p.z - z) < 0.02]
        return min(ys) if ys else None
    fronts = {}
    for i, p in enumerate(pts):
        if abs(p.x - cx) < 0.065 and crotch - 0.13 < p.z < crotch + 0.07:
            key = round(p.z, 2)
            if key not in fronts:
                fronts[key] = thigh_front(p.z)
            front = fronts[key]
            if front is not None and p.y < front - 0.004:
                out[i] = min(1.0, (front - p.y) / 0.012)
    return out


def fuller_labia(rig, body, amount=0.004, log=print):
    """The creator's genital size on a female anatomy body: the vulva pushed out along its normals by up to `amount`
    metres (negative: slimmer), most at the middle line, fading to nothing at the edges."""
    to_rig = rig.matrix_world.inverted() @ body.matrix_world
    rot = to_rig.to_3x3()
    pts = [to_rig @ v.co for v in body.data.vertices]
    normals = [(rot @ v.normal).normalized() for v in body.data.vertices]
    weights = _vulva_weights(rig, pts, normals)
    scale = body.matrix_world.to_scale().x * (rig.matrix_world.to_scale().x or 1.0)
    for i, w in weights.items():
        v = body.data.vertices[i]
        v.co = v.co + v.normal * (amount * w / max(scale, 1e-6))
    body.data.update()
    log("labia", amount, "m on", len(weights), "vertices")


def make_pigment_mask(rig, body, out_path, female=True, size=1024, log=print):
    """A mask (white where darker) of the areolae and the genitals in the skin's texture space, for the skin material's
    pigment (the creator's slider): found from the shape for any body of the base."""
    import numpy as np
    to_rig = rig.matrix_world.inverted() @ body.matrix_world
    rot = to_rig.to_3x3()
    pts = [to_rig @ v.co for v in body.data.vertices]
    normals = [(rot @ v.normal).normalized() for v in body.data.vertices]
    weight = np.zeros(len(pts), dtype=np.float32)
    # Areolae: around the frontmost skin point of each breast.
    radius = 0.021 if female else 0.012
    for bone in ("breast_l", "breast_r"):
        b = rig.data.bones.get(bone)
        if b is None:
            continue
        near = [i for i, p in enumerate(pts) if (p - b.head_local).length < 0.12 and abs(p.x - b.head_local.x) < 0.06]
        if not near:
            continue
        tip = pts[min(near, key=lambda i: pts[i].y)]
        for i in near:
            d = (pts[i] - tip).length
            if d < radius:
                t = d / radius
                weight[i] = max(weight[i], 1.0 if t < 0.7 else (1.0 - t) / 0.3)
    genital = _vulva_weights(rig, pts, normals) if female else _male_genital_weights(rig, pts)
    for i, w in genital.items():
        weight[i] = max(weight[i], w)
    # Into texture space: each triangle that touches the mask, filled by its corners' weights.
    uv_layer = body.data.uv_layers.active
    mask = np.zeros((size, size), dtype=np.float32)
    filled = 0
    for poly in body.data.polygons:
        vids = list(poly.vertices)
        if max(weight[v] for v in vids) <= 0.0:
            continue
        uvs = [uv_layer.data[li].uv for li in poly.loop_indices]
        ws = [weight[v] for v in vids]
        for k in range(1, len(vids) - 1):
            tri = [(uvs[0], ws[0]), (uvs[k], ws[k]), (uvs[k + 1], ws[k + 1])]
            (a, wa), (b, wb), (c, wc) = tri
            xs = [a.x * size, b.x * size, c.x * size]
            ys = [(1.0 - a.y) * size, (1.0 - b.y) * size, (1.0 - c.y) * size]
            x0, x1 = max(int(min(xs)) - 1, 0), min(int(max(xs)) + 2, size)
            y0, y1 = max(int(min(ys)) - 1, 0), min(int(max(ys)) + 2, size)
            if x1 <= x0 or y1 <= y0 or x1 - x0 > size * 0.04 or y1 - y0 > size * 0.04:
                continue  # empty, or a triangle stretched across the texture by a seam (it drew lines on other skin)
            gx, gy = np.meshgrid(np.arange(x0, x1) + 0.5, np.arange(y0, y1) + 0.5)
            den = (ys[1] - ys[2]) * (xs[0] - xs[2]) + (xs[2] - xs[1]) * (ys[0] - ys[2])
            if abs(den) < 1e-9:
                continue
            l1 = ((ys[1] - ys[2]) * (gx - xs[2]) + (xs[2] - xs[1]) * (gy - ys[2])) / den
            l2 = ((ys[2] - ys[0]) * (gx - xs[2]) + (xs[0] - xs[2]) * (gy - ys[2])) / den
            l3 = 1.0 - l1 - l2
            inside = (l1 >= -0.02) & (l2 >= -0.02) & (l3 >= -0.02)
            value = l1 * wa + l2 * wb + l3 * wc
            region = mask[y0:y1, x0:x1]
            np.maximum(region, np.where(inside, value, 0.0), out=region)
            filled += 1
    # No blur: texels across an island edge belong to other skin (a blur drew a line along the hip). The corner
    # weights already fade the edge.
    img = bpy.data.images.new("pigment", size, size, alpha=False)
    img.colorspace_settings.name = "Non-Color"
    flipped = np.flipud(np.clip(mask, 0.0, 1.0))
    rgba = np.stack([flipped, flipped, flipped, np.ones_like(flipped)], axis=-1).astype(np.float32)
    img.pixels.foreach_set(rgba.ravel())
    img.filepath_raw = out_path
    img.file_format = "PNG"
    img.save()
    log("pigment mask", out_path, int((weight > 0).sum()), "vertices,", filled, "triangles")
    return out_path


def _perineum_heights(pts, cx, z_low, z_high):
    """Heights of the skin on the body's middle line between z_low and z_high, leaving out anything that stands out in
    front of the thighs (male genitals hang below the crotch): the lowest of them is the crotch, where the legs part."""
    thighs = {}
    for p in pts:
        if 0.07 < abs(p.x - cx) < 0.13 and z_low < p.z < z_high:
            key = round(p.z * 50.0)
            thighs[key] = min(thighs.get(key, p.y), p.y)   # faces -Y: the front of the thighs at that height
    out = []
    for p in pts:
        if abs(p.x - cx) < 0.012 and z_low < p.z < z_high:
            front = thighs.get(round(p.z * 50.0))
            if front is None or p.y >= front - 0.004:
                out.append(p.z)
    return out
