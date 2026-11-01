"""Agent Buddy 外壳（像素风小电视）参数化模型：manifold3d 生成各零件 STL + 预览图。

坐标（机身坐标系）：X 向右，Y 向上，Z 朝向观众；机身正面在 z = 0，背面在 z = -DEPTH。
单位毫米。运行：.venv/bin/python model.py  → 输出到 stl/ 和 preview/
"""
import glob
import math
import os
import shutil
import subprocess
import tempfile

import numpy as np
from manifold3d import CrossSection, JoinType, Manifold
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))

# ---------------- 尺寸（改这里）----------------
BOARD_W, BOARD_H = 120.0, 70.0      # 板子外形（含安装耳）
HOLE_DX, HOLE_DY = 113.0, 62.0      # 安装孔中心距：短边方向按实物照片从 60 改 62（第一版打出来窄了），前框定位销插这四个孔
GLASS_W, GLASS_H = 105.0, 66.0      # 屏幕玻璃（实测）
GLASS_DX = 0.4                      # 玻璃中心相对安装孔中心往右偏多少（按第一版装好后开窗的缝反推）
GLASS_DY = -0.5                     # 玻璃中心相对安装孔中心往上偏多少：按尺寸图量，装好的方向（SW1 在下沿）玻璃偏下 0.5
BLACK_TOP, BLACK_BOT, BLACK_SIDE = 8.0, 4.0, 4.0   # 玻璃上 / 下 / 左右黑边（实测）：显示区 97 × 54，比玻璃中心低 (8-4)/2 = 2mm
VIEW_MARGIN = 2.0                   # 前框开窗比显示区每边大这么多：盖住玻璃边和不对称的黑边，露出一圈均匀的 2mm 黑边
WIN_CHAMFER = 0.5                   # 开窗正面倒角：点屏幕边上的按钮时手指不被台阶挡住
SKIN = 1.0                          # 盖在玻璃上的那层前框厚度 = 屏幕凹进去多少
POCKET_GAP = 0.6                    # 前框背面放玻璃的凹槽比玻璃每边大这么多（玻璃位置有偏差也放得进）
FPC_TOP = 2.5                       # 凹槽在玻璃上沿再往上让出这么多：屏幕排线从上沿凸出约 2mm
STANDOFF = 5.0                      # PCB 安装耳正面到玻璃正面的距离（实测）
GLASS_FIT = 0.1                     # 玻璃正面和凹槽底（前框面皮背面）之间的间隙：夹紧力都走柱子，玻璃不受压
FRONT_T = 2.0                       # 前框厚度（玻璃外面一圈）；盖住玻璃的地方只剩 SKIN
PCB_Z = -(SKIN + GLASS_FIT + STANDOFF)   # PCB 正面的 z（前框正面 z=0）：玻璃正面正好顶到凹槽底下 GLASS_FIT
POST_L = -PCB_Z - FRONT_T           # 前框柱子长（前框背面到 PCB 正面）
PCB_T = 1.6                         # PCB 厚度：机身托台顶在 PCB 背面，和前框柱子一起把板子夹住
PRELOAD = 0.2                       # 弹簧托台预压：凸台比 PCB 背面往前多出这么多，装好后横梁被压弯、把板子顶紧
SPRING_T = 1.0                      # 弹簧托台横梁厚度（越厚越硬）
PIN_D = 2.9                         # 定位销（插进 M3 安装孔）：2.6 太细，板子会晃
PIN_UP = 1.0                        # 定位销穿过 PCB 以后再往前伸出多少：伸进前框柱子端面的避让孔
PIN_HOLE_D, PIN_HOLE_H = 3.5, 1.6   # 前框柱子端面给销子尖的避让孔：直径、深度（比 PIN_UP 多留 0.6）
POST_D = 5.5                        # 前框柱子直径：内缘离玻璃边留 0.75mm
WALL = 3.0
TOL = 0.25                          # 配合间隙
LIP_TOL = 0.2                       # 唇边和机身内壁的间隙：0.25 时盖子晃，再小 FDM 打出来容易插不进
STEP = 2.0                          # 像素台阶圆角的台阶大小：4 时内腔四角收进去 8mm，机身一矮就碰到板子的安装耳
LEAD = 0.6                          # 导向倒角：机身两个开口内沿、唇边尖端各倒 45°（也吃掉机身后口贴热床那层的“象脚”）

