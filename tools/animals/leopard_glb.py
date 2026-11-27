"""Turn Animated3D's Leopard Hybrid_A2 FBX (Fab, our panther) into a GLB the AnimalArt commandlet imports well.

Unreal's FBX import gave every one of its eight motions the whole file's timeline (all 9.7 s, the jump's length), so
walk and run played their loop and then stood still for eight seconds; it also wired the colour map into emission.
Blender reads each motion with its own frames, so this writes them as separate glTF animations (each sampled over its
own range at the file's 60 fps), with no emission, and the textures packed in.

  blender -b --factory-startup --python tools/animals/leopard_glb.py -- [<in.fbx> <out.glb>]

Art step `leopard-glb` (tools/art_steps.json) runs it with the default paths below.
"""
import os
import sys

import bpy

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if not args:
    args = [os.path.join(ROOT, "assets/raw/fab/leopard-hybrid-a2/FbxBlender/Leopard_Hybrid_A2.Fbx"),
            os.path.join(ROOT, "assets/raw/fab/leopard-hybrid-a2/Leopard.glb")]
if len(args) != 2:
    raise SystemExit("usage: blender -b --python leopard_glb.py -- <in.fbx> <out.glb>")
src, out = args
if not os.path.exists(src):
    raise SystemExit("missing %s: download Leopard Hybrid_A2 (source.zip) from Fab and unzip FbxBlender.zip there" % src)

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=src, use_anim=True)
scene = bpy.context.scene
print("scene fps", scene.render.fps, scene.render.fps_base)

# Each motion keeps its own frames (no manual range stretching it to the file's timeline).
arm = next(o for o in scene.objects if o.type == "ARMATURE")
for action in bpy.data.actions:
    action.use_frame_range = False
    print("action", action.name, tuple(action.frame_range))
# Every action exported on its own: park them in NLA tracks so the glTF exporter sees them all.
if arm.animation_data is None:
    arm.animation_data_create()
arm.animation_data.action = None
for action in bpy.data.actions:
    track = arm.animation_data.nla_tracks.new()
    track.name = action.name.split("|")[-1]
    strip = track.strips.new(track.name, int(action.frame_range[0]), action)
    strip.name = track.name
    track.mute = True

# No glow: the colour map was also wired into emission.
for mat in bpy.data.materials:
    if not mat.use_nodes:
        continue
    for node in mat.node_tree.nodes:
        if node.type == "BSDF_PRINCIPLED":
            for name in ("Emission Color", "Emission"):
                sock = node.inputs.get(name)
                if sock is not None:
                    for link in list(sock.links):
                        mat.node_tree.links.remove(link)
                    sock.default_value = (0.0, 0.0, 0.0, 1.0)
            strength = node.inputs.get("Emission Strength")
            if strength is not None:
                strength.default_value = 0.0

os.makedirs(os.path.dirname(out), exist_ok=True)
bpy.ops.export_scene.gltf(filepath=out, export_format="GLB", export_animations=True, export_animation_mode="NLA_TRACKS",
                          export_force_sampling=True, export_frame_range=False, export_image_format="AUTO")
print("EXPORTED", out)
