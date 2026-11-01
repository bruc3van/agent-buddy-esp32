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


def caption(frame, t, a, b, title, sub=None, pos='bottom', big=76, sub_size=46, align='center'):  # 字号按手机竖屏看横屏视频定
    """苹果风字幕：主标题白色半粗，副标题灰色；淡入时往上浮 24px"""
    op, p = window(t, a, b)
    if op <= 0:
        return
    key = (title, sub, big, sub_size, align)
    if key not in _cache:
        lines = [(title, font(big), WHITE, 0)] if title else []  # title=None：只有一行小字
        if sub:
            lines.append((sub, font(sub_size, 'Regular'), GREY, 6))
        _cache[key] = text_layer(lines, align)
    layer = _cache[key]
    lw, lh = layer.size
    if pos == 'bottom':
        x, y = (W - lw) // 2, H - lh - 70
    elif pos == 'top':
        x, y = (W - lw) // 2, 60
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
        im = Image.open(os.path.join(ROOT, path)).convert('RGB')
        a = np.asarray(im).astype(np.float32)
        if path.startswith('docs/images/'):  # 手机拍的偏暖偏亮，和 3D 渲染的冷暗色调接不上：去暖、压暗、降一点饱和
            g = a.mean(2, keepdims=True)
            a = (a * 0.85 + g * 0.15) * np.array([0.9, 0.95, 1.04]) * 0.86
        elif path.startswith('case/preview/'):  # 建模预览是浅灰底，抠成深底，免得在黑场之间闪白
            bg = np.array([244, 243, 248], np.float32)
            k = np.clip((np.abs(a - bg).max(2, keepdims=True) - 6) / 30, 0, 1)
            a = a * k + np.array([14, 14, 17]) * (1 - k)
        im = Image.fromarray(np.clip(a, 0, 255).astype(np.uint8))
        _cache[key] = im
    return _cache[key]


# ---------------- 镜头 ----------------
def seg_text(lines_spec):
    """黑底文字卡：[(出现, 消失, 主, 副, 位置)]"""
    def f(t, d):
        fr = black()
        for a, b, title, sub, pos in lines_spec:
            caption(fr, t, a, b, title, sub, pos, big=84)
        return fr
    return f


F_SHRINK = (0.74, -95)  # 功能近景缩小上移，给下面的大字幕让位置
FINALE_SHRINK = (0.76, -120, 340)  # 片尾：羽化放宽，地面的灰不会露出一圈方框
DUO_SUB = '两套配色：经典黑灰 · 米白 × Claude 橙\nDIY by @bruc3van · github.com/bruc3van/agent-buddy-esp32'
DUO_SHRINK = (0.8, -110, 340)  # 片尾定格：两台并排，缩小上移给字标让位置
FINALE_SUB = 'DIY by @bruc3van\ngithub.com/bruc3van/agent-buddy-esp32'


def seg_3d(name, captions=(), fade_in=0.0, fade_out=0.0, wordmark=None, shrink=None, t0=0.0, sfx=()):
    """t0：从镜头的第几秒开始用（短版截取片段）；shrink = (比例, 上移, [羽化宽度])；sfx = [(秒, 音效名 / 'say:文字')]"""
    seq = Seq(name)

    def f(t, d):
        fr = seq.frame(t + t0)
        if shrink:  # 画面缩小上移，给下方的字留位置（背景是纯黑，四周补黑看不出来）
            k, dy = shrink[:2]
            small = fr.resize((int(W * k), int(H * k)), Image.LANCZOS)
            small.putalpha(_feather(small.size, *shrink[2:]))
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
    f.sfx = [(a - t0, n) for a, n in sfx]
    return f


_feathers = {}


def _feather(size, edge=160):
    """四周 edge 像素渐隐的遮罩"""
    if (size, edge) not in _feathers:
        w, h = size
        x = np.minimum(np.arange(w), np.arange(w)[::-1]) / edge
        y = np.minimum(np.arange(h), np.arange(h)[::-1]) / edge
        m = np.clip(np.minimum(x[None, :], y[:, None]), 0, 1)
        _feathers[size, edge] = Image.fromarray((m * m * (3 - 2 * m) * 255).astype(np.uint8))
    return _feathers[size, edge]


