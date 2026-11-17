# Prepares a folder of Mixamo downloads for the game in one Blender run (docs/design/makehuman.md, "Animation clips"):
# every animation FBX (packs/<pack>/*.fbx and singles/*.fbx, on the Y Bot skeleton) is loaded, its hips moved so the
# clip starts over the origin (the motion kept: a walk still walks), moved onto the rig of the clips that already work
# (PetitePale's, Mixamo bone names) and written out as that armature alone; the rig with its mesh goes out once as
# Rig.fbx, the shared skeleton the clips import onto. Also writes mixamo.json: name, category and file of each clip.
#   blender.exe -b -noaudio --python tools/makehuman/mixamo_prep.py -- <mocap/mixamo> <game/Saved/Mocap/Mixamo> [rig.fbx]
import json
import os
import re
import sys

import bpy
from mathutils import Vector

src, dst = (os.path.abspath(p) for p in sys.argv[sys.argv.index("--") + 1:][:2])
os.makedirs(dst, exist_ok=True)

# Singles grouped by what they are; the packs are their own groups.
SINGLE_GROUPS = [
    ("Injured", ("agony", "writhing", "injured", "stumble", "dizzy", "stunned", "electrocuted", "hit ", "hit to", "reaction", "react", "tripping")),
    ("Falls", ("falling", "thrown", "getting up")),
    ("Crawling", ("crawl",)),
    ("Fear", ("scared", "terrified", "ducking", "hiding", "cower")),
    ("Cover", ("cover",)),
    ("Rolls", ("roll",)),
    ("Fighting", ("punch", "jab")),
    ("Firearms", ("reload", "gunplay", "focus")),
    ("Interact", ("pick", "rummaging", "searching", "pull heavy", "throw object")),
]


def clear():
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    for action in list(bpy.data.actions):
        bpy.data.actions.remove(action)


def tidy(name):
    words = re.sub(r"[^A-Za-z0-9 ]+", " ", name).split()
    return "".join(w[:1].upper() + w[1:] for w in words)


def anchor(arm):
    """The hips' first-frame ground position to the origin, over the whole clip."""
    if not arm.animation_data or not arm.animation_data.action:
        return 0
    action = arm.animation_data.action
    hips = next(b for b in arm.pose.bones if b.parent is None)
    first = int(round(action.frame_range[0]))
    bpy.context.scene.frame_set(first)
    start = arm.matrix_world @ hips.head
    offset_world = Vector((start.x, start.y, 0.0))
    offset = (arm.matrix_world.to_3x3().inverted() @ offset_world)
    local = hips.bone.matrix_local.to_3x3().inverted() @ offset
    curves = []
    if hasattr(action, "fcurves"):
        curves = [fc for fc in action.fcurves if fc.data_path == f'pose.bones["{hips.name}"].location']
    if not curves:
        for layer in action.layers:
            for strip in layer.strips:
                for bag in strip.channelbags:
                    curves += [fc for fc in bag.fcurves if fc.data_path == f'pose.bones["{hips.name}"].location']
    for fc in curves:
        for key in fc.keyframe_points:
            key.co.y -= local[fc.array_index]
            key.handle_left.y -= local[fc.array_index]
            key.handle_right.y -= local[fc.array_index]
        fc.update()
    scene = bpy.context.scene
    scene.frame_start, scene.frame_end = (int(round(x)) for x in action.frame_range)
    return len(curves)


def action_curves(action):
    if hasattr(action, "fcurves") and len(action.fcurves):
        return list(action.fcurves)
    out = []
    for layer in getattr(action, "layers", []):
        for strip in layer.strips:
            for bag in strip.channelbags:
                out += list(bag.fcurves)
    return out


