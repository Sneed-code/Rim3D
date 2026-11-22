"""Blender script: turn free downloaded models (Fab and the like, assets/raw/fab/, not in Git) into game files for the
GunArt commandlet, the way tools/codex_prep.py does for the Codex models (docs/design/building-draw.md "Downloaded
models with a moving part").

Run from the repo root (Blender 5.x, no window):
  blender -b --factory-startup -P tools/fab_prep.py -- [Name ...]
With no names it does every model in assets/power_art.json ("prep").

Each entry's "source" is a list of globs (repo relative); the first that matches is read: .glb/.gltf, .fbx or .blend.
Writes assets/raw/power/<Name>/ (not in Git):
  <Name>_Game.fbx         the building, one object; footprint middle at x = y = 0, bottom at z = 0, front at Blender -Y
                          (Unreal +Y, the sim's north)
  <Name>Rotor_Game.fbx    kind "rotor" only: the turning part, its origin on the hub, turning about Blender -Y
  T_<Slot>_*.png          per material (codex_prep's names); a model whose colours are all shader nodes and no image
                          (the solar rack) has them baked onto a new UV layout
  <Name>_info.json        sizes, the hub, the axis and the material slots, Unreal frame (cm, Y flipped)

Kinds:
  building  "fit" [x, y] metres: turned so its long side runs along x and scaled to fit; "rows" n puts n copies side by
            side along y, spread over the fit (one rack of six panels is half as deep as the 4 x 4 solar generator).
  rotor     a model whose animation turns part of it (a wind turbine's blades). The moving objects (and their children)
            become the rotor; the hub is the moving object's origin and the axis is read from two animation frames.
            Turned so the rotor faces Blender -Y, scaled so the rotor is "rotorDiameter" across, the tower stretched or
            squeezed so the hub stands at "hub" [x, y, z] metres in the def's frame (x east, y north, z up), which is
            where visual.rotor.at puts it in the game.
"""
import glob
import json
import math
import os
import sys

import bpy
import mathutils

ROOT = os.getcwd()
OUT = os.path.join(ROOT, "assets", "raw", "power")
CONFIG = os.path.join(ROOT, "assets", "power_art.json")

# codex_prep.py's helpers (bounds, joins, texture export). Its main() only prints usage when it sees no arguments.
sys.path.insert(0, os.path.join(ROOT, "tools"))
_argv = sys.argv
sys.argv = [_argv[0]]
import codex_prep as cp  # noqa: E402
sys.argv = _argv

MESHY = {"MESH", "CURVE", "SURFACE", "FONT", "META"}


def log(name, text):
    print(f"FABPREP {name}: {text}")


def find_source(globs):
    for pattern in globs:
        hits = sorted(glob.glob(os.path.join(ROOT, pattern), recursive=True))
        if hits:
            return hits[0]
    return None


def load(path):
    ext = os.path.splitext(path)[1].lower()
    if ext == ".blend":
        bpy.ops.wm.open_mainfile(filepath=path, load_ui=False)
        # Only what a render would show: hidden variants and helper objects stay out.
        scene = bpy.context.scene
        for o in list(scene.objects):
            if o.hide_render or not o.visible_get():
                bpy.data.objects.remove(o, do_unlink=True)
    else:
        bpy.ops.wm.read_factory_settings(use_empty=True)
        if ext in (".glb", ".gltf"):
            bpy.ops.import_scene.gltf(filepath=path)
        elif ext == ".fbx":
            bpy.ops.import_scene.fbx(filepath=path, use_anim=True)
        else:
            raise RuntimeError(f"cannot read {path}")
    bpy.context.view_layer.update()


def descendants(o):
    out = [o]
    for c in o.children:
        out.extend(descendants(c))
    return out


