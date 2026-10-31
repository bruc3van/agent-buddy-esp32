#!/usr/bin/env python3
"""把 magpie 的 Agent / 订阅图标渲染成 16px 像素图标，并提取 logo 主题色，生成 agent_buddy/brand_icons.h。

- Agent 名称 → 图标：读 magpie 源码 internal/agent 里的 Name / Icon
- 渲染：64px 高清渲染后缩到 16px，不透明度按 50% 二值化（像素风，边缘干净）
- 主题色：logo 里饱和度最高的那一类颜色；黑白单色 logo 标为 mono，在深色底上用浅色画

用法：uv run --with resvg-py --with pillow python tools/gen_icons.py <magpie 仓库> agent_buddy/brand_icons.h
"""
import colorsys
import glob
import io
import os
import re
import sys
from collections import Counter

import resvg_py
from PIL import Image

MAGPIE, OUT = sys.argv[1], sys.argv[2]
ICONS = MAGPIE + "/internal/gui/assets/icons/"
OVERRIDES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "logo-overrides", "")  # 官网原版 logo，优先于 magpie 的
SIZE = 16

names = {}
for f in glob.glob(MAGPIE + "/internal/agent/**/*.go", recursive=True):
    if f.endswith("_test.go"):
        continue
    for name, icon in re.findall(r'Name:\s*"([^"]+)",[^}]{0,200}?Icon:\s*"([^"]+)"', open(f, encoding="utf-8").read()):
        names.setdefault(name, icon)
# 额度页分组用到的订阅图标（bridge 发来的 icon 名）
extra = ["vscode", "claude-color", "codex-color", "xai", "grok", "gemini-color", "copilot-color", "githubcopilot", "openai",
         "anthropic", "deepseek-color", "zhipu-color", "kimi-color", "moonshot", "qwen-color", "minimax-color"]

# 另一类写法：piLike(at, "pi", "Pi", ...) 或 {id: "pi", name: "Pi", ...}：id 有同名图标就用它
for f in glob.glob(MAGPIE + "/internal/agent/**/*.go", recursive=True):
    if f.endswith("_test.go"):
        continue
    src = open(f, encoding="utf-8").read()
    for aid, name in re.findall(r'\(\w+,\s*"([\w-]+)",\s*"([^"]+)"', src) + re.findall(r'id:\s*"([\w-]+)",\s*name:\s*"([^"]+)"', src):
        for cand in (aid, aid + "-color"):
            if os.path.exists(ICONS + cand + ".svg") or os.path.exists(ICONS + cand + ".png") or os.path.exists(OVERRIDES + cand + ".svg"):
                names.setdefault(name, cand)
                break
icons = sorted({i for i in names.values()} | {e for e in extra})
names.setdefault("VS Code", "vscode")  # 名字在 vscode.go 里是拼出来的，正则匹配不到
names.setdefault("VS Code Insiders", "vscode")


RASTER = set()  # 位图格式的图标（单色时按亮部取笔画，见下）


def load(icon, px):
    for ext in (".svg", ".png"):
        p = OVERRIDES + icon + ext
        if not os.path.exists(p):
            p = ICONS + icon + ext
        if not os.path.exists(p):
            continue
        if ext == ".svg":
            svg = open(p, encoding="utf-8").read().replace("currentColor", "#000")
            svg = re.sub(r'<svg([^>]*?)\swidth="[^"]*"', r"<svg\1", svg)
            svg = re.sub(r'<svg([^>]*?)\sheight="[^"]*"', r"<svg\1", svg)
            svg = svg.replace("<svg", f'<svg width="{px}" height="{px}"', 1)
            return Image.open(io.BytesIO(resvg_py.svg_to_bytes(svg_string=svg))).convert("RGBA")
        RASTER.add(icon)
        return Image.open(p).convert("RGBA").resize((px, px), Image.LANCZOS)
    return None


def strip_white_bg(im):
    """去掉 logo 外围的近白色底（从四角泛洪），深色界面上才不会出现白方块。"""
    w, h = im.size
    px = im.load()
    seen, stack = set(), [(0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1)]
    while stack:
        x, y = stack.pop()
        if (x, y) in seen or not (0 <= x < w and 0 <= y < h):
            continue
        seen.add((x, y))
        r, g, b, a = px[x, y]
        if a < 128 or min(r, g, b) >= 225:
            px[x, y] = (0, 0, 0, 0)
            stack += [(x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)]
    return im


