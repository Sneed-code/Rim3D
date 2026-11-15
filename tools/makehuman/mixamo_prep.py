# Prepares a folder of Mixamo downloads for the game in one Blender run (docs/design/makehuman.md, "Animation clips"):
# every animation FBX (packs/<pack>/*.fbx and singles/*.fbx, on the Y Bot skeleton, without skin) is loaded, its hips
# moved so the clip starts over the origin (the motion kept: a walk still walks), and written out again; the Y Bot
# character (mesh and skeleton in its T pose) goes out the same way, as the shared skeleton the clips import onto. Also
# writes mixamo.json: name, category and file of each clip, for assets/animations.json.
#   blender.exe -b -noaudio --python tools/makehuman/mixamo_prep.py -- <mocap/mixamo> <game/Saved/Mocap/Mixamo>
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
# The Y Bot first: the shared skeleton, and its mesh (saved as a .blend) goes into every clip's file.
ybot = next(os.path.join(r, f) for r, _, fs in os.walk(src) for f in fs if f.lower() == "y bot.fbx")
clear()
bpy.ops.import_scene.fbx(filepath=ybot)
bake_object_transform(next(o for o in bpy.data.objects if o.type == "ARMATURE"))
export(os.path.join(dst, "YBot.fbx"), False)
ybot_blend = os.path.join(dst, "YBot.blend")
bpy.ops.wm.save_as_mainfile(filepath=ybot_blend, copy=True)
ybot_done = True
for root, _, files in os.walk(src):
    rel = os.path.relpath(root, src)
    pack = os.path.basename(root) if rel.startswith("packs") and rel != "packs" else None
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
        arm = next((o for o in bpy.data.objects if o.type == "ARMATURE"), None)
        if arm is None:
            continue
        anchor(arm)
        bake_object_transform(arm)
        # With the Y Bot's mesh skinned to it, as the Y Bot itself goes out.
        if not any(o.type == "MESH" for o in bpy.data.objects):
            with bpy.data.libraries.load(ybot_blend) as (data_from, data_to):
                data_to.objects = [n for n in data_from.objects if n not in ("Armature", RIG)]
            for obj in data_to.objects:
                if obj is None or obj.type != "MESH":
                    continue
                bpy.context.scene.collection.objects.link(obj)
                obj.parent = arm
                for m in obj.modifiers:
                    if m.type == "ARMATURE":
                        m.object = arm
        out = os.path.join(dst, name + ".fbx")
        export(out, True)
        clips.append({"name": name, "category": group, "fbx": out.replace("\\", "/")})
        print("[mixamo_prep]", group, name, flush=True)
with open(os.path.join(dst, "mixamo.json"), "w", encoding="utf-8") as fh:
    json.dump({"skeleton": os.path.join(dst, "YBot.fbx").replace("\\", "/"), "clips": clips}, fh, indent=1)
print("[mixamo_prep]", len(clips), "clips, Y Bot", ybot_done, flush=True)
