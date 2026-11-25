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
            # (The scalp hair styles go by their own names: long01, bob02, short02... Missing from this list they went in
            # opaque, solid helmets with straight cut ends, and took no hair colour of their own.)
            cutout = any(k in (obj.name + mat.name).lower() for k in ("hair", "brow", "lash", "ponytail", "tail", "eye", "high-poly", "low-poly",
                                                                    "long0", "bob0", "short0", "short_", "afro", "braid"))
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


def add_body_soft_bones(rig, body, log=print):
    """Soft bones for whole-body wobble (soft body step 1, docs/design/soft-body.md), made for any MakeHuman body with no
    hand work, like the buttock bones: a belly bone inside the fullest point of the belly (parented to spine_01, weighted
    onto the front of the trunk within 17 cm), and one soft bone along the middle of each thigh and upper arm (parented to
    that bone, weighted as a bell along its length so the middle of the limb's flesh moves and its joints stay put; the
    back of the upper arm more than the front). Other bones' weights are scaled down to make room. The game's spring
    node (MakeHumanAnimInstance) moves them; the creator scales their softness by the body's weight and muscle."""
    import math
    from mathutils import Vector
    bones = rig.data.bones
    need = ("pelvis", "spine_01", "spine_02", "thigh_l", "thigh_r", "calf_l", "calf_r", "upperarm_l", "upperarm_r", "lowerarm_l", "lowerarm_r")
    if any(n not in bones for n in need) or "belly" in bones:
        return
    world_to_rig = rig.matrix_world.inverted() @ body.matrix_world
    verts = [world_to_rig @ v.co for v in body.data.vertices]
    # MakeHuman faces -Y: the front is -Y.
    plans = {}  # name -> (parent, head, tail, weight(p) -> 0..1)
    pelvis, spine2 = bones["pelvis"].head_local, bones["spine_02"].head_local
    front = [i for i, p in enumerate(verts) if abs(p.x - pelvis.x) < 0.08 and pelvis.z + 0.03 < p.z < spine2.z and p.y < pelvis.y]
    if front:
        surface = verts[min(front, key=lambda i: verts[i].y)]
        head = Vector((pelvis.x, surface.y + 0.07, surface.z))

        def belly_w(p, head=head):
            d = (p - head).length
            if d >= 0.17 or p.y > head.y + 0.03 or p.z < head.z - 0.12:
                return 0.0
            return 0.75 * (1.0 - d / 0.17) ** 2
        plans["belly"] = ("spine_01", head, head + Vector((0.0, -0.07, 0.0)), belly_w)
    else:
        log("no belly found")

    def limb(name, parent, a, b, radius, peak, back_extra, side):
        # Copies: bone.head_local goes stale once the rig enters edit mode below (the weights read it afterwards).
        a, b = a.copy(), b.copy()
        axis = b - a
        length = axis.length
        if length < 1e-4:
            return
        u = axis / length

        def w(p):
            t = (p - a).dot(u) / length
            if t <= 0.15 or t >= 0.85:
                return 0.0
            radial = (p - a) - u * (t * length)
            r = radial.length
            if r >= radius or (p.x - a.x) * side < -radius:
                return 0.0
            bell = math.sin(math.pi * (t - 0.15) / 0.7) ** 2
            share = peak
            if back_extra and r > 1e-5:
                share += back_extra * max(0.0, radial.y / r)  # +Y is the back: the back of the upper arm hangs looser
            return min(0.85, share * bell)
        plans[name] = (parent, a + axis * 0.45, a + axis * 0.55, w)

    for side, sign in (("l", 1.0), ("r", -1.0)):
        limb("thigh_soft_" + side, "thigh_" + side, bones["thigh_" + side].head_local, bones["calf_" + side].head_local, 0.13, 0.55, 0.0, sign)
        limb("arm_soft_" + side, "upperarm_" + side, bones["upperarm_" + side].head_local, bones["lowerarm_" + side].head_local, 0.08, 0.35, 0.35, sign)
    if not plans:
        return
    if bpy.context.object and bpy.context.object.mode != "OBJECT":
        bpy.ops.object.mode_set(mode="OBJECT")
    rig.hide_set(False)
    rig.hide_viewport = False
    bpy.ops.object.select_all(action="DESELECT")
    rig.select_set(True)
    bpy.context.view_layer.objects.active = rig
    bpy.ops.object.mode_set(mode="EDIT")
    for name, (parent, head, tail, _) in plans.items():
        eb = rig.data.edit_bones.new(name)
        eb.head, eb.tail = head, tail
        eb.parent = rig.data.edit_bones[parent]
        eb.use_deform = True
    bpy.ops.object.mode_set(mode="OBJECT")
    groups = _deform_groups(body, rig)
    # Each limb's weights stay off the other side's limb (thigh_soft_l never takes the right thigh's inside).
    for name, (_, _, _, weight) in plans.items():
        group = body.vertex_groups.new(name=name)
        count = 0
        for i, p in enumerate(verts):
            w = weight(p)
            if w <= 0.01:
                continue
            v = body.data.vertices[i]
            for g in v.groups:
                vg = body.vertex_groups[g.group]
                if vg in groups:
                    g.weight *= (1.0 - w)
            group.add([i], w, "REPLACE")
            count += 1
        log("soft bone", name, "weights", count, "vertices")


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


def add_tail(rig, body, length=0.75, radius=0.03, bones=7, name="Tail", taper=0.85, log=print):
    """A tail from the base of the spine for any MakeHuman body: a chain of bones tail_01... under the pelvis (the game
    swings it like hair) and a tapered tube skinned along them, out and down from the lower back and up at the tip.
    Returns the tail object (the imp's material is skin). Called again with another name (the Beast-Strain's furred
    CatTail, thicker and hardly tapering), the tube goes on the same bones."""
    from mathutils import Vector
    to_rig = rig.matrix_world.inverted() @ body.matrix_world
    pelvis = rig.data.bones.get("pelvis")
    if pelvis is None:
        return None
    reuse = "tail_01" in rig.data.bones
    pz = pelvis.head_local.z
    pts = [to_rig @ v.co for v in body.data.vertices]
    back = [p for p in pts if abs(p.x - pelvis.head_local.x) < 0.02 and pz - 0.02 < p.z < pz + 0.08]
    if not back:
        return None
    root = max(back, key=lambda p: p.y) + Vector((0.0, -0.02, 0.0))   # just under the skin of the lower back
    n = 24
    path = [root + Vector((0.0, 0.85 * t, -0.55 * t + 0.6 * t ** 3)) * length for t in [i / (n - 1) for i in range(n)]]
    names = ["tail_%02d" % (i + 1) for i in range(bones)]
    if reuse:
        bones = len([b for b in rig.data.bones if b.name.startswith("tail_")])
        names = ["tail_%02d" % (i + 1) for i in range(bones)]
    else:
        if bpy.context.object and bpy.context.object.mode != "OBJECT":
            bpy.ops.object.mode_set(mode="OBJECT")
        bpy.ops.object.select_all(action="DESELECT")
        rig.select_set(True)
        bpy.context.view_layer.objects.active = rig
        bpy.ops.object.mode_set(mode="EDIT")
    parent = None if reuse else rig.data.edit_bones["pelvis"]
    joints = [path[round(i * (n - 1) / bones)] for i in range(bones + 1)]
    for i in (range(0) if reuse else range(bones)):
        eb = rig.data.edit_bones.new("tail_%02d" % (i + 1))
        eb.head, eb.tail = joints[i], joints[i + 1]
        eb.parent = parent
        eb.use_connect = i > 0
        eb.use_deform = True
        parent = eb
    if not reuse:
        bpy.ops.object.mode_set(mode="OBJECT")
    radii = [max(radius * (1.0 - taper * i / (n - 1)), 0.004) for i in range(n)]
    frames = []
    for i in range(n):
        d = (path[min(i + 1, n - 1)] - path[max(i - 1, 0)]).normalized()
        a = Vector((1, 0, 0))
        frames.append((a, d.cross(a).normalized()))
    obj = _tube(name, path, radii, frames, uv_rect=(0.30, 0.50, 0.36, 0.60) if name == "Tail" else (0.0, 0.0, 1.0, 4.0))
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
    log(name, length, "m,", bones, "bones")
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


def add_tusks(rig, body, length=0.03, radius=0.006, color=(0.78, 0.72, 0.58), inset=0.004, log=print):
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
        want = Vector((hx + side * 0.019, nose.y + 0.018, nose.z - 0.04))  # just under the mouth corners
        near = [p for p in face if abs(p.x - want.x) < 0.006 and abs(p.z - want.z) < 0.006]
        base = min(near, key=lambda p: p.y) if near else want   # the skin's front there
        base = base + Vector((0.0, inset, 0.0))                 # its root just inside the lip (deep inside when hidden)
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
        ref = b.tail_local  # the breast bone starts at the sternum; its tail is in the breast
        near = [i for i, p in enumerate(pts) if (p - ref).length < 0.08 and abs(p.x - ref.x) < 0.06]
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


def _pubic_layout(rig, body, female, count, seed, log=print):
    """Where each pubic hair grows, chosen on this body: [triangle (3 vertex ids), barycentric weights, lateral u (0 at
    the middle line, 1 at the edge), height v (0 at the bottom, 1 at the top), length factor, curl, zone]. Kept as
    vertex ids and weights, so the same hairs follow any morph of the body (the creator's variants)."""
    import random
    rnd = random.Random(seed)
    to_rig = rig.matrix_world.inverted() @ body.matrix_world
    rot = to_rig.to_3x3()
    pts = [to_rig @ v.co for v in body.data.vertices]
    normals = [(rot @ v.normal).normalized() for v in body.data.vertices]
    cx, crotch = _crotch(rig, pts)
    if crotch is None:
        return []
    genital = set() if female else set(_male_genital_weights(rig, pts).keys())
    root_z = max((pts[i].z for i in genital), default=crotch + 0.06)
    tris = []
    mesh = body.data
    mesh.calc_loop_triangles()
    for lt in mesh.loop_triangles:
        a, b, c = lt.vertices
        p = (pts[a] + pts[b] + pts[c]) / 3.0
        n = (normals[a] + normals[b] + normals[c]).normalized()
        dx = abs(p.x - cx)
        zone, u, v, density = None, 0.0, 0.0, 0.0
        if female:
            # An inverted triangle with a level top (no peak in the middle, sneed's line) whose two top corners are
            # only just rounded; the sides thin out so the outline is soft, the top stays a clean line.
            z0 = crotch + 0.022
            top = crotch + 0.08
            half_top = 0.046
            corner = max(0.0, (dx / half_top - 0.78) / 0.22)
            z1 = top - 0.008 * min(corner, 1.0) ** 2
            if z0 <= p.z <= z1 and n.y < -0.25:
                v = (p.z - z0) / (top - z0)
                half = 0.011 + (half_top - 0.011) * min(v, 1.0) ** 0.7
                if dx < half:
                    zone, u = "mons", dx / half
                    density = 1.0 if u < 0.6 else max(0.0, 1.0 - (u - 0.6) / 0.4) ** 1.5
                    density *= 1.0 if v < 0.88 else max(0.25, 1.0 - (v - 0.88) / 0.12)
            elif crotch + 0.004 <= p.z < z0 and 0.006 < dx < 0.02 and n.y < -0.3:
                zone, u, v = "labia", dx / 0.02, (p.z - crotch) / (z0 - crotch) * 0.1
                density = 0.25
        else:
            touching = a in genital or b in genital or c in genital
            z0, z1 = root_z - 0.012, root_z + 0.085
            if not touching and z0 <= p.z <= z1 and n.y < -0.25:
                v = (p.z - z0) / (z1 - z0)
                half = 0.042 + 0.03 * v
                if dx < half:
                    zone, u = "pubis", dx / half
                    density = 1.0 if u < 0.7 else (1.0 - u) / 0.3
                    density *= 1.0 if v < 0.8 else 0.35 + 0.65 * (1.0 - v) / 0.2
            elif touching and p.z < root_z - 0.045:
                zone, u, v, density = "scrotum", 0.0, 0.0, 0.12
        if zone and density > 0.0:
            area = ((pts[b] - pts[a]).cross(pts[c] - pts[a])).length * 0.5
            tris.append((lt.vertices[:], area * density, u, v, zone))
    if not tris:
        log("no pubic area found")
        return []
    total = sum(t[1] for t in tris)
    layout = []
    for _ in range(count):
        r = rnd.random() * total
        for t in tris:
            r -= t[1]
            if r <= 0.0:
                break
        s1, s2 = rnd.random(), rnd.random()
        if s1 + s2 > 1.0:
            s1, s2 = 1.0 - s1, 1.0 - s2
        length = rnd.uniform(0.75, 1.25) * (0.6 if t[4] in ("scrotum", "labia") else 1.0)
        # For the creator's live sliders: a random number (density keeps the hairs under it) and how far out the hair
        # is (coverage keeps the hairs inside it: the edges and the top go first, the labia and scrotum early).
        edge = 0.9 if t[4] in ("scrotum", "labia") else min(1.0, max(t[2], t[3]))  # no jitter: the patch shrinks neatly
        layout.append([list(t[0]), [1.0 - s1 - s2, s1, s2], t[2], t[3], length, rnd.uniform(0.0, 6.283), rnd.choice((-1, 1)), t[4],
                       rnd.random(), edge])
    log("pubic layout", len(layout), "hairs over", len(tris), "triangles")
    return layout