def theme(im):
    """主题色：不透明像素里饱和度高的颜色按色相分桶，取最多的那一桶的平均色。"""
    buckets = Counter()
    acc = {}
    for r, g, b, a in im.getdata():
        if a < 128:
            continue
        h, s, v = colorsys.rgb_to_hsv(r / 255, g / 255, b / 255)
        if s < 0.35 or v < 0.25:
            continue
        k = int(h * 24)
        buckets[k] += 1
        acc.setdefault(k, [0, 0, 0])
        acc[k][0] += r; acc[k][1] += g; acc[k][2] += b
    total = sum(1 for p in im.getdata() if p[3] >= 128) or 1
    if not buckets or sum(buckets.values()) < total * 0.12:
        return None  # 黑白单色 logo
    k, n = buckets.most_common(1)[0]
    return tuple(round(c / n) for c in acc[k])


# 单色 logo 但官方有品牌色的：按品牌色上色
BRAND_TINT = {"vscode": (0x00, 0x7A, 0xCC)}  # VS Code 官方蓝 #007ACC


def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


rows, data, done = [], [], set()
for icon in icons:
    big = load(icon, 64)
    if big is None:
        continue
    done.add(icon)
    if theme(big) is not None:
        big = strip_white_bg(big)
    col = theme(big) or BRAND_TINT.get(icon)
    tint = BRAND_TINT.get(icon) if theme(big) is None else None
    if col is None and icon in RASTER:
        # 位图单色 logo 常是「深色底板 + 浅色字形」（如 ZCode 的黑底白 Z）：只保留浅色字形，底板透明
        px0 = big.load()
        for yy in range(big.size[1]):
            for xx in range(big.size[0]):
                r, g, b, a = px0[xx, yy]
                if a >= 128 and (r + g + b) / 3 < 128:
                    px0[xx, yy] = (0, 0, 0, 0)
    small = big.resize((SIZE, SIZE), Image.LANCZOS)
    px, mask = [], []
    for r, g, b, a in small.getdata():
        on = a >= 128
        mask.append(1 if on else 0)
        if not on:
            px.append(0)
        elif tint:
            px.append(rgb565(*tint))  # 单色 logo 按官方品牌色上色
        elif col is None:
            px.append(0xEF7E)  # 单色 logo：深底上的浅色
        else:
            # 彩色 logo 里接近黑色的部分（描边等）提亮，否则在深底上看不见
            if max(r, g, b) < 60:
                r = g = b = 200
            px.append(rgb565(r, g, b))
    bits = bytearray()
    for i in range(0, len(mask), 8):
        v = 0
        for k in range(8):
            v = (v << 1) | mask[i + k]
        bits.append(v)
    ident = re.sub(r"\W", "_", icon)
    data.append(f"const uint16_t ic_{ident}_px[{SIZE * SIZE}] PROGMEM = {{{','.join(map(str, px))}}};")
    data.append(f"const uint8_t ic_{ident}_mask[{len(bits)}] PROGMEM = {{{','.join(map(str, bits))}}};")
    color = rgb565(*col) if col else 0xEF7E
    rows.append(f'  {{"{icon}", {color}, {"true" if col is None else "false"}, ic_{ident}_px, ic_{ident}_mask}},')
    print(f"{icon:28s} {'mono' if col is None else '#%02x%02x%02x' % col}")

alias = [f'  {{"{n}", "{i}"}},' for n, i in sorted(names.items()) if i in done]  # 没有资源的不进别名表，屏幕上退回字母图标
out = ["// 由 tools/gen_icons.py 生成：magpie 的 Agent / 订阅图标（16px 像素）与 logo 主题色", "#pragma once",
       "#include <Arduino.h>", "", f"#define BRAND_ICON_SIZE {SIZE}",
       "struct BrandIcon { const char *icon; uint16_t color; bool mono; const uint16_t *px; const uint8_t *mask; };",
       "struct BrandAlias { const char *name; const char *icon; };  // Agent 显示名 → 图标名", ""]
out += data
out += ["", f"const BrandIcon BRAND_ICONS[{len(rows)}] = {{"] + rows + ["};",
        f"const BrandAlias BRAND_ALIAS[{len(alias)}] = {{"] + alias + ["};", ""]
open(OUT, "w").write("\n".join(out))
print(len(rows), "icons,", len(alias), "agent names")
