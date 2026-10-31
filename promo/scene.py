"""宣传片 3D 镜头：从 case/外壳预览.blend 取装配好的外壳，搭苹果风影棚，屏幕贴板子实录画面，按镜头渲染 PNG 序列。

blender -b --factory-startup --python promo/scene.py -- <镜头名> [--still 帧号] [--save 场景.blend]
输出：promo/render/<镜头名>/####.png（--still 时输出 promo/render/<镜头名>_still.png）
"""
import math
import os
import sys

import bpy
from mathutils import Euler, Matrix, Vector

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
args = sys.argv[sys.argv.index('--') + 1:]
SHOT = args[0]
STILL = int(args[args.index('--still') + 1]) if '--still' in args else None
SAVE = args[args.index('--save') + 1] if '--save' in args else None

FPS = 30
TILT = Euler((math.radians(75), 0, 0))   # 机身坐标 → 世界：导入时 Y 朝上转 Z 朝上（+90°），整机后仰 15°
FACE = TILT.to_matrix() @ Vector((0, 0, 1))   # 屏幕法线（朝观众）
S = 0.001                                     # 外壳模型是毫米，这里统一换成米


def body(x, y, z):
    """机身坐标（毫米，X 右、Y 上、Z 朝观众）→ 世界坐标（米）"""
    return TILT.to_matrix() @ Vector((x, y, z)) * S


# ---------------- 场景 ----------------
for o in list(bpy.data.objects):
    bpy.data.objects.remove(o)
sc = bpy.context.scene
sc.render.engine = 'BLENDER_EEVEE'
sc.render.resolution_x, sc.render.resolution_y = 1920, 1080
sc.render.fps = FPS
sc.render.film_transparent = False
sc.eevee.taa_render_samples = 32
sc.render.filter_size = float(os.environ.get('FILTER', 0.9))  # 默认 1.5 会把亮底上的深色细字抹淡
for k, v in (('use_raytracing', True), ('use_shadows', True), ('use_gtao', True)):
    if hasattr(sc.eevee, k):
        setattr(sc.eevee, k, v)
if hasattr(sc.eevee, 'ray_tracing_options'):
    sc.eevee.ray_tracing_options.resolution_scale = '1'
sc.view_settings.view_transform = 'AgX'
try:
    sc.view_settings.look = 'AgX - Medium High Contrast'
except TypeError:
    pass

world = bpy.data.worlds.new('黑')
world.use_nodes = True
world.node_tree.nodes['Background'].inputs[0].default_value = (0.0, 0.0, 0.0, 1)
world.node_tree.nodes['Background'].inputs[1].default_value = 0
sc.world = world


def mat(name, color, rough, coat=0.0, spec=0.5):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    p = m.node_tree.nodes['Principled BSDF']
    p.inputs['Base Color'].default_value = color
    p.inputs['Roughness'].default_value = rough
    p.inputs['Specular IOR Level'].default_value = spec
    p.inputs['Coat Weight'].default_value = coat
    return m


# 外壳：从预览 .blend 里拿「装配效果」的四个零件
with bpy.data.libraries.load(os.path.join(ROOT, 'case', '外壳预览.blend')) as (src, dst):
    dst.objects = [n for n in src.objects if n.startswith('装配-')]
M_BLACK = mat('黑 PLA', (0.012, 0.012, 0.014, 1), 0.38, coat=0.15)
M_GREY = mat('深灰 PLA', (0.018, 0.019, 0.023, 1), 0.45, coat=0.1)
parts = {}
for o in dst.objects:
    o.data = o.data.copy()
    o.data.transform(Matrix.Scale(S, 4) @ o.matrix_basis)  # 库里载入的物体还没进场景，matrix_world 没算过
    o.matrix_basis = Matrix.Identity(4)
    o.data.materials.clear()
    key = o.name.split('-')[1]
    o.data.materials.append(M_BLACK if key in ('前框', '支架') else M_GREY)
    for poly in o.data.polygons:
        poly.use_smooth = False
    sc.collection.objects.link(o)
    parts[key] = o

# 屏幕模组：玻璃（黑）+ 显示区（贴实录画面）+ 模组和 PCB（爆炸镜头里看得到）
def plane(name, w, h, center, material):
    bpy.ops.mesh.primitive_plane_add(size=1)
    o = bpy.context.object
    o.name = name
    o.scale = (w * S, h * S, 1)
    o.rotation_euler = TILT
    o.location = center
    o.data.materials.append(material)
    return o


