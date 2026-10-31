"""宣传片成片合成：按时间线逐帧合成 3D 渲染、板子录屏、照片快闪、电脑软件截图和字幕，配上 music.wav 输出 MP4。

bridge/.venv/bin/python promo/edit.py [--cut full|short] [--from 秒] [--to 秒] [--out 文件]   （short 是 1 分钟版；局部预览用 --from/--to）
"""
import glob
import math
import os
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
W, H, FPS = 1920, 1080, 30
PINGFANG = glob.glob('/System/Library/AssetsV2/com_apple_MobileAsset_Font8/*/AssetData/PingFang.ttc')[0]
SF = '/System/Library/Fonts/SFNS.ttf'
GREY = (161, 161, 166)
WHITE = (245, 245, 247)


# ---------------- 小工具 ----------------
_fonts = {}


def font(size, weight='Semibold', sf=False):
    key = (size, weight, sf)
    if key not in _fonts:
        if sf:
            f = ImageFont.truetype(SF, size)
            f.set_variation_by_name(weight)
        else:
            f = ImageFont.truetype(PINGFANG, size, index={'Regular': 3, 'Medium': 7, 'Semibold': 11, 'Light': 15}[weight])
        _fonts[key] = f
    return _fonts[key]


def ease(x):
    x = min(1.0, max(0.0, x))
    return x * x * (3 - 2 * x)


def ease_out(x):
    x = min(1.0, max(0.0, x))
    return 1 - (1 - x) ** 3


def window(t, a, b, fi=0.6, fo=0.5):
    """t 在 [a, b] 之间显示，带淡入淡出；返回 (不透明度, 入场进度)"""
    if t < a or t > b:
        return 0.0, 0.0
    return min(ease((t - a) / fi), ease((b - t) / fo)), ease_out((t - a) / 0.9)


