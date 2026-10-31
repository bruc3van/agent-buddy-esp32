#!/usr/bin/env python3
"""还原 magpie 面板标题栏 logo 的 "wag" 动画（app.css: bird-bob / wing-flap / tail-flick，0.9s）。

从 magpie 的 internal/gui/assets/index.html 取 logo 的 <defs>（鸟的路径 + 身体/尾巴/翅膀裁剪区），
按 CSS 关键帧与 cubic-bezier(.3,0,.3,1) 逐帧算出各部位旋转角，用 resvg 渲染，转成 2bit 灰度头文件。

用法：uv run --with resvg-py --with pillow python tools/gen_wag.py <magpie 仓库> agent_buddy/magpie_wag.h
"""
import io
import re
import sys

import resvg_py
from PIL import Image

FPS, DUR = 25, 0.9
KEYS = {  # (时间比例, 角度)，旋转中心为 viewBox 坐标
    "bird": ((26, 38), [(0, 0), (.18, -4), (.40, 2), (.62, -1), (1, 0)]),
    "tail": ((14, 22), [(0, 0), (.18, 16), (.36, -7), (.54, 9), (.72, -3), (.86, 2), (1, 0)]),
    "wing": ((30, 19.8), [(0, 0), (.12, 10), (.26, -3), (.40, 8), (.56, -2), (.72, 3), (1, 0)]),
}


def bezier(t, x1=.3, y1=0, x2=.3, y2=1):
    """CSS cubic-bezier：给定进度 x 求 y。"""
    lo, hi = 0.0, 1.0
    for _ in range(40):
        u = (lo + hi) / 2
        x = 3 * (1 - u) ** 2 * u * x1 + 3 * (1 - u) * u ** 2 * x2 + u ** 3
        lo, hi = (u, hi) if x < t else (lo, u)
    u = (lo + hi) / 2
    return 3 * (1 - u) ** 2 * u * y1 + 3 * (1 - u) * u ** 2 * y2 + u ** 3


def angle(keys, p):
    for (t0, a0), (t1, a1) in zip(keys, keys[1:]):
        if t0 <= p <= t1:
            return a0 + (a1 - a0) * bezier((p - t0) / (t1 - t0) if t1 > t0 else 1)
    return 0


def frame_svg(defs, p, size):
    rot = {k: f'rotate({angle(v[1], p):.3f} {v[0][0]} {v[0][1]})' for k, v in KEYS.items()}
    return (f'<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" viewBox="0 0 44 44" '
            f'width="{size}" height="{size}" color="#000">{defs}'
            f'<g transform="{rot["bird"]}"><use href="#bird" clip-path="url(#bird-body)"/>'
            f'<g transform="{rot["tail"]}"><use href="#bird" clip-path="url(#bird-tail)"/></g>'
            f'<g transform="{rot["wing"]}"><use href="#bird" clip-path="url(#bird-wing)"/></g></g></svg>')


def pack(png, size):
    im = Image.open(io.BytesIO(png)).convert("RGBA")
    px = im.load()
    out = bytearray()
    for y in range(size):
        for x in range(0, size, 4):
            b = 0
            for k in range(4):
                a = px[x + k, y][3] if x + k < size else 0
                b = (b << 2) | min(3, (a + 42) // 85)
            out.append(b)
    return out


def emit(name, frames):
    n = len(frames[0])
    rows = ",\n".join("  {" + ",".join(map(str, f)) + "}" for f in frames)
    return f"#define {name.upper()}_FRAMES {len(frames)}\nconst uint8_t {name}[{len(frames)}][{n}] PROGMEM = {{\n{rows}\n}};\n"


html = open(sys.argv[1] + "/internal/gui/assets/index.html", encoding="utf-8").read()
defs = re.search(r'<span class="logo".*?(<defs>.*?</defs>)', html, re.S).group(1)
defs = defs.replace('fill="currentColor"', 'fill="#000"')
count = round(FPS * DUR) + 1
out = ["// 由 tools/gen_wag.py 生成：magpie logo 的 wag 动画（bird-bob / wing-flap / tail-flick），来自 magpie（MIT）",
       "#pragma once", "#include <Arduino.h>", ""]
for size in (28, 100):
    frames = [pack(resvg_py.svg_to_bytes(svg_string=frame_svg(defs, i / (count - 1), size)), size) for i in range(count)]
    out.append(f"#define WAG{size}_SIZE {size}")
    out.append(emit(f"wag{size}", frames))
open(sys.argv[2], "w").write("\n".join(out))
print("frames", count)