def box(name, w, h, d, center, material):
    bpy.ops.mesh.primitive_cube_add(size=1)
    o = bpy.context.object
    o.name = name
    o.scale = (w * S, h * S, d * S)
    o.rotation_euler = TILT
    o.location = center
    o.data.materials.append(material)
    return o


M_GLASS = mat('玻璃', (0.003, 0.003, 0.004, 1), 0.04, coat=1.0, spec=0.8)
M_PCB = mat('PCB', (0.02, 0.022, 0.02, 1), 0.5)

# 显示区：发光贴图 + 一层亮面，读得出是玻璃
M_SCREEN = bpy.data.materials.new('屏幕')
M_SCREEN.use_nodes = True
nt = M_SCREEN.node_tree
p = nt.nodes['Principled BSDF']
p.inputs['Base Color'].default_value = (0, 0, 0, 1)
p.inputs['Roughness'].default_value = 0.05
p.inputs['Coat Weight'].default_value = 1.0
p.inputs['Coat Roughness'].default_value = 0.03
tex = nt.nodes.new('ShaderNodeTexImage')
tex.interpolation = 'Closest'  # 像素风：放大不糊
nt.links.new(tex.outputs['Color'], p.inputs['Emission Color'])
p.inputs['Emission Strength'].default_value = 0.0

glass_c = body(0.4, 1.0, 0.15)
disp_c = body(0.4, 0.0, 0.3)
screen_parts = [
    plane('玻璃', 105.8, 67.6, glass_c, M_GLASS),
    plane('显示区', 95.04, 53.86, disp_c, M_SCREEN),
    box('屏幕模组', 105.0, 67.0, 4.6, body(0.4, 1.0, -2.3), M_PCB),
    box('PCB', 120.0, 70.0, 1.6, body(0, 1.5, -5.8), M_PCB),
]

# 地面：深色哑光，不反出灯（亮面会把背后的轮廓灯映成一块白斑）；远处靠灯光衰减自然没入黑色
floor_z = min((o.matrix_world @ Vector(c)).z for c in parts['支架'].bound_box) if '支架' in parts else -0.07
bpy.ops.mesh.primitive_plane_add(size=6, location=(0, 0, floor_z))
floor = bpy.context.object
floor.data.materials.append(mat('地面', (0.004, 0.004, 0.005, 1), 0.7, spec=0.05))


# ---------------- 灯光 ----------------
def area(name, loc, target, size, power, color=(1, 1, 1), shape='RECTANGLE', size_y=None):
    d = bpy.data.lights.new(name, 'AREA')
    d.shape = shape
    d.size = size
    d.size_y = size_y or size
    d.energy = power
    d.color = color
    o = bpy.data.objects.new(name, d)
    sc.collection.objects.link(o)
    o.location = loc
    o.rotation_euler = (Vector(target) - Vector(loc)).to_track_quat('-Z', 'Y').to_euler()
    return o


KEY = area('顶光', (0.05, -0.15, 0.55), (0, 0, 0), 0.9, 6, (1, 0.98, 0.96))
RIM_L = area('左轮廓', (-0.42, 0.32, 0.12), (0, 0, 0), 0.05, 4, (0.78, 0.86, 1.0), size_y=0.7)
RIM_R = area('右轮廓', (0.42, 0.32, 0.12), (0, 0, 0), 0.05, 4, (1.0, 0.9, 0.82), size_y=0.7)
FILL = area('补光', (-0.3, -0.55, 0.05), (0, 0, 0), 0.6, 0.6)
LIGHTS = (KEY, RIM_L, RIM_R, FILL)

# ---------------- 相机 ----------------
cam_d = bpy.data.cameras.new('相机')
cam_d.lens = 70
cam_d.clip_start = 0.01
cam_d.dof.use_dof = True
cam_d.dof.aperture_fstop = 4.0
cam = bpy.data.objects.new('相机', cam_d)
sc.collection.objects.link(cam)
sc.camera = cam
aim = bpy.data.objects.new('焦点', None)
sc.collection.objects.link(aim)
aim.location = (0, 0, 0.0)
tc = cam.constraints.new('TRACK_TO')
tc.target = aim
tc.track_axis = 'TRACK_NEGATIVE_Z'
tc.up_axis = 'UP_Y'
cam_d.dof.focus_object = aim


