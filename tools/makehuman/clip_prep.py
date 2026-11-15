# Prepares a mocap clip for the game (docs/design/makehuman.md, "Animation clips"): markerless mocap (FlowHMR and the
# like) leaves the body wherever it stood in front of the camera, metres from the origin. This moves the hips so the
# clip's average ground position is at the origin (height and all the motion kept), and writes the FBX the MakeHumanArt
# commandlet imports.
#   blender.exe -b -noaudio --python tools/makehuman/clip_prep.py -- <in.fbx> <out.fbx>
import sys

import bpy
from mathutils import Vector

src, out = sys.argv[sys.argv.index("--") + 1:][:2]
for obj in list(bpy.data.objects):
    bpy.data.objects.remove(obj, do_unlink=True)
bpy.ops.import_scene.fbx(filepath=src)
arm = next(o for o in bpy.data.objects if o.type == "ARMATURE")
hips = next(b for b in arm.pose.bones if b.parent is None)
scene = bpy.context.scene
action = arm.animation_data.action
first, last = (int(round(x)) for x in action.frame_range)
# The hips' average ground position over the clip, in the armature's space.
total = Vector()
for f in range(first, last + 1):
    scene.frame_set(f)
    total += hips.head
mean = total / (last - first + 1)
offset = Vector((mean.x, mean.y, 0.0))
# Into the hips' own (rest) space, where its location keys live.
local = hips.bone.matrix_local.to_3x3().inverted() @ offset
fcurves = [fc for fc in action.fcurves if fc.data_path == f'pose.bones["{hips.name}"].location'] if hasattr(action, "fcurves") else []
if not fcurves:
    # Layered actions (Blender 4.4+): the curves sit in the action's channelbag.
    for layer in action.layers:
        for strip in layer.strips:
            for bag in strip.channelbags:
                fcurves += [fc for fc in bag.fcurves if fc.data_path == f'pose.bones["{hips.name}"].location']
for fc in fcurves:
    for key in fc.keyframe_points:
        key.co.y -= local[fc.array_index]
        key.handle_left.y -= local[fc.array_index]
        key.handle_right.y -= local[fc.array_index]
    fc.update()
# Facing: the mocap may face another way than the skeleton's rest pose (towards the camera): the whole motion is turned
# about the vertical so its average facing (from the right hip to the left) is the rest pose's.
from mathutils import Matrix, Quaternion
import math
left = next((b for b in arm.pose.bones if b.name.endswith("LeftUpLeg")), None)
right = next((b for b in arm.pose.bones if b.name.endswith("RightUpLeg")), None)
yaw = 0.0
if left and right:
    rest = left.bone.head_local - right.bone.head_local
    acc = Vector()
    for f in range(first, last + 1):
        scene.frame_set(f)
        acc += left.head - right.head
    a_rest = math.atan2(rest.y, rest.x)
    a_anim = math.atan2(acc.y, acc.x)
    yaw = a_rest - a_anim
    yaw = math.atan2(math.sin(yaw), math.cos(yaw))
if abs(yaw) > math.radians(5):
    turn = Quaternion((0.0, 0.0, 1.0), yaw)
    rest3 = hips.bone.matrix_local.to_3x3()
    rest_q = rest3.to_quaternion()
    head = hips.bone.head_local
    # Sample every frame, turn it, key it back (rotation as quaternions).
    frames = []
    for f in range(first, last + 1):
        scene.frame_set(f)
        frames.append((f, hips.location.copy(), hips.rotation_quaternion.copy() if hips.rotation_mode == "QUATERNION" else hips.rotation_euler.to_quaternion()))
    hips.rotation_mode = "QUATERNION"
    for f, loc, rot in frames:
        world_loc = head + rest3 @ loc
        new_loc = rest3.inverted() @ (turn @ world_loc - head)
        new_rot = rest_q.inverted() @ turn @ rest_q @ rot
        hips.location = new_loc
        hips.rotation_quaternion = new_rot
        hips.keyframe_insert("location", frame=f)
        hips.keyframe_insert("rotation_quaternion", frame=f)
print("[clip_prep] turned by", round(math.degrees(yaw), 1), "degrees to face as the rest pose does", flush=True)
scene.frame_set(first)
print("[clip_prep] hips moved by", tuple(round(v, 3) for v in offset), "on", len(fcurves), "curves; frames", first, "to", last, flush=True)
bpy.ops.object.select_all(action="SELECT")
bpy.ops.export_scene.fbx(filepath=out, use_selection=True, object_types={"ARMATURE", "MESH"}, add_leaf_bones=False, bake_anim=True,
                         bake_anim_use_all_actions=False, bake_anim_use_nla_strips=False, mesh_smooth_type="FACE")
print("[clip_prep] wrote", out, flush=True)
