# Builds the 12 shield models (docs/design/shields.md "Models"). Written by Codex for sneed. Run: blender -b --python tools/shield_models.py -- game/Content/ThirdParty/Shields
import bpy, math, os, random, sys
import numpy as np
from mathutils import Vector
from math import sin, cos, pi
from pathlib import Path

OUT = Path(sys.argv[sys.argv.index('--') + 1]).resolve() if '--' in sys.argv else Path(__file__).resolve().parent
random.seed(42)
# With an output folder (the game's), only the .glb files are written; without, previews and a contact sheet too.
PREVIEWS = '--' not in sys.argv
results=[]

def srgb(c):
    # Image pixels are stored as sRGB, but the colours here are worked out in linear light: encode them, or the maps come out far too dark.
    c=np.clip(c,0,1); return np.where(c<=.0031308,c*12.92,1.055*np.power(c,1/2.4)-.055)

def mat(name, color, metal=0, rough=.5, emission=None, alpha=1):
    m=bpy.data.materials.new(name); m.diffuse_color=(*color,alpha); m.use_nodes=True
    p=m.node_tree.nodes.get('Principled BSDF'); p.inputs['Base Color'].default_value=(*color,alpha); p.inputs['Metallic'].default_value=metal; p.inputs['Roughness'].default_value=rough
    if emission:
        p.inputs['Emission Color'].default_value=(*color,1); p.inputs['Emission Strength'].default_value=emission
    if alpha<1:
        p.inputs['Alpha'].default_value=alpha
        try: m.surface_render_method='DITHERED'
        except: pass
    return m

def mesh(name, verts, faces, material, indices=None):
    me=bpy.data.meshes.new(name); me.from_pydata(verts,[],faces); me.update(); ob=bpy.data.objects.new(name,me); bpy.context.collection.objects.link(ob)
    for m in (material if isinstance(material,list) else [material]): ob.data.materials.append(m)
    if indices:
        for p,i in zip(me.polygons,indices): p.material_index=i
    return ob

def tube(name, pts, radius, material, cyclic=False, sides=6):
    # Explicit tube with ring topology and capped ends.
    vs=[]; fs=[]
    for i,p in enumerate(pts):
        tangent=Vector(pts[(i+1)%len(pts)])-Vector(pts[(i-1)%len(pts)]) if cyclic or (i>0 and i<len(pts)-1) else Vector(pts[min(i+1,len(pts)-1)])-Vector(pts[max(i-1,0)])
        tangent.normalize(); a=tangent.cross(Vector((1,0,0)))
        if a.length<.01: a=tangent.cross(Vector((0,0,1)))
        a.normalize(); b=tangent.cross(a).normalized()
        for j in range(sides): vs.append(Vector(p)+radius*(cos(2*pi*j/sides)*a+sin(2*pi*j/sides)*b))
    for i in range(len(pts) if cyclic else len(pts)-1):
        for j in range(sides): fs.append((i*sides+j,i*sides+(j+1)%sides,((i+1)%len(pts))*sides+(j+1)%sides,((i+1)%len(pts))*sides+j))
    if not cyclic: fs.extend([tuple(reversed(range(sides))),tuple((len(pts)-1)*sides+j for j in range(sides))])
    ob=mesh(name,vs,fs,material)
    for p in ob.data.polygons: p.use_smooth=True
    return ob

def ball(name, loc, scale, material, seg=12, rings=6):
    bpy.ops.mesh.primitive_uv_sphere_add(segments=seg,ring_count=rings,location=loc); o=bpy.context.object; o.name=name; o.scale=scale; bpy.ops.object.transform_apply(location=False,rotation=False,scale=True); o.data.materials.append(material)
    for p in o.data.polygons: p.use_smooth=True
    return o

def cube(name, loc, scale, material, bevel=0):
    bpy.ops.mesh.primitive_cube_add(size=1,location=loc); o=bpy.context.object; o.name=name; o.dimensions=scale; bpy.ops.object.transform_apply(location=False,rotation=False,scale=True); o.data.materials.append(material)
    if bevel:
        mod=o.modifiers.new('Soft manufactured edges','BEVEL'); mod.width=bevel; mod.segments=2
        bpy.context.view_layer.objects.active=o; bpy.ops.object.modifier_apply(modifier=mod.name)
    return o