def orbit(az, el, dist, center=(0, 0, 0)):
    """方位角 az：0 = 正对屏幕（-Y 方向），正值往右转；仰角 el（度）"""
    a, e = math.radians(az), math.radians(el)
    c = Vector(center)
    return c + Vector((math.sin(a) * math.cos(e), -math.cos(a) * math.cos(e), math.sin(e))) * dist


def key(obj, path, frame, value, ease='SINE'):
    setattr(obj, path, value)
    obj.keyframe_insert(path, frame=frame)


def camera_path(frames, f0, f1, a0, a1, focus0=(0, 0, 0), focus1=None, lens0=70, lens1=None):
    """(az, el, dist) 从 a0 平滑过渡到 a1，逐帧打关键帧（球面插值，避免直线穿过模型）"""
    focus1 = focus1 or focus0
    lens1 = lens1 or lens0
    for f in range(f0, f1 + 1):
        t = (f - f0) / max(1, f1 - f0)
        t = t * t * (3 - 2 * t)  # smoothstep：缓入缓出
        az, el, d = (a0[i] + (a1[i] - a0[i]) * t for i in range(3))
        fc = Vector(focus0).lerp(Vector(focus1), t)
        cam.location = orbit(az, el, d, fc)
        cam.keyframe_insert('location', frame=f)
        aim.location = fc
        aim.keyframe_insert('location', frame=f)
        cam_d.lens = lens0 + (lens1 - lens0) * t
        cam_d.keyframe_insert('lens', frame=f)


def screen_video(name, start=1, offset=0):
    path = os.path.join(HERE, 'tex', name + '.mp4')
    img = bpy.data.images.load(path)
    img.source = 'MOVIE'
    tex.image = img
    iu = tex.image_user
    iu.frame_duration = img.frame_duration
    iu.frame_start = start
    iu.frame_offset = offset
    iu.use_cyclic = True
    iu.use_auto_refresh = True


def screen_power(frames):
    """[(帧, 亮度)]：屏幕点亮 / 熄灭"""
    s = p.inputs['Emission Strength']
    for f, v in frames:
        s.default_value = v
        s.keyframe_insert('default_value', frame=f)


SCREEN_ON = 3.2
SCREEN_CENTER = disp_c


def lights_fade(f0, f1):
    for L in LIGHTS:
        e = L.data.energy
        L.data.energy = 0
        L.data.keyframe_insert('energy', frame=f0)
        L.data.energy = e
        L.data.keyframe_insert('energy', frame=f1)


def explode(f_out, f_in):
    """f_out 时零件散开，f_in 时装好：前框往前、屏幕模组往前一点、后盖往后、支架往下"""
    offs = {'前框': 0.08, '后盖': -0.09}
    for k, d in offs.items():
        o = parts[k]
        o.location = FACE * d
        o.keyframe_insert('location', frame=f_out)
        o.location = (0, 0, 0)
        o.keyframe_insert('location', frame=f_in)
    for o in screen_parts:
        base = o.location.copy()
        o.location = base + FACE * 0.05
        o.keyframe_insert('location', frame=f_out)
        o.location = base
        o.keyframe_insert('location', frame=f_in)
    st = parts['支架']
    st.location = (0, 0, -0.07)
    st.keyframe_insert('location', frame=f_out)
    st.location = (0, 0, 0)
    st.keyframe_insert('location', frame=f_in)


# ---------------- 镜头 ----------------
C = (0, 0.005, 0.0)  # 整机大致中心
SC = tuple(SCREEN_CENTER)

SHOTS = {}


def shot(n):
    def deco(fn):
        SHOTS[n] = fn
        return fn
    return deco


@shot('reveal')  # 黑场里亮起轮廓光，镜头从侧面绕到正面，屏幕点亮
def _():
    lights_fade(1, 70)
    camera_path(None, 1, 180, (-75, 4, 0.42), (-12, 8, 0.38), C, C, 85, 70)
    screen_video('home_idle')
    screen_power([(1, 0), (120, 0), (150, SCREEN_ON)])
    return 180