def find_rotor(name, scene):
    """The topmost objects the animation turns, their hub (world) and axis (world unit vector), read at the first frame."""
    f0 = scene.frame_start
    scene.frame_set(f0)
    rest = {o.name: o.matrix_world.copy() for o in scene.objects}
    for step in (1, 2, 5, 10):
        scene.frame_set(f0 + step)
        moved = {}
        for o in scene.objects:
            a, b = rest[o.name].to_quaternion(), o.matrix_world.to_quaternion()
            angle = (b @ a.inverted()).angle
            if angle > 1e-4 and angle < math.pi - 1e-3:
                moved[o.name] = angle
        if moved:
            break
    if not moved:
        scene.frame_set(f0)
        return [], None, None
    roots = [o for o in scene.objects if o.name in moved and not (o.parent and o.parent.name in moved)]
    # The one turning most is the spinning part (a slow yaw of the nacelle would also move, by less).
    root = max(roots, key=lambda o: moved[o.name])
    q = (root.matrix_world.to_quaternion() @ rest[root.name].to_quaternion().inverted())
    axis, angle = q.to_axis_angle()
    # The fixed point of the turn is the hub: the object's origin when it turns about itself; solve it to be safe.
    m0, m1 = rest[root.name], root.matrix_world.copy()
    rot = q.to_matrix()
    # m1.t = rot @ (m0.t - hub) + hub  ->  (I - rot) hub = m1.t - rot @ m0.t; least squares in the plane.
    import numpy as np
    a = np.eye(3) - np.array(rot)
    rhs = np.array(m1.translation) - np.array(rot) @ np.array(m0.translation)
    hub = np.linalg.lstsq(a, rhs, rcond=None)[0]
    # Along the axis the hub is free; keep the origin's position along it.
    ax = np.array(axis)
    hub = hub + ax * (np.dot(np.array(m0.translation) - hub, ax))
    scene.frame_set(f0)
    rotor_objs = descendants(root)
    log(name, f"rotor root {root.name} ({len(rotor_objs)} objects), turns {math.degrees(angle):.2f} deg per {step} frame(s), "
              f"axis {tuple(round(v, 3) for v in axis)}, hub {tuple(round(float(v), 3) for v in hub)}")
    return [o.name for o in rotor_objs], mathutils.Vector(hub), mathutils.Vector(axis).normalized()


def realise():
    """Every visible shape as a plain mesh object, modifiers and instances applied, animation dropped."""
    bpy.ops.object.select_all(action="DESELECT")
    for o in bpy.context.scene.objects:
        if o.type == "EMPTY" and o.instance_type == "COLLECTION":
            o.select_set(True)
    if bpy.context.selected_objects:
        bpy.context.view_layer.objects.active = bpy.context.selected_objects[0]
        bpy.ops.object.duplicates_make_real(use_base_parent=False, use_hierarchy=False)
    bpy.ops.object.select_all(action="DESELECT")
    shapes = [o for o in bpy.context.scene.objects if o.type in MESHY]
    for o in shapes:
        o.select_set(True)
    if shapes:
        bpy.context.view_layer.objects.active = shapes[0]
        bpy.ops.object.convert(target="MESH")
    for o in bpy.context.scene.objects:
        o.animation_data_clear()
    meshes = [o for o in bpy.context.scene.objects if o.type == "MESH" and len(o.data.vertices)]
    for o in list(bpy.context.scene.objects):
        if o not in meshes:
            bpy.data.objects.remove(o, do_unlink=True)
    cp.apply_all(meshes)
    return meshes


def transform(objs, m):
    for o in objs:
        o.data.transform(m)


def points(objs):
    return cp.world_points(objs)


def has_image(mat):
    return bool(mat and mat.use_nodes and any(n.type == "TEX_IMAGE" and n.image for n in mat.node_tree.nodes))


def bake_colours(obj, mats, folder, slots, name):
    """Colours made of shader nodes (no image anywhere in the model): bake each material's base colour, roughness and
    metallic onto one new UV layout. Returns {slot: files}."""
    import numpy as np
    me = obj.data
    while me.uv_layers:
        me.uv_layers.remove(me.uv_layers[0])
    uv = me.uv_layers.new(name="UVMap")
    me.uv_layers.active = uv
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.smart_project(angle_limit=math.radians(60), island_margin=0.004)
    bpy.ops.object.mode_set(mode="OBJECT")
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.cycles.samples = 1
    size = 1024
    images = {}
    for m, slot in zip(mats, slots):
        img = bpy.data.images.new(f"bake_{slot}", size, size, alpha=False, float_buffer=False)
        img.colorspace_settings.name = "sRGB"
        node = m.node_tree.nodes.new("ShaderNodeTexImage")
        node.image = img
        for n in m.node_tree.nodes:
            n.select = False
        node.select = True
        m.node_tree.nodes.active = node
        images[slot] = (m, img, node)
    bake = scene.render.bake
    bake.use_pass_direct = False
    bake.use_pass_indirect = False
    bake.use_pass_color = True
    bake.margin = 4
    bpy.ops.object.bake(type="DIFFUSE", pass_filter={"COLOR"}, use_clear=True, margin=4)
    out = {}
    for slot, (m, img, node) in images.items():
        stem = os.path.join(folder, f"T_{slot}_")
        img.filepath_raw = stem + "BaseColor.png"
        img.file_format = "PNG"
        img.save()
        files = {"baseColor": stem + "BaseColor.png"}
        bsdf = next((n for n in m.node_tree.nodes if n.type == "BSDF_PRINCIPLED"), None)
        rough = bsdf.inputs["Roughness"].default_value if bsdf else 0.6
        metal = bsdf.inputs["Metallic"].default_value if bsdf else 0.0
        cp.save_png(np.tile(np.array([1.0, rough, metal, 1.0], dtype=np.float32), (4, 4, 1)), stem + "ORM.png", "Non-Color")
        files["orm"] = stem + "ORM.png"
        m.node_tree.nodes.remove(node)
        out[slot] = files
    return out