EDGE_TOP, EDGE_BOT = 4.0, 5.0       # 板子上沿 / 下沿到内壁至少留多少（按实物装好后的空间，插头和线够用）
SCREEN_CY = (BLACK_TOP - BLACK_BOT) / 2 - GLASS_DY   # 板子中心比机身中心高这么多：让显示区（也就是开窗）落在前脸正中
IN_W = BOARD_W + 4                  # 机身内腔：左右各留 2mm（USB 插头够得着）
IN_H = 2 * max(BOARD_H / 2 + SCREEN_CY + EDGE_TOP, BOARD_H / 2 - SCREEN_CY + EDGE_BOT)   # 机身上下对称，取上下两边要求大的那个
OUT_W, OUT_H = IN_W + 2 * WALL, IN_H + 2 * WALL   # 130 × 90
LIP_D = 4.0                         # 前框 / 后盖插进机身的唇边深度
HOOK_L, HOOK_W, HOOK_T = 18.0, 8.0, 1.8    # 卡扣悬臂：长、宽、厚；悬臂竖着打印、弯的时候拉的是层间，长度配厚度让插入应变 ≈1%
HOOK_B = 1.4                        # 卡扣倒钩凸出量：咬进窗口 HOOK_B - LIP_TOL = 1.2mm
BARB_L, BARB_LAND = 4.0, 1.2        # 倒钩底座长、卡面后面的平台宽（以前 3 / 0.6，太薄，FDM 打出来直角一圆就咬不住）
HOOK_X_BOT = 9.0                    # 下沿两个卡扣离中心：避开板子下沿 Speak(x≈+20)、BAT(x≈+43) 插座和 SW1(x≈-24)
HOOK_X_TOP = 46.0                   # 上沿两个卡扣离中心：避开上沿 P2～P5 插座和线（x≈-8～+39）
HOOK_X_BOT_BACK, HOOK_X_TOP_BACK = 21.0, 34.0   # 后盖的卡扣和前框错开：机身短了，两边悬臂各 18mm，放在同一个 x 上会撞
SNAP_SLACK = 0.1                    # 倒钩卡面到窗口边的余量（前后方向）：以前 0.2，正好把弹簧托台 0.2 的预压全吃掉，板子没被顶紧、后盖也晃
DEPTH = 35.0                        # 机身深度（不含前框、后盖）：45 时太厚，支架底板不变、机身减 10mm
BACK_T = 3.0

USB_FROM_TOP = 39.0                 # USB-C 中心离板子上沿
USB_W, USB_H = 16.0, 12.0           # 侧面开口（插头外壳约 10mm 厚，留余量）
P1_FROM_TOP = 21.0                  # 侧面开口往上加高到离板子上沿这么远，把 USB 旁边的 P1 也包进去（雷达线从侧面出）
SIDE_DY = -10.0                     # 侧面开口整体上下挪：按实物往下挪 10mm
SIDE_BOT_UP = 4.0                   # 侧面开口下沿再往上收这么多：按实物下方空得太多

TILT = 15.0                         # 整机后仰角度

LEG_X = 40.0                        # 两条腿离中心
LEG_W, LEG_D = 12.0, 22.0
PEG = 7.6                           # 插销截面（插口 8.0）
LEG_PEG = 16.0                      # 支架插销长度：穿过 3mm 底壁、6mm 套筒，再伸出来让倒钩扣在套筒顶上
PEG_SLOT, PEG_SLOT_L = 3.6, 12.0    # 插销中间开的叉口：宽、深（分成左右两片，插进去时往里弹）
PEG_BARB = 0.5                      # 插销倒钩凸出量（比插口单边间隙 TOL 多 0.25，扣住套筒顶面）：拿起机器支架不掉，用力往下拔能拔出（约 40N）
SLEEVE_H = 6.0                      # 机身底部插口内侧的套筒高度

SPK = (30.0, 20.0, 4.2)             # 2030 喇叭
BAT = (50.0, 34.0, 6.0)             # 603450 电池：长、宽、厚
BAT_X, BAT_Y = -20.0, -16.0         # 电池卡座在后盖内侧的中心：避开喇叭框和散热缝
CLIP_B = 0.5                        # 免胶卡点（喇叭、电池）往里凸出量：框壁长，弯得动
BAT_GAP = 0.6                       # 电池卡点卡面比电池名义厚度高这么多：实际厚度 6.0～6.3，软包还会鼓
MIC_CLIP_B = 0.3                    # 麦克风圆环小、硬，卡点凸出少一点
MIC_D = 14.3                        # INMP441 小圆板：实测直径 14mm，圆环内径每边留 0.15
MIC_H = 6.0                         # 圆环高度（从侧壁往里伸出多少）
MIC_T = 1.6                         # 麦克风板厚度：圆环内侧的卡点卡在这个高度
MIC_Y = 0.0                         # 麦克风在机身左侧内壁的位置；前后（MIC_Z）放在 PCB 背面和后盖唇边正中间，见下
MIC_Z = (PCB_Z - PCB_T + (-FRONT_T - DEPTH + LIP_D)) / 2   # 前后放在 PCB 背面和后盖唇边正中间


# ---------------- 工具 ----------------
def stepped(w, h, step=STEP, levels=2):
    """像素风圆角矩形（四角各切 levels 级台阶），中心在原点：几个矩形叠起来"""
    cs = CrossSection.square([w, h - 2 * levels * step], True)
    for k in range(1, levels + 1):
        cs = cs + CrossSection.square([w - 2 * k * step, h - 2 * (levels - k) * step], True)
    return cs


def inset(cs, d):
    """轮廓整体往里收 d（直角拐角保持直角）：墙、唇边都用这个，台阶处厚度才处处一样"""
    return cs.offset(-d, JoinType.Miter, 4.0)


def cavity():
    """机身内腔轮廓：外轮廓往里收一个壁厚。
    以前内腔另外用 stepped() 画、台阶大小和外面对不上，四个角的墙被切断，机身打出来散成 8 块"""
    return inset(stepped(OUT_W, OUT_H), WALL)


