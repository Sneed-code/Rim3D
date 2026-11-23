"""Paints the title screen's sky through the starship window (docs/design/new-game.md "Title"): the planet below, its
clouds and the galaxy behind, as equirectangular maps the game wraps round spheres (ATitleStage, TitleStage.cpp).

    python tools/title_art.py            (needs numpy; PIL is not needed)
    blender -b --factory-startup -P tools/title_art.py --      (how the art step runs it: Blender's Python has numpy)

Writes into game/Saved/TitleArt/ (not in Git):
    planet_color.png   4096 x 2048, the surface: oceans, coasts, continents, deserts, mountains, ice caps (sRGB albedo)
    planet_clouds.png  4096 x 2048, cloud cover 0 to 255 (grey), laid over the surface by the game
    galaxy.png         4096 x 2048, the starfield and a Milky Way band with dust lanes and nebulae (sRGB of linear light;
                       the game multiplies it by its glow)
Row 0 is the north pole, column 0 longitude 0 (the game's sphere UVs). Seeded, so every PC paints the same pictures.
"""
import os
import struct
import sys
import zlib

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "game", "Saved", "TitleArt")
WIDTH = 4096
HEIGHT = 2048
CHUNK = 128  # rows painted at once (memory)


def write_png(path, pixels):
    """pixels: uint8 array H x W x 3 (RGB). Plain zlib PNG writer, so PIL is not needed."""
    h, w, _ = pixels.shape
    raw = np.empty((h, 1 + w * 3), dtype=np.uint8)
    raw[:, 0] = 0
    raw[:, 1:] = pixels.reshape(h, w * 3)

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw.tobytes(), 6)) + chunk(b"IEND", b"")
    tmp = path + ".part"
    with open(tmp, "wb") as f:
        f.write(png)
    os.replace(tmp, path)
    print("wrote", path, len(png) // 1024, "KB")


class Noise3:
    """Value noise on a hashed 3D lattice, fractal sums of it, vectorised over numpy arrays."""

    def __init__(self, seed):
        rng = np.random.default_rng(seed)
        self.perm = np.concatenate([rng.permutation(256)] * 2).astype(np.int32)
        self.values = rng.random(256).astype(np.float32)

    def _hash(self, x, y, z):
        p = self.perm
        return p[p[p[x & 255] + (y & 255)] + (z & 255)]

    def value(self, x, y, z):
        xi = np.floor(x).astype(np.int32)
        yi = np.floor(y).astype(np.int32)
        zi = np.floor(z).astype(np.int32)
        xf = (x - xi).astype(np.float32)
        yf = (y - yi).astype(np.float32)
        zf = (z - zi).astype(np.float32)
        u = xf * xf * (3 - 2 * xf)
        v = yf * yf * (3 - 2 * yf)
        w = zf * zf * (3 - 2 * zf)
        val = self.values
        c000 = val[self._hash(xi, yi, zi)]
        c100 = val[self._hash(xi + 1, yi, zi)]
        c010 = val[self._hash(xi, yi + 1, zi)]
        c110 = val[self._hash(xi + 1, yi + 1, zi)]
        c001 = val[self._hash(xi, yi, zi + 1)]
        c101 = val[self._hash(xi + 1, yi, zi + 1)]
        c011 = val[self._hash(xi, yi + 1, zi + 1)]
        c111 = val[self._hash(xi + 1, yi + 1, zi + 1)]
        x00 = c000 + (c100 - c000) * u
        x10 = c010 + (c110 - c010) * u
        x01 = c001 + (c101 - c001) * u
        x11 = c011 + (c111 - c011) * u
        y0 = x00 + (x10 - x00) * v
        y1 = x01 + (x11 - x01) * v
        return (y0 + (y1 - y0) * w) * 2.0 - 1.0  # -1 to 1

    def fbm(self, p, octaves, scale, gain=0.5, lacunarity=2.03):
        x, y, z = p[0] * scale, p[1] * scale, p[2] * scale
        total = np.zeros_like(x, dtype=np.float32)
        amp = 1.0
        norm = 0.0
        for o in range(octaves):
            total += amp * self.value(x + o * 17.1, y - o * 9.3, z + o * 5.7)
            norm += amp
            amp *= gain
            x, y, z = x * lacunarity, y * lacunarity, z * lacunarity
        return total / norm


def directions(row0, rows):
    """Unit vectors (3 x rows x WIDTH) for the pixel centres of rows row0..row0+rows; also latitude in radians."""
    v = (np.arange(row0, row0 + rows, dtype=np.float32) + 0.5) / HEIGHT
    u = (np.arange(WIDTH, dtype=np.float32) + 0.5) / WIDTH
    theta = (v * np.pi)[:, None]  # 0 at the north pole
    phi = (u * 2.0 * np.pi)[None, :]
    s = np.sin(theta)
    d = np.stack([s * np.cos(phi), s * np.sin(phi), np.cos(theta) * np.ones_like(phi)])
    lat = (np.pi * 0.5 - theta) * np.ones_like(phi)
    return d.astype(np.float32), lat.astype(np.float32)


def smooth(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def mix(a, b, t):
    return a + (b - a) * t[..., None]


def to_srgb8(linear):
    c = np.clip(linear, 0.0, 1.0)
    s = np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1.0 / 2.4) - 0.055)
    return (s * 255.0 + 0.5).astype(np.uint8)