def wm_layer(fr, t, a, b, sub=None, y=None):
    """「Agent Buddy」字标"""
    op, p = window(t, a, b, fi=1.0, fo=0.8)
    if op <= 0:
        return
    key = ('wm', sub)
    if key not in _cache:
        lines = [('Agent Buddy', font(112, 'Semibold', sf=True), WHITE, 0)]
        subs = sub.split('\n') if sub else []
        for i, line in enumerate(subs):  # 第一行灰色大一号；最后一行（仓库地址）蓝色
            url = i == len(subs) - 1 and i > 0
            lines.append((line, font(50 if i == 0 else 40, 'Regular'), (120, 150, 255) if url else GREY, 4 if i == 0 else 14))
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


def seg_flat(name, title, sub, start=0.0, sfx=()):
    """平面像素屏：录屏铺成大卡片"""
    def f(t, d):
        clip = Clip(name)
        fr = black()
        w = 1200  # 固定大小：逐帧缩放会让像素抖动
        op = ease(t / 0.4) * ease((d - t) / 0.3)
        device_card(fr, clip, t, W / 2, H / 2 - 50, w, op, start)
        caption(fr, t, 0.3, d - 0.2, title, sub, 'bottom', big=72)
        return fr
    f.sfx = list(sfx)
    return f


