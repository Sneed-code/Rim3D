# Builds one MakeHuman character with MPFB in a headless Blender and exports it as FBX for Unreal.
#   blender.exe -b -noaudio --python mh_export.py -- <config.json>
# Config: {"out": "...fbx", "macro": {...}, "proxy": "...proxy", "skin": "...mhmat", "clothes": ["...mhclo"],
#          "rig": "game_engine_with_breast"}
import bpy, addon_utils, json, os, sys, traceback

addon_utils.enable("rigify", default_set=True)  # MPFB upgrades older sub-rigs (the ponytail's) with it
addon_utils.enable("bl_ext.user_default.mpfb", default_set=True)
from bl_ext.user_default.mpfb.services.humanservice import HumanService
from bl_ext.user_default.mpfb.services.targetservice import TargetService
from bl_ext.user_default.mpfb.services.exportservice import ExportService
from bl_ext.user_default.mpfb.services.objectservice import ObjectService

cfg_path = sys.argv[sys.argv.index("--") + 1]
cfg = json.load(open(cfg_path, encoding="utf-8"))
repo = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
if not os.path.isabs(cfg["out"]):
    cfg["out"] = os.path.join(repo, cfg["out"])
# {mh_user}: the MakeHuman user data folder (assets you made or downloaded); {mh_system}: MakeHuman's own install.
# Override with the MH_USER_DATA and MH_SYSTEM_DATA environment variables.
places = {
    "mh_user": os.environ.get("MH_USER_DATA", os.path.expanduser("~/OneDrive/Documents/makehuman/v1py3/data")),
    "mh_system": os.environ.get("MH_SYSTEM_DATA", os.path.join(os.environ.get("LOCALAPPDATA", ""), "makehuman-community", "makehuman", "data")),
    "codex": os.environ.get("MH_CHARACTERS", os.path.expanduser("~/Documents/Codex/2026-10-25/dow/outputs")),
    # MPFB's own asset folder (hair, eyes, eyebrows, eyelashes, teeth, skins, clothes).
    "mpfb": os.environ.get("MPFB_DATA", os.path.join(os.environ.get("APPDATA", ""), "Blender Foundation", "Blender", "5.2", "extensions", ".user", "user_default", "mpfb", "data")),
}
def place(path):
    return path.format(**places) if isinstance(path, str) else path
for key in ("proxy", "skin", "mhm"):
    if cfg.get(key):
        cfg[key] = place(cfg[key])
cfg["clothes"] = [place(c) for c in cfg.get("clothes", [])]
# Body parts: {"Hair": "...mhclo", "Eyebrows": ..., "Eyelashes": ..., "Eyes": ..., "Teeth": ...}; materials overrides
# by object or material name ({"eyes": {"color": "...png"}}) and colour tints ({"hair": [r, g, b]}).
cfg["parts"] = {k: place(v) for k, v in cfg.get("parts", {}).items()}
cfg["material_overrides"] = {k: {kk: place(vv) for kk, vv in v.items()} for k, v in cfg.get("material_overrides", {}).items()}

def log(*a):
    print("[mh_export]", *a, flush=True)


sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mh_common import add_soft_bones, lower_pubic_hair, prepare_for_unreal, write_materials  # noqa: E402