def flat_orm(mat, slot, folder, files):
    """A material with no roughness/metal maps still gets its own numbers (M_GunPBR's defaults are fully rough, not metal)."""
    import numpy as np
    if "orm" in files:
        return
    bsdf = next((n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED"), None) if mat and mat.use_nodes else None
    if not bsdf:
        return
    path = os.path.join(folder, f"T_{slot}_ORM.png")
    cp.save_png(np.tile(np.array([1.0, bsdf.inputs["Roughness"].default_value, bsdf.inputs["Metallic"].default_value, 1.0],
                                 dtype=np.float32), (4, 4, 1)), path, "Non-Color")
    files["orm"] = path


def write_materials(name, objs, folder):
    mats = []
    for o in objs:
        for s in o.material_slots:
            if s.material and s.material not in mats:
                mats.append(s.material)
    slots = [name if len(mats) == 1 else f"{name}_{i}" for i in range(len(mats))]
    info = {}
    procedural = mats and not any(has_image(m) for m in mats) and any(
        m.use_nodes and any(n.type == "BSDF_PRINCIPLED" and n.inputs["Base Color"].is_linked for n in m.node_tree.nodes) for m in mats)
    if procedural:
        if len(objs) != 1:
            raise RuntimeError("baking needs the model joined into one object")
        baked = bake_colours(objs[0], mats, folder, slots, name)
        for m, slot in zip(mats, slots):
            info[slot] = {"from": m.name, "maps": sorted(baked[slot]), "baked": True}
    else:
        for m, slot in zip(mats, slots):
            files = cp.write_textures(m, slot, folder)
            flat_orm(m, slot, folder, files)
            info[slot] = {"from": m.name, "maps": sorted(files)}
    for m, slot in zip(mats, slots):
        m.name = slot
    return info


def export(objs, path):
    bpy.ops.object.select_all(action="DESELECT")
    for o in objs:
        o.select_set(True)
    bpy.ops.export_scene.fbx(filepath=path, use_selection=True, object_types={"MESH"}, mesh_smooth_type="FACE",
                             add_leaf_bones=False, bake_anim=False, path_mode="STRIP", use_tspace=True)


def prep_building(name, entry, meshes):
    import numpy as np
    pts = points(meshes)
    ext = pts.max(axis=0) - pts.min(axis=0)
    if ext[1] > ext[0]:
        transform(meshes, mathutils.Matrix.Rotation(math.radians(90), 4, "Z"))
        pts = points(meshes)
        ext = pts.max(axis=0) - pts.min(axis=0)
    fit = entry["fit"]
    rows = max(1, int(entry.get("rows", 1)))
    scale = min(fit[0] / ext[0], fit[1] / (ext[1] * rows))
    transform(meshes, mathutils.Matrix.Scale(scale, 4))
    lo, hi = cp.world_bounds(meshes)
    transform(meshes, mathutils.Matrix.Translation((-(lo.x + hi.x) / 2, -(lo.y + hi.y) / 2, -lo.z)))
    depth = (hi.y - lo.y)
    if rows > 1:
        span = max(fit[1] - depth, 0.0)
        copies = []
        for r in range(rows):
            y = -span / 2 + span * r / (rows - 1)
            for o in meshes:
                c = o.copy()
                c.data = o.data.copy()
                bpy.context.scene.collection.objects.link(c)
                c.data.transform(mathutils.Matrix.Translation((0, y, 0)))
                copies.append(c)
        for o in meshes:
            bpy.data.objects.remove(o, do_unlink=True)
        meshes = copies
    log(name, f"scaled x{scale:.3f}, {rows} row(s)")
    return [cp.join(meshes, name)], None


def prep_rotor(name, entry, meshes, rotor_names, hub, axis):
    import numpy as np
    rotor = [o for o in meshes if o.name in rotor_names]
    base = [o for o in meshes if o.name not in rotor_names]
    if not rotor or not base:
        raise RuntimeError(f"rotor {len(rotor)} objects, base {len(base)}: cannot split")
    if abs(axis.z) > 0.3:
        log(name, f"WARNING: the rotor axis {tuple(axis)} is not level; is the model lying down?")
    # Turn about Z: the hub in front of the tower, at -Y.
    bp = points(base)
    low = bp[bp[:, 2] < bp[:, 2].min() + 0.3 * (bp[:, 2].max() - bp[:, 2].min())]
    tower = low.mean(axis=0)
    d = mathutils.Vector((hub.x - tower[0], hub.y - tower[1]))
    if d.length < 1e-6:
        d = mathutils.Vector((axis.x, axis.y))
    turn = math.atan2(-1.0, 0.0) - math.atan2(d.y, d.x)
    m = mathutils.Matrix.Rotation(turn, 4, "Z")
    transform(meshes, m)
    hub = m @ hub
    axis = (m.to_3x3() @ axis).normalized()
    if axis.y > 0:
        axis = -axis
    # Scale by the rotor's size across.
    rp = points(rotor) - np.array(hub)
    ax = np.array(axis)
    radial = rp - np.outer(rp @ ax, ax)
    diameter = 2.0 * float(np.sqrt((radial ** 2).sum(axis=1)).max())
    scale = entry["rotorDiameter"] / diameter
    s = mathutils.Matrix.Scale(scale, 4)
    transform(meshes, s)
    hub = s @ hub
    # Stand the hub at the wanted height: stretch the tower below the nacelle, lift the rest.
    want = entry["hub"]
    ground = float(points(base)[:, 2].min())
    rise = want[2] - (hub.z - ground)
    cut = hub.z - 0.12 * entry["rotorDiameter"]
    k = max(0.05, (cut + rise - ground) / max(cut - ground, 1e-6))
    for o in base:
        for v in o.data.vertices:
            v.co.z = ground + (v.co.z - ground) * k if v.co.z < cut else v.co.z + rise
        o.data.update()
    transform(rotor, mathutils.Matrix.Translation((0, 0, rise)))
    hub.z += rise
    # Hub to the def's spot: x east, y north (Blender -Y), z up from the ground.
    transform(meshes, mathutils.Matrix.Translation((want[0] - hub.x, -want[1] - hub.y, -ground)))
    hub = mathutils.Vector((want[0], -want[1], want[2]))
    transform(rotor, mathutils.Matrix.Translation(-hub))
    log(name, f"turned {math.degrees(turn):.1f} deg about Z, scaled x{scale:.4f} (rotor was {diameter:.3f} across), "
              f"tower x{k:.3f} below {cut:.2f}, hub {tuple(want)}")
    joined = [cp.join(base, name), cp.join(rotor, name + "Rotor")]
    return joined, {"hubCm": cp.unreal_cm(hub), "axisUnreal": [round(axis.x, 4), round(-axis.y, 4), round(axis.z, 4)],
                    "towerStretch": round(k, 4), "scale": round(scale, 5)}


def preview(name, kept, extra, folder):
    """<Name>_preview.png: a small Cycles picture from the front-left and above, the rotor back on its hub, for checking
    a model without Unreal."""
    scene = bpy.context.scene
    for o in list(scene.objects):
        if o not in kept:
            bpy.data.objects.remove(o, do_unlink=True)
    if extra and len(kept) > 1:
        kept[1].location = mathutils.Vector((extra["hubCm"][0], -extra["hubCm"][1], extra["hubCm"][2])) / 100.0
    bpy.context.view_layer.update()
    for o in kept:
        for s in o.material_slots:  # show the exported maps, as the game will
            m = s.material
            path = os.path.join(folder, f"T_{m.name}_BaseColor.png")
            bsdf = next((n for n in m.node_tree.nodes if n.type == "BSDF_PRINCIPLED"), None)
            if bsdf and os.path.exists(path):
                tex = m.node_tree.nodes.new("ShaderNodeTexImage")
                tex.image = bpy.data.images.load(path)
                m.node_tree.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    lo, hi = cp.world_bounds(kept)
    centre = (lo + hi) / 2
    size = max(hi.x - lo.x, hi.y - lo.y, hi.z - lo.z)
    bpy.ops.mesh.primitive_plane_add(size=size * 4, location=(centre.x, centre.y, 0))
    cam_data = bpy.data.cameras.new("PreviewCam")
    cam = bpy.data.objects.new("PreviewCam", cam_data)
    scene.collection.objects.link(cam)
    cam.location = centre + mathutils.Vector((-1.1, -1.6, 0.9)) * size * 1.1
    cam.rotation_euler = (centre - cam.location).to_track_quat("-Z", "Y").to_euler()
    scene.camera = cam
    sun_data = bpy.data.lights.new("PreviewSun", "SUN")
    sun_data.energy = 3.0
    sun = bpy.data.objects.new("PreviewSun", sun_data)
    sun.rotation_euler = (math.radians(40), math.radians(10), math.radians(-30))
    scene.collection.objects.link(sun)
    if scene.world is None:
        scene.world = bpy.data.worlds.new("PreviewWorld")
    scene.world.color = (0.5, 0.6, 0.7)
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.cycles.samples = 24
    scene.render.resolution_x = 640
    scene.render.resolution_y = 640
    scene.render.filepath = os.path.join(folder, f"{name}_preview.png")
    bpy.ops.render.render(write_still=True)


def prep(name, entry):
    src = find_source(entry["source"])
    if not src:
        log(name, f"no file for {entry['source']} (download it first, assets/manifest.json)")
        return False
    load(src)
    scene = bpy.context.scene
    rotor_names, hub, axis = ([], None, None)
    if entry["kind"] == "rotor":
        rotor_names, hub, axis = find_rotor(name, scene)
        if not rotor_names:
            log(name, "no turning part found in the animation")
            return False
        # Names survive the conversion; keep them by tagging (convert may rename curve objects).
        for o in scene.objects:
            o["fab_rotor"] = o.name in rotor_names
    meshes = realise()
    rotor_names = {o.name for o in meshes if o.get("fab_rotor")}
    if entry["kind"] == "rotor":
        kept, extra = prep_rotor(name, entry, meshes, rotor_names, hub, axis)
    else:
        kept, extra = prep_building(name, entry, meshes)
    bpy.context.view_layer.update()
    folder = os.path.join(OUT, name)
    os.makedirs(folder, exist_ok=True)
    if len(kept) > 1 and not any(has_image(s.material) for o in kept for s in o.material_slots):
        log(name, "WARNING: a rotor model with no images keeps flat colours")
    slots = write_materials(name, kept, folder)
    lo, hi = cp.world_bounds(kept[:1])
    info = {"name": name, "kind": entry["kind"], "source": os.path.relpath(src, ROOT).replace("\\", "/"),
            "boundsCm": cp.unreal_box(lo, hi), "sizeM": [round(hi.x - lo.x, 3), round(hi.y - lo.y, 3), round(hi.z - lo.z, 3)],
            "parts": {o.name: {"boundsCm": cp.unreal_box(*cp.world_bounds([o])), "tris": sum(len(p.vertices) - 2 for p in o.data.polygons)} for o in kept},
            "slots": slots}
    info.update(extra or {})
    with open(os.path.join(folder, f"{name}_info.json"), "w") as f:
        json.dump(info, f, indent=1)
    for o in kept:
        export([o], os.path.join(folder, f"{o.name}_Game.fbx"))
    if os.environ.get("FABPREP_PREVIEW", "1") != "0":
        preview(name, kept, extra, folder)
    log(name, f"{entry['kind']} from {info['source']}: {info['sizeM']} m, parts {list(info['parts'])}, slots {list(slots)} -> {folder}")
    return True


def main():
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    with open(CONFIG) as f:
        table = json.load(f)["prep"]
    names = args or list(table)
    ok, missing, failed = 0, 0, 0
    for name in names:
        if not find_source(table[name]["source"]):
            # Not downloaded on this PC (a fresh clone): the game draws the def's shapes; the step runs again when the
            # files arrive (its raw folders change).
            log(name, f"no file for {table[name]['source']}: skipped (download it, assets/manifest.json)")
            missing += 1
            continue
        try:
            if prep(name, table[name]):
                ok += 1
            else:
                failed += 1
        except Exception as e:  # one bad model should not stop the rest; the step fails below
            log(name, f"FAILED: {e}")
            failed += 1
    print(f"FABPREP done: {ok} made, {missing} not downloaded, {failed} failed")
    if failed:
        sys.exit(1)


main()