def width_at(size,z):
    if size=='Small': return .25*math.sqrt(max(0,1-(z/.25)**2))
    if size=='Medium':
        # Straight sides above the lower third, smoothly sweeping into a point.
        t=(z+.375)/.75
        return .30 if t >= .42 else .30 * (1-(1-t/.42)**2)**.65
    return .325*(1-.08*(abs(z)/.6)**8)

def surf(y, size):
    w={'Small':.25,'Medium':.30,'Tower':.325}[size]
    return (.018 if size=='Small' else .035)*(1-(y/w)**2)

def panel_patch(name, size, z0, z1, f0, f1, material, inset=.012, xraise=.010):
    # Chamfered ceramic plate mapped onto the shield face with an open recessed rim.
    steps=3 if size=='Small' else 6
    boundary=[]
    for i in range(steps+1): boundary.append((f0+(f1-f0)*i/steps,z0))
    for i in range(1,steps+1): boundary.append((f1,z0+(z1-z0)*i/steps))
    for i in range(1,steps+1): boundary.append((f1-(f1-f0)*i/steps,z1))
    for i in range(1,steps): boundary.append((f0,z1-(z1-z0)*i/steps))
    cf=(f0+f1)*.5; cz=(z0+z1)*.5; outer=[]; inner=[]
    for f,z in boundary:
        y=width_at(size,z)*f; outer.append((surf(y,size)+.001,y,z))
        zz=z+(1 if z<cz else -1)*inset
        ff=f+(1 if f<cf else -1)*inset/max(.08,width_at(size,z))
        yy=width_at(size,zz)*ff; inner.append((surf(yy,size)+xraise,yy,zz))
    verts=outer+inner; count=len(boundary); faces=[]
    for i in range(count): faces.append((i,(i+1)%count,(i+1)%count+count,i+count))
    faces.append(tuple(range(count,2*count)))
    return mesh(name,verts,faces,material)

def emit_channel(name, size, points, material, radius=.002):
    pts=[(surf(y,size)+.008,y,z) for y,z in points]
    return tube(name,pts,radius,material,False,4 if size=='Small' else 5)


def bake_surface(tier, size, h):
    # One compact baked colour/roughness map, shared by all front/back shell faces.
    rng=np.random.default_rng(503 if tier=='Wood' else 917)
    yy,zz=np.meshgrid(np.linspace(-1,1,512),np.linspace(-1,1,512))
    if tier=='Wood':
        boundaries=np.array([-1,-.76,-.48,-.21,.12,.38,.67,1.01])
        board=np.clip(np.searchsorted(boundaries,yy,side='right')-1,0,6)
        tones=np.array([.82,1.04,.91,1.12,.94,1.02,.85])[board]
        warp=yy + .008*np.sin(zz*11+board*2) + .003*np.sin(zz*39+board)
        grain=(.075*np.sin(warp*230+np.sin(zz*13+board)*1.4)+.035*np.sin(warp*570+np.sin(zz*23)*2)+.035*np.sin(warp*92+zz*8))
        joint=np.zeros_like(yy)
        for edge in boundaries[1:-1]: joint+=np.exp(-((yy-edge)/.004)**2)*.19
        variation=np.clip(tones+grain-joint, .56,1.2)
        col=np.stack([.44*variation,.235*variation,.11*variation],axis=-1)  # lighter oak: the noon smoke picture read darker than a mannequin
        rough=.82
    else:
        variation=.88+.028*np.sin(yy*680)+.018*np.sin(yy*1310)+.012*np.sin(yy*1930)
        variation-=.13*np.clip((np.abs(yy)-.72)/.28,0,1)
        # irregular short, faint scuffs, darkened with age
        for i in range(55):
            x,z=rng.uniform(-.9,.9,2); length=rng.uniform(.015,.12)
            mark=np.exp(-((yy-x-0.025*(zz-z))/.0025)**2)*np.exp(-((zz-z)/length)**8)
            variation+=mark*rng.choice([-.13,.10])
        col=np.stack([.31*variation,.34*variation,.35*variation],axis=-1)
        rough=.48
    rgba=np.concatenate((srgb(col),np.ones((512,512,1))),axis=2).astype(np.float32)
    im=bpy.data.images.new(tier+' baked wear '+size,width=512,height=512,alpha=True)
    im.pixels.foreach_set(rgba.ravel()); im.pack()
    m=mat(tier+' worn surface',(.4,.4,.4),.78 if tier=='Steel' else 0,rough)
    nodes=m.node_tree.nodes; tex=nodes.new('ShaderNodeTexImage'); tex.image=im
    m.node_tree.links.new(tex.outputs['Color'],nodes.get('Principled BSDF').inputs['Base Color'])
    return m

