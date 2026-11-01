"""在 Blender 界面里摆好几个视角，截下整个窗口（宣传片「自己画外壳」镜头用）。

/Applications/Blender.app/Contents/MacOS/Blender case/外壳预览.blend --window-geometry 0 0 1920 1080 \
    --python promo/blender_shots.py   → promo/blender/<序号>.png，截完自动退出
"""
import math
import os

import bpy
from mathutils import Euler

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'blender')
os.makedirs(OUT, exist_ok=True)

# (文件名, 要框住的对象前缀, 视角欧拉角（度）, 着色, X 光, 拉远倍数)
SHOTS = [
    ('1-all', ('装配', '爆炸', '打印'), (62, 0, 28), 'SOLID', False, 0.6),
    ('2-explode', ('爆炸',), (72, 0, 52), 'SOLID', False, 1.15),
    ('3-xray', ('装配',), (70, 0, -35), 'WIREFRAME', True, 1.25),
    ('4-print', ('打印',), (25, 0, 0), 'SOLID', False, 0.7),
]


def view3d():
    win = bpy.context.window_manager.windows[0]
    areas = [a for a in win.screen.areas if a.type == 'VIEW_3D']
    area = max(areas, key=lambda a: a.width * a.height)
    region = next(r for r in area.regions if r.type == 'WINDOW')
    return win, area, region


def setup(shot):
    name, prefixes, rot, shading, xray, zoom = shot
    win, area, region = view3d()
    space = area.spaces.active
    space.shading.type = shading
    space.shading.show_xray = xray
    space.shading.show_xray_wireframe = xray
    space.overlay.show_floor = True
    space.overlay.show_axis_x = space.overlay.show_axis_y = True
    rv = space.region_3d
    if rv.view_perspective == 'CAMERA':
        rv.view_perspective = 'PERSP'
    bpy.ops.object.select_all(action='DESELECT') if bpy.context.mode == 'OBJECT' else None
    for o in bpy.data.objects:
        o.select_set(o.name.startswith(prefixes))
    rv.view_rotation = Euler([math.radians(v) for v in rot]).to_quaternion()
    with bpy.context.temp_override(window=win, area=area, region=region):
        bpy.ops.view3d.view_selected()
    rv.view_distance *= zoom
    # 选中描边留着：看得出是在软件里操作；活动对象设成第一个
    sel = [o for o in bpy.data.objects if o.select_get()]
    if sel:
        bpy.context.view_layer.objects.active = sel[0]


def shoot(name):
    win, area, region = view3d()
    with bpy.context.temp_override(window=win):
        bpy.ops.screen.screenshot(filepath=os.path.join(OUT, name + '.png'))


state = {'i': 0, 'phase': 0}


def tick():
    i = state['i']
    if i >= len(SHOTS):
        bpy.ops.wm.quit_blender()
        return None
    if state['phase'] == 0:
        setup(SHOTS[i])
        state['phase'] = 1
        return 1.5  # 等视图重画完
    shoot(SHOTS[i][0])
    print('SHOT', SHOTS[i][0], flush=True)
    state['i'] += 1
    state['phase'] = 0
    return 0.3


bpy.app.timers.register(tick, first_interval=3.0)
