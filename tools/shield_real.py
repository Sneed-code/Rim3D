# Writes the downloaded shields (assets/shield_real.json, fetched by tools/shield_download.py) as GLBs over the Codex-made
# ones, in the same frame as tools/shield_models.py: centred, the face (boss) to +x, the arm side behind, up +z, one mesh,
# sized to the list's diameter (metres). The ShieldArt commandlet imports them.
# Run: blender -b --factory-startup -P tools/shield_real.py -- game/Content/ThirdParty/Shields
import bpy, json, sys
import numpy as np
from mathutils import Matrix, Vector
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = Path(sys.argv[sys.argv.index('--') + 1]).resolve() if '--' in sys.argv and len(sys.argv) > sys.argv.index('--') + 1 \
    else ROOT / 'game' / 'Content' / 'ThirdParty' / 'Shields'


def load_image(path, colour):
    im = bpy.data.images.load(str(path))
    im.colorspace_settings.name = 'sRGB' if colour else 'Non-Color'
    return im


def packed_orm(tex, m):
    """AO in red, roughness in green (1 - the Unity-style metallic map's smoothness alpha), metallic in blue: the glTF
    exporter writes green and blue as its metallicRoughness map."""
    ao = load_image(tex / m['ao'], False)
    met = load_image(tex / m['metal'], False)
    w, h = met.size
    mp = np.array(met.pixels[:], dtype=np.float32).reshape(h, w, 4)
    if tuple(ao.size) == (w, h):
        ap = np.array(ao.pixels[:], dtype=np.float32).reshape(h, w, 4)[:, :, 0]
    else:
        ap = np.ones((h, w), dtype=np.float32)
    out = np.ones((h, w, 4), dtype=np.float32)
    out[:, :, 0] = ap
    # Softened for the game's light (PC 2026-11-28: as painted the steel face read black, a mirror of a dark sky): rougher,
    # and the plate a little less than fully metal, the bands and rim still bright metal.
    out[:, :, 1] = np.clip(1.0 - 0.8 * mp[:, :, 3], 0.35, 1.0)
    out[:, :, 2] = np.clip(0.3 + 0.7 * (mp[:, :, 0] - 0.5) / 0.5, 0.0, 1.0)
    orm = bpy.data.images.new(m['id'] + '_ORM', width=w, height=h, alpha=False)
    orm.colorspace_settings.name = 'Non-Color'
    orm.pixels[:] = out.ravel()
    orm.filepath_raw = str(tex / (m['id'] + '_ORM.png'))
    orm.file_format = 'PNG'
    orm.save()
    return orm


def material(m, tex):
    mat = bpy.data.materials.new(m['id'])
    mat.use_nodes = True
    nodes, links = mat.node_tree.nodes, mat.node_tree.links
    bsdf = nodes.get('Principled BSDF')
    col = nodes.new('ShaderNodeTexImage'); col.image = load_image(tex / m['color'], True)
    links.new(col.outputs['Color'], bsdf.inputs['Base Color'])
    orm = nodes.new('ShaderNodeTexImage'); orm.image = packed_orm(tex, m)
    sep = nodes.new('ShaderNodeSeparateColor')
    links.new(orm.outputs['Color'], sep.inputs['Color'])
    links.new(sep.outputs['Green'], bsdf.inputs['Roughness'])
    links.new(sep.outputs['Blue'], bsdf.inputs['Metallic'])
    nrm = nodes.new('ShaderNodeTexImage'); nrm.image = load_image(tex / m['normal'], False)
    nmap = nodes.new('ShaderNodeNormalMap')
    links.new(nrm.outputs['Color'], nmap.inputs['Color'])
    links.new(nmap.outputs['Normal'], bsdf.inputs['Normal'])
    return mat


def build(m):
    raw = ROOT / 'assets' / 'raw' / m['raw']
    fbx = raw / m['fbx']
    tex = raw / m['textures']
    if not fbx.exists():
        print(f"{m['id']}: no {fbx}: run python tools/shield_download.py first")
        return False
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=str(fbx))
    meshes = [o for o in bpy.context.scene.objects if o.type == 'MESH']
    for o in bpy.context.scene.objects:
        o.select_set(o.type == 'MESH')
    bpy.context.view_layer.objects.active = meshes[0]
    if len(meshes) > 1:
        bpy.ops.object.join()
    ob = bpy.context.view_layer.objects.active
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    for o in [o for o in bpy.context.scene.objects if o != ob]:
        bpy.data.objects.remove(o, do_unlink=True)
    co = np.array([v.co[:] for v in ob.data.vertices])
    lo, hi = co.min(0), co.max(0)
    # The disc's face points along its thinnest axis; the boss (furthest out near the middle) is the front.
    normal = int(np.argmin(hi - lo))
    centre = (lo + hi) / 2
    inplane = [a for a in range(3) if a != normal]
    radius = max(hi[a] - lo[a] for a in inplane) / 2
    middle = np.all([np.abs(co[:, a] - centre[a]) < radius * 0.2 for a in inplane], axis=0)
    mid = co[middle, normal] if middle.any() else co[:, normal]
    front = 1.0 if (mid.max() - centre[normal]) >= (centre[normal] - mid.min()) else -1.0
    if 'front' in m:  # the list says which way the face points in the file ("-y": the boss side); a centre grip can stand out further than the boss
        normal = 'xyz'.index(m['front'][-1])
        front = -1.0 if m['front'].startswith('-') else 1.0
        inplane = [a for a in range(3) if a != normal]
    # Rotate so the front axis is +x and the other in-plane axis that was up stays up (+z where it can).
    axes = [Vector((1, 0, 0)), Vector((0, 1, 0)), Vector((0, 0, 1))]
    fwd = axes[normal] * front
    up = axes[2] if normal != 2 else axes[1]
    side = up.cross(fwd)  # the new +y (z cross x): a proper rotation, no mirroring
    rot = Matrix((fwd, side, up)).to_4x4()  # rows: the new x, y and z in the file's coordinates
    scale = m['diameter'] / (2 * radius)
    ob.data.transform(Matrix.Translation(-Vector(centre)))
    ob.data.transform(rot)
    ob.data.transform(Matrix.Scale(scale, 4))
    co = np.array([v.co[:] for v in ob.data.vertices])
    lo, hi = co.min(0), co.max(0)
    ob.data.transform(Matrix.Translation(-Vector((lo + hi) / 2)))
    ob.data.materials.clear()
    ob.data.materials.append(material(m, tex))
    ob.name = m['id']
    ob.data.name = m['id']
    OUT.mkdir(parents=True, exist_ok=True)
    path = OUT / (m['id'] + '.glb')
    bpy.ops.export_scene.gltf(filepath=str(path), export_format='GLB', use_selection=False, export_apply=True)
    size = hi - lo
    print(f"{m['id']}: {m['name']} -> {path} ({size[0] * 100:.0f} x {size[1] * 100:.0f} x {size[2] * 100:.0f} cm, front {'+' if front > 0 else '-'}{'xyz'[normal]} of the file)")
    return True


def main():
    models = json.loads((ROOT / 'assets' / 'shield_real.json').read_text(encoding='utf-8'))['models']
    ok = all([build(m) for m in models])
    sys.exit(0 if ok else 1)


main()