PARTS = [  # 高清商品图（promo/parts/hd-*.webp）、裁剪框（None = 整张）、名字、规格、到手价
    ('hd-1-board', None, 'JC4827W543', 'ESP32-S3 · 4.3″ 480×272 电容触摸屏', '¥99'),
    ('hd-2-radar', None, 'HLK-LD2450', '24 GHz 毫米波雷达', '¥28.2'),
    ('hd-3-mic', None, 'INMP441', 'I2S 数字麦克风', '¥15.8'),
    ('hd-4-speaker', None, '2030 腔体喇叭', '8Ω · 0.5–2W', '¥5.4'),
    ('hd-5-battery', (0, 0, 1106, 548), '聚合物锂电池', '3.7V · 1200 mAh', '¥13.86'),
    ('hd-6-wire', (40, 175, 1180, 1000), '端子转杜邦线', 'XH2.54 / PH2.0 / 1.25 多规格', '¥12.74'),
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
        _cache[key] = text_layer([(name, font(72), WHITE, 0), (sub, font(44, 'Regular'), GREY, 6),
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
    fr = Image.new('RGBA', (W, H), (14, 14, 17, 255))
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
    caption(fr, t, 0.2, step * 2 - 0.1, '买来零件', '屏幕主板、雷达、麦克风、喇叭、电池', 'bottom', big=72)
    caption(fr, t, step * 2 + 0.1, step * 5 - 0.1, '亲手接线', '一根线、一个口，慢慢拼起来', 'bottom', big=72)
    caption(fr, t, step * 5 + 0.1, d - 0.1, '自己设计外壳', '参数化建模，改一个数字就能重新出图', 'bottom', big=72)
    return fr


SIDEBAR = {'claude.png': 580, 'dsh.png': 575}  # 侧栏右边界（截图像素）
APPS = [('promo/apps/claude.png', 'Claude Code', '你照常在电脑上写代码'),
        ('promo/apps/dsh.png', 'DeepSeek Harness', 'Codex、DeepSeek 一样接得上')]
APP_CROP = {'claude.png': (200, 1150, 1800, 1952), 'dsh.png': (180, 150, 1500, 960)}  # 去掉侧栏后再裁：只留对话区关键部分，放大看得清


def app_img(path):
    key = ('app', path)
    if key not in _cache:
        im = Image.open(os.path.join(ROOT, path)).convert('RGBA')
        if 'claude' in path or 'dsh' in path:  # 左侧是对话 / 工作区列表，涉及隐私，只留右边的对话区
            im = im.crop((SIDEBAR[os.path.basename(path)], 0, im.size[0], im.size[1])).crop(APP_CROP[os.path.basename(path)])
            im = Image.fromarray((np.asarray(im.convert('RGB')) * 0.86).astype(np.uint8))  # 浅色窗口压暗一点，夹在黑场之间不刺眼
            im = rounded(im, 22)
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
    caption(fr, lt, 0.3, step - 0.15, title, sub, 'bottom', big=68)
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
    caption(fr, t, 0.8, d - 0.2, '每一个界面，都是一格格画出来的像素', '480 × 272，每秒刷新二十多帧', 'bottom', big=68)
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
    op = ease(lt / 0.15)  # 一张接一张直接切，不再每张都淡出到黑场
    fr.alpha_composite(fade_alpha(L, op), ((W - L.size[0]) // 2, (H - L.size[1]) // 2))
    return fr


# ---------------- 时间线 ----------------


# 1 分钟短版：只看成品——3D 镜头和屏幕上的各个功能
SPLIT_SFX = [(2.97, 'done')]  # 录屏里 Claude 的显示器 2.97 秒变成 ✓
WELCOME_SFX = [(0.55, 'alert')]  # 弹窗 0.5 秒出来，板子叮咚一声（不放语音播报）

TIMELINE_SHORT = [
    (9.5, seg_3d('opener', fade_in=0.8, wordmark=(5.1, 9.3, '放在桌上的 AI 编程搭子', 805))),  # 亮起 + 环绕一镜到底
    (4, seg_3d('f_buddy_claude', [(0.3, 3.9, 'Claude Code', '思考、写代码、运行命令、等你确认')], shrink=F_SHRINK)),
    (4, seg_3d('f_buddy_codex', [(0.3, 3.9, 'Codex', '云朵小机器人抱着笔记本敲代码')], shrink=F_SHRINK)),
    (4, seg_3d('f_buddy_deepseek', [(0.3, 3.9, 'DeepSeek', '小鲸鱼住在墙上的鱼缸里')], shrink=F_SHRINK)),
    (4, seg_3d('f_home_split', [(0.3, 3.9, '一起开工', '几个 Agent 同时干活，显示器自动分屏')], shrink=F_SHRINK, sfx=SPLIT_SFX)),
    (5, seg_3d('turn', [(0.4, 4.8, '3D 打印外壳', '四个零件，全卡扣，一颗螺丝都不用')], t0=2.5)),  # 转到 124°，背面镜头从 130° 接着转
    (4, seg_3d('back', [(0.3, 3.8, '喇叭、麦克风、电池', '都收进 130 × 89 mm 的机身里')], t0=1.0)),
    (4, seg_3d('f_magpie_route', [(0.3, 3.9, '每一次请求', '都会从屏幕上飞过')], shrink=F_SHRINK)),
    (4, seg_3d('f_radar', [(0.3, 3.9, '毫米波雷达', '知道你在不在座位，身后有没有人靠近')], shrink=F_SHRINK)),
    (3.5, seg_3d('f_standby', [(0.3, 3.4, '离开时', '自动变成翻页钟')], shrink=F_SHRINK)),
    (3.5, seg_3d('f_welcome_back', [(0.3, 3.4, '回来时', '告诉你这段时间完成了什么')], shrink=F_SHRINK, sfx=WELCOME_SFX)),
    (5, seg_3d('lowfront', [(0.4, 4.8, '自己动手，做一台。', '外壳 STL、固件、电脑端，全部开源')], t0=3.0)),
    (2, seg_3d('finale', fade_out=0.4, shrink=FINALE_SHRINK, t0=1.5)),
    (3.5, seg_3d('duo', fade_in=0.4, wordmark=(0.3, 99, DUO_SUB, 745), shrink=DUO_SHRINK, t0=1.5)),  # 定格在两套配色上
]

# ---------------- 60 秒 DIY 版：零件 → 接线 → 画外壳 → 3D 打印组装 → 成品 ----------------
B = 60 / 118.05  # 配乐一拍（118 BPM），剪辑点都落在拍子上


def seg_wiring(t, d):
    """手拍接线照片：2 拍、3 拍、3 拍（屏幕实拍偏色，不用）"""
    shots = [
        (2, lambda u, s: full(u, 'docs/images/photo-back.jpg', s)),
        (3, lambda u, s: panels(u, PORTRAITS[:3], s)),
        (3, lambda u, s: full(u, 'docs/images/photo-radar.jpg', s)),
    ]
    a = 0.0
    for beats, shot in shots:
        if t < a + beats * B or beats == shots[-1][0] and shot is shots[-1][1]:
            fr = shot(t - a, beats * B)
            break
        a += beats * B
    caption(fr, t, 0.2, d - 0.1, '亲手接线', '一根线、一个口，慢慢拼起来', 'bottom')
    return fr


BLENDER = [  # Blender 界面截图（promo/blender_shots.py 截的，3840×2160）、取景框（None = 整个窗口）
    ('1-all', None),
    ('2-explode', (0, 90, 3160, 1868)),
    ('3-xray', (0, 90, 3160, 1868)),
    ('4-print', (300, 260, 3060, 1812)),
]


def seg_design(t, d):
    """在 Blender 里画外壳：先看整个窗口，再切进视图里看爆炸图、线框、打印摆放，每张两拍慢慢推近"""
    step = d / len(BLENDER)
    i = min(len(BLENDER) - 1, int(t / step))
    lt = t - i * step
    name, box = BLENDER[i]
    key = ('blender', name)
    if key not in _cache:
        im = Image.open(os.path.join(HERE, 'blender', name + '.png')).convert('RGB')
        _cache[key] = im.crop(box) if box else im
    fr = cover(_cache[key], W, H, zoom=1.0 + 0.05 * lt / step).convert('RGBA')
    caption(fr, t, 0.2, d - 0.1, '自己画外壳', '在 Blender 里建模，四个零件分开打印', 'bottom')
    return fr


def seg_parts_diy(t, d):
    fr = seg_parts(t, d)
    caption(fr, t, 0.1, d - 0.1, None, '买来零件，一共 ¥175', 'top')
    return fr


TIMELINE_DIY = [
    (6 * B, seg_text([(0.5, 6 * B - 0.1, '¥175，自己做一台 AI 编程搭子。', '买零件、接线、画外壳、3D 打印、组装', 'center')])),
    (12 * B, seg_parts_diy),
    (8 * B, seg_wiring),
    (8 * B, seg_design),
    (16 * B, seg_3d('assemble', [(0.6, 16 * B - 0.2, '3D 打印外壳', '四个零件，全卡扣，一颗螺丝都不用')], sfx=[(6.3, 'poke')])),
    (6 * B, seg_3d('side', [(0.3, 6 * B - 0.2, '后仰 15°', '刚好对着你的视线')], t0=1.6)),  # 侧面镜头开头太黑，从 1.6 秒用起
    (10 * B, seg_3d('back', [(0.4, 10 * B - 0.2, '喇叭、麦克风、电池', '都收进 130 × 89 mm 的机身里')], t0=0.6)),
    (7 * B, seg_3d('f_home_split', [(0.3, 7 * B - 0.1, '装好，接上电脑', 'Claude Code、Codex 在干什么，一眼就知道')],
                   shrink=F_SHRINK, sfx=SPLIT_SFX)),
    (7 * B, seg_3d('f_radar', [(0.3, 7 * B - 0.1, '毫米波雷达', '知道你在不在座位')], shrink=F_SHRINK)),
    (7 * B, seg_3d('f_standby', [(0.3, 7 * B - 0.1, '离开时', '自动变成翻页钟')], shrink=F_SHRINK)),
    (7 * B, seg_3d('f_welcome_back', [(0.3, 7 * B - 0.1, '回来时', '告诉你这段时间完成了什么')], shrink=F_SHRINK, sfx=WELCOME_SFX)),
    (8 * B, seg_3d('lowfront', [(0.4, 8 * B - 0.2, '自己动手，做一台。', '外壳 STL、固件、电脑端，全部开源')], t0=3.0)),
    (6 * B, seg_3d('finale', fade_out=0.4, shrink=FINALE_SHRINK)),
    (60 - 108 * B, seg_3d('duo', fade_in=0.4, wordmark=(0.4, 99, DUO_SUB, 745), shrink=DUO_SHRINK)),  # 定格在两套配色上
]


# ---------------- 完整版 ----------------
MONO = '/System/Library/Fonts/SFNSMono.ttf'
SETUP_LINES = [  # 照抄 setup.sh → buddy.py setup 的输出（步骤标题、✓ 行）
    ('$ ./setup.sh', WHITE),
    ('Agent Buddy 一键配置（macOS）', WHITE),
    ('', None),
    ('[1] 检查 Python 环境', (130, 170, 255)),
    ('  ✓ Python 3.13.14，依赖齐全', (110, 220, 140)),
    ('[2] 注册 Claude Code / Codex / DeepSeek Harness 的 hooks', (130, 170, 255)),
    ('[3] 连接板子', (130, 170, 255)),
    ('  · 板子上已经是 Agent Buddy 固件 4.1.1，跳过烧录', GREY),
    ('[4] 开机自启', (130, 170, 255)),
    ('[5] 启动并自检', (130, 170, 255)),
    ('  ✓ bridge 已连上板子', (110, 220, 140)),
    ('  ✓ 已发一条测试通知：屏幕上应该弹出「欢迎回来」', (110, 220, 140)),
]
SETUP_T = [0.0, 0.9, 0.9, 1.1, 1.4, 1.6, 2.0, 2.3, 2.6, 2.8, 3.2, 3.5]  # 每行出现的时间（第一行是逐字敲出来的）
SETUP_DONE = SETUP_T[-1]


def mono_text(d, xy, txt, col, size=30):
    """英文数字用等宽字体，中文用苹方"""
    x, y = xy
    fm, fc = ImageFont.truetype(MONO, size), font(size, 'Regular')
    for ch in txt:
        f = fm if ord(ch) < 0x2E80 and ch not in '·「」' else fc  # ✓ 苹方里没有，用等宽字体的
        d.text((x, y), ch, font=f, fill=col)
        x += f.getlength(ch)


def seg_setup(t, d):
    """左边终端跑 ./setup.sh，最后一行打出来的那一刻，右边板子弹出「欢迎回来」"""
    fr = Image.new('RGBA', (W, H), (10, 10, 12, 255))
    tw, th = 1040, 640
    term = Image.new('RGBA', (tw, th), (28, 28, 32, 255))
    dd = ImageDraw.Draw(term)
    dd.rectangle((0, 0, tw, 44), fill=(44, 44, 50))
    for k, c in enumerate(((255, 95, 87), (254, 188, 46), (40, 200, 64))):
        dd.ellipse((18 + k * 26, 15, 32 + k * 26, 29), fill=c)
    yy = 70
    for (txt, col), at in zip(SETUP_LINES, SETUP_T):
        if t >= at and txt:
            shown = txt[:2 + int((t - 0.15) / 0.06)] if at == 0 else txt  # 命令逐字敲出
            mono_text(dd, (28, yy), shown, col, 28)
        yy += 44
    term = rounded(term, 18)
    sh, pad = shadow(term.size, 18)
    x, y = 80, (H - th) // 2 - 60
    op = ease(t / 0.4)
    fr.alpha_composite(fade_alpha(sh, op), (x - pad, y - pad + 20))
    fr.alpha_composite(fade_alpha(term, op), (x, y))
    if t < SETUP_DONE - 0.5:
        device_card(fr, Clip('home_idle'), t, 1500, H / 2 - 60, 640, op)
    else:  # 录屏 0.5 秒时弹窗出来，正好对上最后一行
        device_card(fr, Clip('welcome_back'), t - (SETUP_DONE - 0.5), 1500, H / 2 - 60, 640)
    caption(fr, t, 0.3, d - 0.2, '一条命令，装好就能用', 'macOS 与 Windows 都支持', 'bottom')
    return fr


seg_setup.sfx = [(SETUP_DONE, 'alert')]
SOUND_SFX = [(0.7, 'start'), (1.4, 'done'), (2.7, 'alert')]  # 「会出声」：板子上的几种提示音（不放语音）

TIMELINE = [
    (6, seg_text([(0.6, 3.0, 'AI 正在替你写代码。', None, 'center'),
                  (3.3, 5.8, '它在想什么？做到哪了？在等你什么？', None, 'center')])),
    (14, seg_3d('opener_full', fade_in=1.0, wordmark=(7.0, 13.6, '放在桌上的 AI 编程搭子', 805))),  # 亮起 + 环绕一镜到底
    (2, seg_text([(0.15, 1.9, '从一块 ¥99 的屏幕开始。', None, 'center')])),
    (6, seg_parts),
    (6, seg_wiring),
    (5, seg_design),
    (8, seg_3d('assemble', [(0.8, 7.6, '3D 打印外壳', '四个零件，全卡扣，一颗螺丝都不用')], sfx=[(6.3, 'poke')])),
    (4, seg_3d('side', [(0.4, 3.8, '后仰 15°', '刚好对着你的视线')], t0=1.6)),
    (5, seg_3d('back', [(0.5, 4.7, '喇叭、麦克风、电池', '都收进 130 × 89 mm 的机身里')], fade_out=0.5, t0=0.6)),
    (3, seg_text([(0.3, 2.8, '它知道你的 AI 在做什么。', None, 'center')])),
    (3.5, seg_3d('f_buddy_claude', [(0.3, 3.4, 'Claude Code', '思考、写代码、运行命令、等你确认')], shrink=F_SHRINK)),
    (3.5, seg_3d('f_buddy_codex', [(0.3, 3.4, 'Codex', '云朵小机器人抱着笔记本敲代码')], shrink=F_SHRINK)),
    (3.5, seg_3d('f_buddy_deepseek', [(0.3, 3.4, 'DeepSeek', '小鲸鱼住在墙上的鱼缸里')], shrink=F_SHRINK)),
    (3.5, seg_3d('f_home_split', [(0.3, 3.4, '一起开工', '几个 Agent 同时干活，显示器自动分屏')], shrink=F_SHRINK, sfx=SPLIT_SFX)),
    (3, seg_3d('hero', t0=0.5)),  # 整机换口气：连着几个屏幕近景之后，让观众重新看到整台机器
    (7, seg_apps),
    (3.5, seg_3d('f_magpie_usage', [(0.3, 3.4, '用量一目了然', 'Token、请求、费用、缓存命中率')], shrink=F_SHRINK)),
    (3.5, seg_3d('f_magpie_route', [(0.3, 3.4, '每一次请求', '都会从屏幕上飞过')], shrink=F_SHRINK)),
    (3, seg_3d('hero', t0=4.5)),  # 整机
    (3.5, seg_3d('f_radar', [(0.3, 3.4, '毫米波雷达', '知道你在不在座位，身后有没有人靠近')], shrink=F_SHRINK)),
    (3.5, seg_3d('f_standby', [(0.3, 3.4, '离开时', '自动变成翻页钟')], shrink=F_SHRINK)),
    (3.5, seg_3d('f_welcome_back', [(0.3, 3.4, '回来时', '告诉你这段时间完成了什么')], shrink=F_SHRINK, sfx=WELCOME_SFX)),
    (5, seg_grid),
    (4.5, seg_flat('settings_sound', '会出声', '完成、等你确认、出错，各有各的提示音', sfx=SOUND_SFX)),
    (4, seg_flat('settings_pc', '拔掉线，也在线', '自动切到 Wi-Fi，双向认证 + AES-256-GCM 加密')),
    (6, seg_setup),
    (10, seg_3d('turn')),
    (12, seg_specs),
    (10, seg_3d('lowfront', [(0.8, 9.6, '自己动手，做一台。', '外壳 STL、固件、电脑端，全部开源')], t0=1.0)),
    (3.5, seg_3d('finale', fade_out=0.4, shrink=FINALE_SHRINK)),
    (6, seg_3d('duo', fade_in=0.4, wordmark=(0.4, 99, DUO_SUB, 745), shrink=DUO_SHRINK)),  # 定格在两套配色上
]

FULL_TIMELINE = TIMELINE
CUTS = {  # --cut：时间线、配乐、输出文件、封面取哪一秒、封面文件
    'full': ('TIMELINE', 'music.wav', 'Agent-Buddy-宣传片.mp4', 17.5, 'cover.png'),
    'short': ('TIMELINE_SHORT', 'music_60.wav', 'Agent-Buddy-宣传片-60s.mp4', 17.5, 'cover.png'),  # 封面和完整版同一张
    'diy': ('TIMELINE_DIY', 'music_60.wav', 'Agent-Buddy-DIY-60s.mp4', 17.5, 'cover.png'),
}
COVER_AT, COVER_PNG = 17.5, 'cover.png'   # 封面取这一刻的画面（主视角 + Agent Buddy 字标），main 里按 --cut 改
COVER_HOLD, COVER_FADE = 0.4, 0.6


# ---------------- 声音：板子的 8-bit 提示音和语音播报，混进配乐 ----------------
SR = 48000
SFX = {  # 照抄固件 hw_spk.ino 的 sfxPlay：(频率 Hz 或 0 = 停顿, 毫秒, 音量, 波形 0 方波 / 1 三角波)
    'start': [(1320, 45, 0.18, 1)],
    'done': [(784, 70, 0.22, 0), (988, 70, 0.22, 0), (1175, 70, 0.22, 0), (1568, 180, 0.22, 0)],
    'alert': [(988, 130, 0.25, 0), (0, 50, 0, 0), (1319, 200, 0.25, 0)],
    'poke': [(1047, 50, 0.2, 0)],
}


def sfx_wave(name):
    out = []
    for f, ms, amp, wave in SFX[name]:
        n = SR * ms // 1000
        i = np.arange(n)
        if not f:
            out.append(np.zeros(n))
            continue
        ph = (i * f / SR) % 1.0
        w = np.where(ph < 0.5, 1.0, -1.0) if wave == 0 else np.where(ph < 0.5, 4 * ph - 1, 3 - 4 * ph)
        env = np.minimum(1, np.minimum(i / (SR * 0.003), (n - i) / (SR * 0.015)))
        out.append(amp * env * w)
    return np.concatenate(out)


def say_wave(text):
    """和 bridge 一样用 macOS 的 say（婷婷），16 kHz 生成再升到 48 kHz"""
    path = os.path.join(HERE, '.say.wav')
    subprocess.run(['say', '-v', 'Tingting', '-o', path, '--data-format=LEI16@16000', '--', text], check=True)
    raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', path, '-ar', str(SR), '-ac', '1', '-f', 'f32le', '-'],
                         capture_output=True, check=True).stdout
    os.remove(path)
    return np.frombuffer(raw, np.float32).astype(np.float64) * 0.9


def speaker(x):
    """板子上是 2030 小喇叭：低频出不来，高频也有限"""
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1 / SR) + 1e-3
    X /= np.sqrt(1 + (350 / f) ** 4) * np.sqrt(1 + (f / 6500) ** 4)
    return np.fft.irfft(X, len(x))


def mix_audio(music, timeline):
    """把各镜头的 sfx 按时间铺好；出声时配乐压低一些，提示音才听得清"""
    raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', os.path.join(HERE, music), '-ar', str(SR), '-ac', '2',
                          '-f', 'f32le', '-'], capture_output=True, check=True).stdout
    m = np.frombuffer(raw, np.float32).reshape(-1, 2).astype(np.float64)
    fx = np.zeros(len(m))
    duck = np.zeros(len(m))
    start = 0.0
    for d, fn in timeline:
        for at, name in getattr(fn, 'sfx', []):
            if not 0 <= at < d:
                continue
            x = say_wave(name[4:]) if name.startswith('say:') else sfx_wave(name)
            x = speaker(np.concatenate([x, np.zeros(SR // 10)]))
            i = int((start + at) * SR)
            j = min(len(fx), i + len(x))
            fx[i:j] += x[:j - i]
            duck[max(0, i - SR // 10):j] = 1
        start += d
    k = int(0.15 * SR)  # 压低和恢复各 150ms 的斜坡
    duck = np.convolve(duck, np.ones(k) / k, 'same')
    y = m * (1 - 0.55 * duck)[:, None] + fx[:, None] * 0.5
    y /= max(1.0, np.abs(y).max() / 0.95)
    path = os.path.join(HERE, '.mix.wav')
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'f32le', '-ar', str(SR), '-ac', '2', '-i', '-',
                    '-c:a', 'pcm_s16le', path], input=y.astype(np.float32).tobytes(), check=True)
    return path


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
    mixed = mix_audio(music, TIMELINE)
    ff = subprocess.Popen(['ffmpeg', '-v', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', f'{W}x{H}',
                           '-r', str(FPS), '-i', '-', '-ss', str(t_from), '-t', str(t_to - t_from),
                           '-i', mixed,
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
    os.remove(mixed)
    tmp = out + '.tmp.mp4'
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', out, '-i', os.path.join(HERE, COVER_PNG), '-map', '0', '-map', '1',
                    '-c', 'copy', '-c:v:1', 'mjpeg', '-disposition:v:1', 'attached_pic', '-movflags', '+faststart', tmp],
                   check=True)
    os.replace(tmp, out)
    print('写出', n, '帧 →', out)


if __name__ == '__main__':
    main()