def text_layer(lines, align='center', y=None, anchor_y=0.5):
    """lines: [(文字, 字体, 颜色, 行距前)]，返回 RGBA 层（只有文字那么大）和尺寸"""
    sizes = []
    for txt, f, col, gap in lines:
        b = f.getbbox(txt)
        sizes.append((b[2] - b[0], f.size, gap))
    w = max(s[0] for s in sizes) + 20
    h = sum(s[1] * 1.3 + s[2] for s in sizes) + 20
    im = Image.new('RGBA', (int(w), int(h)), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    yy = 10
    for (txt, f, col, gap), (tw, fs, _) in zip(lines, sizes):
        yy += gap
        x = (w - tw) / 2 if align == 'center' else 10
        d.text((x, yy), txt, font=f, fill=col)
        yy += fs * 1.3
    return im


_cache = {}


def caption(frame, t, a, b, title, sub=None, pos='bottom', big=64, sub_size=34, align='center'):
    """苹果风字幕：主标题白色半粗，副标题灰色；淡入时往上浮 24px"""
    op, p = window(t, a, b)
    if op <= 0:
        return
    key = (title, sub, big, sub_size, align)
    if key not in _cache:
        lines = [(title, font(big), WHITE, 0)]
        if sub:
            lines.append((sub, font(sub_size, 'Regular'), GREY, 6))
        _cache[key] = text_layer(lines, align)
    layer = _cache[key]
    lw, lh = layer.size
    if pos == 'bottom':
        x, y = (W - lw) // 2, H - lh - 70
    elif pos == 'top':
        x, y = (W - lw) // 2, 70
    elif pos == 'left':
        x, y = 140, (H - lh) // 2
    else:
        x, y = (W - lw) // 2, (H - lh) // 2
    y += int(24 * (1 - p))
    if pos == 'bottom':  # 底部压一层渐变，保证字看得清
        grad = _bottom_grad()
        frame.alpha_composite(grad, (0, 0)) if op >= 1 else frame.alpha_composite(fade_alpha(grad, op))
    frame.alpha_composite(fade_alpha(layer, op), (x, y))


_grad = None


def _bottom_grad():
    global _grad
    if _grad is None:
        a = np.zeros((H, W), np.uint8)
        ys = np.arange(H)
        a[:] = (np.clip((ys - H * 0.55) / (H * 0.45), 0, 1) ** 1.6 * 200).astype(np.uint8)[:, None]
        g = Image.new('RGBA', (W, H), (0, 0, 0, 0))
        g.putalpha(Image.fromarray(a))
        _grad = g
    return _grad


def fade_alpha(im, op):
    if op >= 0.999:
        return im
    a = im.getchannel('A').point(lambda v: int(v * op))
    out = im.copy()
    out.putalpha(a)
    return out


def black():
    return Image.new('RGBA', (W, H), (0, 0, 0, 255))


def dim(frame, k):
    """整体压暗 k（0~1，1 = 全黑）"""
    if k <= 0:
        return frame
    ov = Image.new('RGBA', (W, H), (0, 0, 0, int(255 * min(1, k))))
    frame.alpha_composite(ov)
    return frame


def rounded(im, r):
    m = Image.new('L', im.size, 0)
    ImageDraw.Draw(m).rounded_rectangle((0, 0, im.size[0] - 1, im.size[1] - 1), r, fill=255)
    out = im.convert('RGBA')
    out.putalpha(m)
    return out


def shadow(size, r, blur=40, alpha=170):
    pad = blur * 2
    s = Image.new('RGBA', (size[0] + pad * 2, size[1] + pad * 2), (0, 0, 0, 0))
    ImageDraw.Draw(s).rounded_rectangle((pad, pad, pad + size[0], pad + size[1]), r, fill=(0, 0, 0, alpha))
    return s.filter(ImageFilter.GaussianBlur(blur)), pad


def glow(size, r, color, blur=60, alpha=90):
    pad = blur * 2
    s = Image.new('RGBA', (size[0] + pad * 2, size[1] + pad * 2), (0, 0, 0, 0))
    ImageDraw.Draw(s).rounded_rectangle((pad, pad, pad + size[0], pad + size[1]), r, fill=color + (alpha,))
    return s.filter(ImageFilter.GaussianBlur(blur)), pad


def cover(im, w, h, zoom=1.0, fx=0.5, fy=0.5):
    """等比铺满 w×h（裁掉多余部分），zoom > 1 往里推，fx/fy 是推进的中心"""
    iw, ih = im.size
    s = max(w / iw, h / ih) * zoom
    cw, ch = w / s, h / s
    x0 = (iw - cw) * fx
    y0 = (ih - ch) * fy
    return im.resize((w, h), Image.LANCZOS, box=(x0, y0, x0 + cw, y0 + ch))


# ---------------- 素材读取 ----------------
class Seq:
    """Blender 渲染的 PNG 序列"""

    def __init__(self, name):
        self.name = name
        self.files = []

    def frame(self, t):
        if not self.files:  # 用到时才找文件：渲染还没出完时也能预览别的段落
            self.files = sorted(glob.glob(os.path.join(HERE, 'render', self.name, '*.png')))
        if not self.files:
            fr = black()
            ImageDraw.Draw(fr).text((60, 60), f'[{self.name} 未渲染]', font=font(40), fill=(90, 90, 90))
            return fr
        i = min(len(self.files) - 1, int(t * FPS))
        return Image.open(self.files[i]).convert('RGBA')


class Clip:
    """板子录屏（960×544，30fps），整段解码进内存，循环播放"""
    cache = {}

    def __init__(self, name):
        if name not in Clip.cache:
            path = os.path.join(HERE, 'tex', name + '.mp4')
            raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', path, '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'],
                                 capture_output=True, check=True).stdout
            Clip.cache[name] = np.frombuffer(raw, np.uint8).reshape(-1, 544, 960, 3)
        self.frames = Clip.cache[name]

    def frame(self, t, w=None, start=0.0):
        a = self.frames[int((t + start) * FPS) % len(self.frames)]
        im = Image.fromarray(a)
        if w:
            h = round(w * 544 / 960)
            im = im.resize((w, h), Image.NEAREST if w % 480 == 0 else Image.LANCZOS)
        return im


def photo(path):
    key = ('photo', path)
    if key not in _cache:
        _cache[key] = Image.open(os.path.join(ROOT, path)).convert('RGB')
    return _cache[key]


# ---------------- 镜头 ----------------
def seg_text(lines_spec):
    """黑底文字卡：[(出现, 消失, 主, 副, 位置)]"""
    def f(t, d):
        fr = black()
        for a, b, title, sub, pos in lines_spec:
            caption(fr, t, a, b, title, sub, pos, big=60)
        return fr
    return f