def chamfer_ring(cs, z_face, sign, size=LEAD, out=True, n=6):
    """45° 倒角要切掉的部分（薄片台阶逼近）：cs 是轮廓，z_face 是端面，sign 指向材料里面（+1 往 +Z）。
    out=True：孔口倒角，把轮廓往外扩着切；out=False：凸起（唇边）尖端倒角，切掉轮廓内侧一圈"""
    m = Manifold()
    for k in range(n):
        d = size * (1 - k / n)                      # 端面处最大，往里逐渐变 0
        z0, z1 = sorted((z_face + sign * size * k / n, z_face + sign * size * (k + 1) / n))
        if out:
            m += slab(cs.offset(d, JoinType.Miter, 4.0), z0, z1)
        else:
            m += slab(cs, z0, z1) - slab(inset(cs, d), z0 - 1, z1 + 1)
    return m


def cavity_top(x):
    """内腔在 |x| 处的上下内壁 y（四角两级台阶，每级 STEP）"""
    k = sum(abs(x) > IN_W / 2 - j * STEP for j in (1, 2))   # 落在第几级台阶上
    return IN_H / 2 - k * STEP


def slab(cs, z0, z1):
    """把 2D 轮廓拉伸成 z0..z1 的板"""
    return Manifold.extrude(cs, z1 - z0).translate([0, 0, z0])


def box(cx, cy, cz, w, h, d):
    return Manifold.cube([w, h, d], center=True).translate([cx, cy, cz])


def cyl(cx, cy, z0, z1, d, seg=40):
    return Manifold.cylinder(z1 - z0, d / 2, d / 2, seg).translate([cx, cy, z0])


def hull(pts):
    return Manifold.hull_points([list(map(float, p)) for p in pts])


def hook(L=HOOK_L, w=HOOK_W, t=HOOK_T, b=HOOK_B):
    """卡扣悬臂（局部坐标）：沿 +Z 伸出 0..L，外表面在 y=0、往 +Y 凸出倒钩。
    倒钩在尖端：尖端一侧是斜坡（插进去时把悬臂压弯），根部一侧是直角卡面。
    根部内侧带 1.2mm 的 45° 加强角：悬臂竖着打印，根部正好是层间结合面，直角根部应力集中、最容易从这里断；
    外侧贴着机身内壁，不能加"""
    arm = box(0, -t / 2, L / 2, w, t, L)
    arm += hull([(x, y, z) for x in (-w / 2, w / 2) for y, z in ((-t, 0), (-t - 1.2, 0), (-t, 1.2))])  # 根部内侧加强角
    c = L - BARB_L                                  # 卡面
    tip = hull([(x, y, z) for x in (-w / 2, w / 2) for y, z in ((0, c), (b, c), (b, c + BARB_LAND), (0, L))])
    return arm + tip


def clip(w, zc, ramp, b=CLIP_B):
    """免胶卡点（局部坐标）：贴在 y=0 的内壁上往 +Y 凸出 b；z=zc 是直角卡面（压住零件），往上 ramp 长的斜坡回到壁面，
    零件从上面按下去时把壁撑开一点，过了卡面就弹回来扣住"""
    return hull([(x, y, z) for x in (-w / 2, w / 2) for y, z in ((-0.3, zc), (b, zc), (-0.3, zc + ramp))])


def holes_xy():
    for sx in (-1, 1):
        for sy in (-1, 1):
            yield sx * HOLE_DX / 2, SCREEN_CY + sy * HOLE_DY / 2


def pixel_text(text, px=0.8, size=11):
    """用像素字体渲染文字 → 一堆小方块（凹刻用），返回 (Manifold, 宽, 高)"""
    fonts = ['/System/Library/Fonts/Hiragino Sans GB.ttc', 'C:/Windows/Fonts/msyh.ttc', 'C:/Windows/Fonts/simhei.ttf']
    f = ImageFont.truetype(next(p for p in fonts if os.path.exists(p)), size)
    l, t, r, b = f.getbbox(text)
    im = Image.new('1', (r - l + 2, b - t + 2), 0)
    d = ImageDraw.Draw(im)
    d.fontmode = '1'
    d.text((-l + 1, -t + 1), text, font=f, fill=1)
    a = np.array(im)
    h, w = a.shape
    m = Manifold()
    for yy in range(h):
        xx = 0
        while xx < w:  # 同一行连续的像素合成一条，少一些零件
            if a[yy, xx]:
                x0 = xx
                while xx < w and a[yy, xx]:
                    xx += 1
                m += box((x0 + xx) / 2 * px - w * px / 2, (h / 2 - yy - 0.5) * px, 0, (xx - x0) * px, px, 1)
            else:
                xx += 1
    return m, w * px, h * px


# ---------------- 零件 ----------------
def lip(depth, t):
    """插进机身开口的唇边（外圈贴着机身内壁，壁厚 t），z ∈ [-depth, 0]"""
    o = inset(cavity(), LIP_TOL)
    i = inset(o, t)
    return slab(o, -depth, 0) - slab(i, -depth - 1, 1) - chamfer_ring(o, -depth - 0.01, 1, out=False)


