"""Shades the raw blast flipbooks from tools/blast_flipbook_preview.cpp the way the game's materials do
(docs/design/blast-draw.md), over a daylight sky and a night sky, so the cloud can look at them.

    python3 tools/blast_preview.py <dir with fire.rgba smoke.rgba scorch.rgba> <out.png>
"""
import sys

import numpy as np
from PIL import Image


def load(path):
    raw = np.fromfile(path, dtype=np.uint8)
    size = int(round((raw.size // 4) ** 0.5))
    return raw.reshape(size, size, 4).astype(np.float32) / 255.0


def blackbody(heat):
    """The materials' heat ramp (BlastArtCommandlet BlastFireColor): dark red, orange, yellow, white."""
    h = np.clip(heat, 0.0, 1.0)[..., None]
    stops = [(0.0, (0.0, 0.0, 0.0)), (0.15, (0.35, 0.03, 0.0)), (0.4, (1.0, 0.25, 0.02)),
             (0.7, (1.0, 0.62, 0.15)), (1.0, (1.0, 0.95, 0.75))]
    out = np.zeros(heat.shape + (3,), np.float32)
    for (a, ca), (b, cb) in zip(stops, stops[1:]):
        t = np.clip((h - a) / (b - a), 0, 1)
        mask = (h >= a) & (h <= b)
        col = np.array(ca) * (1 - t) + np.array(cb) * t
        out = np.where(mask, col, out)
    return out


def shade(sheet, smoke_rgb, sun, ambient, fire_gain, bg):
    heat, light, depth, alpha = sheet[..., 0], sheet[..., 1], sheet[..., 2], sheet[..., 3]
    lit = (ambient + sun * light)[..., None] * np.array(smoke_rgb) * (0.85 + 0.3 * depth[..., None])
    glow = blackbody(heat) * (heat ** 1.5)[..., None] * fire_gain
    col = lit * (1 - np.clip(heat * 2, 0, 1)[..., None]) + glow
    out = bg * (1 - alpha[..., None]) + col * alpha[..., None]
    return out


def tonemap(x):
    x = np.maximum(x, 0)
    return np.clip((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0, 1) ** (1 / 2.2)


def main():
    folder, out_path = sys.argv[1], sys.argv[2]
    fire = load(folder + "/fire.rgba")
    smoke = load(folder + "/smoke.rgba")
    scorch = load(folder + "/scorch.rgba")
    n = fire.shape[0]
    day = np.ones((n, n, 3), np.float32) * np.array([0.45, 0.6, 0.8])
    night = np.ones((n, n, 3), np.float32) * np.array([0.01, 0.012, 0.02])
    panels = [
        shade(fire, (0.08, 0.075, 0.07), 1.1, 0.25, 6.0, day),
        shade(fire, (0.08, 0.075, 0.07), 0.02, 0.01, 6.0, night),
        shade(smoke, (0.35, 0.33, 0.3), 1.2, 0.3, 0.0, day),
        shade(smoke, (0.55, 0.45, 0.33), 1.2, 0.3, 0.0, day),
    ]
    ground = np.ones((n // 2, n // 2, 3), np.float32) * np.array([0.18, 0.14, 0.09])
    soot = scorch[..., 0:1]
    rim = scorch[..., 1:2]
    a = scorch[..., 3:4]
    scol = ground * (1 - soot * 0.9) + rim * np.array([0.12, 0.1, 0.08])
    sc = ground * (1 - a) + scol * a
    big = np.concatenate([tonemap(p) for p in panels], axis=1)
    scorch_img = np.zeros((n, n, 3), np.float32)
    scorch_img[: n // 2, : n // 2] = tonemap(sc)
    big = np.concatenate([big, scorch_img], axis=1)
    Image.fromarray((big * 255).astype(np.uint8)).save(out_path)
    # One frame strip of the fire at a bigger size, for a closer look.
    cell = n // 8
    frames = [0, 3, 8, 14, 22, 32, 44, 60]
    strip = []
    for f in frames:
        y, x = (f // 8) * cell, (f % 8) * cell
        strip.append(tonemap(shade(fire[y:y + cell, x:x + cell], (0.08, 0.075, 0.07), 1.1, 0.25, 6.0, day[:cell, :cell])))
    strip2 = []
    for f in frames:
        y, x = (f // 8) * cell, (f % 8) * cell
        strip2.append(tonemap(shade(smoke[y:y + cell, x:x + cell], (0.3, 0.29, 0.27), 1.2, 0.3, 0.0, day[:cell, :cell])))
    s = np.concatenate([np.concatenate(strip, axis=1), np.concatenate(strip2, axis=1)], axis=0)
    Image.fromarray((s * 255).astype(np.uint8)).resize((s.shape[1] * 2, s.shape[0] * 2), Image.LANCZOS).save(out_path.replace(".png", "_strip.png"))


if __name__ == "__main__":
    main()