try:
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)

    if cfg.get("mhm"):
        # A MakeHuman save file: everything it lists (sliders, proxy, skin, eyes, hair, materials), our skeleton.
        settings = HumanService.get_default_deserialization_settings()
        settings.update({"subdiv_levels": 0, "override_rig": cfg.get("rig", "game_engine_with_breast"), "override_skin_model": "GAMEENGINE",
                         "clothes_deep_search": True, "bodypart_deep_search": True})
        basemesh = HumanService.deserialize_from_mhm(place(cfg["mhm"]), settings)
        rig = ObjectService.find_object_of_type_amongst_nearest_relatives(basemesh, "Skeleton")
        proxy = ObjectService.find_object_of_type_amongst_nearest_relatives(basemesh, "Proxymeshes")
        log("from", cfg["mhm"], "rig", rig.name, len(rig.data.bones), "proxy", proxy.name if proxy else None)
        for line in open(place(cfg["mhm"]), encoding="utf-8", errors="ignore"):
            if line.startswith("skinMaterial "):
                cfg["skin_texture_from"] = line.split(None, 1)[1].strip()
    else:
        basemesh = None
    if basemesh is None:
        macro = TargetService.get_default_macro_info_dict()
        for k, v in cfg.get("macro", {}).items():
            if k == "race":
                macro["race"].update(v)
            else:
                macro[k] = v
        basemesh = HumanService.create_human(macro_detail_dict=macro, scale=0.1)
        log("basemesh", basemesh.name, len(basemesh.data.vertices))

        rig = HumanService.add_builtin_rig(basemesh, cfg.get("rig", "game_engine_with_breast"))
        log("rig", rig.name, len(rig.data.bones))

        proxy = None
        if cfg.get("proxy"):
            proxy = HumanService.add_mhclo_asset(cfg["proxy"], basemesh, asset_type="Proxymeshes", subdiv_levels=0)
            log("proxy", proxy.name, len(proxy.data.vertices))

        if cfg.get("skin"):
            HumanService.set_character_skin(cfg["skin"], basemesh, bodyproxy=proxy, skin_type="GAMEENGINE")
            cfg["skin_texture_from"] = cfg["skin"]

    # The skin's colour map goes beside the FBX as skin.png (the MakeHumanArt commandlet reads it there).
    if cfg.get("skin_texture_from"):
        import shutil
        for line in open(cfg["skin_texture_from"], encoding="utf-8", errors="ignore"):
            if line.strip().startswith("diffuseTexture"):
                tex = line.split(None, 1)[1].strip()
                if not os.path.isabs(tex):
                    tex = os.path.join(os.path.dirname(cfg["skin_texture_from"]), tex)
                os.makedirs(os.path.dirname(cfg["out"]), exist_ok=True)
                shutil.copyfile(tex, os.path.join(os.path.dirname(cfg["out"]), "skin.png"))
                log("skin texture", tex)

    for kind, path in cfg["parts"].items():
        part = HumanService.add_mhclo_asset(path, basemesh, asset_type=kind, subdiv_levels=0, material_type="GAMEENGINE")
        log("part", kind, part.name, len(part.data.vertices))

    for path in cfg.get("clothes", []):
        c = HumanService.add_mhclo_asset(path, basemesh, asset_type="Clothes", subdiv_levels=0, material_type="GAMEENGINE")
        log("clothes", c.name, len(c.data.vertices))

    # Sub-rigs (the ponytail's bones) are separate armatures: joined into the main skeleton, so the game has one.
    subrigs = [o for o in bpy.data.objects if o.type == "ARMATURE" and o != rig]
    if subrigs:
        for mesh in [o for o in bpy.data.objects if o.type == "MESH"]:
            for m in mesh.modifiers:
                if m.type == "ARMATURE" and m.object in subrigs:
                    m.object = rig
        bpy.ops.object.mode_set(mode="OBJECT") if bpy.context.object and bpy.context.object.mode != "OBJECT" else None
        bpy.ops.object.select_all(action="DESELECT")
        for o in subrigs + [rig]:
            o.hide_set(False)
            o.select_set(True)
        bpy.context.view_layer.objects.active = rig
        with bpy.context.temp_override(active_object=rig, selected_editable_objects=subrigs + [rig], selected_objects=subrigs + [rig]):
            bpy.ops.object.join()
        # A sub-rig's own root hangs from the head (hair) so the skeleton keeps one root.
        main_root = rig.data.bones[0].name
        with bpy.context.temp_override(active_object=rig, object=rig, selected_objects=[rig], selected_editable_objects=[rig]):
            bpy.ops.object.mode_set(mode="EDIT")
            head = rig.data.edit_bones.get("head")
            for bone in rig.data.edit_bones:
                if bone.parent is None and bone.name != main_root and head is not None:
                    bone.use_connect = False
                    bone.parent = head
                    log("sub-rig root", bone.name, "now under head")
            bpy.ops.object.mode_set(mode="OBJECT")
        log("joined sub-rigs into", rig.name, "now", len(rig.data.bones), "bones")

    # Bake the macro shape into the meshes, drop helper geometry and subdivision.
    ExportService.bake_modifiers_remove_helpers(basemesh, bake_masks=True, bake_subdiv=False, remove_helpers=True, also_proxy=True)

    for obj in [o for o in bpy.data.objects if o.type == "MESH"]:
        if obj.data.shape_keys:
            # Mix the shape keys into the mesh: the export is one fixed character.
            obj.shape_key_add(name="__mix", from_mix=True)
            keys = obj.data.shape_keys.key_blocks
            mix = [k.co.copy() for k in keys["__mix"].data] if hasattr(keys["__mix"].data[0], "co") else None
            for i, v in enumerate(obj.data.vertices):
                v.co = keys["__mix"].data[i].co
            bpy.context.view_layer.objects.active = obj
            obj.shape_key_clear()
        for m in list(obj.modifiers):
            if m.type not in ("ARMATURE",):
                obj.modifiers.remove(m)

    body = proxy if proxy else basemesh
    # Soft tissue bones for jiggle (buttocks; the skeleton has the breast bones) and natural pubic hair.
    if cfg.get("soft_bones", True):
        add_soft_bones(rig, body, log=log)
    for o in [o for o in bpy.data.objects if o.type == "MESH" and "pubic" in o.name.lower()]:
        lower_pubic_hair(o, body, top_share=cfg.get("pubic_top_share", 0.6), above_crotch=cfg.get("pubic_above_crotch", 0.025), log=log)

    # With a body proxy the basemesh itself is not exported.
    export = [rig] + [o for o in bpy.data.objects if o.type == "MESH" and (o != basemesh or proxy is None)]
    bpy.ops.object.select_all(action="DESELECT")
    for o in export:
        o.hide_set(False)
        o.hide_viewport = False
        o.select_set(True)
    bpy.context.view_layer.objects.active = rig
    prepare_for_unreal(rig, [o for o in export if o != rig])
    bpy.ops.object.select_all(action="DESELECT")
    for o in export:
        o.select_set(True)
    log("exporting", [o.name for o in export])
    os.makedirs(os.path.dirname(cfg["out"]), exist_ok=True)
    write_materials([o for o in export if o != rig], os.path.dirname(cfg["out"]), skin_meshes=[proxy] if proxy else [],
                    tints=cfg.get("tints", {}), overrides=cfg["material_overrides"])
    bpy.ops.export_scene.fbx(filepath=cfg["out"], use_selection=True, object_types={"ARMATURE", "MESH"},
                             apply_scale_options="FBX_SCALE_NONE", add_leaf_bones=False, bake_anim=False,
                             mesh_smooth_type="FACE", use_armature_deform_only=True, path_mode="COPY", embed_textures=False,
                             axis_forward="-Y", axis_up="Z")
    log("done", cfg["out"])
    if cfg.get("save_blend"):
        bpy.ops.wm.save_as_mainfile(filepath=cfg["save_blend"])
except Exception:
    traceback.print_exc()
    sys.exit(1)