def bake_object_transform(arm):
    """Mixamo's armature object is turned 90 degrees about X and scaled 0.01 (centimetres). Unreal reads that object
    transform one way for the skeleton and another for an animation, so the clips came in lying on the ground: it is
    applied to the bones (and the skinned meshes), and the bones' location keys scaled to match, leaving the armature
    object identity, as the clips that import upright have it. World positions are unchanged."""
    scale = arm.scale.x
    bpy.ops.object.select_all(action="SELECT")
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)
    if arm.animation_data and arm.animation_data.action and abs(scale - 1.0) > 1e-6:
        for fc in action_curves(arm.animation_data.action):
            if fc.data_path.startswith("pose.bones") and fc.data_path.endswith(".location"):
                for key in fc.keyframe_points:
                    key.co.y *= scale
                    key.handle_left.y *= scale
                    key.handle_right.y *= scale
                fc.update()


RIG = "MixamoRig"


def export(path, anim):
    # Not "Armature": Unreal drops a top node of that name and turns only the top bone's rest position into
    # centimetres, not an animation's keys for it, so the retargeter put every body's hips on the ground (sunk to the
    # waist). Under another name the armature is the skeleton's root bone at the origin, as the clips that import well
    # have it.
    for obj in bpy.data.objects:
        if obj.type == "ARMATURE":
            obj.name = RIG
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.export_scene.fbx(filepath=path, use_selection=True, object_types={"ARMATURE", "MESH"}, add_leaf_bones=False,
                             bake_anim=anim, bake_anim_use_all_actions=False, bake_anim_use_nla_strips=False, mesh_smooth_type="FACE")


clips = []
# The skeleton every clip goes onto: the rig of the clips that already import and retarget well (sneed's PetitePale,
# Mixamo bone names, from game/Saved/Mocap/Skip/Skip.fbx). Taken onto the Y Bot itself, Unreal's retargeter bent the
# Mixamo spines about twice as far as the clip does (bodies folded at the waist); moved onto this rig here first, by
# each bone's turn from its rest pose, the clips reach Unreal like the ones that work.
repo = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
target_fbx = os.path.abspath(sys.argv[sys.argv.index("--") + 3]) if len(sys.argv) > sys.argv.index("--") + 3 else os.path.join(repo, "game", "Saved", "Mocap", "Skip", "Skip.fbx")
clear()
bpy.ops.import_scene.fbx(filepath=target_fbx)
target = next(o for o in bpy.data.objects if o.type == "ARMATURE")
if target.animation_data:
    target.animation_data.action = None
for action in list(bpy.data.actions):
    bpy.data.actions.remove(action)
bake_object_transform(target)
export(os.path.join(dst, "Rig.fbx"), False)   # the shared skeleton, with its mesh
target_blend = os.path.join(dst, "Rig.blend")
for obj in [o for o in bpy.data.objects if o.type == "MESH"]:
    bpy.data.objects.remove(obj, do_unlink=True)   # the clips go out as the armature alone
bpy.ops.wm.save_as_mainfile(filepath=target_blend, copy=True)


def bone_order(arm):
    out = []

    def walk(b):
        out.append(b)
        for c in b.children:
            walk(c)
    for b in arm.data.bones:
        if b.parent is None:
            walk(b)
    return out


