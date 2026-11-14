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