def grow_pubic_hair(rig, body, female=True, layout_path=None, count=4500, style="natural", length=0.018, width=0.00034,
                    color=(0.03, 0.025, 0.022), seed=7, curl=1.0, trim=1.0, log=print):
    """Pubic hair as hairs growing out of the skin, for any body: each a thin tapered curly ribbon rooted on the skin,
    lying down and in towards the middle line as they grow, weighted like the skin at its root (so it moves with the
    body, the genitals' jiggle included). The female shape is the mons, an inverted triangle down to the top of the
    vulva and a little along the labia; the male shape is over the pubis above the penis with a little on the scrotum
    and a thin trail up. The layout is saved to layout_path and reused if it is there (the creator's variants), so the
    same hairs follow every morph. style: natural, trimmed, strip or none (the other styles fold hairs away into the
    skin, so every variant has the same hairs)."""
    import json as _json
    import math
    from mathutils import Vector, Quaternion
    layout = None
    if layout_path and os.path.exists(layout_path):
        layout = _json.load(open(layout_path, encoding="utf-8"))
    if layout is None:
        layout = _pubic_layout(rig, body, female, count, seed, log=log)
        if layout_path:
            with open(layout_path, "w", encoding="utf-8") as f:
                _json.dump(layout, f)
    to_rig = rig.matrix_world.inverted() @ body.matrix_world
    rot = to_rig.to_3x3()
    pts = [to_rig @ v.co for v in body.data.vertices]
    normals = [(rot @ v.normal).normalized() for v in body.data.vertices]
    cx = rig.data.bones["pelvis"].head_local.x
    deform = {b.name for b in rig.data.bones if b.use_deform}
    groups = {g.index: g.name for g in body.vertex_groups if g.name in deform}
    # Fine hairs (sneed 2026-11-25: "more fine, natural, detailed ... smooth the edges, they are square like"): thinner
    # ribbons (0.34 mm at the root, was 0.6) on twice the points, so each curls in a smooth arc instead of five straight
    # pieces, tapering to a fine point instead of a flat cut end; half as many again of them keep the patch as full.
    segments = 10
    verts, faces, weights = [], [], []
    hair_uv = []
    for tri, bary, u, v, factor, phase, spin, zone, rand, edge in layout:
        hair_uv.append((rand, edge))
        keep = True
        shrink = 1.0
        if style == "none":
            keep = False
        elif style == "trimmed":
            keep = zone in ("mons", "pubis") and v < 0.65 and u < 0.75
            shrink = 0.45
        elif style == "strip":
            keep = zone in ("mons", "pubis") and u < 0.24
            shrink = 0.7
        root = pts[tri[0]] * bary[0] + pts[tri[1]] * bary[1] + pts[tri[2]] * bary[2]
        n = (normals[tri[0]] * bary[0] + normals[tri[1]] * bary[1] + normals[tri[2]] * bary[2]).normalized()
        down = Vector((0.0, 0.0, -1.0))
        tangent = (down - n * down.dot(n))
        side = Vector(((cx - root.x), 0.0, 0.0))
        side = side - n * side.dot(n)
        # Lying down and in towards the middle, the more so at the edges (the outline stays tidy).
        inward = 0.35 + 0.45 * u
        tangent = (tangent.normalized() * 0.8 + (side.normalized() * inward if side.length > 1e-5 else Vector())).normalized()
        L = length * factor * shrink * trim * (0.85 if not female else 1.0) * (1.25 if (not female and zone == "pubis") else 1.0)
        L *= 1.0 - 0.55 * max(u, v) ** 4   # shorter towards the outline
        if not keep:
            L = 0.0004
            root = root - n * 0.004   # folded away under the skin
        # The curve: out of the skin, then lying down along it, curling from side to side.
        pts_h = [root - n * 0.0005]
        d = (n * 0.55 + tangent * 0.45).normalized()
        step = L / segments
        for k in range(segments):
            # The same curl and lie as the five-piece hairs had, spread over the finer steps (each turns half as far).
            kk = k * 5.0 / segments
            twist = Quaternion(n, 0.5 * spin * (0.9 + 0.5 * math.sin(phase + kk * 1.7)) * curl)   # curl 0: straight
            d = (twist @ d)
            d = (d * 0.78 + tangent * 0.22 - n * 0.045 * kk).normalized()   # bends down onto the skin
            pts_h.append(pts_h[-1] + d * step)
        base = len(verts)
        for k, p in enumerate(pts_h):
            t = k / segments
            dirk = (pts_h[min(k + 1, segments)] - pts_h[max(k - 1, 0)]).normalized()
            across = dirk.cross(n)
            across = across.normalized() if across.length > 1e-6 else Vector((1, 0, 0))
            w = width * (1.0 - 0.97 * t ** 0.8)   # to a fine point
            verts.append(p - across * w * 0.5)
            verts.append(p + across * w * 0.5)
        for k in range(segments):
            i0 = base + k * 2
            faces.append((i0, i0 + 1, i0 + 3, i0 + 2))
        # Weighted like the skin under its root.
        wsum = {}
        for vid, bw in zip(tri, bary):
            for g in body.data.vertices[vid].groups:
                name = groups.get(g.group)
                if name:
                    wsum[name] = wsum.get(name, 0.0) + g.weight * bw
        weights.extend([wsum] * ((segments + 1) * 2))
    mesh = bpy.data.meshes.new("PubicHair")
    mesh.from_pydata([tuple(p) for p in verts], [], faces)
    mesh.update()
    uv = mesh.uv_layers.new(name="UVMap")
    per_hair = (segments + 1) * 2
    for poly in mesh.polygons:
        for li in poly.loop_indices:
            uv.data[li].uv = hair_uv[mesh.loops[li].vertex_index // per_hair]   # the material's density and coverage
    obj = bpy.data.objects.new("PubicHair", mesh)
    bpy.context.scene.collection.objects.link(obj)
    # In the rig's space, like the other generated parts.
    obj.data.materials.append(_material("PubicHair", color))
    _skin_to(obj, rig, {i: w for i, w in enumerate(weights)})
    log("pubic hair grown:", len(layout), "hairs,", style, "female" if female else "male")
    return obj


def round_hair_ends(obj, rig, start_share=0.35, side_cut=0.45, log=print):
    """Long scalp hair cut straight across at the back looks wrong: its lower part is drawn up more the further a strand
    is from the middle, so the ends make a soft U, longest in the middle (as hair falls from a round head)."""
    to_rig = rig.matrix_world.inverted() @ obj.matrix_world
    back = to_rig.inverted()
    pts = [to_rig @ v.co for v in obj.data.vertices]
    if not pts:
        return
    zmin = min(p.z for p in pts)
    zmax = max(p.z for p in pts)
    zs = zmin + (zmax - zmin) * start_share
    cx = rig.data.bones["head"].head_local.x if "head" in rig.data.bones else 0.0
    low = [p for p in pts if p.z < zs]
    width = max((abs(p.x - cx) for p in low), default=0.0)
    if width <= 1e-4:
        return
    for v, p in zip(obj.data.vertices, pts):
        if p.z < zs:
            t = min(abs(p.x - cx) / width, 1.0)
            p.z = zs - (zs - p.z) * (1.0 - side_cut * t ** 1.5)
            v.co = back @ p
    obj.data.update()
    log("hair ends rounded on", obj.name, "below", round(zs, 3))


def make_wrinkle_map(body, morphs, groups, out_path, size=2048, spacing=0.016, log=print):
    """Wrinkles for the face units, found from how each unit squeezes the skin, for any body: for each group of units
    (forehead, brow furrow, smile, nose; {"name": [morph names]}), where the skin shrinks in that unit, creases run
    across the squeeze, `spacing` metres apart, deeper the more it shrinks. Written as heights in RGBA (one group a
    channel) in the skin's texture space; the skin material turns them into a normal by how far each group's units
    are on (the creator's face)."""
    import numpy as np
    mesh = body.data
    nv = len(mesh.vertices)
    base = np.empty(nv * 3, dtype=np.float32)
    mesh.vertices.foreach_get("co", base)
    base = base.reshape(-1, 3)
    scale = body.matrix_world.to_scale().x
    edges = np.empty(len(mesh.edges) * 2, dtype=np.int64)
    mesh.edges.foreach_get("vertices", edges)
    edges = edges.reshape(-1, 2)
    # One UV per vertex (its first loop): good enough away from seams, and the face has few.
    uv_layer = mesh.uv_layers.active
    loop_uv = np.empty(len(mesh.loops) * 2, dtype=np.float32)
    uv_layer.data.foreach_get("uv", loop_uv)
    loop_uv = loop_uv.reshape(-1, 2)
    loop_vert = np.empty(len(mesh.loops), dtype=np.int64)
    mesh.loops.foreach_get("vertex_index", loop_vert)
    vuv = np.zeros((nv, 2), dtype=np.float32)
    vuv[loop_vert[::-1]] = loop_uv[::-1]
    channels = []
    for name, units in groups.items():
        # The squeeze tensor per vertex: sum over its edges of shrink * d d^T (d the edge's UV direction), and the
        # UV-per-metre scale of its edges.
        tensor = np.zeros((nv, 3), dtype=np.float64)   # xx, xy, yy
        amount = np.zeros(nv)
        count = np.zeros(nv)
        uvscale = np.zeros(nv)
        for unit in units:
            path = morphs.get(unit)
            if not path:
                continue
            data = np.load(path)
            if body.name not in data.files:
                continue
            moved = data[body.name].reshape(-1, 3)
            a, b = edges[:, 0], edges[:, 1]
            old = np.linalg.norm(base[a] - base[b], axis=1) + 1e-9
            new = np.linalg.norm(moved[a] - moved[b], axis=1)
            shrink = np.clip(1.0 - new / old, 0.0, None)
            d = vuv[b] - vuv[a]
            dl = np.linalg.norm(d, axis=1) + 1e-9
            d = d / dl[:, None]
            parts = np.stack([d[:, 0] * d[:, 0], d[:, 0] * d[:, 1], d[:, 1] * d[:, 1]], axis=1) * shrink[:, None]
            for idx in (a, b):
                np.add.at(tensor, idx, parts)
                np.add.at(amount, idx, shrink)
                np.add.at(count, idx, 1.0)
                np.add.at(uvscale, idx, dl / (old * scale))
        count = np.maximum(count, 1.0)
        amount /= count
        uvscale /= count
        tensor /= count[:, None]
        # Smoothed over the surface (neighbour averaging): the squeeze direction is noisy edge by edge, and creases
        # need it to flow smoothly or they swirl like fingerprints.
        a_, b_ = edges[:, 0], edges[:, 1]
        degree = np.zeros(nv)
        np.add.at(degree, a_, 1.0)
        np.add.at(degree, b_, 1.0)
        degree = np.maximum(degree, 1.0)
        for _ in range(40):
            for field in (tensor, amount, uvscale):
                acc = np.zeros_like(field)
                np.add.at(acc, a_, field[b_])
                np.add.at(acc, b_, field[a_])
                acc /= degree[:, None] if field.ndim == 2 else degree
                field[...] = 0.5 * field + 0.5 * acc
        # Each group at full depth where it squeezes most (the units squeeze by different amounts).
        live = amount[amount > 1e-4]
        peak = float(np.percentile(live, 98)) if live.size else 1.0
        amount = np.clip(amount / max(peak, 1e-6), 0.0, 1.0)
        # Where each group may crease (sneed: the sneer and pain units drew a crease across the upper lip, which skin
        # does not do): the nose group only on the nose and above its tip, the smile group not on the upper lip's middle.
        top = base[:, 2].max()
        head = base[base[:, 2] > top - 0.25]
        nose_tip = head[np.argmin(head[:, 1])]
        if name.lower() == "nose":
            amount *= np.clip((base[:, 2] - (nose_tip[2] - 0.004)) / 0.006, 0.0, 1.0)
        if name.lower() == "smile":
            lip = (np.abs(base[:, 0] - nose_tip[0]) < 0.017) & (base[:, 2] < nose_tip[2]) & (base[:, 2] > nose_tip[2] - 0.035)
            amount[lip] = 0.0
        channels.append((name, tensor, amount, uvscale))
    # Rasterise every face triangle where some group squeezes.
    out = np.zeros((size, size, 4), dtype=np.float32)
    mesh.calc_loop_triangles()
    tris = np.empty(len(mesh.loop_triangles) * 3, dtype=np.int64)
    mesh.loop_triangles.foreach_get("loops", tris)
    tris = tris.reshape(-1, 3)
    for ci, (name, tensor, amount, uvscale) in enumerate(channels):
        live = amount > 0.05
        # One crease direction per side of the face (the group's mean squeeze there): straight parallel creases, not
        # the rings a direction that turns from place to place gives.
        mid = float(np.median(vuv[live, 0])) if live.any() else 0.5
        side_angle = {}
        for side in (0, 1):
            sel = live & ((vuv[:, 0] >= mid) if side else (vuv[:, 0] < mid))
            Tm = (tensor[sel] * amount[sel, None]).sum(axis=0) if sel.any() else np.array([1.0, 0.0, 0.0])
            side_angle[side] = 0.5 * np.arctan2(2.0 * Tm[1], Tm[0] - Tm[2])
        period_px = max(spacing * float(np.median(uvscale[live])) * size, 3.0) if live.any() else 6.0  # one crease spacing for the group
        drawn = 0
        for t in tris:
            vs = loop_vert[t]
            if not live[vs].any():
                continue
            uvs = loop_uv[t] * size
            xs, ys = uvs[:, 0], (1.0 - loop_uv[t][:, 1]) * size
            x0, x1 = int(max(xs.min() - 1, 0)), int(min(xs.max() + 2, size))
            y0, y1 = int(max(ys.min() - 1, 0)), int(min(ys.max() + 2, size))
            if x1 <= x0 or y1 <= y0 or x1 - x0 > size * 0.05 or y1 - y0 > size * 0.05:
                continue
            gx, gy = np.meshgrid(np.arange(x0, x1) + 0.5, np.arange(y0, y1) + 0.5)
            den = (ys[1] - ys[2]) * (xs[0] - xs[2]) + (xs[2] - xs[1]) * (ys[0] - ys[2])
            if abs(den) < 1e-9:
                continue
            l1 = ((ys[1] - ys[2]) * (gx - xs[2]) + (xs[2] - xs[1]) * (gy - ys[2])) / den
            l2 = ((ys[2] - ys[0]) * (gx - xs[2]) + (xs[0] - xs[2]) * (gy - ys[2])) / den
            l3 = 1.0 - l1 - l2
            inside = (l1 >= -0.01) & (l2 >= -0.01) & (l3 >= -0.01)
            if not inside.any():
                continue
            w = np.stack([l1, l2, l3], axis=-1)
            T = w @ tensor[vs]                     # xx, xy, yy per texel
            A = w @ amount[vs]
            S = w @ uvscale[vs]
            # The side's squeeze direction (in texture space; v points down the image) and the creases across it, a
            # little wavy.
            angle = side_angle[1 if loop_uv[t][:, 0].mean() >= mid else 0]
            dx, dy = np.cos(angle), -np.sin(angle)
            period = period_px
            phase = (gx * dx + gy * dy) / period + 0.35 * np.sin((gx * dy - gy * dx) / (period * 7.0))  # gently wavy
            crease = 1.0 - np.abs(np.sin(np.pi * phase)) ** 0.5   # narrow creases
            h = np.clip(A, 0.0, 1.0) ** 1.6 * crease
            region = out[y0:y1, x0:x1, ci]
            np.maximum(region, np.where(inside, h, 0.0), out=region)
            drawn += 1
        log("wrinkles", name, "on", drawn, "triangles")
    img = bpy.data.images.new("wrinkles", size, size, alpha=True)
    img.colorspace_settings.name = "Non-Color"
    img.pixels.foreach_set(np.flipud(out).astype(np.float32).ravel())
    img.filepath_raw = out_path
    img.file_format = "PNG"
    img.save()
    return out_path


def write_body_areas(rig, body, out_path, log=print):
    """Where markings go on this body (the creator's tattoos, scars, paint): for each area a point on the skin found
    from the bones (the outside of the upper arm, the chest, the back...), its texture coordinates, which way is up
    the body there in texture space, and how much texture a metre of skin there is. Written as areas.json."""
    import json as _json
    import math
    from mathutils import Vector
    import numpy as np
    to_rig = rig.matrix_world.inverted() @ body.matrix_world
    mesh = body.data
    pts = np.array([tuple(to_rig @ v.co) for v in mesh.vertices])
    uv_layer = mesh.uv_layers.active
    loop_uv = np.empty(len(mesh.loops) * 2, dtype=np.float32)
    uv_layer.data.foreach_get("uv", loop_uv)
    loop_uv = loop_uv.reshape(-1, 2)
    loop_vert = np.empty(len(mesh.loops), dtype=np.int64)
    mesh.loops.foreach_get("vertex_index", loop_vert)
    vuv = np.zeros((len(pts), 2), dtype=np.float32)
    vuv[loop_vert[::-1]] = loop_uv[::-1]
    bones = rig.data.bones

    def B(name, at=0.5):
        b = bones.get(name)
        return None if b is None else b.head_local.lerp(b.tail_local, at)

    def axis(name):
        b = bones.get(name)
        return None if b is None else (b.tail_local - b.head_local).normalized()

    up = Vector((0, 0, 1))
    front, back = Vector((0, -1, 0)), Vector((0, 1, 0))
    specs = {}
    for s, sx in (("L", 1.0), ("R", -1.0)):
        side = "l" if s == "L" else "r"
        out = Vector((sx, 0, 0))
        specs["UpperArm" + s] = (B("upperarm_" + side), out, -axis("upperarm_" + side) if axis("upperarm_" + side) else up)
        specs["Forearm" + s] = (B("lowerarm_" + side), (out + back * 0.6).normalized(), -axis("lowerarm_" + side) if axis("lowerarm_" + side) else up)
        specs["Shoulder" + s] = (B("upperarm_" + side, 0.05), (out + up * 0.6).normalized(), up)
        specs["Chest" + s] = ((B("clavicle_" + side, 0.55) or Vector()) - up * 0.09, front, up)  # the upper chest, under the collarbone
        specs["Thigh" + s] = (B("thigh_" + side), out, -axis("thigh_" + side) if axis("thigh_" + side) else up)
        specs["Calf" + s] = (B("calf_" + side, 0.35), (out + back).normalized(), -axis("calf_" + side) if axis("calf_" + side) else up)
        specs["Cheek" + s] = ((B("head", 0.02) or Vector()) + out * 0.03, (front + out * 0.25).normalized(), up)
    specs["Belly"] = (B("spine_01", 0.6), front, up)
    specs["UpperBack"] = (B("spine_03", 0.4), back, up)
    specs["LowerBack"] = (B("spine_01", 0.3), back, up)
    specs["Neck"] = (B("neck_01", 0.5), front, up)
    specs["Forehead"] = ((B("head", 0.6) or Vector()), front, up)
    areas = {}
    for name, (p, d, u) in specs.items():
        if p is None:
            continue
        P = np.array(tuple(p))
        D = np.array(tuple(d))
        rel = pts - P
        along = rel @ D
        lateral = np.linalg.norm(rel - np.outer(along, D), axis=1)
        score = np.where((lateral < 0.06) & (along > -0.02), along - 3.0 * lateral, -1e9)
        i = int(np.argmax(score))
        if score[i] < -1e8:
            log("area", name, "not found")
            continue
        def measure(direction):
            # Texture per metre along a direction: both ways, the side that does not jump a seam.
            best = None
            for sign in (1.0, -1.0):
                k = int(np.argmin(np.linalg.norm(pts - (pts[i] + sign * 0.025 * direction), axis=1)))
                m = float(np.linalg.norm(pts[k] - pts[i]))
                duv = vuv[k] - vuv[i]
                if m < 1e-4 or np.linalg.norm(duv) > 0.08:
                    continue
                if best is None or np.linalg.norm(duv) / m < best[0]:
                    best = (float(np.linalg.norm(duv)) / m, sign * duv)
            return best
        U = np.array(tuple(u))
        up_m = measure(U)
        across = np.cross(U, D)
        across = across / (np.linalg.norm(across) or 1.0)
        across_m = measure(across)
        if up_m is None:
            log("area", name, "on a seam")
            continue
        du = up_m[1]
        # In image space (v down), the angle of "up the body" from the image's up.
        angle = math.atan2(float(du[0]), float(du[1]))
        areas[name] = {"u": float(vuv[i][0]), "v": float(1.0 - vuv[i][1]), "angle": angle, "scale": up_m[0],
                       "scale_across": across_m[0] if across_m else up_m[0]}
    with open(out_path, "w", encoding="utf-8") as f:
        _json.dump(areas, f, indent=1)
    log("body areas", len(areas), "to", out_path)
    return areas


def _torus(name, centre, axis, major, minor, rings=24, segments=8):
    """A ring (torus) round `axis` at `centre`: hoops, rings, bands."""
    import math
    from mathutils import Vector
    axis = axis.normalized()
    a = axis.cross(Vector((1, 0, 0)))
    if a.length < 0.1:
        a = axis.cross(Vector((0, 1, 0)))
    a.normalize()
    b = axis.cross(a).normalized()
    verts, faces = [], []
    for i in range(rings):
        t = 2 * math.pi * i / rings
        radial = a * math.cos(t) + b * math.sin(t)
        c = centre + radial * major
        for j in range(segments):
            s = 2 * math.pi * j / segments
            verts.append(c + radial * (minor * math.cos(s)) + axis * (minor * math.sin(s)))
    for i in range(rings):
        for j in range(segments):
            p0 = i * segments + j
            p1 = i * segments + (j + 1) % segments
            q0 = ((i + 1) % rings) * segments + j
            q1 = ((i + 1) % rings) * segments + (j + 1) % segments
            faces.append((p0, q0, q1, p1))
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata([tuple(v) for v in verts], [], faces)
    mesh.update()
    for poly in mesh.polygons:
        poly.use_smooth = True
    mesh.uv_layers.new(name="UVMap")
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(obj)
    return obj


def _band(name, centre, axis, radius, height, thickness, rings=32):
    """A flat band round `axis` (a collar, cuff or shackle): a short thick tube."""
    import math
    from mathutils import Vector
    axis = axis.normalized()
    a = axis.cross(Vector((1, 0, 0)))
    if a.length < 0.1:
        a = axis.cross(Vector((0, 1, 0)))
    a.normalize()
    b = axis.cross(a).normalized()
    profile = [(radius, -height / 2), (radius + thickness, -height / 2), (radius + thickness, height / 2), (radius, height / 2)]
    verts, faces = [], []
    for i in range(rings):
        t = 2 * math.pi * i / rings
        radial = a * math.cos(t) + b * math.sin(t)
        for r, h in profile:
            verts.append(centre + radial * r + axis * h)
    n = len(profile)
    for i in range(rings):
        for j in range(n):
            p0 = i * n + j
            p1 = i * n + (j + 1) % n
            q0 = ((i + 1) % rings) * n + j
            q1 = ((i + 1) % rings) * n + (j + 1) % n
            faces.append((p0, q0, q1, p1))
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata([tuple(v) for v in verts], [], faces)
    mesh.update()
    mesh.uv_layers.new(name="UVMap")
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(obj)
    return obj


def _join(objs):
    """Joins meshes into the first (a piece made of parts)."""
    first = objs[0]
    bpy.ops.object.select_all(action="DESELECT")
    for o in objs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = first
    with bpy.context.temp_override(active_object=first, selected_editable_objects=objs, selected_objects=objs):
        bpy.ops.object.join()
    return first


def _sphere(name, centre, radius, rings=6, segments=8):
    """A small ball (a stud, a barbell end, a gem)."""
    import math
    verts, faces = [], []
    for i in range(1, rings):
        phi = math.pi * i / rings
        for j in range(segments):
            th = 2 * math.pi * j / segments
            verts.append(centre + radius * __import__("mathutils").Vector((math.sin(phi) * math.cos(th), math.sin(phi) * math.sin(th), math.cos(phi))))
    top, bottom = len(verts), len(verts) + 1
    verts += [centre + __import__("mathutils").Vector((0, 0, radius)), centre - __import__("mathutils").Vector((0, 0, radius))]
    for i in range(rings - 2):
        for j in range(segments):
            p0, p1 = i * segments + j, i * segments + (j + 1) % segments
            faces.append((p0, p1, p1 + segments, p0 + segments))
    for j in range(segments):
        faces.append((top, (j + 1) % segments, j))
        last = (rings - 2) * segments
        faces.append((bottom, last + j, last + (j + 1) % segments))
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata([tuple(v) for v in verts], [], faces)
    mesh.update()
    for poly in mesh.polygons:
        poly.use_smooth = True
    mesh.uv_layers.new(name="UVMap")
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(obj)
    return obj


def _rod(name, a, b, radius, segments=8):
    """A straight bar from a to b (a barbell, an industrial bar, a dangle's wire)."""
    d = (b - a)
    frames = []
    axis = d.normalized()
    side = axis.cross(__import__("mathutils").Vector((0, 0, 1)))
    if side.length < 0.1:
        side = axis.cross(__import__("mathutils").Vector((1, 0, 0)))
    side.normalize()
    up = axis.cross(side).normalized()
    centres = [a.lerp(b, t / 3.0) for t in range(4)]
    return _tube(name, centres, [radius] * 4, [(side, up)] * 4, segments=segments)


def _chain(name, path, link=0.0075, wire=0.0011):
    """A chain of oval links along `path` (points), each link turned a quarter round from the last, as real chains lie."""
    import math
    from mathutils import Vector
    # Even spacing along the path.
    lengths = [0.0]
    for i in range(1, len(path)):
        lengths.append(lengths[-1] + (path[i] - path[i - 1]).length)
    total = lengths[-1]
    count = max(int(total / (link * 0.72)), 2)
    parts = []
    k = 0
    for n in range(count):
        s = total * n / count
        while k + 1 < len(lengths) - 1 and lengths[k + 1] < s:
            k += 1
        t = (s - lengths[k]) / max(lengths[k + 1] - lengths[k], 1e-6)
        p = path[k].lerp(path[k + 1], t)
        d = (path[k + 1] - path[k]).normalized()
        # A link: an oval ring whose long side runs along the chain, flat one way then the other.
        flat = d.cross(Vector((0, 0, 1)))
        if flat.length < 0.1:
            flat = d.cross(Vector((1, 0, 0)))
        flat.normalize()
        if n % 2:
            flat = d.cross(flat).normalized()
        o = _torus(name + "_%d" % n, p, flat, link * 0.38, wire, rings=10, segments=5)
        # Stretched along the chain: an oval.
        for v in o.data.vertices:
            rel = v.co - p
            v.co = p + rel + d * (rel.dot(d) * 0.45)
        parts.append(o)
    obj = _join(parts)
    obj.name = name
    obj.data.name = name
    return obj


def add_jewellery(rig, body, female, color=(0.8, 0.7, 0.45), log=print):
    """Jewellery fitted to this body (the creator shows each piece or not), each found from the body's shape and bones:
    ear piercings (lobe hoop and stud, a dangle, helix ring, tragus stud, industrial bar), a nose ring, nipple
    barbells, navel jewellery (ring, gem stud, dangle), a genital piercing (a woman's through the clitoral hood), a
    finger ring, a chain necklace lying on the skin, a collar resting low on the neck, wrist cuffs and ankle shackles.
    Piercings take the skin weights of the skin they go through, so they move exactly as it does (with the jiggle);
    bands follow their bone. Each piece is its own mesh and material, Jewel_<name>."""
    import numpy as np
    from mathutils import Vector
    to_rig = rig.matrix_world.inverted() @ body.matrix_world
    pts = np.array([tuple(to_rig @ v.co) for v in body.data.vertices])
    bones = rig.data.bones
    deform = {b.name for b in bones if b.use_deform}
    group_names = {g.index: g.name for g in body.vertex_groups}
    made = []

    def V(a):
        return Vector(tuple(float(x) for x in a))

    def skin_weights(point):
        """The skin weights of the body point nearest `point`."""
        i = int(np.argmin(np.linalg.norm(pts - np.array(tuple(point)), axis=1)))
        w = {}
        for g in body.data.vertices[i].groups:
            name = group_names.get(g.group)
            if name in deform and g.weight > 1e-3:
                w[name] = g.weight
        total = sum(w.values())
        return {k: v / total for k, v in w.items()} if total > 0 else None

    def piece(obj, bone, through=None):
        mat = _material("Jewel_" + obj.name.replace("Jewel_", ""), color)
        obj.data.materials.clear()
        obj.data.materials.append(mat)
        for poly in obj.data.polygons:
            poly.material_index = 0
        if bone not in bones:
            bone = "head" if "head" in bones else bones[0].name
        if isinstance(through, str) and through == "each":
            # Every point takes the skin weights under it (a chain lying on the body bends with it).
            per = {}
            for i, v in enumerate(obj.data.vertices):
                per[i] = skin_weights(v.co) or {bone: 1.0}
            _skin_to(obj, rig, per)
        else:
            weights = (skin_weights(through) if through is not None else None) or {bone: 1.0}
            _skin_to(obj, rig, {i: weights for i in range(len(obj.data.vertices))})
        made.append(obj.name)

    def limb_radius(point, axis, slab=0.012, near=0.12):
        P, A = np.array(tuple(point)), np.array(tuple(axis.normalized()))
        rel = pts - P
        along = rel @ A
        sel = np.abs(along) < slab
        if not sel.any():
            return 0.03
        lateral = np.linalg.norm(rel[sel] - np.outer(along[sel], A), axis=1)
        close = lateral[lateral < near]
        return float(np.percentile(close, 95)) if close.size else 0.03

    def fit_ring(point, axis, near, slab=0.005, keep=None, share=80):
        A = np.array(tuple(axis.normalized()))
        C = np.array(tuple(point))
        sel = np.abs((pts - C) @ A) < slab
        ring = pts[sel]
        if keep is not None:
            ring = ring[keep(ring)]
        for _ in range(3):
            rel = ring - C
            flat = rel - np.outer(rel @ A, A)
            dist = np.linalg.norm(flat, axis=1)
            inner = ring[dist < near]
            if len(inner) < 6:
                break
            rel_i = inner - C
            C = C + (rel_i - np.outer(rel_i @ A, A)).mean(axis=0)
        rel = ring - C
        dist = np.linalg.norm(rel - np.outer(rel @ A, A), axis=1)
        dist = dist[dist < near]
        return V(C), (float(np.percentile(dist, share)) if dist.size else near * 0.6)

    head = bones.get("head")
    hx = head.head_local.x if head else 0.0
    hz = head.head_local.z if head else 1.5
    face = pts[(pts[:, 2] > hz - 0.02) & (pts[:, 2] < hz + 0.12)]
    if head is not None and len(face):
        width = np.max(np.abs(face[:, 0] - hx))
        for side, sx in (("L", 1.0), ("R", -1.0)):
            ear = face[(face[:, 0] - hx) * sx > width * 0.82]
            if len(ear) < 5:
                continue
            out = Vector((sx, 0, 0))
            lobe = V(ear[np.argmin(ear[:, 2])])
            pierce = lobe + Vector((0, 0, 0.004))     # through the lobe, 4 mm up from its lowest point
            # Lobe hoop: its plane across the lobe (round the forward axis), so it goes through the ear and hangs.
            piece(_torus("Jewel_Earring" + side, pierce + Vector((0, 0, -0.0055)), Vector((0, 1, 0)), 0.0068, 0.0009), "head", pierce)
            # Lobe stud: a small ball on the front of the lobe with its bar through it.
            stud = _join([_sphere("stud", pierce + out * 0.0022, 0.0018), _rod("bar", pierce + out * 0.002, pierce - out * 0.003, 0.0005)])
            stud.name = "Jewel_EarStud" + side
            piece(stud, "head", pierce)
            # A dangle: a short wire from the lobe and a drop.
            drop = _join([_rod("wire", pierce, pierce + Vector((0, 0, -0.016)), 0.0005), _sphere("drop", pierce + Vector((0, 0, -0.019)), 0.0032)])
            drop.name = "Jewel_EarDangle" + side
            piece(drop, "head", pierce)
            # Helix: a small ring through the upper back rim of the ear.
            top = ear[ear[:, 2] > ear[:, 2].max() - 0.012]
            helix = V(top[np.argmax(top[:, 1])]) + Vector((0, -0.002, -0.004))
            piece(_torus("Jewel_Helix" + side, helix + Vector((0, 0, 0.0032)), Vector((0, 1, 0)), 0.0042, 0.0007), "head", helix)
            # Tragus: a stud on the small flap in front of the ear opening.
            mid = ear[np.abs(ear[:, 2] - (lobe.z + 0.022)) < 0.006]
            if len(mid):
                tragus = V(mid[np.argmin(mid[:, 1])]) + Vector((0, 0.004, 0))
                t = _join([_sphere("tragus", tragus + out * 0.0015, 0.0016), _rod("bar", tragus + out * 0.0015, tragus - out * 0.003, 0.0005)])
                t.name = "Jewel_Tragus" + side
                piece(t, "head", tragus)
            # Industrial: one bar across the top of the ear, front to back.
            upper = ear[(ear[:, 2] > helix.z - 0.016) & (np.linalg.norm(ear - np.array(tuple(helix)), axis=1) < 0.026)]
            front = (V(upper[np.argmin(upper[:, 1])]) if len(upper) else helix + Vector((0, -0.015, -0.004))) + Vector((0, 0.002, -0.003))
            back = helix
            ind = _join([_rod("bar", front - (back - front).normalized() * 0.003, back + (back - front).normalized() * 0.003, 0.0007),
                         _sphere("a", front - (back - front).normalized() * 0.004, 0.0013), _sphere("b", back + (back - front).normalized() * 0.004, 0.0013)])
            ind.name = "Jewel_Industrial" + side
            piece(ind, "head", (front + back) * 0.5)
        # Nose ring through the left nostril.
        tip = V(face[np.argmin(face[:, 1])])
        nost = V(pts[np.argmin(np.linalg.norm(pts - np.array(tuple(tip + Vector((0.011, 0.006, -0.012)))), axis=1))])
        piece(_torus("Jewel_NoseRing", nost + Vector((0.002, -0.003, -0.004)), Vector((1, 0, 0)), 0.005, 0.0009), "head", nost)
    # Nipple barbells: they take the nipple's own skin weights, so they move with the breast and not beyond it.
    for side, sx in (("L", 1.0), ("R", -1.0)):
        b = bones.get("breast_" + side.lower())
        if b is None:
            continue
        P = np.array(tuple(b.tail_local))  # the breast bone starts at the sternum; its tail is in the breast
        near = pts[(np.linalg.norm(pts - P, axis=1) < 0.08) & (np.abs(pts[:, 0] - P[0]) < 0.06)]
        if len(near):
            tip = V(near[np.argmin(near[:, 1])])
            centre = tip + Vector((0, 0.0035, 0.0))
            parts = [_band("Jewel_Nipple" + side, centre, Vector((1, 0, 0)), 0.0, 0.02, 0.0012, rings=8)]
            for end in (-0.0105, 0.0105):
                parts.append(_torus("ball", centre + Vector((end, 0, 0)), Vector((1, 0, 0)), 0.0012, 0.0021, rings=10, segments=8))
            obj = _join(parts)
            piece(obj, "breast_" + side.lower(), tip)
    # Navel: the deepest point of the belly's middle line between the hips and the ribs. A ring, a gem stud (a curved
    # bar with a gem above and a ball below) and a dangle.
    if "spine_01" in bones and "spine_02" in bones:
        cx = bones["pelvis"].head_local.x
        z0, z1 = bones["spine_01"].head_local.z, bones["spine_02"].head_local.z
        mid = pts[(np.abs(pts[:, 0] - cx) < 0.006) & (pts[:, 2] > z0) & (pts[:, 2] < z1) & (pts[:, 1] < bones["spine_01"].head_local.y)]
        if len(mid):
            navel = V(mid[np.argmax(mid[:, 1])])
            upper = navel + Vector((0, -0.002, 0.007))
            piece(_torus("Jewel_Navel", navel + Vector((0, -0.003, -0.006)), Vector((1, 0, 0)), 0.006, 0.0011), "spine_01", navel)
            gem = _join([_rod("bar", upper, navel + Vector((0, -0.001, -0.009)), 0.0008), _sphere("gem", upper + Vector((0, -0.0015, 0.002)), 0.0034),
                         _sphere("ball", navel + Vector((0, -0.003, -0.011)), 0.0024)])
            gem.name = "Jewel_NavelGem"
            piece(gem, "spine_01", navel)
            dangle = _join([_sphere("top", upper + Vector((0, -0.0015, 0.001)), 0.0024), _rod("bar", upper, navel + Vector((0, -0.002, -0.008)), 0.0008),
                            _rod("wire", navel + Vector((0, -0.003, -0.009)), navel + Vector((0, -0.004, -0.024)), 0.0006),
                            _sphere("drop1", navel + Vector((0, -0.004, -0.014)), 0.0022), _sphere("drop2", navel + Vector((0, -0.0045, -0.027)), 0.0036)])
            dangle.name = "Jewel_NavelDangle"
            piece(dangle, "spine_01", navel)
    # Genital piercing: a man's ring at the tip; a woman's through the clitoral hood, at the top of the vulva where
    # the body's front, coming up from between the legs, turns upright (the mons is above it).
    cx, crotch = _crotch(rig, [Vector(tuple(p)) for p in pts])
    if crotch is not None:
        if female:
            mid = pts[(np.abs(pts[:, 0] - cx) < 0.006) & (pts[:, 2] > crotch) & (pts[:, 2] < crotch + 0.06) & (pts[:, 1] < 0.0)]
            hood = None
            prev = None
            for dz in np.arange(0.0, 0.05, 0.0025):
                s = mid[np.abs(mid[:, 2] - (crotch + dz)) < 0.002]
                if not len(s):
                    continue
                front = s[np.argmin(s[:, 1])]
                if prev is not None and (prev[1] - front[1]) < 0.0012 * (front[2] - prev[2]) / 0.0025 and dz > 0.012:
                    hood = prev
                    break
                prev = front
            if hood is None and prev is not None:
                hood = prev
            if hood is not None:
                p = V(hood)
                piece(_torus("Jewel_Genital", p + Vector((0, -0.0025, -0.004)), Vector((1, 0, 0)), 0.0048, 0.0010), "pelvis", p)
        else:
            g = list(_male_genital_weights(rig, [Vector(tuple(p)) for p in pts]).keys())
            if g:
                gp = pts[g]
                tip = V(gp[np.argmin(gp[:, 2] - 0.3 * gp[:, 1])])
                piece(_torus("Jewel_Genital", tip + Vector((0, -0.004, -0.004)), Vector((1, 0, 0)), 0.006, 0.0012), "genital", tip)
    # A finger ring on the left ring finger: its size from that finger alone (the hand and the other fingers nearby made
    # it a halo).
    if "ring_01_l" in bones:
        b = bones["ring_01_l"]
        c = b.head_local.lerp(b.tail_local, 0.55)
        ax = b.tail_local - b.head_local
        A = np.array(tuple(ax.normalized()))

        def axis_distance(points, start, direction):
            rel = points - start
            return np.linalg.norm(rel - np.outer(rel @ direction, direction), axis=1)

        def own_finger(points):
            # Nearer this finger than the fingers either side.
            d_own = axis_distance(points, np.array(tuple(c)), A)
            keep = d_own < 0.016
            for other in ("middle_01_l", "pinky_01_l"):
                if other in bones:
                    ob = bones[other]
                    oa = np.array(tuple((ob.tail_local - ob.head_local).normalized()))
                    keep &= d_own < axis_distance(points, np.array(tuple(ob.head_local)), oa)
            return keep

        centre, r = fit_ring(c, ax, near=0.013, keep=own_finger, share=85)
        piece(_band("Jewel_Ring", centre, ax, r + 0.0005, 0.0035, 0.0012, rings=24), "ring_01_l")
    # Necklace: a chain lying on the skin round the base of the neck, lower at the front (over the collarbones, down
    # towards the breastbone), as a chain falls; collar: resting low on the neck, its front lower than its back.
    if "neck_01" in bones:
        import math
        b = bones["neck_01"]
        ax = (b.tail_local - b.head_local).normalized()
        base = b.head_local
        r_neck = limb_radius(base + ax * 0.01, ax)
        path = []
        for i in range(73):
            th = 2 * math.pi * i / 72
            radial = Vector((math.sin(th), math.cos(th), 0.0))   # th 0: the back (+Y), pi: the front
            frontness = max(0.0, -math.cos(th))
            want = base + radial * (r_neck + 0.01 + 0.03 * frontness) + Vector((0, 0, -0.012 - 0.07 * frontness ** 1.6))
            # Onto the skin, 2 mm off it.
            j = int(np.argmin(np.linalg.norm(pts - np.array(tuple(want)), axis=1)))
            surf = V(pts[j])
            normal = (surf - (base + Vector((0, 0, surf.z - base.z)))).normalized()
            path.append(surf + normal * 0.0025)
        chain = _chain("Jewel_Necklace", path)
        piece(chain, "neck_01", "each")
        tilt = (ax + Vector((0, -0.35, 0))).normalized()
        low = base + ax * 0.012 + Vector((0, -0.006, 0))
        centre, r = fit_ring(low, tilt, near=0.07, slab=0.008, share=60)
        piece(_band("Jewel_Collar", centre, tilt, r + 0.004, 0.028, 0.006), "neck_01")
    # Cuffs on the wrists and shackles on the ankles.
    for side in ("l", "r"):
        for bone, at, name, h in (("lowerarm_" + side, 0.92, "Cuff", 0.035), ("calf_" + side, 0.9, "Shackle", 0.04)):
            if bone in bones:
                b = bones[bone]
                c = b.head_local.lerp(b.tail_local, at)
                ax = b.tail_local - b.head_local
                piece(_band("Jewel_" + name + side.upper(), c, ax, limb_radius(c, ax) + 0.004, h, 0.006), bone)
    log("jewellery", len(made), made)
    return made


# ---- Beast-Strain (cat): parts made for the body they go on, like the horns and the tail ----

def _shared_material(name, color):
    mat = bpy.data.materials.get(name)
    return mat if mat else _material(name, color)


def _loft(name, rings, apex=None):
    """A closed mesh through rings of points (each the same count), capped at the end by `apex`. UV: u around, v
    along. Returns the object."""
    seg = len(rings[0])
    verts = [tuple(p) for ring in rings for p in ring]
    faces = []
    for i in range(len(rings) - 1):
        for s in range(seg):
            p0, p1 = i * seg + s, i * seg + (s + 1) % seg
            faces.append((p0, p1, p1 + seg, p0 + seg))
    last = (len(rings) - 1) * seg
    if apex is not None:
        verts.append(tuple(apex))
        faces += [(last + s, last + (s + 1) % seg, len(verts) - 1) for s in range(seg)]
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(verts, [], faces)
    mesh.update()
    uv = mesh.uv_layers.new(name="UVMap")
    for poly in mesh.polygons:
        poly.use_smooth = True
        for li in poly.loop_indices:
            vi = mesh.loops[li].vertex_index
            ring = min(vi // seg, len(rings) - 1)
            uv.data[li].uv = ((vi % seg) / seg if vi < len(rings) * seg else 0.5, ring / max(len(rings) - 1, 1))
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(obj)
    return obj


def flatten_human_ears(rig, body, log=print):
    """Takes the human ears away (the Beast-Strain's cat ears are on top of the head): every point of each ear is laid
    onto the side of the skull, which is drawn across the ear from the skull just in front of and behind it, blended out
    at the edges. Found from the shape, for any head (the ears: the head's points standing out furthest, as the
    earrings find them)."""
    import numpy as np
    from mathutils import Vector
    head = rig.data.bones.get("head")
    if head is None:
        return
    to_rig = rig.matrix_world.inverted() @ body.matrix_world
    from_rig = to_rig.inverted()
    pts = np.array([tuple(to_rig @ v.co) for v in body.data.vertices])
    hx, hz = head.head_local.x, head.head_local.z
    face = pts[(pts[:, 2] > hz - 0.02) & (pts[:, 2] < hz + 0.12)]
    width = np.max(np.abs(face[:, 0] - hx))
    moved = 0
    for sx in (1.0, -1.0):
        ear = face[(face[:, 0] - hx) * sx > width * 0.82]
        if len(ear) < 10:
            continue
        y0, y1 = ear[:, 1].min() - 0.012, ear[:, 1].max() + 0.012
        z0, z1 = ear[:, 2].min() - 0.01, ear[:, 2].max() + 0.01
        lat = (pts[:, 0] - hx) * sx

        def skull_at(y_lo, y_hi, z):
            sel = (pts[:, 1] > y_lo) & (pts[:, 1] < y_hi) & (np.abs(pts[:, 2] - z) < 0.006) & (lat > 0.02)
            return lat[sel].max() if sel.any() else None

        zs = np.arange(z0 - 0.01, z1 + 0.0101, 0.004)
        front = [skull_at(y0 - 0.012, y0 - 0.002, z) for z in zs]
        back = [skull_at(y1 + 0.002, y1 + 0.012, z) for z in zs]
        ok = [i for i in range(len(zs)) if front[i] is not None and back[i] is not None]
        if len(ok) < 3:
            log("ear", sx, ": no skull around it, kept")
            continue
        fz = np.interp(zs, zs[ok], [front[i] for i in ok])
        bz = np.interp(zs, zs[ok], [back[i] for i in ok])
        def surface_at(y, z):
            t = min(max((y - (y0 - 0.006)) / (y1 - y0 + 0.012), 0.0), 1.0)
            return np.interp(z, zs, fz) * (1.0 - t) + np.interp(z, zs, bz) * t

        box = (pts[:, 1] > y0 - 0.006) & (pts[:, 1] < y1 + 0.006) & (pts[:, 2] > z0 - 0.006) & (pts[:, 2] < z1 + 0.006) & (lat > 0.02)
        idx = [i for i in np.nonzero(box)[0] if lat[i] > surface_at(pts[i, 1], pts[i, 2]) - 0.03]   # not deep inside
        weight = {}
        for i in idx:
            y, z = pts[i, 1], pts[i, 2]
            edge = min(y - (y0 - 0.006), (y1 + 0.006) - y, z - (z0 - 0.006), (z1 + 0.006) - z)
            weight[i] = min(edge / 0.006, 1.0)   # fully inside the ear's box, fading out over 6 mm around it
        # The ear folded onto the skull overlaps itself: its points are spread out again over the patch (smoothed in
        # height and front to back, the patch's rim held), then laid on the skull.
        nbrs = {i: [] for i in idx}
        for e in body.data.edges:
            a, b = e.vertices
            if a in nbrs:
                nbrs[a].append(b)
            if b in nbrs:
                nbrs[b].append(a)
        yz = {i: pts[i, 1:3].copy() for i in idx}
        get = lambda j: yz[j] if j in yz else pts[j, 1:3]
        for _ in range(40):
            new = {}
            for i in idx:
                if weight[i] < 1.0 or not nbrs[i]:
                    new[i] = yz[i]
                    continue
                new[i] = 0.5 * yz[i] + 0.5 * np.mean([get(j) for j in nbrs[i]], axis=0)
            yz = new
        for i in idx:
            w = weight[i]
            y, z = pts[i, 1] + (yz[i][0] - pts[i, 1]) * w, pts[i, 2] + (yz[i][1] - pts[i, 2]) * w
            new_lat = lat[i] + (surface_at(y, z) - 0.0005 - lat[i]) * w
            p = np.array([hx + sx * new_lat, y, z])
            body.data.vertices[i].co = from_rig @ Vector(tuple(float(c) for c in p))
            moved += 1
    body.data.update()
    log("human ears laid flat:", moved, "points")


def make_fur_textures(out_dir, size=512, log=print):
    """A light fur colour map (the creator's fur colour multiplies it), strands along v, and its normal map: fur.png
    and fur_normal.png in out_dir."""
    import numpy as np
    rng = np.random.default_rng(7)
    u = np.arange(size)
    strands = np.zeros(size)
    for octave, amp in ((4, 0.5), (16, 0.3), (64, 0.25), (256, 0.2)):
        strands += amp * np.interp(u * octave / size, np.arange(octave + 1), rng.random(octave + 1))
    rows = rng.random((size // 32 + 2, size))
    along = np.array([np.interp(np.arange(size) / 32, np.arange(rows.shape[0]), rows[:, c]) for c in range(size)]).T
    height = strands[None, :] + 0.25 * along
    height = (height - height.min()) / (height.max() - height.min())
    colour = 0.72 + 0.28 * height
    img = np.stack([colour, colour * 0.985, colour * 0.97], axis=2)
    gy, gx = np.gradient(height)
    n = np.stack([-gx * 6.0, -gy * 2.0, np.ones_like(height)], axis=2)
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    paths = []
    os.makedirs(out_dir, exist_ok=True)
    for name, data in (("fur.png", img), ("fur_normal.png", n * 0.5 + 0.5)):
        im = bpy.data.images.new(name, size, size, alpha=False)
        if name == "fur_normal.png":
            im.colorspace_settings.name = "Non-Color"
        px = np.concatenate([data, np.ones((size, size, 1))], axis=2).astype(np.float32)
        im.pixels.foreach_set(px.ravel())
        path = os.path.join(out_dir, name)
        im.filepath_raw = path
        im.file_format = "PNG"
        im.save()
        paths.append(path)
    log("fur textures", paths)
    return paths


def _cat_ear_mesh(base, axis, fwd, lat, height, width, depth, thick, rows=14, cols=13):
    """The points and faces of one realistic cat ear (a pinna): a thin shell, not a solid. Seen from the front it is a
    broad-based triangle with gently bowed sides and a rounded tip; across, the front is a deep cup (pushed back in the
    middle, the edges rolled forward a little) and the back follows it, a few millimetres behind and fuller in the
    middle, as the cartilage is. The base runs 1.5 cm down into the skull so no seam shows under the hair.
    `base` is the middle of the ear's root on the skull; `axis` up the ear, `fwd` the way its opening faces, `lat` across.
    Returns (verts, faces, front_faces, uvs): front_faces are the inside of the cup (the pink skin)."""
    import math
    sink = 0.015
    front, back = [], []
    for i in range(rows + 1):
        v = i / rows
        # Outline: broad base, slightly bowed sides, rounded tip (the last rows close in on a curve, not a point).
        half = 0.5 * width * (1.0 - v ** 1.7) ** 0.75 * (1.0 + 0.12 * math.sin(math.pi * v))
        up = -sink + (height + sink) * (1.0 - (1.0 - v) ** 1.08)
        cup_v = (1.0 - v) ** 0.6 * min(1.0, v * 6.0 + 0.35)       # deepest above the root, shallow at the tip
        t = thick * (1.0 - 0.6 * v) + 0.0006                      # thinner towards the tip
        frow, brow = [], []
        for j in range(cols + 1):
            s = -1.0 + 2.0 * j / cols
            cup = depth * (1.0 - s * s) * cup_v                     # the middle pushed back: the cup opens forward
            roll = 0.004 * s ** 4 * (1.0 - v)                       # the rim curls forward a little
            p = base + axis * up + lat * (s * half) - fwd * cup + fwd * roll
            frow.append(p)
            bulge = 0.0025 * (1.0 - s * s) * (1.0 - v)
            brow.append(p - fwd * (t + bulge) * (1.0 if abs(s) < 0.999 else 0.25))
        front.append(frow)
        back.append(brow)
    n = (rows + 1) * (cols + 1)
    verts = [tuple(p) for row in front for p in row] + [tuple(p) for row in back for p in row]
    idx = lambda i, j: i * (cols + 1) + j
    faces, front_faces, uvs = [], [], []
    for i in range(rows):
        for j in range(cols):
            a, b, c, d = idx(i, j), idx(i, j + 1), idx(i + 1, j + 1), idx(i + 1, j)
            faces.append((a, b, c, d))                    # front, facing forward
            front_faces.append(len(faces) - 1)
            faces.append((n + a, n + d, n + c, n + b))    # back, facing back
    for i in range(rows):                                  # the two edges, closed
        faces.append((idx(i, 0), idx(i + 1, 0), n + idx(i + 1, 0), n + idx(i, 0)))
        faces.append((idx(i, cols), n + idx(i, cols), n + idx(i + 1, cols), idx(i + 1, cols)))
    for j in range(cols):                                  # the tip row, closed
        faces.append((idx(rows, j), idx(rows, j + 1), n + idx(rows, j + 1), n + idx(rows, j)))
    for j in range(cols):                                  # the root, closed (inside the skull)
        faces.append((idx(0, j), n + idx(0, j), n + idx(0, j + 1), idx(0, j + 1)))
    for k in range(2 * n):
        i, j = (k % n) // (cols + 1), (k % n) % (cols + 1)
        uvs.append((j / cols * 0.5 + (0.5 if k >= n else 0.0), i / rows))
    return verts, faces, front_faces, uvs


def add_cat_ears(rig, body, height=0.078, width=0.068, depth=0.022, tilt=0.2, thick=0.003, tufts=36, skirt=96, log=print):
    """Realistic cat ears for any MakeHuman head (sneed 2026-11-19: the old ones looked like cones): up out of the top of
    the head either side of the crown, tilted out a little and turned a little outwards, each a thin cupped pinna
    (`_cat_ear_mesh`) with a few pale fur tufts growing out of the bottom of the cup, weighted wholly to the head. The
    outside is the fur material (CatFur), the inside of the cup CatEarInner (skin, pinker). A skirt of short CatFur strands
    round the base lies down over the scalp into the hair, so the ear grows out of it instead of sitting on it (PC look
    2026-11-20: the base had a hard edge). Returns the ear objects."""
    import math
    import bmesh
    from mathutils import Vector
    head = rig.data.bones.get("head")
    if head is None:
        return []
    to_rig = rig.matrix_world.inverted() @ body.matrix_world
    hx, hz = head.head_local.x, head.head_local.z
    pts = [to_rig @ v.co for v in body.data.vertices]
    skull = [p for p in pts if p.z > hz + 0.02 and abs(p.x - hx) < 0.12]
    if not skull:
        return []
    top_p = max(skull, key=lambda p: p.z)
    top = top_p.z
    centre = Vector((hx, top_p.y, top - 0.09))
    fur = _shared_material("CatFur", (0.6, 0.45, 0.3))
    inner = _shared_material("CatEarInner", (0.85, 0.6, 0.55))
    upper = [p for p in skull if p.z > top - 0.05]
    objs = []
    for side in (1.0, -1.0):
        want = Vector((hx + side * 0.052, top_p.y + 0.006, top - 0.02))
        base = min(upper, key=lambda p: (p - want).length)
        normal = (base - centre).normalized()
        axis = (normal * 0.35 + Vector((0, 0, 1)) * 0.65 + Vector((side, 0, 0)) * tilt).normalized()
        fwd = Vector((side * math.sin(math.radians(25)), -math.cos(math.radians(25)), 0.0))   # forward, turned out
        fwd = (fwd - axis * fwd.dot(axis)).normalized()
        lat = axis.cross(fwd).normalized()
        verts, faces, front_faces, uvs = _cat_ear_mesh(base, axis, fwd, lat, height, width, depth, thick)
        name = "CatEar_" + ("l" if side > 0 else "r")
        mesh = bpy.data.meshes.new(name)
        mesh.from_pydata(verts, [], faces)
        mesh.update()
        uv = mesh.uv_layers.new(name="UVMap")
        for poly in mesh.polygons:
            poly.use_smooth = True
            for li in poly.loop_indices:
                uv.data[li].uv = uvs[mesh.loops[li].vertex_index]
        mesh.materials.append(fur)
        mesh.materials.append(inner)
        front_set = set(front_faces)
        rows, cols = 14, 13
        for k, poly in enumerate(mesh.polygons):
            if k in front_set:
                i, j = (k // 2) // cols, (k // 2) % cols
                # The pink skin inside the cup; a furred rim round it and up the tip, as on a real cat.
                if 1 <= i < rows - 3 and 2 <= j < cols - 2:
                    poly.material_index = 1
        obj = bpy.data.objects.new(name, mesh)
        bpy.context.scene.collection.objects.link(obj)
        bm = bmesh.new()
        bm.from_mesh(obj.data)
        bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
        bm.to_mesh(obj.data)
        bm.free()
        parts = [obj]
        # Ear furnishings: pale tufts out of the bottom of the cup, curving up and fanning out past the rim (the inner
        # material), long and thick enough to read against the dark fur at play distance.
        for k in range(tufts):
            s = -0.6 + 1.2 * (k + 0.5) / tufts
            v0 = 0.1 + 0.16 * ((k * 7) % 3) / 2.0
            half = 0.5 * width * (1.0 - v0 ** 1.7) ** 0.75
            root = base + axis * (height * v0) + lat * (s * half) - fwd * (depth * (1.0 - s * s) * 0.8 - 0.002)
            length = height * (0.36 + 0.16 * ((k * 5) % 4) / 3.0)
            out = (axis * 0.85 + fwd * 0.3 + lat * (s * 0.35)).normalized()
            # Soft and curling: they start up the cup, then bend out of it and to the side, like a wisp of hair.
            bend = (fwd * 0.7 + lat * (s * 0.9 + 0.25 * (((k * 11) % 5) / 4.0 - 0.5))).normalized()
            n = 7
            centres = [root + out * length * t + bend * length * 0.45 * t * t for t in [q / (n - 1) for q in range(n)]]
            radii = [max(0.0009 * (1.0 - q / (n - 1)) ** 0.8, 0.00008) for q in range(n)]
            frames = []
            for q in range(n):
                dd = (centres[min(q + 1, n - 1)] - centres[max(q - 1, 0)]).normalized()
                a = dd.cross(fwd).normalized()
                frames.append((a, dd.cross(a).normalized()))
            tuft = _tube("%s_tuft%d" % (name, k), centres, radii, frames, segments=5)
            tuft.data.materials.append(inner)  # pale, as a cat's ear furnishings are
            tuft.data.materials.append(fur)
            parts.append(tuft)
        # The fur skirt: two rings of short strands round the foot of the ear, rooted a little inside the scalp and lying
        # out and down over it into the hair, longer behind than in front, the way a cat's ear fur runs into its coat.
        for k in range(skirt):
            ring = k % 2
            a = 2.0 * math.pi * (k // 2 + 0.5 * ring) / max(skirt // 2, 1)
            radial = (lat * math.cos(a) * 0.5 * width * (1.05 + 0.15 * ring) + fwd * math.sin(a) * (depth * 0.9 + 0.006))
            root = base + radial - normal * 0.003 + axis * (0.004 + 0.004 * ring)
            flat = (radial - normal * radial.dot(normal)).normalized() if radial.length > 1e-6 else lat
            behind = 0.5 - 0.5 * math.sin(a)  # 1 at the back of the ear (fwd points forward)
            length = 0.006 + 0.006 * behind + 0.002 * ((k * 3) % 4) / 3.0
            # Short, back over the scalp and bending down onto it: fur lying flat, not a frill (PC look v2: it stuck out sideways).
            down_back = (fwd * -1.0 - normal * fwd.dot(normal) * -1.0)
            down_back = down_back.normalized() if down_back.length > 1e-6 else flat
            out = (flat * 0.3 + down_back * 0.7 + axis * (0.08 - 0.06 * ring)).normalized()
            out = (out - normal * max(out.dot(normal), 0.0) * 0.8).normalized()
            n = 5
            centres = [root + out * length * t - normal * length * 0.8 * t * t for t in [q / (n - 1) for q in range(n)]]
            radii = [max(0.0008 * (1.0 - q / (n - 1)) ** 0.6, 0.0001) for q in range(n)]
            frames = []
            for q in range(n):
                dd = (centres[min(q + 1, n - 1)] - centres[max(q - 1, 0)]).normalized()
                side_axis = dd.cross(normal)
                side_axis = side_axis.normalized() if side_axis.length > 1e-6 else lat
                frames.append((side_axis, dd.cross(side_axis).normalized()))
            strand = _tube("%s_skirt%d" % (name, k), centres, radii, frames, segments=4)
            strand.data.materials.append(fur)
            parts.append(strand)
        ear = _join(parts) if len(parts) > 1 else obj
        ear.name = name
        _skin_to(ear, rig, {i: {"head": 1.0} for i in range(len(ear.data.vertices))})
        objs.append(ear)
    log("cat ears", height, "m, thin cupped pinnae with", tufts, "tufts and a", skirt, "strand fur skirt, on the skull at z", round(top, 3))
    return objs


def add_cat_claws(rig, body, length=0.009, radius=0.0021, color=(0.88, 0.84, 0.76), log=print):
    """Small cat claws at the tip of every finger and thumb: a curved cone from the nail side of the fingertip, out along
    the finger and curling towards the palm, weighted to the finger's last bone. One mesh, CatClaws, material CatClaw."""
    import numpy as np
    from mathutils import Vector
    bones = rig.data.bones
    to_rig = rig.matrix_world.inverted() @ body.matrix_world
    pts = np.array([tuple(to_rig @ v.co) for v in body.data.vertices])
    mat = _shared_material("CatClaw", color)
    objs = []
    for side, sx in (("l", 1.0), ("r", -1.0)):
        i1, p1, m3 = bones.get("index_01_" + side), bones.get("pinky_01_" + side), bones.get("middle_03_" + side)
        hand = bones.get("hand_" + side)
        if not (i1 and p1 and m3 and hand):
            continue
        # The back of the hand: across the knuckles crossed with along the hand, the way that points up and out in the
        # rest pose (palms down).
        across = Vector(p1.head_local) - Vector(i1.head_local)
        along = Vector(m3.tail_local) - Vector(hand.head_local)
        back = across.cross(along).normalized()
        if back.z + 0.3 * sx * back.x < 0:
            back = -back
        for finger in ("thumb", "index", "middle", "ring", "pinky"):
            b = bones.get("%s_03_%s" % (finger, side))
            if b is None:
                continue
            head, tail = Vector(b.head_local), Vector(b.tail_local)
            d = (tail - head).normalized()
            D = np.array(tuple(d))
            near = pts[np.linalg.norm(pts - np.array(tuple(tail)), axis=1) < 0.025]
            if not len(near):
                continue
            rel = near - np.array(tuple(head))
            alongs = rel @ D
            tip_along = float(alongs.max())
            ring = rel[np.abs(alongs - (tip_along - 0.008)) < 0.003]
            r_f = float(np.median(np.linalg.norm(ring - np.outer(ring @ D, D), axis=1))) if len(ring) else 0.006
            up = (back - d * back.dot(d)).normalized()
            start = head + d * (tip_along - 0.005) + up * r_f * 0.55
            n = 8
            centres = [start + d * length * t - up * length * 0.5 * t * t for t in [k / (n - 1) for k in range(n)]]
            radii = [max(radius * (1.0 - k / (n - 1)) ** 0.8, 0.0003) for k in range(n)]
            frames = []
            for k in range(n):
                dd = (centres[min(k + 1, n - 1)] - centres[max(k - 1, 0)]).normalized()
                a = dd.cross(up).normalized()
                frames.append((a * 0.75, dd.cross(a).normalized()))   # a little flat side to side, like a claw
            obj = _tube("CatClaw_%s_%s" % (finger, side), centres, radii, frames, segments=8)
            obj.data.materials.append(mat)
            _skin_to(obj, rig, {i: {b.name: 1.0} for i in range(len(obj.data.vertices))})
            objs.append(obj)
    if not objs:
        return []
    claws = _join(objs)
    claws.name = "CatClaws"
    log("cat claws:", len(objs))
    return [claws]


def feather_hair_ends(src, dst, tip_share=0.13, fade_share=0.025, seed=3, log=print):
    """MakeHuman's long hair card ends in a solid straight edge (its alpha only blurs the last few pixels), which reads
    as hair cut with one snip. This rewrites the colour map's alpha so the strands end at different lengths: in each
    column of the texture (one strand's width, along which the hair runs), the hair's lowest point is pulled up by a
    random share of its length (fine per strand, coarser in clumps), with a short fade, so the ends are wispy."""
    import numpy as np
    img = bpy.data.images.load(src, check_existing=False)
    w, h = img.size
    flat = np.empty(w * h * 4, dtype=np.float32)
    img.pixels.foreach_get(flat)
    px = flat.reshape(h, w, 4)[::-1].copy()   # Blender keeps the bottom row first: top row first here
    alpha = px[:, :, 3].copy()
    rng = np.random.default_rng(seed)

    def smooth_noise(n, width):
        raw = rng.random(n + 2 * width)
        kernel = np.ones(width) / width
        return np.convolve(raw, kernel, mode="same")[width:width + n]

    fine = smooth_noise(w, 2)
    clump = smooth_noise(w, 28)
    clump = (clump - clump.min()) / max(clump.max() - clump.min(), 1e-6)
    fine = (fine - fine.min()) / max(fine.max() - fine.min(), 1e-6)
    pull = 0.45 * fine ** 2.0 + 0.55 * clump
    rows = np.arange(h)[:, None]
    solid = alpha > 0.5
    has = solid.any(axis=0)
    bottom = np.where(has, h - 1 - np.argmax(solid[::-1], axis=0), 0)
    top = np.where(has, np.argmax(solid, axis=0), 0)
    length = (bottom - top).astype(np.float32)
    cut = bottom - length * tip_share * pull
    fade = np.maximum(length * fade_share, 2.0)
    factor = np.clip((cut[None, :] - rows) / fade[None, :] + 1.0, 0.0, 1.0)
    factor[:, ~has] = 1.0
    px[:, :, 3] = alpha * factor
    out = bpy.data.images.new(os.path.basename(dst), w, h, alpha=True)
    out.pixels.foreach_set(px[::-1].ravel())
    out.filepath_raw = dst
    out.file_format = "PNG"
    out.save()
    bpy.data.images.remove(img)
    log("hair ends feathered:", os.path.basename(src), "->", dst)
    return dst


def add_hair_bones(rig, hairs, azimuths=(0.0, 55.0, -55.0, 115.0, -115.0), bones=4, log=print):
    """Bones for long scalp hair to swing on (MakeHuman's long, bob and braid styles have none, so they stayed stiff):
    chains hair_c1_1 ... under the head, hanging down the hair at the back (azimuth 0), the back corners and the sides,
    shaped along the longest hair, shared by every style (only one shows at a time). Each style's points below the back of
    the skull are weighted to the nearest two chains by direction round the head and along the chain by height, blending
    from the head bone near the top. The game swings the chains (MakeHumanAnimInstance)."""
    import math
    import numpy as np
    from mathutils import Vector
    head = rig.data.bones.get("head")
    hairs = [h for h in hairs if h is not None]
    if head is None or not hairs or "hair_c1_1" in rig.data.bones:
        return
    hx, hy, hz = head.head_local
    to_rig = {h.name: rig.matrix_world.inverted() @ h.matrix_world for h in hairs}
    pts = {h.name: np.array([tuple(to_rig[h.name] @ v.co) for v in h.data.vertices]) for h in hairs}
    root_z = hz + 0.04   # about the back of the skull, where long hair leaves the head

    def azimuth(p):
        return np.degrees(np.arctan2(p[:, 0] - hx, p[:, 1] - hy))   # 0 behind (+Y), +90 the left side, 180 the face

    longest = max(hairs, key=lambda h: np.ptp(pts[h.name][:, 2]))
    lp = pts[longest.name]
    laz = azimuth(lp)
    chains = []
    for c, az in enumerate(azimuths):
        near = lp[(np.abs(((laz - az) + 180.0) % 360.0 - 180.0) < 20.0) & (lp[:, 2] < root_z)]
        if len(near) < 10:
            continue
        bottom = float(near[:, 2].min())
        if root_z - bottom < 0.08:
            continue
        joints = []
        for i in range(bones + 1):
            z = root_z - (root_z - bottom) * i / bones
            ring = near[np.abs(near[:, 2] - z) < max((root_z - bottom) / bones * 0.5, 0.01)]
            if not len(ring):
                ring = near[np.argsort(np.abs(near[:, 2] - z))[:5]]
            # The hair's middle at this height, a little inside its surface.
            r = np.hypot(ring[:, 0] - hx, ring[:, 1] - hy)
            a = math.radians(az)
            rr = float(np.median(r)) * 0.92
            joints.append(Vector((hx + rr * math.sin(a), hy + rr * math.cos(a), z)))
        chains.append((c + 1, az, bottom, joints))
    if not chains:
        log("hair bones: no long hair")
        return
    if bpy.context.object and bpy.context.object.mode != "OBJECT":
        bpy.ops.object.mode_set(mode="OBJECT")
    bpy.ops.object.select_all(action="DESELECT")
    rig.select_set(True)
    bpy.context.view_layer.objects.active = rig
    bpy.ops.object.mode_set(mode="EDIT")
    for c, az, bottom, joints in chains:
        parent = rig.data.edit_bones["head"]
        for i in range(bones):
            eb = rig.data.edit_bones.new("hair_c%d_%d" % (c, i + 1))
            eb.head, eb.tail = joints[i], joints[i + 1]
            eb.parent = parent
            eb.use_connect = i > 0
            eb.use_deform = True
            parent = eb
    bpy.ops.object.mode_set(mode="OBJECT")
    chain_az = np.array([az for _, az, _, _ in chains])
    for h in hairs:
        p = pts[h.name]
        az = azimuth(p)
        deform = {b.name for b in rig.data.bones if b.use_deform}
        groups = {g.index: g for g in h.vertex_groups}
        made = 0
        new_groups = {}
        for vi in np.nonzero(p[:, 2] < root_z + 0.02)[0]:
            z = p[vi, 2]
            # The two nearest chains round the head, blended linearly between them.
            d = np.abs(((az[vi] - chain_az) + 180.0) % 360.0 - 180.0)
            order = np.argsort(d)
            if d[order[0]] > 75.0:
                continue   # in front of the face: stays on the head
            w_chain = {}
            if len(order) > 1 and d[order[1]] < 90.0:
                span = d[order[0]] + d[order[1]]
                w_chain[order[0]] = d[order[1]] / span
                w_chain[order[1]] = d[order[0]] / span
            else:
                w_chain[order[0]] = 1.0
            weights = {}
            hold = min(max(1.0 - (root_z + 0.02 - z) / 0.08, 0.0), 1.0)   # the head holds the top
            if hold > 0:
                weights["head"] = hold
            for ci, wc in w_chain.items():
                c, _, bottom, _ = chains[ci]
                f = min(max((root_z - z) / max(root_z - bottom, 1e-3), 0.0), 0.999) * bones
                k = int(f)
                frac = f - k
                for bi, wb in ((k, 1.0 - frac), (min(k + 1, bones - 1), frac)):
                    name = "hair_c%d_%d" % (c, bi + 1)
                    weights[name] = weights.get(name, 0.0) + (1.0 - hold) * wc * wb
            for g in h.vertex_groups:
                if g.name in deform:
                    g.remove([int(vi)])
            for name, wv in weights.items():
                if wv <= 1e-4:
                    continue
                g = h.vertex_groups.get(name) or new_groups.get(name)
                if g is None:
                    g = h.vertex_groups.new(name=name)
                    new_groups[name] = g
                g.add([int(vi)], float(wv), "REPLACE")
            made += 1
        log("hair bones on", h.name, ":", made, "points")
    log("hair bones:", len(chains), "chains of", bones)


def _mask_groups(obj):
    """{vertex group name: {vertex index: weight}} for every group a MASK modifier on obj reads (MakeHuman's "delete"
    groups, which cut away the skin under clothes)."""
    if obj is None:
        return {}
    names = {m.vertex_group for m in obj.modifiers if m.type == "MASK" and m.vertex_group}
    out = {}
    for name in names:
        group = obj.vertex_groups.get(name)
        if group is None:
            continue
        members = {}
        for v in obj.data.vertices:
            for g in v.groups:
                if g.group == group.index:
                    members[v.index] = g.weight
        out[name] = members
    return out


def _restore_mask_groups(obj, before, log=print):
    """Puts obj's mask groups (and MASK modifiers) back as they were before garments were fitted, so a garment never
    cuts the body: the game puts it on and takes it off."""
    if obj is None:
        return
    for m in list(obj.modifiers):
        if m.type == "MASK" and m.vertex_group not in before:
            obj.modifiers.remove(m)
    for name, members in _mask_groups(obj).items():
        old = before.get(name, {})
        group = obj.vertex_groups.get(name)
        extra = [i for i in members if i not in old]
        if group is not None and extra:
            group.remove(extra)
            log("garments: kept", len(extra), "body vertices out of", name, "on", obj.name)


def fit_garments(basemesh, proxy, garments, place=lambda p: p, log=print):
    """Fits each garment ({"name", "mhclo"}) to the human as MPFB clothes and names its object G_<name> (the same in every
    creator variant, so its morphs line up). The body keeps every vertex. Returns the garment objects."""
    if not garments:
        return []
    from bl_ext.user_default.mpfb.services.humanservice import HumanService
    before = {o.name: _mask_groups(o) for o in (basemesh, proxy) if o is not None}
    out = []
    for g in garments:
        path = place(g["mhclo"])
        if os.path.isdir(path):  # an asset folder: the .mhclo in it
            found = sorted(f for f in os.listdir(path) if f.lower().endswith(".mhclo"))
            path = os.path.join(path, found[0]) if found else path
        if not os.path.isfile(path):
            log("garments: missing", g["name"], path)
            continue
        obj = HumanService.add_mhclo_asset(path, basemesh, asset_type="Clothes", subdiv_levels=0, material_type="GAMEENGINE")
        obj.name = "G_" + g["name"]
        obj["rim3d_garment"] = g["name"]
        out.append(obj)
        log("garment", g["name"], len(obj.data.vertices), "vertices")
    for o in (basemesh, proxy):
        if o is not None:
            _restore_mask_groups(o, before.get(o.name, {}), log=log)
    return out


def export_garments(rig, garments, out_dir, log=print):
    """Each garment with the skeleton as <out_dir>/<name>/<name>.fbx and its materials.json beside it (as the body's;
    the MakeHumanArt commandlet imports them onto the body's skeleton). Call after the body's export: the objects are
    already in centimetres, morphs and all."""
    for obj in garments:
        name = obj.get("rim3d_garment") or obj.name
        folder = os.path.join(out_dir, name)
        os.makedirs(folder, exist_ok=True)
        manifest = write_materials([obj], folder)
        for entry in manifest.values():
            entry["garment"] = True
        with open(os.path.join(folder, "materials.json"), "w", encoding="utf-8") as f:
            json.dump(manifest, f, indent=1)
        bpy.ops.object.select_all(action="DESELECT")
        rig.select_set(True)
        obj.hide_set(False)
        obj.select_set(True)
        bpy.context.view_layer.objects.active = rig
        path = os.path.join(folder, name + ".fbx")
        bpy.ops.export_scene.fbx(filepath=path, use_selection=True, object_types={"ARMATURE", "MESH"},
                                 apply_scale_options="FBX_SCALE_NONE", add_leaf_bones=False, bake_anim=False,
                                 mesh_smooth_type="FACE", use_armature_deform_only=True, path_mode="COPY", embed_textures=False,
                                 axis_forward="-Y", axis_up="Z")
        keys = len(obj.data.shape_keys.key_blocks) - 1 if obj.data.shape_keys else 0
        log("garment exported", name, len(obj.data.vertices), "vertices", keys, "morphs", path)


def _erode_cover(verts, covered, margin=0.04):
    """A covered vertex stays covered only if no uncovered one is within `margin` metres: the skin near a garment's edge
    stays drawn, so a hem that rides up when the body moves, or the gap between a top and trousers, never shows a hole
    through the body (bare skin at a hem is fine; a see-through gap is not)."""
    from mathutils.kdtree import KDTree
    open_ = [i for i in range(len(verts)) if not covered[i]]
    if not open_:
        return covered
    tree = KDTree(len(open_))
    for n, i in enumerate(open_):
        tree.insert(verts[i], n)
    tree.balance()
    keep = covered.copy()
    for i in range(len(verts)):
        if covered[i] and tree.find(verts[i])[2] < margin:
            keep[i] = False
    return keep


def _cover_raster(bits, uv, size, faces, mask):
    """Sets `mask` in `bits` (size x size, on the body's UVs) under each face (vertex ids, loop ids)."""
    import numpy as np
    for _, loops in faces:
        a, b, c = (uv[k] * size for k in loops)
        lo = np.floor(np.minimum(np.minimum(a, b), c)).astype(int)
        hi = np.ceil(np.maximum(np.maximum(a, b), c)).astype(int)
        lo = np.clip(lo, 0, size - 1)
        hi = np.clip(hi, 0, size)
        if hi[0] <= lo[0] or hi[1] <= lo[1]:
            continue
        xs, ys = np.meshgrid(np.arange(lo[0], hi[0]) + 0.5, np.arange(lo[1], hi[1]) + 0.5)
        d = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
        if abs(d) < 1e-12:
            continue
        w0 = ((b[1] - c[1]) * (xs - c[0]) + (c[0] - b[0]) * (ys - c[1])) / d
        w1 = ((c[1] - a[1]) * (xs - c[0]) + (a[0] - c[0]) * (ys - c[1])) / d
        inside = (w0 >= -1e-6) & (w1 >= -1e-6) & (w0 + w1 <= 1 + 1e-6)
        rows = ys[inside].astype(int)
        cols = xs[inside].astype(int)
        bits[rows, cols] |= mask


def bake_garment_cover(body, garments, out_png, size=1024, reach=0.03, rig=None, log=print):
    """Where each garment covers the body, as bits in a texture on the body's UVs (docs/design/clothes-draw.md "Skin under
    clothes"): a body face is covered when a ray out of each of its corners along the skin's normal meets the garment
    within `reach` metres (or the garment lies just under the skin). Garment i that covers anything gets bit i (up to 32,
    in RGBA, 8 bits a channel); the game hides the skin where a worn garment's bit is set, so nothing pokes through.
    Writes the PNG and returns {garment name: bit}."""
    import numpy as np
    from mathutils.bvhtree import BVHTree
    deps = bpy.context.evaluated_depsgraph_get()
    to_world = body.matrix_world
    normal_world = to_world.to_3x3().inverted().transposed()
    verts = [to_world @ v.co for v in body.data.vertices]
    normals = [(normal_world @ v.normal).normalized() for v in body.data.vertices]
    body.data.calc_loop_triangles()
    uv_layer = body.data.uv_layers.active
    if uv_layer is None:
        log("garment cover: the body has no UVs")
        return {}
    uv = np.empty(len(body.data.loops) * 2, dtype=np.float64)
    uv_layer.data.foreach_get("uv", uv)
    uv = uv.reshape(-1, 2)
    tris = [(t.vertices[:], t.loops[:]) for t in body.data.loop_triangles]
    bits = np.zeros((size, size), dtype=np.uint32)
    out = {}
    zs = [p.z for p in verts]
    head_line = min(zs) + 0.80 * (max(zs) - min(zs))  # about the chin of a standing body
    for g in garments:
        if len(out) >= 31:
            log("garment cover: more than 31 covering garments;", g.name, "left out")
            break
        # Headwear never hides skin: a brim or visor is far from the face, and the scalp is under the hair anyway.
        lowest = min((g.matrix_world @ v.co).z for v in g.data.vertices) if g.data.vertices else 0.0
        if lowest > head_line:
            log("garment cover:", g.name, "is headwear: no cover")
            continue
        tree = BVHTree.FromObject(g, deps)  # in the garment's own space
        inv = g.matrix_world.inverted()
        inv3 = inv.to_3x3()
        covered = np.zeros(len(verts), dtype=bool)
        for i, (p, n) in enumerate(zip(verts, normals)):
            pl = inv @ p
            nl = inv3 @ n
            k = nl.length  # metres to the garment's units
            nl.normalize()
            hit = tree.ray_cast(pl + nl * (0.004 * k), nl, reach * k)
            if hit[0] is None:
                hit = tree.ray_cast(pl + nl * (0.01 * k), -nl, 0.012 * k)  # the garment sunk just under the skin
            covered[i] = hit[0] is not None
        covered = _erode_cover(verts, covered)
        faces = [(v, l) for v, l in tris if covered[v[0]] and covered[v[1]] and covered[v[2]]]
        if len(faces) < 40:
            continue  # a hat, gloves or a strap: nothing worth hiding
        bit = len(out)
        _cover_raster(bits, uv, size, faces, np.uint32(1 << bit))
        out[g.get("rim3d_garment") or g.name] = bit
        log("garment cover:", g.name, "bit", bit, len(faces), "body faces")
    # Bit 31: the crotch (genitals, pubic area, inner thighs). They stand out past trousers fitted to the basemesh, so the
    # game hides them whenever trousers are drawn (MakeHumanCrotchCoverBit), whatever the rays found.
    if rig is not None and "pelvis" in rig.data.bones:
        local = [v.co for v in body.data.vertices]
        cx, crotch = _crotch(rig, local)
        if crotch is not None:
            gi = body.vertex_groups["genital"].index if "genital" in body.vertex_groups else -1
            near = np.zeros(len(local), dtype=bool)
            for v in body.data.vertices:
                p = v.co
                in_box = abs(p.x - cx) < 0.075 and crotch - 0.10 < p.z < crotch + 0.09
                in_group = gi >= 0 and any(e.group == gi and e.weight > 0.02 for e in v.groups)
                near[v.index] = in_box or in_group
            faces = [(v, l) for v, l in tris if near[v[0]] or near[v[1]] or near[v[2]]]
            _cover_raster(bits, uv, size, faces, np.uint32(1 << 31))
            log("garment cover: crotch bit 31,", len(faces), "body faces")
    # RGBA8: bits 0-7 red, 8-15 green, 16-23 blue, 24-31 alpha. Blender images start at the bottom row, like UV v.
    rgba = np.stack([(bits >> s) & 0xFF for s in (0, 8, 16, 24)], axis=-1).astype(np.float32) / 255.0
    image = bpy.data.images.new("GarmentCover", size, size, alpha=True, float_buffer=False)
    image.colorspace_settings.name = "Non-Color"
    image.alpha_mode = "STRAIGHT"
    image.pixels.foreach_set(rgba.ravel())
    os.makedirs(os.path.dirname(out_png), exist_ok=True)
    image.filepath_raw = out_png
    image.file_format = "PNG"
    image.save()
    with open(os.path.splitext(out_png)[0] + ".json", "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1)
    log("garment cover:", len(out), "garments in", out_png)
    return out