def paint_planet():
    land_noise = Noise3(6650)
    detail = Noise3(711)
    moist_noise = Noise3(4242)
    cloud_noise = Noise3(9001)
    warp_noise = Noise3(313)
    color = np.zeros((HEIGHT, WIDTH, 3), dtype=np.uint8)
    clouds = np.zeros((HEIGHT, WIDTH, 3), dtype=np.uint8)
    deep = np.array([0.010, 0.035, 0.105], np.float32)
    ocean = np.array([0.018, 0.070, 0.180], np.float32)
    shallow = np.array([0.040, 0.200, 0.300], np.float32)
    jungle = np.array([0.050, 0.140, 0.040], np.float32)
    grass = np.array([0.150, 0.230, 0.070], np.float32)
    desert = np.array([0.560, 0.420, 0.240], np.float32)
    rock = np.array([0.300, 0.250, 0.210], np.float32)
    snow = np.array([0.850, 0.880, 0.920], np.float32)
    for row0 in range(0, HEIGHT, CHUNK):
        d, lat = directions(row0, CHUNK)
        warp = np.stack([warp_noise.fbm(d, 3, 1.5), warp_noise.fbm(d + 3.3, 3, 1.5), warp_noise.fbm(d - 7.1, 3, 1.5)]) * 0.35
        h = land_noise.fbm(d + warp, 7, 1.3) + 0.35 * detail.fbm(d, 5, 5.0)
        sea = 0.02
        land = h > sea
        alt = np.clip((h - sea) / 0.45, 0.0, 1.0)
        depth = np.clip((sea - h) / 0.35, 0.0, 1.0)
        water = mix(mix(shallow, ocean, smooth(0.0, 0.25, depth)), deep, smooth(0.3, 1.0, depth))
        moist = moist_noise.fbm(d, 5, 2.2) * 0.7 + 0.45 - 0.55 * smooth(0.25, 0.0, np.abs(np.abs(lat) - 0.42))  # dry belts
        moist += 0.25 * smooth(0.25, 0.0, np.abs(lat))  # wet equator
        green = mix(grass, jungle, smooth(0.35, 0.8, moist))
        ground = mix(desert, green, smooth(0.05, 0.45, moist))
        ground = mix(ground, rock, smooth(0.45, 0.85, alt))
        ground = mix(ground, snow, smooth(0.8, 0.98, alt))
        tone = 1.0 + 0.18 * detail.fbm(d, 4, 18.0)
        ground = ground * tone[..., None]
        surface = np.where(land[..., None], ground, water)
        ice = smooth(1.22, 1.36, np.abs(lat) + 0.08 * detail.fbm(d, 4, 7.0))
        surface = mix(surface, snow, ice)
        color[row0:row0 + CHUNK] = to_srgb8(surface)

        # Clouds: swirled bands, thicker over the equator and the storm belts, clear over the dry belts.
        cw = np.stack([cloud_noise.fbm(d * 2.0, 4, 1.0), cloud_noise.fbm(d * 2.0 + 5.2, 4, 1.0), cloud_noise.fbm(d * 2.0 - 2.7, 4, 1.0)])
        stretched = d + 0.45 * cw
        stretched[2] *= 1.8  # zonal streaks
        c = cloud_noise.fbm(stretched, 7, 2.6) + 0.18 * np.cos(np.abs(lat) * 6.0) - 0.05
        cover = smooth(0.0, 0.45, c)
        cover = np.clip(cover * (0.85 + 0.3 * smooth(0.2, 0.0, np.abs(lat))), 0.0, 1.0)
        g = (cover * 255.0 + 0.5).astype(np.uint8)
        clouds[row0:row0 + CHUNK] = g[..., None]
        print("planet rows", row0 + CHUNK, "of", HEIGHT, flush=True)
    write_png(os.path.join(OUT, "planet_color.png"), color)
    write_png(os.path.join(OUT, "planet_clouds.png"), clouds)