def snaps(z_root, sign):
    """一块盖板（前框 sign=-1 往机身里是 -Z；后盖 sign=+1）的卡扣：上下沿各两个悬臂卡扣，卡进机身上下壁的窗口。
    以前上沿只有一条两边带斜面的凸舌，用力一拉就出来，前框还被弹簧托台一直往外顶。
    返回 (要加的, 唇边上要切掉的缝)"""
    add, cut = Manifold(), Manifold()
    y_in = IN_H / 2 - LIP_TOL
    for x, sy in hook_spots(sign):
        h = hook() if sy > 0 else hook().mirror([0, 1, 0])   # 倒钩朝外（上沿朝上、下沿朝下）
        if sign < 0:
            h = h.mirror([0, 0, 1])
        add += h.translate([x, sy * y_in, z_root])
        cut += box(x, sy * (y_in - 2), z_root + sign * LIP_D / 2, HOOK_W + 2, 6, LIP_D + 0.02)  # 悬臂两边留 1mm 缝
    return add, cut


def hook_spots(sign):
    """四个卡扣的位置 (x, 上沿 +1 / 下沿 -1)；sign 同 snaps()：前框 -1、后盖 +1"""
    bot, top = (HOOK_X_BOT, HOOK_X_TOP) if sign < 0 else (HOOK_X_BOT_BACK, HOOK_X_TOP_BACK)
    for sx in (-1, 1):
        yield sx * bot, -1
        yield sx * top, 1


def snap_holes(z_root, sign):
    """机身上和 snaps() 对应的：上下壁各两个卡扣窗口（穿透，拆的时候从这里顶）"""
    m = Manifold()
    tip = z_root + sign * HOOK_L                   # 卡扣尖端
    catch = z_root + sign * (HOOK_L - BARB_L)      # 倒钩卡面
    z0, z1 = sorted((tip + sign * 0.3, catch - sign * SNAP_SLACK))
    for x, sy in hook_spots(sign):
        m += box(x, sy * (IN_H / 2 + WALL / 2), (z0 + z1) / 2, HOOK_W + 0.6, WALL + 2, z1 - z0)
    return m


def side_open_y():
    """机身右侧开口（USB-C + P1）的上下沿 y"""
    board_top = SCREEN_CY + BOARD_H / 2
    return board_top - USB_FROM_TOP - USB_H / 2 + SIDE_DY + SIDE_BOT_UP, board_top - P1_FROM_TOP + SIDE_DY


def bezel():
    """前框（黑）：开窗压住玻璃四边、背面放玻璃的凹槽、4 根柱子（端面压住 PCB 安装孔周围，中间避让孔让开机身上的定位销）、
    插进机身的唇边和卡扣、下巴上的凹刻字"""
    t = FRONT_T
    m = slab(stepped(OUT_W, OUT_H), -t, 0)
    gy = SCREEN_CY + GLASS_DY                                               # 玻璃中心
    # 开窗：以显示区为中心，比显示区每边大 VIEW_MARGIN，正面倒角
    win_w = GLASS_W - 2 * BLACK_SIDE + 2 * VIEW_MARGIN
    win_h = GLASS_H - BLACK_TOP - BLACK_BOT + 2 * VIEW_MARGIN
    win = CrossSection.square([win_w, win_h], True).translate([GLASS_DX, gy + (BLACK_BOT - BLACK_TOP) / 2])
    m -= slab(win, -t - 1, 1) + chamfer_ring(win, 0.01, -1, WIN_CHAMFER)
    # 背面凹槽：玻璃嵌进去，正面只隔一层 SKIN；上沿多让出排线
    m -= box(GLASS_DX, gy, (-t - 1 - SKIN) / 2, GLASS_W + 2 * POCKET_GAP, GLASS_H + 2 * POCKET_GAP, t + 1 - SKIN)
    m -= box(GLASS_DX, gy + GLASS_H / 2 + FPC_TOP / 2, (-t - 1 - SKIN) / 2, GLASS_W, FPC_TOP + 2 * POCKET_GAP, t + 1 - SKIN)
    for x, y in holes_xy():
        m += cyl(x, y, PCB_Z, -t, POST_D) - cyl(x, y, PCB_Z - 1, PCB_Z + PIN_HOLE_H, PIN_HOLE_D, 24)
    add, cut = snaps(-t, -1)
    m += lip(LIP_D, 2.0).translate([0, 0, -t]) - cut + add
    y0, y1 = side_open_y()
    m -= box(IN_W / 2 - 1, (y0 + y1) / 2, -t - LIP_D / 2, 4, y1 - y0, LIP_D + 0.02)   # 唇边让开侧面开口，粗插头不会顶到
    # 唇边尾部让开板子：板子贴得离前框很近，左右两侧唇边会碰到板边，在板子范围里削掉（上下沿和卡扣不受影响）
    m -= box(0, SCREEN_CY, PCB_Z - 10, BOARD_W + 0.6, BOARD_H + 0.6, 20)
    chin_y = (-OUT_H / 2 - win_h / 2) / 2                            # 开窗下沿和前框下边的正中间（开窗以机身中心为中心）
    txt, tw, th = pixel_text('Agent Buddy')
    m -= txt.translate([0, chin_y, 0])
    return m