def build(tier,size):
    bpy.ops.object.select_all(action='SELECT'); bpy.ops.object.delete(use_global=False)
    h={'Small':.5,'Medium':.75,'Tower':1.2}[size]; n=24 if size=='Small' else 24 if size=='Medium' else 16; ny=6 if size=='Small' else 12
    if tier=='Plasteel': n=(16 if size=='Small' else 18); ny=12
    if tier=='Composite': ny=12
    woods=[mat('Oak plank %02d'%i,c,0,.78) for i,c in enumerate([(0.27,.135,.057),(.34,.19,.085),(.23,.11,.045),(.39,.22,.105),(.29,.15,.065)])]
    steel=mat('Tempered steel',(.31,.34,.35),.83,.34); edge=mat('Dark iron',(.105,.115,.112),.8,.43); leather=mat('Worn leather',(.13,.075,.038),0,.8)
    polymer=mat('Charcoal ballistic composite',(.075,.085,.095),0,.78); trim=mat('Dark rubber edge guard',(.018,.022,.027),0,.88)
    ceramic=mat('Pale ceramic armour',(.78,.82,.82),.12,.26); seam=mat('Gunmetal recessed frame',(.035,.047,.055),.72,.34); cyan=mat('Cyan photonic channel',(.01,.72,1.0),.05,.2,14.0)
    # Baked, directional crosshatch gives the composite a visible woven-fibre surface.
    if tier=='Composite':
        uv=np.linspace(0,2*pi*36,512,endpoint=False,dtype=np.float32)
        yy,zz=np.meshgrid(uv,uv)
        weave=(np.sin(yy+0.48*np.sin(zz))**8 + np.sin(zz+0.48*np.sin(yy))**8)
        variation=.88+.14*weave
        c=np.array([.075,.085,.095],dtype=np.float32)
        rgba=np.concatenate((srgb(c[None,None,:]*variation[:,:,None]),np.ones((512,512,1))),axis=2).astype(np.float32)
        im=bpy.data.images.new('Charcoal fibre weave',width=512,height=512,alpha=True); im.pixels.foreach_set(rgba.ravel()); im.pack()
        tex=polymer.node_tree.nodes.new('ShaderNodeTexImage'); tex.image=im; bsdf=polymer.node_tree.nodes.get('Principled BSDF'); polymer.node_tree.links.new(tex.outputs['Color'],bsdf.inputs['Base Color'])
        bump=polymer.node_tree.nodes.new('ShaderNodeBump'); bump.inputs['Strength'].default_value=.14; bump.inputs['Distance'].default_value=.0015; polymer.node_tree.links.new(tex.outputs['Color'],bump.inputs['Height']); polymer.node_tree.links.new(bump.outputs['Normal'],bsdf.inputs['Normal'])
    base={'Wood':[bake_surface('Wood',size,h)],'Steel':[bake_surface('Steel',size,h)],'Composite':[polymer],'Plasteel':[seam]}[tier]
    if tier in ('Wood','Steel'):
        for rough in ((.79,.85) if tier=='Wood' else (.45,.51)):
            variant=base[0].copy(); variant.node_tree.nodes.get('Principled BSDF').inputs['Roughness'].default_value=rough; base.append(variant)
    rim={'Wood':edge,'Steel':steel,'Composite':trim,'Plasteel':seam}[tier]
    verts=[]; faces=[]; ids=[]; thick={'Wood':.024,'Steel':.012,'Composite':.019,'Plasteel':.017}[tier]
    # Front and back gridded shell. Per-column assignments give real plank boards.
    for side in (0,1):
        for iz in range(n+1):
            z=-h/2+h*iz/n; w=max(.001,width_at(size,z))
            for iy in range(ny+1):
                y=-w+2*w*iy/ny
                x=surf(y,size)-(thick if side else 0)
                if side and tier=='Composite':
                    for fc in (-.62,-.31,0,.31,.62):
                        x-=.006*math.exp(-((y/max(.001,width_at(size,z))-fc)/.065)**2)
                verts.append((x,y,z))
    row=ny+1; layer=(n+1)*row
    for side in (0,1):
        for iz in range(n):
            for iy in range(ny):
                
                a=side*layer+iz*row+iy; quad=(a,a+1,a+1+row,a+row)
                faces.append(quad if side==0 else tuple(reversed(quad))); ids.append(0)
    for iz in range(n):
        for iy in (0,ny):
            a=iz*row+iy; b=(iz+1)*row+iy; faces.append((a,b,b+layer,a+layer) if iy==0 else (b,a,a+layer,b+layer)); ids.append(0)
    for iz in (0,n):
        for iy in range(ny):
            a=iz*row+iy; faces.append((a,a+layer,a+layer+1,a+1) if iz==0 else (a+1,a+1+layer,a+layer,a)); ids.append(0)
    body=mesh('Shield shell',verts,faces,base,ids)
    # Smooth the curved face; retain flat side walls.
    for poly in body.data.polygons: poly.use_smooth=(abs(poly.normal.x)>.5)
    if tier in ('Wood','Steel','Composite'):
        uv=body.data.uv_layers.new(name='Baked surface')
        wmax={'Small':.25,'Medium':.30,'Tower':.325}[size]
        for poly in body.data.polygons:
            for li in poly.loop_indices:
                co=body.data.vertices[body.data.loops[li].vertex_index].co
                uv.data[li].uv=(co.y/(2*wmax)+.5,co.z/h+.5)
    # perimeter follows outline in vertical plane
    pts=[]
    for iz in range(n+1):
        z=-h/2+h*iz/n; y=-max(.001,width_at(size,z)); pts.append((surf(y,size)+.002,y,z))
    for iz in reversed(range(n+1)):
        z=-h/2+h*iz/n; y=max(.001,width_at(size,z)); pts.append((surf(y,size)+.002,y,z))
    if size=='Small':
        pts=[(surf(.25*cos(2*pi*i/32),size)+.002,.25*cos(2*pi*i/32),.25*sin(2*pi*i/32)) for i in range(32)]
    tube('Bound perimeter',pts,.009 if tier in ('Wood','Steel') else .014 if tier=='Composite' else .010,rim,True,6)
    if tier in ('Wood','Steel'):
        wear=mat('Polished edge wear',(.42,.40,.34) if tier=='Wood' else (.53,.57,.59),.8,.28)
        highlight=[(x+.007,y*.995,z*.995) for x,y,z in pts]
        tube('Worn perimeter highlight',highlight,.0017,wear,True,4)
    if tier in ('Wood','Steel'):
        # Raised forged boss and its collar on smaller shields.
        if True:
            rad=.105 if size=='Small' else .09
            ball('Forged central boss',(surf(0,size)+.032,0,.015),(.025,rad,rad),edge if tier=='Wood' else steel,16,8)
            ring=[(surf(0,size)+.013,rad*1.02*cos(2*pi*i/24),.015+rad*1.02*sin(2*pi*i/24)) for i in range(24)]
            tube('Boss collar',ring,.006,edge,True)
            tube('Worn boss edge',[(x+.005,y,z) for x,y,z in ring],.0018,wear,True,4)
        for i in range(0,len(pts),4 if size=='Small' else 6):
            x,y,z=pts[i]; ball('Rim rivet',(x+.012,y*.955,z*.975),(.006,.010,.010),edge,8,4)
    if tier=='Plasteel':
        # Individually chamfered ceramic tiles sit above the gunmetal shell, leaving open recessed channels.
        if size=='Small': rows=[-.215,-.075,.075,.215]
        else: rows=[-h*.43,-h*.14,h*.14,h*.43]
        fcuts=[-.82,-.28,.28,.82]
        for rz in range(len(rows)-1):
            z0,z1=rows[rz],rows[rz+1]
            for fy in range(len(fcuts)-1):
                panel_patch('Chamfered ceramic plate',size,z0,z1,fcuts[fy],fcuts[fy+1],ceramic,.014 if size!='Small' else .011,.012)
        # Glowing traces sit at the bottom of the exposed plate channels.
        for z in rows[1:-1]:
            pts=[]
            for k in range(17):
                f=-.84+1.68*k/16; y=width_at(size,z)*f; pts.append((y,z))
            emit_channel('Cyan horizontal channel',size,pts,cyan,.0026)
        for f in fcuts[1:-1]:
            pts=[]
            for k in range(25):
                z=-h*.43+h*.86*k/24; pts.append((width_at(size,z)*f,z))
            emit_channel('Cyan vertical channel',size,pts,cyan,.0024)
        ball('Luminous central node',(surf(0,size)+.016,0,0),(.008,.021,.021),cyan,16,8)
    if tier=='Composite' and size=='Tower':
        glass=mat('Smoked transparent viewport',(.15,.25,.24),.1,.16,alpha=.42)
        window=[(surf(-.108,size)+.002,-.108,.3),(surf(.108,size)+.002,.108,.3),(surf(.108,size)+.002,.108,.375),(surf(-.108,size)+.002,-.108,.375)]
        mesh('Viewport glazing',window,[(0,1,2,3)],glass)
        tube('Viewport gasket',window,.006,trim,True,6)
        cube('Viewport upper frame',(surf(0,size)+.010,0,.386),(.018,.235,.018),trim,.004)
        cube('Viewport lower frame',(surf(0,size)+.010,0,.289),(.018,.235,.018),trim,.004)
        cube('Viewport left frame',(surf(-.116,size)+.010,-.116,.337),(.018,.016,.105),trim,.004)
        cube('Viewport right frame',(surf(.116,size)+.010,.116,.337),(.018,.016,.105),trim,.004)
        cube('Light mounting shoe',(surf(.145,size)+.012,.145,.337),(.035,.052,.045),edge,.006)
        light=mat('Viewport task light',(.65,.91,1),.05,.2,5.5)
        ball('Viewport task light emitter',(surf(.145,size)+.034,.145,.337),(.012,.018,.018),light,12,6)
    if tier=='Composite':
        for z in (-.07,.07):
            for y in (.078,.102): ball('Composite handle face bolt',(surf(y,size)+.005,y,z),(.006,.006,.007),edge,10,5)
    # Forearm runs along Y; grip is vertical and the arched band is 25 cm toward the elbow.
    mount=leather if tier in ('Wood','Steel') else trim
    for z in (-.07,.07):
        cube('Grip mounting plate',(-.025,.09,z),(.018,.045,.035),edge if tier in ('Wood','Steel') else trim,.004)
    tube('Hand grip',[(-.035,.09,-.07),(-.085,.09,-.055),(-.085,.09,.055),(-.035,.09,.07)],.012,mount,False,8)
    strap=[]
    for i in range(13):
        a=pi*i/12; strap.append((-.025-.068*sin(a),-.16,-.065*cos(a)))
    vs=[(x,y+dy,z) for x,y,z in strap for dy in (-.027,.027)]
    band=mesh('Broad forearm strap',vs,[(2*i,2*i+1,2*i+3,2*i+2) for i in range(12)],mount)
    sol=band.modifiers.new('Strap thickness','SOLIDIFY'); sol.thickness=.005
    bpy.context.view_layer.objects.active=band; bpy.ops.object.modifier_apply(modifier=sol.name)
    for z in (-.065,.065): ball('Strap fastener',(-.028,-.16,z),(.008,.012,.012),edge,8,4)
    # Apply object transforms while preserving the common shield origin.
    obs=list(bpy.context.scene.objects)
    for ob in obs:
        if ob.type=='MESH':
            bpy.context.view_layer.objects.active=ob; ob.select_set(True); bpy.ops.object.transform_apply(location=True,rotation=True,scale=True); ob.select_set(False)
    # Detect coincident polygon faces across all mesh objects after transforms.
    seen={}; duplicates=[]
    for ob in obs:
        if ob.type!='MESH': continue
        for poly in ob.data.polygons:
            key=tuple(sorted(tuple(round(v,6) for v in ob.data.vertices[i].co) for i in poly.vertices))
            if key in seen: duplicates.append((ob.name,poly.index,seen[key]))
            else: seen[key]=(ob.name,poly.index)
    print('COINCIDENT_FACE_CHECK',tier,size,'duplicates',len(duplicates),duplicates[:5],flush=True)
    triangles=sum(sum(len(p.vertices)-2 for p in ob.data.polygons) for ob in obs if ob.type=='MESH')
    # One object per shield (its parts' materials kept as slots), so Unreal imports it as one static mesh.
    bpy.ops.object.select_all(action='DESELECT')
    parts=[ob for ob in bpy.context.scene.objects if ob.type in ('MESH','CURVE')]
    for ob in parts: ob.select_set(True)
    bpy.context.view_layer.objects.active=parts[0]
    bpy.ops.object.convert(target='MESH')
    bpy.ops.object.join()
    joined=bpy.context.view_layer.objects.active
    while len(joined.data.uv_layers)>1: joined.data.uv_layers.remove(joined.data.uv_layers[-1])  # only UV0: the extra map was all one point
    joined=bpy.context.view_layer.objects.active; joined.name='SM_Shield_%s_%s'%(tier,size)
    bpy.ops.object.transform_apply(location=True,rotation=True,scale=True)  # origin at the middle of the shield
    path=OUT/('SM_Shield_%s_%s.glb'%(tier,size))
    bpy.ops.export_scene.gltf(filepath=str(path),export_format='GLB',use_selection=True,export_yup=True)
    results.append((path.name,path.stat().st_size/1024,triangles))
    if not PREVIEWS: return
    # Presentation only, after export.
    scene=bpy.context.scene; scene.render.engine='CYCLES' if False else 'BLENDER_EEVEE'; scene.render.resolution_x=512; scene.render.resolution_y=512; scene.render.resolution_percentage=100
    scene.render.image_settings.file_format='PNG'; scene.render.film_transparent=False
    scene.world.color=(.23,.23,.23)
    world=scene.world; world.use_nodes=True; world.node_tree.nodes.get('Background').inputs['Color'].default_value=(.19,.19,.19,1); world.node_tree.nodes.get('Background').inputs['Strength'].default_value=.8
    def area(loc,power,size):
        bpy.ops.object.light_add(type='AREA',location=loc); o=bpy.context.object; o.data.energy=power; o.data.shape='DISK'; o.data.size=size; o.rotation_euler=(Vector((0,0,0))-o.location).to_track_quat('-Z','Y').to_euler()
    area((2,-3,4),450,4); area((3,3,1.5),250,3); area((-2,1,3),300,2)
    bpy.ops.object.camera_add(location=(3.5,-2.25,.8)); cam=bpy.context.object; cam.rotation_euler=(Vector((0,0,0))-cam.location).to_track_quat('-Z','Y').to_euler(); cam.data.type='ORTHO'; cam.data.ortho_scale=h*1.35; scene.camera=cam
    scene.render.filepath=str(OUT/('SM_Shield_%s_%s.png'%(tier,size))); scene.render.image_settings.color_mode='RGBA'; bpy.ops.render.render(write_still=True)
    if tier=='Steel' and size=='Medium':
        cam.location=(-3.5,-2.0,.75); cam.rotation_euler=(Vector((0,0,0))-cam.location).to_track_quat('-Z','Y').to_euler()
        scene.render.filepath=str(OUT/'back.png'); bpy.ops.render.render(write_still=True)

