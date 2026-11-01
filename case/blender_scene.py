"""在 Blender 里搭外壳预览场景并存成 .blend：装配效果、爆炸图、打印摆放三组。

由 model.py 在后台调用，不用手动跑：
  blender --background --factory-startup --python blender_scene.py -- <装配姿态 STL 目录> <stl/ 目录> <输出 .blend>
装配姿态的 STL 是 model.py 临时导出的（机身坐标 Y 朝上、Z 朝观众，整机已后仰）。
"""
import math
import os
import sys

import bmesh
import bpy

asm_dir, stl_dir, out = sys.argv[sys.argv.index('--') + 1:][:3]

for o in list(bpy.data.objects):
    bpy.data.objects.remove(o)
sc = bpy.context.scene
sc.unit_settings.system = 'METRIC'
sc.unit_settings.scale_length = 0.001
sc.unit_settings.length_unit = 'MILLIMETERS'

COLORS = {'前框': (0.04, 0.04, 0.05, 1), '支架': (0.04, 0.04, 0.05, 1),
          '机身': (0.16, 0.16, 0.2, 1), '后盖': (0.22, 0.22, 0.27, 1)}


def material(name):
    key = next(k for k in COLORS if k in name)
    m = bpy.data.materials.get(key) or bpy.data.materials.new(key)
    m.diffuse_color = COLORS[key]
    m.roughness = 0.6
    return m


def collection(name):
    c = bpy.data.collections.new(name)
    sc.collection.children.link(c)
    return c


def load(path, coll, name, loc=(0, 0, 0), **axes):
    bpy.ops.wm.stl_import(filepath=path, **axes)
    o = bpy.context.selected_objects[0]
    o.name = name
    o.location = loc
    # STL 全是三角面，大平面上会拉出一堆细长三角形；有限融并把共面的三角形合成大面，编辑模式里看着干净
    bm = bmesh.new()
    bm.from_mesh(o.data)
    bmesh.ops.dissolve_limit(bm, angle_limit=math.radians(1), verts=bm.verts, edges=bm.edges)
    bm.to_mesh(o.data)
    bm.free()
    o.data.materials.append(material(name))
    for c in o.users_collection:
        c.objects.unlink(o)
    coll.objects.link(o)
    return o


# 1. 装配效果（机身坐标 Y 朝上、Z 朝观众 → Blender Z 朝上、-Y 朝观众）
parts = ('前框', '机身', '后盖', '支架')
c1 = collection('装配效果')
for n in parts:
    load(os.path.join(asm_dir, n + '.stl'), c1, '装配-' + n, forward_axis='NEGATIVE_Z', up_axis='Y')

# 2. 爆炸图：放在右边，前框往前、后盖往后、支架往下
c2 = collection('爆炸图')
offs = {'前框': (0, -60, 0), '机身': (0, 0, 0), '后盖': (0, 70, 0), '支架': (0, 0, -45)}
for n in parts:
    x, y, z = offs[n]
    load(os.path.join(asm_dir, n + '.stl'), c2, '爆炸-' + n, (220 + x, y, z), forward_axis='NEGATIVE_Z', up_axis='Y')

# 3. 打印摆放：stl/ 里的文件原样导入，排在后面一排
c3 = collection('打印摆放')
for i, f in enumerate(sorted(f for f in os.listdir(stl_dir) if f.endswith('.stl'))):
    load(os.path.join(stl_dir, f), c3, '打印-' + os.path.splitext(f)[0], (i * 150 - 100, 220, -60))

# 视图：实体着色按材质色，对准装配效果和爆炸图（不带灯光，以免 Home 框住太大范围）
for screen in bpy.data.screens:
    for area in screen.areas:
        if area.type != 'VIEW_3D':
            continue
        sp = area.spaces.active
        sp.shading.type = 'SOLID'
        sp.shading.color_type = 'MATERIAL'
        sp.shading.show_cavity = True
        sp.clip_start = 0.1
        sp.clip_end = 100000
        sp.overlay.grid_scale = 0.001
        r3d = sp.region_3d
        r3d.view_location = (110, 0, 0)
        r3d.view_distance = 520
for o in bpy.data.objects:
    o.select_set(False)
bpy.context.preferences.filepaths.save_version = 0   # 不留 .blend1 备份
bpy.ops.wm.save_as_mainfile(filepath=out, compress=True)
