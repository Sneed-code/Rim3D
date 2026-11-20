#!/usr/bin/env python3
"""Cloud preview of M_GrimeOverlay's dirt (docs/design/grime.md): a plain mannequin drawn with the old dirt (one noise
patch layer) and the new one (smudges + fine spray, the GrimeDirt HLSL node in BloodArtCommandlet.cpp ported line by
line). Not the game's lighting or skin: only to judge the pattern before a PC round.

    python3 tools/grime_dirt_preview.py out.png [--legs 0.8] [--body 0.3]
"""
import argparse

import numpy as np
from PIL import Image, ImageDraw


def h1(p):
    p = np.modf(p * 0.3183099 + 0.1)[0]
    p = np.where(p < 0, p + 1, p) * 17.0
    x, y, z = p[..., 0], p[..., 1], p[..., 2]
    v = x * y * z * (x + y + z)
    return v - np.floor(v)


def h3(p):
    return np.stack([h1(p), h1(p + 19.19), h1(p + 47.31)], -1)


def vnoise(x):
    i = np.floor(x)
    f = x - i
    f = f * f * (3.0 - 2.0 * f)

    def c(a, b, cc):
        return h1(i + np.array([a, b, cc], dtype=np.float64))

    fx, fy, fz = f[..., 0], f[..., 1], f[..., 2]
    l = lambda a, b, t: a + (b - a) * t
    return l(l(l(c(0, 0, 0), c(1, 0, 0), fx), l(c(0, 1, 0), c(1, 1, 0), fx), fy),
             l(l(c(0, 0, 1), c(1, 0, 1), fx), l(c(0, 1, 1), c(1, 1, 1), fx), fy), fz)


def fbm(x):
    return vnoise(x) * 0.55 + vnoise(x * 2.03 + 7.7) * 0.3 + vnoise(x * 4.11 + 3.1) * 0.15


def sat(x):
    return np.clip(x, 0.0, 1.0)


def smoothstep(a, b, x):
    t = sat((x - a) / np.maximum(b - a, 1e-6))
    return t * t * (3 - 2 * t)


def drops(p, cell, density, rmin, rmax, stretch, fw, seed):
    q = p / cell
    q = q.copy()
    q[..., 2] *= stretch
    i = np.floor(q) + seed
    f = q - np.floor(q)
    keep = (h1(i + 5.31) <= density).astype(np.float64)
    h = h1(i + 2.17)
    r = rmin + (rmax - rmin) * h * h
    c = r[..., None] + h3(i + 11.1) * (1.0 - 2.0 * r[..., None])
    aa = np.maximum(fw / cell, 0.01)
    d = np.linalg.norm(f - c, axis=-1)
    vis = sat((cell / np.maximum(fw, 1e-4) - 2.0) * 0.5)
    return keep * (1.0 - smoothstep(r - aa, r + aa, d)), vis


def dirt_new(p, H, legs_lv, body_lv, scale, fw):
    ragged = (vnoise(p * 0.05 + 3.7) - 0.5) * 0.14
    legs = sat(1.35 - 2.6 * (H + ragged))
    amount = sat(legs_lv * legs + body_lv)
    broad = fbm(p * scale * 0.8)
    mid = vnoise(p * scale * 3.0 + 17.0)
    field = amount * 0.78 - broad * 0.74 - mid * 0.3 + 0.02
    smudge = sat(field * 5.0)
    grain_vis = sat((1.0 / fw - 2.0) * 0.5)
    grain = 0.6 + (vnoise(p * 1.0 + 3.3) - 0.6) * grain_vis
    smudge *= 0.62 + 0.38 * grain
    core = sat((field - 0.2) * 3.0)
    near_edge = sat((field + 0.22) * 3.0)
    spray_legs = sat(1.25 - 1.8 * (H + ragged * 0.5))
    spray = sat(legs_lv * spray_legs * 1.1 + body_lv * 0.55)
    fine_d = spray * (0.25 + 0.75 * near_edge) * 0.6
    big_d = spray * near_edge * near_edge * 0.3
    stretch = 1.0 - 0.3 * sat(legs_lv * spray_legs)
    mist, mv = drops(p, 0.35, fine_d * 0.9, 0.12, 0.32, stretch, fw, 0.0)
    fine, fv = drops(p, 0.8, fine_d, 0.08, 0.3, stretch, fw, 31.0)
    big, bv = drops(p, 2.4, big_d, 0.07, 0.24, stretch, fw, 67.0)
    dr = np.maximum(np.maximum(mist * mv, fine * fv), big * bv)
    tint = sat((1 - mv) * fine_d * 0.9 * 0.18 + (1 - fv) * fine_d * 0.15 + (1 - bv) * big_d * 0.12)
    cover = 1 - (1 - smudge * 0.85) * (1 - dr * 0.95) * (1 - tint)
    dark = sat(np.maximum(dr, core * (0.55 + 0.45 * mid)) + tint * 0.5)
    return sat(cover) * 0.9, dark