def body():
    """机身（深灰）：前后开口的方管，像素台阶圆角；右侧 USB-C + P1 开口，底部两条腿的插口和套筒，
    左侧麦克风座，内壁四个弹簧托台（带定位销）顶住 PCB 背面；前框、后盖的卡扣窗口"""
    outer = stepped(OUT_W, OUT_H)
    inner = cavity()
    z0, z1 = -FRONT_T - DEPTH, -FRONT_T
    m = slab(outer, z0, z1) - slab(inner, z0 - 1, z1 + 1)
    m -= chamfer_ring(inner, z1 + 0.01, -1) + chamfer_ring(inner, z0 - 0.01, 1)   # 两个开口内沿的导向倒角
    # PCB 弹簧托台：左右内壁、每个安装孔后面一根横梁（两头架在竖肋上，打印时是一段平桥），横梁中间一圈凸台顶在孔周围的 PCB 背面。
    # 凸台比 PCB 背面高 PRELOAD，前框扣上后横梁被压弯，一直把板子往前顶在前框柱子上；屏幕厚度有点偏差也能吃掉
    zp = PCB_Z - PCB_T                               # PCB 背面
    zt = zp - 0.3                                    # 横梁顶面：比 PCB 背面低 0.3，只有凸台碰板子
    bx0, bx1 = 54.3, 57.8                            # 横梁 x 范围：正好在柱子后面，又不碰内腔四角的台阶（x>58 处 y<36）
    for x, y in holes_xy():
        sx, sy = (1 if x > 0 else -1), (1 if y > 0 else -1)
        X = lambda v: sx * v
        Y = lambda v: y + sy * v                     # 相对孔中心、往外为正
        xw = IN_W / 2 + 0.5
        # 里侧竖肋：贴在侧壁上，底下 45° 斜面；外侧（靠墙）顶面压低一点，别碰到板子背面的元件
        m += hull([(X(xx), Y(dy), zz) for dy in (-9.5, -7.5) for xx, zz in
                   ((bx0, zt), (bx1, zt), (xw, zt - 1.1), (bx0, zt - 2), (xw, zt - 2 - (xw - bx0)))])
        # 外侧竖肋：一直伸到内腔四角台阶的墙上（x 在 54～58 这一段，墙在 y=±(IN_H/2-4)）。
        # 板子上移以后，上面的肋离墙近、下面的离墙远，竖肋长度跟着变，横梁跨度不变，四个角弹力一样
        dw = cavity_top(bx1) - sy * y + 0.5          # 孔中心到墙（往外为正），多伸 0.5 吃进墙里
        m += hull([(X(xx), Y(dy), zz) for xx in (bx0, bx1) for dy, zz in
                   ((6.5, zt), (dw, zt), (6.5, zt - 2), (dw, zt - 2 - (dw - 6.5)))])
        # 横梁
        m += box(X((bx0 + bx1) / 2), Y(0.75), zt - SPRING_T / 2, bx1 - bx0, 17.5, SPRING_T)
        # 凸台：顶在安装孔周围，和前框柱子正对，板子只受压不受弯。
        # 中间立定位销：板子放进来就套在销子上自己定好位，再盖前框；销子跟着横梁一起动，尖端始终伸出板子正面 PIN_UP
        top = zp + PRELOAD
        m += cyl(x, y, zt - 0.01, top, 5.0)
        m += cyl(x, y, top - 0.01, top + PCB_T + PIN_UP - 0.4, PIN_D, 24)
        m += Manifold.cylinder(0.4, PIN_D / 2, PIN_D / 2 - 0.4, 24).translate([x, y, top + PCB_T + PIN_UP - 0.4])   # 尖端倒角，好套
    # 板子下沿不再用底壁支撑肋托（会挡住下沿的排线），上下左右全靠四根定位销定位
    # 前框唇边插进来的那一圈（留 0.3）：板子贴近前框以后，弹簧竖肋伸到了这里，削掉
    m -= slab(cavity() - inset(cavity(), LIP_TOL + 2.0 + 0.3), z1 - LIP_D - 0.3, z1 + 1)
    # 支架插口内侧的套筒，插销插得深、不晃
    zm = -FRONT_T - DEPTH / 2
    for sx in (-1, 1):
        sw = PEG + 2 * TOL + 3.2
        yw, yt = -IN_H / 2 - 0.5, -IN_H / 2 + SLEEVE_H     # 贴墙一侧、伸进机身的顶面
        # 朝后口那面（打印时朝下）做成 45° 斜面贴回墙上，不然是 11×6mm 的水平悬空
        m += hull([(sx * LEG_X + dx, y, z) for dx in (-sw / 2, sw / 2) for y, z in
                   ((yw, zm + sw / 2), (yt, zm + sw / 2), (yt, zm - sw / 2), (yw, zm - sw / 2 - (yt - yw)))])
    # 麦克风座：左侧内壁一圈纯圆的圆环，INMP441 收音孔朝外，线从圆环朝机身里的口出来；外面一圈像素圆形收音孔，和圆环同心。
    # 机身竖着打印，圆环横着伸出侧壁：外圈底部和内孔顶部是小悬空，FDM 能直接打，孔顶可能略塌，板子按不进去就把孔顶刮一下
    ring = (Manifold.cylinder(MIC_H, MIC_D / 2 + 1.5, MIC_D / 2 + 1.5, 64)
            - Manifold.cylinder(MIC_H + 2, MIC_D / 2, MIC_D / 2, 64).translate([0, 0, -1]))
    for a in (0, 120, 240):  # 三个卡点；板子按进圆环就扣住，不用胶
        ring += clip(3, MIC_T + 0.1, MIC_H - MIC_T - 0.1, MIC_CLIP_B).translate([0, -MIC_D / 2, 0]).rotate([0, 0, a + 90])
    m += ring.rotate([0, 90, 0]).translate([-IN_W / 2, MIC_Y, MIC_Z])
    for i in range(-3, 4):          # 方孔排成像素圆，最外一圈比圆环内壁小 1mm 左右
        for j in range(-3, 4):
            if i * i + j * j <= 9:
                m -= box(-OUT_W / 2 + WALL / 2, MIC_Y + i * 2.0, MIC_Z + j * 2.0, WALL + 2, 1.2, 1.2)
    # USB-C + P1：右侧，位置跟着板子上沿；深度方向在 PCB 附近
    usb_z = PCB_Z - 3
    y0, y1 = side_open_y()
    m -= box(OUT_W / 2 - WALL / 2, (y0 + y1) / 2, usb_z, WALL + 2, y1 - y0, USB_W)
    # 两条腿的插口（底部，连同套筒打通）
    for sx in (-1, 1):
        m -= box(sx * LEG_X, -OUT_H / 2 + (WALL + SLEEVE_H) / 2, zm, PEG + 2 * TOL, WALL + SLEEVE_H + 2, PEG + 2 * TOL)
    # 散热：上下壁靠后各两道缝，冷风从下面进、热风从上面出（避开中间的卡扣窗口和两边的支架插口）
    for sy in (-1, 1):
        for sx in (-1, 1):
            m -= box(sx * 24, sy * (OUT_H / 2 - WALL / 2), z0 + 8, 12, WALL + 2, 4.0)
    # 前框、后盖的卡扣
    m -= snap_holes(z1, -1) + snap_holes(z0, 1)
    return m