def transfer(source, target):
    """The source clip onto the target rig: every bone of the same name turned from its rest pose as the source's is
    (in the world), the hips moved as the source's, scaled by the two rigs' hip heights; other bones keep their rest
    pose. The rests differ (the Y Bot stands in a T pose, PetitePale in an A pose), so each target bone's rest is first
    turned to point where the source bone's rest points (as Unreal's retarget pose does), and the source's turns are
    applied to that. Keyed on every frame of the clip."""
    action = source.animation_data.action
    first, last = (int(round(x)) for x in action.frame_range)
    scene = bpy.context.scene
    scene.frame_start, scene.frame_end = first, last
    s_bones = {b.name: b for b in source.data.bones}
    order = bone_order(target)
    s_hips = next(b for b in source.data.bones if b.parent is None)
    t_hips = target.data.bones.get(s_hips.name) or next(b for b in target.data.bones if b.name.endswith("Hips"))
    scale = t_hips.head_local.z / max(s_hips.head_local.z, 1e-4)
    rel = {b.name: (b.parent.matrix_local.inverted() @ b.matrix_local) if b.parent else b.matrix_local.copy() for b in order}
    aligned = {}
    for b in order:
        s = s_bones.get(b.name)
        if s is None:
            continue
        d_t = (b.tail_local - b.head_local).normalized()
        d_s = (s.tail_local - s.head_local).normalized()
        aligned[b.name] = d_t.rotation_difference(d_s).to_matrix() @ b.matrix_local.to_3x3()
    target.animation_data_create()
    target.animation_data.action = bpy.data.actions.new(action.name + "_rig")
    for pb in target.pose.bones:
        pb.rotation_mode = "QUATERNION"
    for f in range(first, last + 1):
        scene.frame_set(f)
        s_pose = {pb.name: pb.matrix.copy() for pb in source.pose.bones}
        done = {}
        for b in order:
            parent_m = done[b.parent.name] if b.parent else None
            base = (parent_m @ rel[b.name]) if parent_m is not None else rel[b.name].copy()
            s = s_bones.get(b.name)
            if s is not None:
                turn = s_pose[b.name].to_3x3() @ s.matrix_local.to_3x3().inverted()
                rot = (turn @ aligned[b.name]).to_4x4()
                if b == t_hips:
                    head = s_pose[s.name].to_translation() * scale
                else:
                    head = base.to_translation()
                m = rot
                m.translation = head
            else:
                m = base
            done[b.name] = m
            basis = (base.inverted() @ m) if parent_m is not None else (rel[b.name].inverted() @ m)
            pb = target.pose.bones[b.name]
            pb.matrix_basis = basis
            pb.keyframe_insert("location", frame=f)
            pb.keyframe_insert("rotation_quaternion", frame=f)


for root, _, files in os.walk(src):
    rel_dir = os.path.relpath(root, src)
    pack = os.path.basename(root) if rel_dir.startswith("packs") and rel_dir != "packs" else None
    for f in sorted(files):
        if not f.lower().endswith(".fbx"):
            continue
        path = os.path.join(root, f)
        base = os.path.splitext(f)[0]
        if base.lower() == "y bot":
            continue
        if pack:
            group = re.sub(r"^Pro ", "", pack).replace(" Pack", "").replace("_", " / ")
        else:
            low = base.lower()
            group = next((g for g, keys in SINGLE_GROUPS if any(k in low for k in keys)), "Other")
        name = tidy((pack.replace(" Pack", "") + " " if pack else "") + base)
        clear()
        bpy.ops.import_scene.fbx(filepath=path)
        source = next((o for o in bpy.data.objects if o.type == "ARMATURE"), None)
        if source is None or not source.animation_data or not source.animation_data.action:
            continue
        anchor(source)
        bake_object_transform(source)
        for obj in [o for o in bpy.data.objects if o.type == "MESH"]:
            bpy.data.objects.remove(obj, do_unlink=True)
        source.name = "MixamoSource"
        with bpy.data.libraries.load(target_blend) as (data_from, data_to):
            data_to.objects = list(data_from.objects)
        rig = None
        for obj in data_to.objects:
            if obj is not None and obj.type == "ARMATURE":
                bpy.context.scene.collection.objects.link(obj)
                rig = obj
        transfer(source, rig)
        bpy.data.objects.remove(source, do_unlink=True)
        out = os.path.join(dst, name + ".fbx")
        export(out, True)
        clips.append({"name": name, "category": group, "fbx": out.replace("\\", "/")})
        print("[mixamo_prep]", group, name, flush=True)
with open(os.path.join(dst, "mixamo.json"), "w", encoding="utf-8") as fh:
    json.dump({"skeleton": os.path.join(dst, "Rig.fbx").replace("\\", "/"), "clips": clips}, fh, indent=1)
print("[mixamo_prep]", len(clips), "clips onto", target_fbx, flush=True)