def dirt_old(p, H, legs_lv, body_lv, scale):
    legs = sat(1.35 - 2.6 * H)
    amount = sat(legs_lv * legs + body_lv)
    n = fbm(p * scale)  # stands in for UE's Noise node (4 levels)
    return sat((amount * 0.8 - n) * 3.0) * 0.8, np.full(H.shape, 0.5)


# A plain standing mannequin from capsules (cm, z up, feet at 0), seen from the front (camera looks along +y).
CAPS = [
    ((-11, 0, 8), (-10, 0, 88), 7.0), ((11, 0, 8), (10, 0, 88), 7.0),  # legs
    ((-11, -6, 3), (-11, 8, 3), 4.0), ((11, -6, 3), (11, 8, 3), 4.0),   # feet
    ((0, 0, 92), (0, 0, 140), 17.0),                                    # torso
    ((-25, 0, 140), (-30, 0, 88), 5.0), ((25, 0, 140), (30, 0, 88), 5.0),  # arms
    ((0, 0, 150), (0, 0, 156), 6.0), ((0, 0, 167), (0, 0, 170), 10.0),     # neck, head
]


def sdf(p):
    d = np.full(p.shape[:-1], 1e9)
    for a, b, r in CAPS:
        a = np.array(a, float)
        b = np.array(b, float)
        pa = p - a
        ba = b - a
        t = sat((pa @ ba) / (ba @ ba))
        d = np.minimum(d, np.linalg.norm(pa - t[..., None] * ba, axis=-1) - r)
    return d


def render(x0, x1, z0, z1, width, legs_lv, body_lv, which):
    height = int(width * (z1 - z0) / (x1 - x0))
    xs = np.linspace(x0, x1, width)
    zs = np.linspace(z1, z0, height)
    X, Z = np.meshgrid(xs, zs)
    p = np.stack([X, np.full_like(X, -60.0), Z], -1)
    hit = np.zeros(X.shape, bool)
    for _ in range(90):
        d = sdf(p)
        hit |= d < 0.02
        p[..., 1] += np.where(hit, 0.0, np.maximum(d, 0.02))
    e = 0.05
    n = np.stack([sdf(p + [e, 0, 0]) - sdf(p - [e, 0, 0]), sdf(p + [0, e, 0]) - sdf(p - [0, e, 0]),
                  sdf(p + [0, 0, e]) - sdf(p - [0, 0, e])], -1)
    n /= np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-6)
    px = (x1 - x0) / width
    fw = px / np.maximum(np.abs(n[..., 1]), 0.25) * 1.4
    H = p[..., 2] / 180.0
    if which == "new":
        cover, dark = dirt_new(p, H, legs_lv, body_lv, 0.06, fw)
    elif which == "old":
        cover, dark = dirt_old(p, H, legs_lv, body_lv, 0.06)
    else:
        cover, dark = np.zeros(H.shape), np.zeros(H.shape)
    light = np.array([-0.4, -0.75, 0.55])
    light /= np.linalg.norm(light)
    lit = 0.25 + 0.75 * sat(n @ light)
    skin = np.array([0.80, 0.60, 0.48])
    dirt = np.array([0.11, 0.085, 0.06]) * 1.35
    deep = np.array([0.11, 0.085, 0.06]) * 0.55
    dcol = dirt + (deep - dirt) * dark[..., None]
    # Linear-ish dirt colour to display: lift to sRGB.
    dcol = np.power(dcol, 1 / 2.2)
    col = skin + (dcol - skin) * cover[..., None]
    col = col * lit[..., None]
    bg = np.array([0.18, 0.2, 0.22])
    img = np.where(hit[..., None], col, bg)
    return Image.fromarray((sat(img) * 255).astype(np.uint8))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--legs", type=float, default=0.8)
    ap.add_argument("--body", type=float, default=0.3)
    a = ap.parse_args()
    panels = []
    for which in ("old", "new"):
        full = render(-45, 45, -2, 182, 360, a.legs, a.body, which)
        close = render(-22, 2, 10, 60, 360, a.legs, a.body, which)
        panels.append((which, full, close))
    w = 360 * 4 + 50
    hgt = max(p[1].height for p in panels) + 40
    sheet = Image.new("RGB", (w, hgt), (30, 30, 30))
    dr = ImageDraw.Draw(sheet)
    x = 10
    for which, full, close in panels:
        sheet.paste(full, (x, 30))
        dr.text((x, 8), f"{'BEFORE' if which == 'old' else 'AFTER'}: whole body", fill=(230, 230, 230))
        x += 370
        sheet.paste(close, (x, 30))
        dr.text((x, 8), f"{'BEFORE' if which == 'old' else 'AFTER'}: shin close-up", fill=(230, 230, 230))
        x += 370
    sheet.save(a.out)


if __name__ == "__main__":
    main()