def paint_galaxy():
    """Linear light: the band ~0.05-0.15, its core ~0.4, background ~0.002, stars up to 1."""
    band_noise = Noise3(1977)
    dust_noise = Noise3(2049)
    neb_noise = Noise3(88)
    rng = np.random.default_rng(6650)
    normal = np.array([0.18, -0.62, 0.76], np.float32)
    normal /= np.linalg.norm(normal)
    core = np.array([0.80, 0.35, 0.19], np.float32)
    core -= normal * np.dot(core, normal)  # the bulge lies in the band
    core /= np.linalg.norm(core)
    image = np.zeros((HEIGHT, WIDTH, 3), dtype=np.float32)
    for row0 in range(0, HEIGHT, CHUNK):
        d, _ = directions(row0, CHUNK)
        b = d[0] * normal[0] + d[1] * normal[1] + d[2] * normal[2]  # distance from the band's plane
        to_core = np.arccos(np.clip(d[0] * core[0] + d[1] * core[1] + d[2] * core[2], -1.0, 1.0))
        clump = np.clip(band_noise.fbm(d, 6, 3.5) * 1.6 + 0.45, 0.0, 1.4)
        grain = 0.6 + 0.4 * np.clip(band_noise.fbm(d + 4.0, 3, 48.0) * 2.0 + 0.5, 0.0, 1.0)  # unresolved star clouds
        texture = clump * grain
        band = np.exp(-(b / 0.13) ** 2) * texture * 0.10 + np.exp(-(b / 0.045) ** 2) * texture * 0.08 + np.exp(-(b / 0.4) ** 2) * 0.012
        bulge = np.exp(-(to_core / 0.38) ** 2) * np.exp(-(b / 0.2) ** 2) * 0.30 * (0.75 + 0.25 * grain)
        light = band + bulge
        lane_warp = np.stack([dust_noise.fbm(d, 3, 2.0), dust_noise.fbm(d + 2.0, 3, 2.0), dust_noise.fbm(d - 2.0, 3, 2.0)]) * 0.25
        filaments = dust_noise.fbm(d + lane_warp, 6, 7.0)
        dust = smooth(-0.15, 0.3, filaments) * np.exp(-((b + 0.012 * band_noise.fbm(d, 3, 6.0)) / 0.06) ** 2)
        light *= 1.0 - 0.8 * dust
        warm = np.clip(bulge / 0.32, 0.0, 1.0)
        tint = mix(np.array([0.75, 0.85, 1.0], np.float32) * np.ones_like(b)[..., None], np.array([1.0, 0.82, 0.6], np.float32), warm)
        rgb = light[..., None] * tint
        # Nebulae: soft magenta and teal clouds near the band.
        n1 = smooth(0.15, 0.6, neb_noise.fbm(d, 5, 3.0)) * np.exp(-(b / 0.35) ** 2)
        n2 = smooth(0.2, 0.65, neb_noise.fbm(d + 11.0, 5, 2.5)) * np.exp(-(b / 0.5) ** 2)
        rgb += n1[..., None] * np.array([0.050, 0.010, 0.040], np.float32)
        rgb += n2[..., None] * np.array([0.005, 0.030, 0.040], np.float32)
        rgb += np.array([0.0015, 0.0018, 0.0030], np.float32)
        image[row0:row0 + CHUNK] = rgb
        print("galaxy rows", row0 + CHUNK, "of", HEIGHT, flush=True)

    # Stars: uniform on the sphere plus more in the band; most faint, a few bright with a small glow.
    count = 160000
    v = rng.normal(size=(count, 3)).astype(np.float32)
    v /= np.linalg.norm(v, axis=1)[:, None]
    in_band = rng.normal(size=(count // 2, 3)).astype(np.float32)
    in_band -= np.outer(in_band @ normal, normal) * (1.0 - rng.random(count // 2)[:, None] * 0.25).astype(np.float32)
    in_band /= np.linalg.norm(in_band, axis=1)[:, None]
    stars = np.concatenate([v, in_band])
    bright = (rng.pareto(1.8, size=len(stars)) * 0.06 + 0.012).astype(np.float32)
    bright = np.clip(bright, 0.0, 1.6)
    temp = rng.random(len(stars)).astype(np.float32)
    colours = np.stack([0.8 + 0.2 * temp, 0.85 + 0.1 * np.ones_like(temp), 1.0 - 0.35 * temp], axis=1)
    theta = np.arccos(np.clip(stars[:, 2], -1.0, 1.0))
    phi = np.mod(np.arctan2(stars[:, 1], stars[:, 0]), 2.0 * np.pi)
    px = np.clip((phi / (2.0 * np.pi) * WIDTH).astype(np.int32), 0, WIDTH - 1)
    py = np.clip((theta / np.pi * HEIGHT).astype(np.int32), 0, HEIGHT - 1)
    np.add.at(image, (py, px), colours * bright[:, None])
    big = np.nonzero(bright > 0.35)[0]
    for i in big:
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                if dx == 0 and dy == 0:
                    continue
                fall = 0.22 if dx == 0 or dy == 0 else 0.08
                image[np.clip(py[i] + dy, 0, HEIGHT - 1), (px[i] + dx) % WIDTH] += colours[i] * bright[i] * fall
    write_png(os.path.join(OUT, "galaxy.png"), to_srgb8(image))


def main():
    os.makedirs(OUT, exist_ok=True)
    only = [a for a in sys.argv[1:] if a in ("planet", "galaxy")]
    if not only or "planet" in only:
        paint_planet()
    if not only or "galaxy" in only:
        paint_galaxy()


if __name__ == "__main__":
    main()
