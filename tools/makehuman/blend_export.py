# Exports a finished character from a .blend (an armature and the meshes it deforms) as FBX for the MakeHumanArt
# commandlet, with materials.json beside it.
#   blender.exe -b "<file.blend>" -noaudio --python blend_export.py -- <out.fbx> [armature name]
# The meshes are every mesh with an armature modifier on that armature (hidden ones included only if visible in renders).
import os
import sys

import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mh_common import prepare_for_unreal, write_materials  # noqa: E402

args = sys.argv[sys.argv.index("--") + 1:]
out = os.path.abspath(args[0])
rig = bpy.data.objects[args[1]] if len(args) > 1 else next(o for o in bpy.data.objects if o.type == "ARMATURE")

meshes = []
for o in bpy.data.objects:
    if o.type != "MESH" or o.hide_render:
        continue
    if any(m.type == "ARMATURE" and m.object == rig for m in o.modifiers):
        meshes.append(o)
for o in [rig] + meshes:
    if o.name not in bpy.context.view_layer.objects:
        bpy.context.scene.collection.objects.link(o)
    o.hide_set(False)
    o.hide_viewport = False
for o in meshes:
    if o.data.shape_keys:
        o.shape_key_add(name="__mix", from_mix=True)
        mix = o.data.shape_keys.key_blocks["__mix"]
        for i, v in enumerate(o.data.vertices):
            v.co = mix.data[i].co
        o.shape_key_clear()
    for m in list(o.modifiers):
        if m.type != "ARMATURE":
            o.modifiers.remove(m)
# The body: the mesh with the most vertices.
body = max(meshes, key=lambda o: len(o.data.vertices))
print("[blend_export] rig", rig.name, len(rig.data.bones), "meshes", [(o.name, len(o.data.vertices)) for o in meshes], "body", body.name)

os.makedirs(os.path.dirname(out), exist_ok=True)
prepare_for_unreal(rig, meshes)
write_materials(meshes, os.path.dirname(out), skin_meshes=[body])
bpy.ops.object.select_all(action="DESELECT")
for o in [rig] + meshes:
    o.select_set(True)
bpy.context.view_layer.objects.active = rig
bpy.ops.export_scene.fbx(filepath=out, use_selection=True, object_types={"ARMATURE", "MESH"}, apply_scale_options="FBX_SCALE_NONE",
                         add_leaf_bones=False, bake_anim=False, mesh_smooth_type="FACE", use_armature_deform_only=True,
                         path_mode="AUTO", embed_textures=False, axis_forward="-Y", axis_up="Z")
print("[blend_export] done", out)