def back():
    """后盖（深灰）：唇边 + 卡扣（和前框一样：先插上沿，再按下沿），喇叭像素格栅 + 喇叭卡槽，几道散热缝"""
    t = BACK_T
    z0 = -FRONT_T - DEPTH - t
    m = slab(stepped(OUT_W, OUT_H), z0, z0 + t)
    add, cut = snaps(z0 + t, 1)
    m += lip(LIP_D, 1.6).mirror([0, 0, 1]).translate([0, 0, z0 + t]) - cut + add
    # 喇叭：右上方，2.4mm 方孔格栅（像素点阵），内侧一圈卡槽
    sx0, sy0 = 28.0, 18.0
    for i in range(-4, 5):
        for j in range(-3, 4):
            if abs(i) + abs(j) > 6:
                continue  # 去掉四角，格栅轮廓也是台阶形
            m -= box(sx0 + i * 3.2, sy0 + j * 3.2, z0 + t / 2, 2.0, 2.0, t + 2)
    sw, sh = SPK[0] + 2 * TOL, SPK[1] + 2 * TOL
    fh = SPK[2] + 1.8                                                # 框比喇叭高一截，上面留给卡点
    frame = box(sx0, sy0, z0 + t + fh / 2, sw + 3, sh + 3, fh) - box(sx0, sy0, z0 + t + fh / 2, sw, sh, fh + 1)
    frame -= box(sx0 - sw / 2 - 0.75, sy0, z0 + t + fh / 2, 3, 5, fh + 1)   # 左侧出线槽
    for sy in (-1, 1):  # 上下两条长边各一个卡点，喇叭按进去就扣住
        c = clip(12, SPK[2] - 0.05, fh - SPK[2] + 0.05)   # 卡面略低于喇叭顶面：卡点斜坡一直压着喇叭，响的时候不打框
        frame += (c if sy < 0 else c.mirror([0, 1, 0])).translate([sx0, sy0 + sy * sh / 2, z0 + t])
    m += frame
    # 电池卡座：四面矮墙围住电池，两条长边各一个卡点；右侧短边开口出线（BAT 插座在板子下沿）
    bw, bh, bt = BAT[0] + 0.6, BAT[1] + 0.6, BAT[2]
    ch = bt + BAT_GAP + 1.2
    cradle = box(BAT_X, BAT_Y, z0 + t + ch / 2, bw + 3.2, bh + 3.2, ch) - box(BAT_X, BAT_Y, z0 + t + ch / 2, bw, bh, ch + 1)
    cradle -= box(BAT_X + bw / 2 + 0.8, BAT_Y, z0 + t + ch / 2, 3, 14, ch + 1)
    for sy in (-1, 1):
        c = clip(16, bt + BAT_GAP, ch - bt - BAT_GAP)
        cradle += (c if sy < 0 else c.mirror([0, 1, 0])).translate([BAT_X, BAT_Y + sy * bh / 2, z0 + t])
    m += cradle
    # 散热缝：和喇叭格栅左右对称、上下对齐（同一条水平中线），也正好避开左下的电池
    for k in range(5):
        m -= box(-sx0, sy0 + (k - 2) * 6, z0 + t / 2, 30, 2.4, t + 2)
    return m


