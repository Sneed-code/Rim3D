"""Bake the Fab "Snake Cobra animated (Base Model)" rig (DaVe inc., CC BY) into one in-place glTF clip.

The .blend drives its 37-bone armature with Track-To constraints aimed at 32 helper cubes that ride a curve
(Follow Path), and the armature itself is parented to one of the cubes, so the snake travels along the path.
This bakes the visual bone poses over the scene's frame range into one action, "Slither", drops the helpers,
the curve, the camera and the light, un-parents the armature, holds the body on the first frame's heading and
centre (the path turns, so it swung about 90 degrees around its tail), puts its middle on the origin, and writes
a GLB for the AnimalArt commandlet (assets/animal_art.json, model "Cobra").

  blender -b --factory-startup --python tools/animals/cobra_bake.py -- <in.blend> <out.glb>

Art step `cobra-bake` (tools/art_steps.json) runs it with the paths below when no paths are given.
"""
import math
import os
import sys

import bpy
from mathutils import Matrix, Vector

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if len(args) == 0:
    args = [os.path.join(ROOT, "assets/raw/fab/snake-cobra/source/Suake_01_BASE4ALL_01a.blend"),
            os.path.join(ROOT, "assets/raw/fab/snake-cobra/source/Cobra.glb")]
if len(args) != 2:
    raise SystemExit("usage: blender -b --python cobra_bake.py -- <in.blend> <out.glb>")
src, out = args
if not os.path.exists(src):
    raise SystemExit("missing %s: download Snake Cobra animated (Base Model) from Fab and unzip it there" % src)
bpy.ops.wm.open_mainfile(filepath=src)

sc = bpy.context.scene
arm = bpy.data.objects["Armature.001"]
mesh = bpy.data.objects["COBRA"]
print("scene frames", sc.frame_start, sc.frame_end, "fps", sc.render.fps)

# Bake the visual pose of every bone over the timeline into one action.
for o in bpy.context.view_layer.objects:
    o.select_set(False)
bpy.context.view_layer.objects.active = arm
arm.select_set(True)
bpy.ops.object.mode_set(mode="POSE")
bpy.ops.nla.bake(frame_start=sc.frame_start, frame_end=sc.frame_end, only_selected=False, visual_keying=True,
                 clear_constraints=True, clear_parents=False, use_current_action=False, bake_types={"POSE"})
bpy.ops.object.mode_set(mode="OBJECT")
action = arm.animation_data.action
action.name = "Slither"

# Keep only the snake and its rig, standing still.
if arm.parent:
    mw = arm.matrix_world.copy()
    arm.parent = None
    arm.matrix_world = mw
for pb in arm.pose.bones:
    pb.custom_shape = None
for o in list(bpy.data.objects):
    if o not in (arm, mesh):
        bpy.data.objects.remove(o, do_unlink=True)
for a in list(bpy.data.actions):
    if a != action:
        bpy.data.actions.remove(a)

# Hold it still: each frame, turn and move the root bone so the tail-to-head line keeps the first frame's heading,
# and put the body's middle (first frame) on the origin so the game draws it over its actor.
B = arm.pose.bones
root = next(b for b in B if b.parent is None)
chain = [b for b in B if b.name.startswith("Bone")]


def cobraChainPose():
    pts = [arm.matrix_world @ b.head for b in chain]
    c = sum(pts, Vector()) / len(pts)
    tail = arm.matrix_world @ B["Bone.030"].head
    head = arm.matrix_world @ B["Bone.031"].head
    return c, math.atan2(head.y - tail.y, head.x - tail.x)


frames = range(sc.frame_start, sc.frame_end + 1)
poses = {}
for f in frames:
    sc.frame_set(f)
    poses[f] = (cobraChainPose(), (arm.matrix_world @ root.matrix).copy())
(c0, yaw0), _ = poses[sc.frame_start]
target = Vector((0.0, 0.0, c0.z))
for f in frames:
    (c, yaw), m = poses[f]
    fix = Matrix.Translation(target) @ Matrix.Rotation(yaw0 - yaw, 4, "Z") @ Matrix.Translation(-c)
    sc.frame_set(f)
    root.matrix = arm.matrix_world.inverted() @ (fix @ m)
    bpy.context.view_layer.update()
    root.keyframe_insert("location", frame=f)
    root.keyframe_insert("rotation_quaternion" if root.rotation_mode == "QUATERNION" else "rotation_euler", frame=f)
print("baked action", action.name, tuple(action.frame_range))
os.makedirs(os.path.dirname(out), exist_ok=True)
bpy.ops.export_scene.gltf(filepath=out, export_format="GLB", export_animations=True)
print("EXPORTED", out)
