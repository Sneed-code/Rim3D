# The creator's markings (docs/design/makehuman.md, "Markings"): an atlas of 16 designs, 4 by 4 cells of 512 px (white
# where the mark is, black elsewhere): tattoos, scars, a brand and body paint, drawn here so they are ours to ship.
# The skin material stamps up to four of them on a body by texture coordinates (areas.json from mh_export.py).
#   python tools/makehuman/markings.py [out.png]
import math
import os
import random
import sys

from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "game", "Saved", "MakeHuman", "Markings", "markings.png")
CELL, SS = 512, 2       # cell size, supersampling
N = CELL * SS
NAMES = ["Tribal band", "Tribal flame", "Mandala", "Star", "Script", "Spiral", "Runes", "Dot ring",
         "Scar: slash", "Scar: claws", "Scar: burn", "Scar: stitched", "Brand", "Paint: stripes", "Paint: dots", "Paint: hand"]


def canvas():
    img = Image.new("L", (N, N), 0)
    return img, ImageDraw.Draw(img)


def tribal_band(d):
    # A band of interlocking points round the arm (the cell's width is round the limb).
    d.rectangle((0, N * 0.42, N, N * 0.46), fill=255)
    d.rectangle((0, N * 0.58, N, N * 0.62), fill=255)
    for i in range(6):
        x = i * N / 6
        d.polygon([(x, N * 0.46), (x + N / 12, N * 0.56), (x + N / 6, N * 0.46)], fill=255)
        d.polygon([(x + N / 12, N * 0.58), (x + N / 6, N * 0.49), (x + N / 4, N * 0.58)], fill=255)


def tribal_flame(d):
    rnd = random.Random(3)
    for k in range(5):
        cx = N * (0.25 + 0.125 * k)
        pts = []
        for t in range(40):
            a = t / 39.0
            y = N * (0.85 - 0.7 * a)
            w = N * 0.05 * (1 - a) ** 0.7 * (1.0 + 0.3 * math.sin(a * 9 + k))
            pts.append((cx + w + N * 0.06 * math.sin(a * 4 + k), y))
        for t in range(39, -1, -1):
            a = t / 39.0
            y = N * (0.85 - 0.7 * a)
            w = N * 0.05 * (1 - a) ** 0.7 * (1.0 + 0.3 * math.sin(a * 9 + k))
            pts.append((cx - w + N * 0.06 * math.sin(a * 4 + k), y))
        d.polygon(pts, fill=255)


def mandala(d):
    c = N / 2
    for r in (0.44, 0.40, 0.25, 0.21, 0.08):
        w = N * 0.012
        d.ellipse((c - N * r, c - N * r, c + N * r, c + N * r), outline=255, width=int(w))
    for i in range(16):
        a = i * math.pi / 8
        pts = [(c + math.cos(a) * N * 0.21, c + math.sin(a) * N * 0.21),
               (c + math.cos(a + 0.18) * N * 0.32, c + math.sin(a + 0.18) * N * 0.32),
               (c + math.cos(a) * N * 0.40, c + math.sin(a) * N * 0.40),
               (c + math.cos(a - 0.18) * N * 0.32, c + math.sin(a - 0.18) * N * 0.32)]
        d.polygon(pts, outline=255, width=int(N * 0.008))
    d.ellipse((c - N * 0.04, c - N * 0.04, c + N * 0.04, c + N * 0.04), fill=255)


def star(d):
    c = N / 2
    pts = []
    for i in range(10):
        r = N * (0.42 if i % 2 == 0 else 0.17)
        a = -math.pi / 2 + i * math.pi / 5
        pts.append((c + math.cos(a) * r, c + math.sin(a) * r))
    d.polygon(pts, outline=255, width=int(N * 0.02))
    inner = [(c + (x - c) * 0.6, c + (y - c) * 0.6) for x, y in pts]
    d.polygon(inner, fill=255)


def script(d):
    rnd = random.Random(5)
    for row in range(3):
        y0 = N * (0.35 + row * 0.13)
        x = N * 0.1
        while x < N * 0.88:
            w = N * rnd.uniform(0.02, 0.06)
            h = N * rnd.uniform(0.03, 0.07)
            d.arc((x, y0 - h, x + w, y0 + h * 0.3), rnd.randint(0, 120), rnd.randint(200, 360), fill=255, width=int(N * 0.01))
            x += w * 0.8
        d.line((N * 0.1, y0 + N * 0.02, N * 0.88, y0 + N * 0.02), fill=255, width=int(N * 0.006))


def spiral(d):
    c = N / 2
    pts = []
    for i in range(400):
        a = i * 0.06
        r = N * 0.01 * a
        pts.append((c + math.cos(a) * r, c + math.sin(a) * r))
    d.line(pts, fill=255, width=int(N * 0.025), joint="curve")
    for i in range(6):
        a = i * math.pi / 3
        d.ellipse((c + math.cos(a) * N * 0.43 - N * 0.03, c + math.sin(a) * N * 0.43 - N * 0.03,
                   c + math.cos(a) * N * 0.43 + N * 0.03, c + math.sin(a) * N * 0.43 + N * 0.03), fill=255)


def runes(d):
    rnd = random.Random(9)
    for k in range(5):
        x0 = N * (0.1 + k * 0.17)
        y0, y1 = N * 0.3, N * 0.7
        d.line((x0 + N * 0.05, y0, x0 + N * 0.05, y1), fill=255, width=int(N * 0.018))
        for _ in range(2):
            a = rnd.uniform(0.3, 0.7)
            d.line((x0 + N * 0.05, y0 + (y1 - y0) * a, x0 + N * rnd.choice((0.0, 0.11)), y0 + (y1 - y0) * (a + rnd.uniform(-0.2, 0.2))),
                   fill=255, width=int(N * 0.018))