def peg(x):
    """支架插销（机身坐标）：方销从机身底面往上插，中间开叉成左右两片，尖端外侧各一个倒钩。
    插进去时两片往里弹，倒钩过了套筒顶面就弹开扣住；卡面是斜的（约 35°），用力往下拔能拔出来"""
    zm = -FRONT_T - DEPTH / 2
    y0, tip = -OUT_H / 2, -OUT_H / 2 + LEG_PEG
    m = box(x, (y0 + tip) / 2, zm, PEG, LEG_PEG, PEG) - box(x, tip - PEG_SLOT_L / 2 + 0.5, zm, PEG_SLOT, PEG_SLOT_L + 1, PEG + 2)
    b = PEG_BARB
    # 卡面是斜的：让斜面正好在插口边（离插销面 TOL）碰到套筒顶面，支架插到底不上下晃（只留 0.05）
    yc = -IN_H / 2 + SLEEVE_H + 0.05 - (TOL + 0.3) / (b + 0.3) * 1.4 * b
    for sx in (-1, 1):
        xo = x + sx * PEG / 2
        m += hull([(xo + sx * d, y, zm + dz) for dz in (-PEG / 2, PEG / 2) for d, y in
                   ((-0.3, yc), (b, yc + 1.4 * b), (b, yc + 1.4 * b + 0.6), (-0.3, min(tip, yc + 1.4 * b + 3.0)))])
    return m


def stand():
    """支架（黑）：两条腿 + 底板；腿顶的方销插进机身底部。整机后仰 TILT，底板水平"""
    legs = Manifold()
    for sx in (-1, 1):
        x = sx * LEG_X
        legs += box(x, -OUT_H / 2 - 10, -FRONT_T - DEPTH / 2, LEG_W, 20, LEG_D)
        legs += peg(x)
    tilted = legs.rotate([-TILT, 0, 0])
    lo = tilted.bounding_box()[1]  # min y
    floor = lo + 6
    keep = box(0, floor + 200, 0, 400, 400, 400)
    tilted = tilted ^ keep
    bb = tilted.bounding_box()
    base = box(0, floor - 2, (bb[2] + bb[5]) / 2, 2 * LEG_X + LEG_W + 16, 4, 52)
    # 底板四角也做台阶
    return tilted + base, floor


# ---------------- 导出 ----------------
def write_stl(m, path):
    mesh = m.to_mesh()
    v = np.asarray(mesh.vert_properties)[:, :3]
    f = np.asarray(mesh.tri_verts)
    with open(path, 'wb') as fp:
        fp.write(b'Agent Buddy case'.ljust(80, b' '))
        fp.write(np.uint32(len(f)).tobytes())
        tri = v[f]
        n = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
        n /= np.linalg.norm(n, axis=1, keepdims=True) + 1e-12
        rec = np.zeros(len(f), dtype=[('n', '<f4', 3), ('v', '<f4', (3, 3)), ('a', '<u2')])
        rec['n'] = n
        rec['v'] = tri
        rec.tofile(fp)


def on_bed(m, rot=(0, 0, 0)):
    """打印摆放：旋转后放到热床上（最低点 z=0，居中）"""
    m = m.rotate(list(rot))
    b = m.bounding_box()
    return m.translate([-(b[0] + b[3]) / 2, -(b[1] + b[4]) / 2, -b[2]])


def render(parts, path, size=(900, 700), view=(-24, 32), scale=None, bg=(244, 243, 248)):
    """简易渲染：正交投影 + 画家算法 + 平面光照。parts = [(Manifold, (r,g,b))]"""
    rx, ry = map(math.radians, view)
    Ry = np.array([[math.cos(ry), 0, math.sin(ry)], [0, 1, 0], [-math.sin(ry), 0, math.cos(ry)]])
    Rx = np.array([[1, 0, 0], [0, math.cos(rx), -math.sin(rx)], [0, math.sin(rx), math.cos(rx)]])
    R = Rx @ Ry
    tris, cols = [], []
    for m, c in parts:
        mesh = m.to_mesh()
        v = np.asarray(mesh.vert_properties)[:, :3] @ R.T
        f = np.asarray(mesh.tri_verts)
        tris.append(v[f])
        cols.append(np.tile(np.array(c, float), (len(f), 1)))
    T = np.concatenate(tris)
    C = np.concatenate(cols)
    n = np.cross(T[:, 1] - T[:, 0], T[:, 2] - T[:, 0])
    n /= np.linalg.norm(n, axis=1, keepdims=True) + 1e-12
    vis = n[:, 2] > 0
    T, C, n = T[vis], C[vis], n[vis]
    light = np.array([-0.4, 0.6, 0.7]); light /= np.linalg.norm(light)
    shade = 0.45 + 0.55 * np.clip(n @ light, 0, 1)
    allv = T.reshape(-1, 3)
    mn, mx = allv.min(0), allv.max(0)
    W, H = size
    s = scale or 0.86 * min(W / (mx[0] - mn[0]), H / (mx[1] - mn[1]))
    cx, cy = (mn[0] + mx[0]) / 2, (mn[1] + mx[1]) / 2
    order = np.argsort(T[:, :, 2].mean(1))
    im = Image.new('RGB', size, bg)
    d = ImageDraw.Draw(im)
    for i in order:
        p = [(W / 2 + (x - cx) * s, H / 2 - (y - cy) * s) for x, y, _ in T[i]]
        col = tuple(int(min(255, c * shade[i])) for c in C[i])
        d.polygon(p, fill=col, outline=col)
    im.save(path)


