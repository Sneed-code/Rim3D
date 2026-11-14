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
    for obj in meshes:
        for index, mat in enumerate([slot.material for slot in obj.material_slots]):
            if not mat:
                continue
            if mat.name in [m["blender"] for m in manifest.values()]:
                continue
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


def lower_pubic_hair(obj, body, top_share=0.6, above_crotch=0.025, log=print):
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
    middle = [p.z for p in body_pts if abs(p.x - centre_x) < 0.012 and bottom - 0.25 < p.z < top]
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
        nx = centre_x + (x - centre_x) * (1.0 - 0.15 * t)
        new_skin = skin_y(nx, nz)
        ny = y if old_skin is None or new_skin is None else new_skin + (y - old_skin)
        v.co = (nx, ny, nz)
    obj.data.update()
    log("pubic hair lowered: top", round(top - bottom, 3), "->", round((top - bottom) * top_share, 3), "above its lowest point")