def dot_ring(d):
    c = N / 2
    for ring, (r, n, s) in enumerate(((0.42, 24, 0.022), (0.30, 16, 0.03), (0.16, 8, 0.04))):
        for i in range(n):
            a = i * 2 * math.pi / n + ring * 0.2
            x, y = c + math.cos(a) * N * r, c + math.sin(a) * N * r
            d.ellipse((x - N * s, y - N * s, x + N * s, y + N * s), fill=255)


def scar_slash(d):
    pts = [(N * (0.12 + 0.76 * t), N * (0.70 - 0.40 * t + 0.03 * math.sin(t * 9))) for t in [i / 30 for i in range(31)]]
    for k, p in enumerate(pts[:-1]):
        t = k / 30
        w = N * (0.012 + 0.03 * math.sin(math.pi * t))
        d.line((p, pts[k + 1]), fill=255, width=int(w))


def scar_claws(d):
    for k in range(3):
        off = (k - 1) * N * 0.13
        pts = [(N * 0.25 + off + N * 0.5 * t, N * (0.2 + 0.6 * t) + 0.02 * N * math.sin(t * 7 + k)) for t in [i / 24 for i in range(25)]]
        for i, p in enumerate(pts[:-1]):
            t = i / 24
            d.line((p, pts[i + 1]), fill=255, width=int(N * (0.006 + 0.022 * math.sin(math.pi * t))))


def scar_burn(img):
    rnd = random.Random(2)
    d = ImageDraw.Draw(img)
    for _ in range(60):
        x, y = rnd.gauss(N / 2, N * 0.12), rnd.gauss(N / 2, N * 0.1)
        r = N * rnd.uniform(0.03, 0.09)
        d.ellipse((x - r, y - r * 0.8, x + r, y + r * 0.8), fill=rnd.randint(150, 255))
    return img.filter(ImageFilter.GaussianBlur(N * 0.01))


def scar_stitched(d):
    d.line((N * 0.15, N * 0.5, N * 0.85, N * 0.5), fill=255, width=int(N * 0.025))
    for i in range(9):
        x = N * (0.2 + i * 0.075)
        d.line((x - N * 0.02, N * 0.42, x + N * 0.02, N * 0.58), fill=200, width=int(N * 0.012))


def brand(d):
    c = N / 2
    d.ellipse((c - N * 0.3, c - N * 0.3, c + N * 0.3, c + N * 0.3), outline=255, width=int(N * 0.05))
    # A colony mark: a triangle over a bar.
    d.polygon([(c, c - N * 0.17), (c + N * 0.15, c + N * 0.08), (c - N * 0.15, c + N * 0.08)], outline=255, width=int(N * 0.045))
    d.line((c - N * 0.16, c + N * 0.17, c + N * 0.16, c + N * 0.17), fill=255, width=int(N * 0.045))


def paint_stripes(d):
    for k in range(3):
        y = N * (0.32 + k * 0.16)
        d.line((N * 0.08, y, N * 0.92, y - N * 0.06), fill=255, width=int(N * 0.07))


def paint_dots(d):
    rnd = random.Random(4)
    for _ in range(40):
        x, y = rnd.uniform(0.15, 0.85) * N, rnd.uniform(0.15, 0.85) * N
        r = N * rnd.uniform(0.015, 0.035)
        d.ellipse((x - r, y - r, x + r, y + r), fill=255)


def paint_hand(d):
    c = N / 2
    d.rounded_rectangle((c - N * 0.15, c - N * 0.02, c + N * 0.15, c + N * 0.30), radius=N * 0.08, fill=255)
    for i, (a, l) in enumerate(((-0.55, 0.20), (-0.2, 0.27), (0.0, 0.29), (0.2, 0.27), (0.45, 0.22))):
        x0, y0 = c + math.sin(a) * N * 0.13 + (N * -0.06 if i == 0 else 0), c + (N * 0.1 if i == 0 else N * 0.02)
        x1, y1 = x0 + math.sin(a) * N * l, y0 - math.cos(a) * N * l
        d.line((x0, y0, x1, y1), fill=255, width=int(N * 0.07))
        d.ellipse((x1 - N * 0.035, y1 - N * 0.035, x1 + N * 0.035, y1 + N * 0.035), fill=255)


def main():
    atlas = Image.new("L", (CELL * 4, CELL * 4), 0)
    makers = [tribal_band, tribal_flame, mandala, star, script, spiral, runes, dot_ring,
              scar_slash, scar_claws, None, scar_stitched, brand, paint_stripes, paint_dots, paint_hand]
    for i, make in enumerate(makers):
        img, d = canvas()
        if make is None:
            img = scar_burn(img)
        else:
            make(d)
        # Margin: nothing at the cell's edge (the stamp would smear across its neighbours when sampled).
        img = img.filter(ImageFilter.GaussianBlur(SS * 0.7)).resize((CELL, CELL), Image.LANCZOS)
        mask = Image.new("L", (CELL, CELL), 0)
        ImageDraw.Draw(mask).rectangle((6, 6, CELL - 7, CELL - 7), fill=255)
        img = Image.composite(img, Image.new("L", (CELL, CELL), 0), mask)
        atlas.paste(img, ((i % 4) * CELL, (i // 4) * CELL))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    atlas.save(OUT)
    print("[markings]", len(makers), "designs to", OUT)


if __name__ == "__main__":
    main()