DARK = (64, 64, 78)
BLACK = (24, 24, 30)
LIGHT = (150, 150, 170)
GREEN = (61, 220, 132)
BLUE = (87, 131, 244)


def find_blender():
    """找 Blender：环境变量 BLENDER > PATH > 默认安装位置（Windows / macOS），找不到返回 None"""
    cands = [os.environ.get('BLENDER'), shutil.which('blender')]
    cands += sorted(glob.glob(r'C:\Program Files\Blender Foundation\Blender*\blender.exe'), reverse=True)
    cands += ['/Applications/Blender.app/Contents/MacOS/Blender']
    return next((c for c in cands if c and os.path.exists(c)), None)


def write_blend(parts, path):
    """把装配姿态的零件交给 Blender，生成能双击打开的预览场景（blender_scene.py）；没装 Blender 就跳过"""
    exe = find_blender()
    if not exe:
        print('没找到 Blender，跳过 .blend（可以用环境变量 BLENDER 指定 blender 可执行文件）')
        return
    with tempfile.TemporaryDirectory() as tmp:
        for name, m in parts:
            write_stl(m, os.path.join(tmp, name + '.stl'))
        r = subprocess.run([exe, '--background', '--factory-startup', '--python', os.path.join(HERE, 'blender_scene.py'),
                            '--', tmp, os.path.join(HERE, 'stl'), path], capture_output=True, text=True, encoding='utf-8', errors='replace')
    if r.returncode or not os.path.exists(path):
        print('生成 .blend 失败：\n' + r.stdout[-2000:] + r.stderr[-2000:])


def main():
    os.makedirs(os.path.join(HERE, 'stl'), exist_ok=True)
    os.makedirs(os.path.join(HERE, 'preview'), exist_ok=True)
    stl = lambda name: os.path.join(HERE, 'stl', name)

    bz, bd, bk = bezel(), body(), back()
    st, floor = stand()

    # 各零件按打印摆放导出
    write_stl(on_bed(bz, (180, 0, 0)), stl('1-前框-黑.stl'))           # 正面朝下
    write_stl(on_bed(bd, (0, 0, 0)), stl('2-机身-深灰.stl'))            # 后口朝下
    write_stl(on_bed(bk, (0, 0, 0)), stl('3-后盖-深灰.stl'))            # 外面朝下
    write_stl(on_bed(st, (90, 0, 0)), stl('4-支架-黑.stl'))            # 底板朝下（装配坐标里底板是水平的 XZ 面）

    # 组装预览：在机身坐标系里摆好，再整体后仰
    screen = box(GLASS_DX, SCREEN_CY + GLASS_DY, -SKIN - GLASS_FIT - 0.3, GLASS_W, GLASS_H, 0.6)  # 预览用：屏幕玻璃
    asm = [(bz, BLACK), (bd, DARK), (bk, DARK), (screen, (30, 60, 90))]
    asm = [(m.rotate([-TILT, 0, 0]), c) for m, c in asm] + [(st, BLACK)]
    render(asm, os.path.join(HERE, 'preview', 'assembly-front.png'), view=(-12, 28))
    render(asm, os.path.join(HERE, 'preview', 'assembly-back.png'), view=(-14, 200))
    render(asm, os.path.join(HERE, 'preview', 'assembly-side.png'), view=(0, 90))
    import json
    vols = {}
    for name, m in [('1-前框-黑', bz), ('2-机身-深灰', bd), ('3-后盖-深灰', bk), ('4-支架-黑', st)]:
        b = m.bounding_box()
        vols[name] = dict(grams=round(m.volume() / 1000 * 1.24, 1), size=[round(b[3] - b[0]), round(b[4] - b[1]), round(b[5] - b[2])])
    json.dump(vols, open(os.path.join(HERE, 'preview', 'parts.json'), 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
    # 双击就能在 Blender 里看的预览场景：装配效果、爆炸图、打印摆放
    write_blend([(n, m.rotate([-TILT, 0, 0])) for n, m in (('前框', bz), ('机身', bd), ('后盖', bk))] + [('支架', st)],
                os.path.join(HERE, '外壳预览.blend'))
    write_zip(os.path.join(HERE, 'Agent-Buddy-外壳.zip'))
    print('ok')


def write_zip(path):
    """打印包：说明 + STL + 预览图 + .blend，发给别人打印用"""
    import zipfile
    files = ['打印说明.md', '组装清单.md', '外壳预览.blend']
    files += ['stl/' + f for f in sorted(os.listdir(os.path.join(HERE, 'stl'))) if f.endswith('.stl')]
    files += ['preview/' + f for f in sorted(os.listdir(os.path.join(HERE, 'preview'))) if f.endswith('.png')]
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        for f in files:
            if os.path.exists(os.path.join(HERE, f)):
                z.write(os.path.join(HERE, f), f)


if __name__ == '__main__':
    main()
