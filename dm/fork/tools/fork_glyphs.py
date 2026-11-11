#!/usr/bin/env python3
# FORK: draws the fork's touch-control icons into cells 12-16 of the overlay's label atlas, dm/gfx/nvn/overlay/ovl_font.h (made by font_atlas.swift:
# 16 cells of 64x64, R8 coverage; cells 0-11 are A B X Y L R ZL ZR + - L3 R3). The atlas is widened to 32 cells here (ovl.frag reads the cell count from its width).
# Shapes only (no font needed), drawn 4x larger and scaled down for smooth edges.
#   12 fast-forward (two triangles)   13 Poke Ball (battle X: the quick ball)   14 back arrow (battle B)   15 info (battle Y)   16 turbo (a lightning bolt)
# Run from the repository root: python3 dm/fork/tools/fork_glyphs.py   (nvn_ovl.h OVL_G_FF ... OVL_G_TURBO name the cells)
import math, re, sys
from PIL import Image, ImageDraw

H = 'dm/gfx/nvn/overlay/ovl_font.h'
CELL, CELLS, S = 64, 32, 4
src = open(H, encoding='utf-8').read()
m = re.search(r'static const unsigned char ovl_font\[\] = \{(.*?)\};', src, re.S)
vals = [int(v) for v in m.group(1).replace('\n', '').split(',') if v.strip()]
W = CELL * CELLS
w0 = len(vals) // CELL; assert w0 * CELL == len(vals) and w0 in (CELL * 16, W), len(vals)
if w0 != W: vals = [v for y in range(CELL) for v in vals[y * w0:(y + 1) * w0] + [0] * (W - w0)]   # 16 cells -> 32, the new ones empty

def cell(draw_fn):
    im = Image.new('L', (CELL * S, CELL * S), 0); d = ImageDraw.Draw(im); draw_fn(d, CELL * S); return im.resize((CELL, CELL), Image.LANCZOS)

def ff(d, n):   # two triangles pointing right
    m = n * 0.20; h = n * 0.30
    for x0 in (n * 0.18, n * 0.50):
        d.polygon([(x0, n / 2 - h), (x0 + n * 0.32, n / 2), (x0, n / 2 + h)], fill=255)
def ball(d, n):   # a Poke Ball in outline: ring, band, centre button
    c = n / 2; r = n * 0.34; w = n * 0.07
    d.ellipse([c - r, c - r, c + r, c + r], outline=255, width=int(w))
    d.rectangle([c - r, c - w / 2, c + r, c + w / 2], fill=255)
    d.ellipse([c - r * 0.36, c - r * 0.36, c + r * 0.36, c + r * 0.36], fill=255)
    d.ellipse([c - r * 0.18, c - r * 0.18, c + r * 0.18, c + r * 0.18], fill=0)
def back(d, n):   # a U-turn arrow pointing left (back / cancel): an even stroke, the arrowhead on the upper arm
    w = int(n * 0.085); r = n * 0.19; cx, cy = n * 0.58, n * 0.54
    d.arc([cx - r, cy - r, cx + r, cy + r], start=-90, end=90, fill=255, width=w)   # the bend, on the right
    d.rectangle([n * 0.30, cy + r - w, cx, cy + r], fill=255)                        # the lower arm, out to the left
    d.rectangle([n * 0.36, cy - r, cx, cy - r + w], fill=255)                        # the upper arm, to the arrowhead
    ty = cy - r + w / 2; d.polygon([(n * 0.16, ty), (n * 0.38, ty - n * 0.15), (n * 0.38, ty + n * 0.15)], fill=255)
def info(d, n):   # an "i" in a ring
    c = n / 2; r = n * 0.34; w = n * 0.065
    d.ellipse([c - r, c - r, c + r, c + r], outline=255, width=int(w))
    d.ellipse([c - n * 0.05, c - r * 0.62 - n * 0.05, c + n * 0.05, c - r * 0.62 + n * 0.05], fill=255)
    d.rectangle([c - n * 0.045, c - r * 0.28, c + n * 0.045, c + r * 0.62], fill=255)
def turbo(d, n):   # a lightning bolt
    p = [(0.58, 0.14), (0.30, 0.54), (0.48, 0.54), (0.40, 0.86), (0.70, 0.42), (0.52, 0.42), (0.62, 0.14)]
    d.polygon([(x * n, y * n) for x, y in p], fill=255)

for idx, fn in ((12, ff), (13, ball), (14, back), (15, info), (16, turbo)):
    px = cell(fn).load()
    for y in range(CELL):
        for x in range(CELL): vals[y * W + idx * CELL + x] = px[x, y]

body = ',\n'.join(','.join(str(v) for v in vals[r * 100:(r + 1) * 100]) for r in range((len(vals) + 99) // 100))
out = src[:m.start()] + 'static const unsigned char ovl_font[] = {\n' + body + '\n};' + src[m.end():]
out = re.sub(r'ovl_font_w = \d+', 'ovl_font_w = %d' % W, out)
if '// FORK:' not in out.splitlines()[1]:
    out = out.replace('\n', '\n// FORK: widened to 32 cells; cells 12-16 drawn by dm/fork/tools/fork_glyphs.py (fast-forward, Poke Ball, back, info, turbo)\n', 1)
open(H, 'w', encoding='utf-8').write(out)
print('cells 12-16 written to', H)