def seg_3d(name, captions=(), fade_in=0.0, fade_out=0.0, wordmark=None, shrink=None, t0=0.0):
    """t0：从镜头的第几秒开始用（短版截取片段）"""
    seq = Seq(name)

    def f(t, d):
        fr = seq.frame(t + t0)
        if shrink:  # 画面缩小上移，给下方的字留位置（背景是纯黑，四周补黑看不出来）
            k, dy = shrink
            small = fr.resize((int(W * k), int(H * k)), Image.LANCZOS)
            small.putalpha(_feather(small.size))
            fr = black()
            fr.alpha_composite(small, ((W - small.size[0]) // 2, (H - small.size[1]) // 2 + dy))
        for c in captions:
            caption(fr, t, *c)
        if wordmark:
            wm_layer(fr, t, *wordmark)
        if fade_in and t < fade_in:
            dim(fr, 1 - ease(t / fade_in))
        if fade_out and t > d - fade_out:
            dim(fr, ease((t - (d - fade_out)) / fade_out))
        return fr
    return f


_feathers = {}


def _feather(size, edge=160):
    """四周 edge 像素渐隐的遮罩"""
    if size not in _feathers:
        w, h = size
        x = np.minimum(np.arange(w), np.arange(w)[::-1]) / edge
        y = np.minimum(np.arange(h), np.arange(h)[::-1]) / edge
        m = np.clip(np.minimum(x[None, :], y[:, None]), 0, 1)
        _feathers[size] = Image.fromarray((m * m * (3 - 2 * m) * 255).astype(np.uint8))
    return _feathers[size]


def wm_layer(fr, t, a, b, sub=None, y=None):
    """「Agent Buddy」字标"""
    op, p = window(t, a, b, fi=1.0, fo=0.8)
    if op <= 0:
        return
    key = ('wm', sub)
    if key not in _cache:
        lines = [('Agent Buddy', font(112, 'Semibold', sf=True), WHITE, 0)]
        if sub:
            lines.append((sub, font(40, 'Regular'), GREY, 4))
        _cache[key] = text_layer(lines)
    L = _cache[key]
    if y is not None and y > H / 2:
        fr.alpha_composite(fade_alpha(_bottom_grad(), op))
    yy = (y if y is not None else (H - L.size[1]) // 2) + int(30 * (1 - p))
    fr.alpha_composite(fade_alpha(L, op), ((W - L.size[0]) // 2, yy))


def device_card(fr, clip, t, cx, cy, w, op=1.0, start=0.0, glow_col=(90, 110, 200)):
    """把板子录屏做成一块发光的圆角屏幕卡片放在 (cx, cy)"""
    im = clip.frame(t, w, start)
    r = max(8, w // 40)
    card = rounded(im, r)
    g, pad = glow(card.size, r, glow_col, blur=max(30, w // 16), alpha=70)
    x, y = int(cx - card.size[0] / 2), int(cy - card.size[1] / 2)
    fr.alpha_composite(fade_alpha(g, op), (x - pad, y - pad))
    fr.alpha_composite(fade_alpha(card, op), (x, y))


def seg_flat(name, title, sub, start=0.0):
    """平面像素屏：录屏铺成大卡片，慢慢放大"""
    def f(t, d):
        clip = Clip(name)
        fr = black()
        w = 1200  # 固定大小：逐帧缩放会让像素抖动
        op = ease(t / 0.4) * ease((d - t) / 0.3)
        device_card(fr, clip, t, W / 2, H / 2 - 50, w, op, start)
        caption(fr, t, 0.3, d - 0.2, title, sub, 'bottom', big=56)
        return fr
    return f


PARTS = [  # 高清商品图（promo/parts/hd-*.webp）、裁剪框（None = 整张）、名字、规格、到手价
    ('hd-1-board', None, 'JC4827W543', 'ESP32-S3 · 4.3″ 480×272 电容触摸屏', '¥99'),
    ('hd-2-radar', None, 'HLK-LD2450', '24 GHz 毫米波雷达', '¥28.2'),
    ('hd-3-mic', None, 'INMP441', 'I2S 数字麦克风', '¥15.8'),
    ('hd-4-speaker', None, '2030 腔体喇叭', '8Ω · 0.5–2W', '¥5.4'),
    ('hd-5-battery', (0, 0, 1106, 548), '聚合物锂电池', '3.7V · 1200 mAh', '¥13.86'),
]


def part_card(name, box):
    key = ('part', name)
    if key not in _cache:
        im = Image.open(os.path.join(HERE, 'parts', name + '.webp')).convert('RGB')
        if box:
            im = im.crop(box)
        S = 660
        k = min(S / im.size[0], S / im.size[1])
        im = im.resize((round(im.size[0] * k), round(im.size[1] * k)), Image.LANCZOS)
        a = np.asarray(im)
        edge = np.concatenate([a[0], a[-1], a[:, 0], a[:, -1]])
        card = Image.new('RGB', (S, S), tuple(int(v) for v in np.median(edge, 0)))  # 底色取图片边缘色，横图上下不留白边
        card.paste(im, ((S - im.size[0]) // 2, (S - im.size[1]) // 2))
        _cache[key] = rounded(card, 36)
    return _cache[key]


def seg_parts(t, d):
    """配件快闪：高清商品图在左，名字、规格、到手价在右，每拍一张"""
    step = d / len(PARTS)
    i = min(len(PARTS) - 1, int(t / step))
    lt = t - i * step
    fname, box, name, sub, price = PARTS[i]
    fr = black()
    card = part_card(fname, box)
    prog = ease_out(lt / 0.35)
    cx, cy = 260, (H - card.size[1]) // 2
    fr.alpha_composite(fade_alpha(card, prog), (cx, cy + round(40 * (1 - prog))))
    key = ('partt', name)
    if key not in _cache:
        _cache[key] = text_layer([(name, font(72), WHITE, 0), (sub, font(36, 'Regular'), GREY, 6),
                                  (price, font(84, 'Semibold', sf=True), (255, 170, 120), 40)], 'left')
    L = _cache[key]
    tp = ease_out((lt - 0.08) / 0.35)
    fr.alpha_composite(fade_alpha(L, tp), (cx + card.size[0] + 110 + round(30 * (1 - tp)), (H - L.size[1]) // 2))
    return fr


def dim_white(fr, k):
    ov = Image.new('RGBA', (W, H), (255, 255, 255, int(255 * k)))
    fr.alpha_composite(ov)


PORTRAITS = ['docs/images/dupont-hand.jpg', 'docs/images/radar-cables.jpg', 'docs/images/wiring-mic.jpg',
             'docs/images/wiring-back.jpg', 'docs/images/v2-magpie-photo.jpg']


def panels(t, paths, dur):
    """几张竖图并排，依次从下往上滑入"""
    fr = black()
    n = len(paths)
    pw = W // n
    for k, p in enumerate(paths):
        lt = t - k * 0.12
        prog = ease_out(lt / 0.5)
        im = cover(photo(p), pw - 8, H, zoom=1.06 + 0.04 * t / dur)
        fr.alpha_composite(fade_alpha(im.convert('RGBA'), prog), (k * pw + 4, int(80 * (1 - prog))))
    return fr


def full(t, path, dur, fx=0.5, fy=0.5):
    im = cover(photo(path), W, H, zoom=1.04 + 0.08 * t / dur, fx=fx, fy=fy)
    return im.convert('RGBA')


def design(t, dur):
    """CAD 三视图（浅色底）"""
    fr = Image.new('RGBA', (W, H), (244, 243, 248, 255))
    for k, n in enumerate(('assembly-front', 'assembly-side', 'assembly-back')):
        prog = ease_out((t - k * 0.15) / 0.5)
        im = photo(f'case/preview/{n}.png').convert('RGBA')
        im = im.resize((620, int(620 * im.size[1] / im.size[0])), Image.LANCZOS)
        fr.alpha_composite(fade_alpha(im, prog), (20 + k * 630, (H - im.size[1]) // 2 + int(60 * (1 - prog))))
    return fr


def seg_photos(t, d):
    """手拍照片快闪：横图满屏、竖图并排，1.25 秒一张（2.5 拍）"""
    shots = [
        lambda u, s: full(u, 'docs/images/photo-back.jpg', s),
        lambda u, s: panels(u, PORTRAITS[:3], s),
        lambda u, s: full(u, 'docs/images/photo-radar.jpg', s),
        lambda u, s: panels(u, PORTRAITS[3:], s),
        lambda u, s: full(u, 'docs/images/photo-screen.jpg', s, fy=0.4),
        lambda u, s: design(u, s),
    ]
    step = d / len(shots)
    i = min(len(shots) - 1, int(t / step))
    fr = shots[i](t - i * step, step)
    caption(fr, t, 0.2, step * 2 - 0.1, '买来零件', '屏幕主板、雷达、麦克风、喇叭、电池', 'bottom', big=56)
    caption(fr, t, step * 2 + 0.1, step * 5 - 0.1, '亲手接线', '一根线、一个口，慢慢拼起来', 'bottom', big=56)
    caption(fr, t, step * 5 + 0.1, d - 0.1, '自己设计外壳', '参数化建模，改一个数字就能重新出图', 'bottom', big=56)
    return fr


SIDEBAR = {'claude.png': 580, 'dsh.png': 575}  # 侧栏右边界（截图像素）
APPS = [('promo/apps/claude.png', 'Claude Code', '你照常在电脑上写代码'),
        ('promo/apps/terminal.png', 'buddy status', '一条命令检查连接、额度和 hooks'),
        ('promo/apps/dsh.png', 'DeepSeek Harness', 'Codex、DeepSeek 一样接得上')]


def app_img(path):
    key = ('app', path)
    if key not in _cache:
        im = Image.open(os.path.join(ROOT, path)).convert('RGBA')
        if 'claude' in path or 'dsh' in path:  # 左侧是对话 / 工作区列表，涉及隐私，只留右边的对话区
            im = rounded(im.crop((SIDEBAR[os.path.basename(path)], 0, im.size[0], im.size[1])).convert('RGB'), 22)
        if 'terminal' in path:  # 去掉 DSH 插件版本那条提示和退出后的输出，只留状态
            top = im.crop((0, 0, im.size[0], 345))
            bot = im.crop((0, 410, im.size[0], 545))
            pad = Image.new('RGBA', (im.size[0], 30), im.getpixel((200, 500)))
            im2 = Image.new('RGBA', (im.size[0], top.size[1] + bot.size[1] + 30))
            im2.paste(top, (0, 0))
            im2.paste(bot, (0, top.size[1]))
            im2.paste(pad, (0, top.size[1] + bot.size[1]))
            im = rounded(im2.convert('RGB'), 22)
        _cache[key] = im
    return _cache[key]


def seg_apps(t, d):
    """电脑软件 + 板子同步：左边是电脑窗口截图，右边是板子画面"""
    clip = Clip('home_claude')
    step = d / len(APPS)
    i = min(len(APPS) - 1, int(t / step))
    lt = t - i * step
    path, title, sub = APPS[i]
    fr = Image.new('RGBA', (W, H), (10, 10, 12, 255))
    im = app_img(path)
    maxw, maxh = 1080, 760
    s = min(maxw / im.size[0], maxh / im.size[1])
    win = im.resize((int(im.size[0] * s), int(im.size[1] * s)), Image.LANCZOS)
    prog = ease_out(lt / 0.6)
    x = 90 + int(-60 * (1 - prog))
    y = (H - win.size[1]) // 2 - 30
    sh, pad = shadow(win.size, 20)
    fr.alpha_composite(fade_alpha(sh, prog), (x - pad, y - pad + 20))
    fr.alpha_composite(fade_alpha(win, prog), (x, y))
    device_card(fr, clip, t, 1500, H / 2 - 60, 640, 1.0)
    # 两边之间一条细的「同步」光点
    for k in range(6):
        px = 1180 + ((t * 160 + k * 30) % 180)
        a = int(200 * math.sin(math.pi * ((t * 160 + k * 30) % 180) / 180))
        ImageDraw.Draw(fr).ellipse((px - 3, H / 2 - 63, px + 3, H / 2 - 57), fill=(120, 160, 255, a))
    caption(fr, lt, 0.3, step - 0.15, title, sub, 'bottom', big=52)
    return fr


GRID = ['buddy_claude', 'buddy_codex', 'buddy_deepseek', 'magpie_agent', 'radar', 'standby']


def seg_grid(t, d):
    fr = black()
    cw, gap = 560, 40
    chh = round(cw * 544 / 960)
    x0 = (W - 3 * cw - 2 * gap) // 2
    y0 = (H - 2 * chh - gap) // 2 - 50
    z = 1.0  # 不做推进：逐帧缩放会让像素抖动
    for k, n in enumerate(GRID):
        op = ease((t - k * 0.15) / 0.5) * ease((d - t) / 0.4)
        cx = x0 + (k % 3) * (cw + gap) + cw / 2
        cy = y0 + (k // 3) * (chh + gap) + chh / 2
        cx = W / 2 + (cx - W / 2) * z
        cy = H / 2 - 50 + (cy - H / 2 + 50) * z
        device_card(fr, Clip(n), t, cx, cy, int(cw * z), op, start=k * 0.7, glow_col=(70, 80, 140))
    caption(fr, t, 0.8, d - 0.2, '每一个界面，都是一格格画出来的像素', '480 × 272，每秒刷新二十多帧', 'bottom', big=52)
    return fr


SPECS = [
    ('4.3″', '480 × 272 电容触摸屏'),
    ('ESP32-S3', '双核 240 MHz · Wi-Fi · 8 MB PSRAM'),
    ('3 个 Agent', 'Claude Code · Codex · DeepSeek Harness'),
    ('24 GHz', '毫米波雷达，知道你在不在座位'),
    ('AES-256', 'Wi-Fi 链路双向认证、逐帧加密'),
    ('0 颗螺丝', '四个 3D 打印零件，全卡扣'),
    ('¥175', '主板加全部配件的成本'),
    ('MIT', '固件、外壳、电脑端全部开源'),
]


def seg_specs(t, d):
    step = d / len(SPECS)
    i = min(len(SPECS) - 1, int(t / step))
    lt = t - i * step
    big, sub = SPECS[i]
    fr = black()
    key = ('spec', big)
    if key not in _cache:
        latin = all(ord(c) < 0x2E80 for c in big)
        f = font(190, 'Semibold', sf=True) if latin else font(170, 'Semibold')
        L = Image.new('RGBA', (W, 400), (0, 0, 0, 0))
        dd = ImageDraw.Draw(L)
        b = f.getbbox(big)
        # 主数字用渐变色：苹果发布会那种冷暖渐变
        mask = Image.new('L', (W, 400), 0)
        ImageDraw.Draw(mask).text(((W - (b[2] - b[0])) / 2 - b[0], 40), big, font=f, fill=255)
        gx = np.linspace(0, 1, W)[None, :, None]
        c0, c1 = np.array([255, 160, 110]), np.array([150, 130, 255])
        col = (c0 * (1 - gx) + c1 * gx).repeat(400, 0).astype(np.uint8)
        grad = Image.fromarray(col, 'RGB').convert('RGBA')
        grad.putalpha(mask)
        L.alpha_composite(grad)
        s = font(44, 'Regular')
        sb = s.getbbox(sub)
        dd.text(((W - (sb[2] - sb[0])) / 2, 300), sub, font=s, fill=GREY)
        _cache[key] = L
    L = _cache[key]
    op = min(ease(lt / 0.35), ease((step - lt) / 0.3))
    fr.alpha_composite(fade_alpha(L, op), ((W - L.size[0]) // 2, (H - L.size[1]) // 2))
    return fr


# ---------------- 时间线 ----------------
TIMELINE = [
    # (时长, 镜头)
    (6, seg_text([(0.6, 3.0, 'AI 正在替你写代码。', None, 'center'),
                  (3.3, 5.8, '它在想什么？做到哪了？在等你什么？', None, 'center')])),
    (14, seg_3d('opener_full', fade_in=1.0, wordmark=(7.0, 13.6, '放在桌上的 AI 编程搭子', 805))),  # 亮起 + 环绕一镜到底
    (2, seg_text([(0.15, 1.9, '从一块 ¥99 的屏幕开始。', None, 'center')])),
    (5, seg_parts),
    (10, seg_photos),
    (3, seg_text([(0.2, 2.8, '然后，给它一个家。', None, 'center')])),
    (8, seg_3d('assemble', [(0.8, 7.6, '3D 打印外壳', '四个零件，全卡扣，一颗螺丝都不用')])),
    (5, seg_3d('side', [(0.5, 4.7, '后仰 15°', '刚好对着你的视线')])),
    (6, seg_3d('back', [(0.6, 5.7, '喇叭、麦克风、电池', '都收进 130 × 87 mm 的机身里')], fade_out=0.6)),
    (3, seg_text([(0.3, 2.8, '它知道你的 AI 在做什么。', None, 'center')])),
    (4, seg_3d('f_buddy_claude', [(0.3, 3.9, 'Claude Code', '思考、写代码、运行命令、等你确认')], shrink=(0.78, -80))),
    (4, seg_3d('f_buddy_codex', [(0.3, 3.9, 'Codex', '云朵小机器人抱着笔记本敲代码')], shrink=(0.78, -80))),
    (4, seg_3d('f_buddy_deepseek', [(0.3, 3.9, 'DeepSeek', '小鲸鱼住在墙上的鱼缸里')], shrink=(0.78, -80))),
    (4, seg_3d('f_home_split', [(0.3, 3.9, '一起开工', '几个 Agent 同时干活，显示器自动分屏')], shrink=(0.78, -80))),
    (10, seg_apps),
    (4, seg_3d('f_magpie_usage', [(0.3, 3.9, '用量一目了然', 'Token、请求、费用、缓存命中率')], shrink=(0.78, -80))),
    (4, seg_3d('f_magpie_route', [(0.3, 3.9, '每一次请求', '都会从屏幕上飞过')], shrink=(0.78, -80))),
    (4, seg_3d('f_radar', [(0.3, 3.9, '毫米波雷达', '知道你在不在座位，身后有没有人靠近')], shrink=(0.78, -80))),
    (4, seg_3d('f_standby', [(0.3, 3.9, '离开时', '自动变成翻页钟')], shrink=(0.78, -80))),
    (4, seg_3d('f_welcome_back', [(0.3, 3.9, '回来时', '告诉你这段时间完成了什么')], shrink=(0.78, -80))),
    (10, seg_grid),
    (4, seg_flat('settings_sound', '会出声', '8-bit 提示音，中文语音播报')),
    (4, seg_flat('settings_pc', '拔掉线，也在线', '自动切到 Wi-Fi，双向认证 + AES-256-GCM 加密')),
    (4, seg_text([(0.2, 3.8, '一条命令，装好就能用。', 'macOS 与 Windows 都支持 · ./setup.sh', 'center')])),
    (10, seg_3d('turn')),
    (20, seg_specs),
    (12, seg_3d('lowfront', [(1.0, 11.4, '自己动手，做一台。', '外壳 STL、固件、电脑端，全部开源')])),
    (7, seg_3d('finale', wordmark=(1.2, 7.0, 'DIY by @bruc3van', 805), fade_out=1.6, shrink=(0.8, -90))),
    (1, lambda t, d: black()),
]


F_SHRINK = (0.78, -80)
# 1 分钟短版：只看成品——3D 镜头和屏幕上的各个功能
TIMELINE_SHORT = [
    (9.5, seg_3d('opener', fade_in=0.8, wordmark=(5.1, 9.3, '放在桌上的 AI 编程搭子', 805))),  # 亮起 + 环绕一镜到底
    (4, seg_3d('f_buddy_claude', [(0.3, 3.9, 'Claude Code', '思考、写代码、运行命令、等你确认')], shrink=F_SHRINK)),
    (4, seg_3d('f_buddy_codex', [(0.3, 3.9, 'Codex', '云朵小机器人抱着笔记本敲代码')], shrink=F_SHRINK)),
    (4, seg_3d('f_buddy_deepseek', [(0.3, 3.9, 'DeepSeek', '小鲸鱼住在墙上的鱼缸里')], shrink=F_SHRINK)),
    (4, seg_3d('f_home_split', [(0.3, 3.9, '一起开工', '几个 Agent 同时干活，显示器自动分屏')], shrink=F_SHRINK)),
    (5, seg_3d('turn', [(0.4, 4.8, '3D 打印外壳', '四个零件，全卡扣，一颗螺丝都不用')], t0=1.0)),
    (4, seg_3d('back', [(0.3, 3.8, '喇叭、麦克风、电池', '都收进 130 × 87 mm 的机身里')], t0=1.0)),
    (4, seg_3d('f_magpie_route', [(0.3, 3.9, '每一次请求', '都会从屏幕上飞过')], shrink=F_SHRINK)),
    (4, seg_3d('f_radar', [(0.3, 3.9, '毫米波雷达', '知道你在不在座位，身后有没有人靠近')], shrink=F_SHRINK)),
    (3.5, seg_3d('f_standby', [(0.3, 3.4, '离开时', '自动变成翻页钟')], shrink=F_SHRINK)),
    (3.5, seg_3d('f_welcome_back', [(0.3, 3.4, '回来时', '告诉你这段时间完成了什么')], shrink=F_SHRINK)),
    (5, seg_3d('lowfront', [(0.4, 4.8, '自己动手，做一台。', '外壳 STL、固件、电脑端，全部开源')], t0=3.0)),
    (5.5, seg_3d('finale', wordmark=(0.8, 5.5, 'DIY by @bruc3van', 805), fade_out=1.5, shrink=(0.8, -90), t0=1.5)),
]
FULL_TIMELINE = TIMELINE
CUTS = {  # --cut：时间线、配乐、输出文件、封面取哪一秒、封面文件
    'full': ('TIMELINE', 'music.wav', 'Agent-Buddy-宣传片.mp4', 17.5, 'cover.png'),
    'short': ('TIMELINE_SHORT', 'music_60.wav', 'Agent-Buddy-宣传片-60s.mp4', 17.5, 'cover.png'),  # 封面和完整版同一张
}
COVER_AT, COVER_PNG = 17.5, 'cover.png'   # 封面取这一刻的画面（主视角 + Agent Buddy 字标），main 里按 --cut 改
COVER_HOLD, COVER_FADE = 0.4, 0.6


def frame_at(T):
    start = 0.0
    for d, fn in FULL_TIMELINE:  # 封面都从完整版取
        if start <= T < start + d:
            return fn(T - start, d)
        start += d


def cover_frame():
    if 'cover' not in _cache:
        path = os.path.join(HERE, COVER_PNG)
        if os.path.exists(path):  # 已经定稿的封面直接用（换了开场镜头也不变）；删掉文件才会重新截
            _cache['cover'] = Image.open(path).convert('RGBA')
        else:
            _cache['cover'] = frame_at(COVER_AT)
            _cache['cover'].convert('RGB').save(path)
    return _cache['cover']


def main():
    global TIMELINE, COVER_AT, COVER_PNG
    a = sys.argv
    tl, music, name, COVER_AT, COVER_PNG = CUTS[a[a.index('--cut') + 1] if '--cut' in a else 'full']
    TIMELINE = globals()[tl]
    t_from = float(a[a.index('--from') + 1]) if '--from' in a else 0.0
    t_to = float(a[a.index('--to') + 1]) if '--to' in a else 1e9
    out = a[a.index('--out') + 1] if '--out' in a else os.path.join(HERE, name)
    total = sum(d for d, _ in TIMELINE)
    t_to = min(t_to, total)
    print(f'总长 {total:.1f} 秒，输出 {t_from:.1f}–{t_to:.1f}')
    ff = subprocess.Popen(['ffmpeg', '-v', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', f'{W}x{H}',
                           '-r', str(FPS), '-i', '-', '-ss', str(t_from), '-t', str(t_to - t_from),
                           '-i', os.path.join(HERE, music),
                           '-map', '0:v', '-map', '1:a', '-c:v', 'libx264', '-preset', 'slow', '-crf', '17',
                           '-pix_fmt', 'yuv420p', '-movflags', '+faststart', '-c:a', 'aac', '-b:a', '256k',
                           '-shortest', out], stdin=subprocess.PIPE)
    start = 0.0
    n = 0
    for d, fn in TIMELINE:
        end = start + d
        if end > t_from and start < t_to:
            f0 = max(0, round((t_from - start) * FPS))
            f1 = min(round(d * FPS), round((t_to - start) * FPS))
            for i in range(f0, f1):
                fr = fn(i / FPS, d)
                gt = start + i / FPS
                if gt < COVER_HOLD + COVER_FADE:  # 片头先停在封面上再淡入：微信、播放器拿第一帧当缩略图
                    k = 1 - ease((gt - COVER_HOLD) / COVER_FADE) if gt > COVER_HOLD else 1.0
                    fr = Image.blend(fr.convert('RGB'), cover_frame().convert('RGB'), k).convert('RGBA')
                ff.stdin.write(fr.convert('RGB').tobytes())
                n += 1
            print(f'{start:6.1f}s  {getattr(fn, "__name__", "")}', flush=True)
        start = end
    ff.stdin.close()
    ff.wait()
    tmp = out + '.tmp.mp4'
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', out, '-i', os.path.join(HERE, COVER_PNG), '-map', '0', '-map', '1',
                    '-c', 'copy', '-c:v:1', 'mjpeg', '-disposition:v:1', 'attached_pic', '-movflags', '+faststart', tmp],
                   check=True)
    os.replace(tmp, out)
    print('写出', n, '帧 →', out)


if __name__ == '__main__':
    main()
