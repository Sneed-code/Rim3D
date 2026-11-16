"""Blender script: turn the downloaded gun packs into clean game FBX files for the GunArt commandlet.

Run (Blender 5.x, no window):
  blender -b --factory-startup -P tools/gun_prep.py -- [AssaultRifle] [Autopistol] [PumpShotgun]
from the repo root. With no names it does all of them.

Each pack comes as the creator made it: a showcase scene (the pistol pack has several copies of the pistol and
all its attachments), a rig (the shotgun), loose shells. This keeps one gun, joins its pieces into the named parts
assets/guns.json picks ('match'), and turns and moves it into the gun frame: +X out of the muzzle, +Z up, the firing
hand's palm on the grip at the origin, in Blender metres (1 = 100 cm in Unreal). Blender +Y comes out as Unreal -Y,
so the gun's right side (Unreal +Y) is Blender -Y. Writes assets/raw/guns/<folder>/<folder>_Game.fbx.
"""
import math
import os
import sys

import bpy
import mathutils

RAW = os.path.join(os.getcwd(), "assets", "raw", "guns")


def in_box(obj, lo, hi):
    pts = [obj.matrix_world @ mathutils.Vector(c) for c in obj.bound_box]
    return all(lo[i] - 1e-4 <= min(p[i] for p in pts) and max(p[i] for p in pts) <= hi[i] + 1e-4 for i in range(3))


def cm(v):
    return tuple(x / 100.0 for x in v)


# For each pack: the source FBX, which objects make which part, how to turn it (degrees about Blender Z), where
# the grip is in the source (cm, Blender axes) and an optional scale about the grip.
PACKS = {
    "AssaultRifle": {
        "fbx": "AssaultRifle/AK47Subdiv.fbx",
        "parts": {"RifleBody": lambda o: True},
        "turn": 90.0,  # the muzzle points to Blender -Y
        "grip": (0.0, 9.0, -4.0),
    },
    "Autopistol": {
        "fbx": "Autopistol/source/CustomPistol9mmPack.fbx",
        # The assembled pistol without attachments (the pack also has a reload copy, an exploded copy, sights,
        # lights, a laser and rails).
        "parts": {
            "PistolBody": lambda o: o.name.startswith("CustomPistol9mm") and "Reload" not in o.name
            and in_box(o, cm((-1.6, -27.3, 0.9)), cm((1.6, -8.0, 13.3))),
            "PistolMagazine": lambda o: o.name.startswith("Magazine9mm")
            and in_box(o, cm((-1.6, -27.0, 0.0)), cm((1.6, -18.0, 11.0))),
        },
        "turn": -90.0,  # the muzzle points to Blender +Y
        "grip": (0.0, -22.5, 6.0),
    },
    "PumpShotgun": {
        "fbx": "PumpShotgun/source/Pump action shotgun/Model12_shotgun.fbx",
        "parts": {
            # Loose shells lie beside the gun (shell1..6) and three sit hidden in the tube (shellshot*).
            "ShotgunBody": lambda o: not o.name.startswith("shell") and o.name != "corncob",
            "ShotgunPump": lambda o: o.name == "corncob",
        },
        "turn": 0.0,
        "grip": (-9.0, 0.0, 0.5),
        # The model is 152 cm long; a Model 12 with a 30-inch barrel is about 126 cm.
        "scale": 0.83,
    },
}


def prep(folder):
    pack = PACKS[folder]
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=os.path.join(RAW, pack["fbx"]))
    meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]
    # Unparent (keeping where they are), drop everything that is not a mesh (rig, camera, lights).
    for o in meshes:
        world = o.matrix_world.copy()
        o.parent = None
        o.matrix_world = world
    for o in list(bpy.context.scene.objects):
        if o.type != "MESH":
            bpy.data.objects.remove(o, do_unlink=True)

    fix = (mathutils.Matrix.Scale(pack.get("scale", 1.0), 4)
           @ mathutils.Matrix.Rotation(math.radians(pack["turn"]), 4, "Z")
           @ mathutils.Matrix.Translation(-mathutils.Vector(cm(pack["grip"]))))
    kept = []
    names = [o.name for o in meshes]  # joining removes objects, so look them up by name each time
    for part, pick in pack["parts"].items():
        objs = [bpy.data.objects[n] for n in names if n in bpy.data.objects]
        objs = [o for o in objs if o not in kept and pick(o)]
        if not objs:
            print(f"GUNPREP {folder}: nothing for {part}")
            continue
        bpy.ops.object.select_all(action="DESELECT")
        for o in objs:
            o.select_set(True)
        bpy.context.view_layer.objects.active = objs[0]
        if len(objs) > 1:
            bpy.ops.object.join()
        joined = bpy.context.view_layer.objects.active
        joined.name = part
        joined.data.name = part
        joined.data.transform(fix @ joined.matrix_world)
        joined.matrix_world = mathutils.Matrix.Identity(4)
        kept.append(joined)
        bpy.context.view_layer.update()
        pts = [mathutils.Vector(c) * 100.0 for c in joined.bound_box]
        lo = [round(min(p[i] for p in pts), 2) for i in range(3)]
        hi = [round(max(p[i] for p in pts), 2) for i in range(3)]
        # Printed in the Unreal frame (Y flipped), cm.
        print(f"GUNPREP {folder}: {part} from {len(objs)} objects, "
              f"x {lo[0]}..{hi[0]}  y {-hi[1]}..{-lo[1]}  z {lo[2]}..{hi[2]} cm")
    for o in list(bpy.context.scene.objects):
        if o not in kept:
            bpy.data.objects.remove(o, do_unlink=True)
    out = os.path.join(RAW, folder, f"{folder}_Game.fbx")
    bpy.ops.export_scene.fbx(filepath=out, use_selection=False, object_types={"MESH"}, mesh_smooth_type="FACE",
                             add_leaf_bones=False, bake_anim=False, path_mode="AUTO")
    print(f"GUNPREP {folder}: wrote {out}")


names = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
for name in names or list(PACKS):
    prep(name)
