import bpy, sys, math, mathutils
out = sys.argv[sys.argv.index("--") + 1]
scene = bpy.context.scene
meshes = [o for o in scene.objects if o.type == "MESH" and o.visible_get()]
mn = mathutils.Vector((1e9,)*3); mx = mathutils.Vector((-1e9,)*3)
for o in meshes:
    for c in o.bound_box:
        w = o.matrix_world @ mathutils.Vector(c)
        mn = mathutils.Vector(map(min, mn, w)); mx = mathutils.Vector(map(max, mx, w))
print("bounds", mn, mx)
cam_data = bpy.data.cameras.new("cam"); cam = bpy.data.objects.new("cam", cam_data); scene.collection.objects.link(cam)
centre = (mn + mx) / 2; h = mx.z - mn.z
cam.location = (centre.x, mn.y - h * 1.6, centre.z); cam.rotation_euler = (math.radians(90), 0, 0)
cam_data.lens = 50; scene.camera = cam
light = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN")); light.rotation_euler = (math.radians(50), math.radians(10), math.radians(-30)); scene.collection.objects.link(light)
scene.render.engine = "BLENDER_EEVEE" if "BLENDER_EEVEE" in [e.identifier for e in bpy.types.RenderSettings.bl_rna.properties["engine"].enum_items] else "BLENDER_WORKBENCH"
scene.render.resolution_x = 900; scene.render.resolution_y = 1400
scene.render.filepath = out
bpy.ops.render.render(write_still=True)