def opener(n, on):
    """开场：reveal + hero 合成一个连续运镜，从侧面亮起一路绕到 3/4 视角，中途不回头"""
    lights_fade(1, 60)
    camera_path(None, 1, n, (-75, 4, 0.42), (30, 10, 0.48), C, C, 85, 75)
    screen_video('home_claude')
    screen_power([(1, 0), (on, 0), (on + 30, SCREEN_ON)])
    return n


SHOTS['opener'] = lambda: opener(285, 75)         # 短版 9.5 秒
SHOTS['opener_full'] = lambda: opener(420, 120)   # 完整版 14 秒


@shot('hero')  # 3/4 视角慢慢环绕
def _():
    camera_path(None, 1, 240, (-35, 14, 0.52), (30, 10, 0.48), C, C, 70, 75)
    screen_video('home_claude')
    screen_power([(1, SCREEN_ON)])
    return 240


@shot('assemble')  # 零件从散开到合拢，扣上后屏幕点亮
def _():
    explode(30, 170)
    camera_path(None, 1, 240, (-50, 20, 0.62), (-25, 12, 0.50), C, C, 60, 65)
    screen_video('home_idle')
    screen_power([(1, 0), (185, 0), (210, SCREEN_ON)])
    return 240


@shot('back')  # 绕到背后：后盖、散热缝、侧面开口
def _():
    camera_path(None, 1, 180, (125, 16, 0.55), (200, 12, 0.52), C, C, 70)
    screen_video('home_idle')
    screen_power([(1, SCREEN_ON)])
    return 180


@shot('side')  # 侧面低角度：后仰 15° 的姿态
def _():
    camera_path(None, 1, 150, (88, 2, 0.42), (65, 6, 0.40), C, C, 85)
    screen_video('home_idle')
    screen_power([(1, SCREEN_ON)])
    return 150


@shot('finale')  # 正面拉远，留出放标题的空间
def _():
    camera_path(None, 1, 210, (0, 6, 0.30), (0, 9, 0.62), SC, (0, 0, 0.02), 70, 70)
    screen_video('home_idle')
    screen_power([(1, SCREEN_ON)])
    return 210


@shot('turn')  # 360° 转台
def _():
    camera_path(None, 1, 300, (-180, 12, 0.56), (180, 12, 0.56), C, C, 70)
    screen_video('home_claude')
    screen_power([(1, SCREEN_ON)])
    return 300


@shot('lowfront')  # 低机位从右往左平移，屏幕上是分屏
def _():
    camera_path(None, 1, 360, (32, 0, 0.52), (-28, 4, 0.50), C, C, 85, 85)
    screen_video('home_split')
    screen_power([(1, SCREEN_ON)])
    return 360


FEATURE_FSTOP = float(os.environ.get("FSTOP", 11))
FEATURE_SCREEN = 2.4  # 近景屏幕暗一点：太亮时浅色底上的深色字会被抹淡


def feature(name, side):
    def fn():
        a = 14 * side
        camera_path(None, 1, 120, (a, 6, 0.27), (a * 0.4, 4, 0.22), SC, SC, 70, 75)
        cam_d.dof.aperture_fstop = FEATURE_FSTOP  # 屏幕斜着拍，f/4 景深太浅，边上的字会糊
        screen_video(name)
        screen_power([(1, FEATURE_SCREEN)])
        return 120
    return fn


for i, n in enumerate(('buddy_claude', 'buddy_codex', 'buddy_deepseek', 'home_split', 'magpie_agent', 'magpie_usage',
                       'magpie_route', 'radar', 'standby', 'welcome_back', 'settings_sound', 'settings_pc')):
    SHOTS['f_' + n] = feature(n, 1 if i % 2 else -1)

end = SHOTS[SHOT]()
sc.frame_start, sc.frame_end = 1, end

if SAVE:
    bpy.ops.wm.save_as_mainfile(filepath=SAVE)
out = os.path.join(HERE, 'render', SHOT)
if STILL is not None:
    sc.frame_set(STILL)
    sc.render.filepath = out + f'_still{STILL}.png'
    bpy.ops.render.render(write_still=True)
else:
    os.makedirs(out, exist_ok=True)
    sc.render.image_settings.file_format = 'PNG'
    sc.render.filepath = out + '/'
    bpy.ops.render.render(animation=True)