for t in ('Wood','Steel','Composite','Plasteel'):
    for s in ('Small','Medium','Tower'): build(t,s)

# Compose four tier rows and three size columns from the finished previews.
if not PREVIEWS:
    for name,kb,tris in results: print('{:<38} {:>9.1f} {:>10}'.format(name,kb,tris),flush=True)
    sys.exit(0)
import numpy as np
sheet=np.empty((2048,1536,4),dtype=np.float32)
for r,t in enumerate(('Wood','Steel','Composite','Plasteel')):
    for c,s in enumerate(('Small','Medium','Tower')):
        im=bpy.data.images.load(str(OUT/('SM_Shield_%s_%s.png'%(t,s))),check_existing=False)
        pixels=np.empty(512*512*4,dtype=np.float32); im.pixels.foreach_get(pixels)
        sheet[(3-r)*512:(4-r)*512,c*512:(c+1)*512,:]=pixels.reshape(512,512,4)
        bpy.data.images.remove(im)
im=bpy.data.images.new('Contact sheet',width=1536,height=2048,alpha=True)
im.pixels.foreach_set(sheet.ravel()); im.filepath_raw=str(OUT/'sheet.png'); im.file_format='PNG'; im.save()
print('Contact sheet saved:',OUT/'sheet.png',flush=True)


print('\n{:<38} {:>9} {:>10}'.format('File','Size KB','Triangles'))
for name,kb,tris in results: print('{:<38} {:>9.1f} {:>10}'.format(name,kb,tris),flush=True)
