"""Paints the title screen's sky through the starship window (docs/design/new-game.md "Title"): the planet below, its
clouds, its city lights and the stars behind (ATitleStage, TitleStage.cpp).

    python tools/title_art.py [planet] [galaxy]      (needs numpy; PIL is not needed)
    blender -b --factory-startup -P tools/title_art.py --      (how the art step runs it: Blender's Python has numpy)

Writes into game/Saved/TitleArt/ (not in Git). The planet maps are equirectangular (row 0 the north pole, column 0
longitude 0: the game's sphere UVs) and come as .r3tx files: a tiny header and every mip level, ready to copy into a
texture (BC1 = DXT1 for the colour maps, so an 8K surface costs 21 MB of video memory):
    planet_color.r3tx    8192 x 4096 BC1, the surface with the clouds laid over it (sRGB albedo)
    planet_normal.r3tx   4096 x 2048 BGRA, the relief (tangent space along the sphere: x east, y north)
    planet_rough.r3tx    2048 x 1024 BGRA, roughness in red: smooth sea (the sun's glint), rough land, clouds
    planet_lights.r3tx   4096 x 2048 BC1, the night side's city lights (sRGB of linear light, times the game's glow)
The sky (sRGB of linear light; the game multiplies it by its glow):
    sky_view.png         4096 x 2240, the stars seen through the window, crisp: a flat (gnomonic) picture of the part
                         of the sky the title camera sees, about one texel per screen pixel at 4K, stars drawn as
                         small round dots of many brightnesses, the Milky Way band with its star clouds and dust lanes
    galaxy.png           2048 x 1024, the whole sky round the stage (mostly hidden behind sky_view)
Seeded, so every PC paints the same pictures.

.r3tx layout (little endian): b"R3TX", u32 version 1, u32 format (1 BC1, 2 BGRA8), u32 width, u32 height, u32 mips,
then per mip: u32 width, u32 height, u32 bytes, the bytes (BC1 blocks row by row; BGRA rows top first).
"""
import math
import os
import struct
import sys
import time
import zlib

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "game", "Saved", "TitleArt")

PLANET_W, PLANET_H = 8192, 4096   # the surface colour
LOW_W, LOW_H = 2048, 1024         # broad fields painted at a quarter and upsampled
HALF_W, HALF_H = 4096, 2048       # relief and city lights
ROUGH_W, ROUGH_H = 2048, 1024
GALAXY_W, GALAXY_H = 2048, 1024
VIEW_W, VIEW_H = 4096, 2240
# The window's piece of sky (TitleStage.cpp TsViewHalfX/Y): tangent half extents of the flat picture, square texels.
VIEW_HALF_X = 0.46
VIEW_HALF_Y = VIEW_HALF_X * VIEW_H / VIEW_W
VIEW_TURN_DEGREES = 14.0  # the galaxy sphere's yaw is -14 (TsGalaxyTurn): sky directions turn back by this
CHUNK = 64  # rows painted at once (memory)


# ---- Files ---------------------------------------------------------------------------------------------------------

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
    print("wrote", path, len(png) // 1024, "KB", flush=True)


FORMAT_BC1 = 1
FORMAT_BGRA8 = 2


def write_r3tx(path, fmt, levels):
    """levels: list of (width, height, bytes), the full picture first."""
    w, h, _ = levels[0]
    tmp = path + ".part"
    with open(tmp, "wb") as f:
        f.write(b"R3TX" + struct.pack("<5I", 1, fmt, w, h, len(levels)))
        for lw, lh, data in levels:
            f.write(struct.pack("<3I", lw, lh, len(data)))
            f.write(data)
    os.replace(tmp, path)
    print("wrote", path, os.path.getsize(path) // 1024, "KB,", len(levels), "mips", flush=True)


def srgb_to_linear(c8):
    c = c8.astype(np.float32) / 255.0
    return np.where(c <= 0.04045, c / 12.92, np.power((c + 0.055) / 1.055, 2.4)).astype(np.float32)


def to_srgb8(linear):
    c = np.clip(linear, 0.0, 1.0)
    s = np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1.0 / 2.4) - 0.055)
    return (s * 255.0 + 0.5).astype(np.uint8)


def halve(img):
    """2 x 2 box filter of a float H x W x C picture."""
    return (img[0::2, 0::2] + img[1::2, 0::2] + img[0::2, 1::2] + img[1::2, 1::2]) * 0.25


def bc1_encode(rgb):
    """BC1 (DXT1) blocks of an H x W x 3 uint8 picture (H and W multiples of 4): per block the colour line through its
    pixels (principal axis), its ends rounded to 5:6:5, every pixel the nearest of the four colours on the line."""
    h, w, _ = rgb.shape
    out = bytearray()
    strip = 64 * 4  # pixel rows per pass
    for y0 in range(0, h, strip):
        part = rgb[y0:y0 + strip].astype(np.float32)
        bh = part.shape[0] // 4
        bw = w // 4
        blocks = part.reshape(bh, 4, bw, 4, 3).transpose(0, 2, 1, 3, 4).reshape(bh * bw, 16, 3)
        mean = blocks.mean(axis=1)
        centred = blocks - mean[:, None, :]
        cov = np.einsum("bpi,bpj->bij", centred, centred)
        axis = np.ones((len(blocks), 3), np.float32) / math.sqrt(3.0)
        for _ in range(6):
            axis = np.einsum("bij,bj->bi", cov, axis)
            norm = np.linalg.norm(axis, axis=1)
            axis = np.where(norm[:, None] > 1e-6, axis / np.maximum(norm, 1e-6)[:, None], 1.0 / math.sqrt(3.0))
        t = np.einsum("bpi,bi->bp", centred, axis)
        tmin = t.min(axis=1)
        tmax = t.max(axis=1)
        inset = (tmax - tmin) / 32.0
        e0 = np.clip(mean + axis * (tmax - inset)[:, None], 0.0, 255.0)
        e1 = np.clip(mean + axis * (tmin + inset)[:, None], 0.0, 255.0)

        def pack565(e):
            r = np.clip(np.round(e[:, 0] * 31.0 / 255.0), 0, 31).astype(np.uint32)
            g = np.clip(np.round(e[:, 1] * 63.0 / 255.0), 0, 63).astype(np.uint32)
            b = np.clip(np.round(e[:, 2] * 31.0 / 255.0), 0, 31).astype(np.uint32)
            return (r << 11) | (g << 5) | b

        def unpack565(c):
            r = (c >> 11) & 31
            g = (c >> 5) & 63
            b = c & 31
            return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], axis=1).astype(np.float32)

        c0 = pack565(e0)
        c1 = pack565(e1)
        swap = c0 < c1
        c0, c1 = np.where(swap, c1, c0), np.where(swap, c0, c1)
        p0 = unpack565(c0)
        p1 = unpack565(c1)
        palette = np.stack([p0, p1, (2.0 * p0 + p1) / 3.0, (p0 + 2.0 * p1) / 3.0], axis=1)  # B x 4 x 3
        dist = ((blocks[:, :, None, :] - palette[:, None, :, :]) ** 2).sum(axis=3)  # B x 16 x 4
        idx = np.argmin(dist, axis=2).astype(np.uint32)
        idx[c0 == c1] = 0
        bits = np.zeros(len(blocks), np.uint32)
        for p in range(16):
            bits |= idx[:, p] << (2 * p)
        packed = np.empty(len(blocks), dtype=[("c0", "<u2"), ("c1", "<u2"), ("bits", "<u4")])
        packed["c0"] = c0
        packed["c1"] = c1
        packed["bits"] = bits
        out += packed.tobytes()
    return bytes(out)


def bc1_decode(data, w, h):
    """For checking the encoder: the H x W x 3 uint8 picture back."""
    packed = np.frombuffer(data, dtype=[("c0", "<u2"), ("c1", "<u2"), ("bits", "<u4")])
    c0 = packed["c0"].astype(np.uint32)
    c1 = packed["c1"].astype(np.uint32)

    def unpack565(c):
        r = (c >> 11) & 31
        g = (c >> 5) & 63
        b = c & 31
        return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], axis=1).astype(np.float32)

    p0 = unpack565(c0)
    p1 = unpack565(c1)
    four = (c0 > c1)[:, None]
    p2 = np.where(four, (2 * p0 + p1) / 3.0, (p0 + p1) / 2.0)
    p3 = np.where(four, (p0 + 2 * p1) / 3.0, 0.0)
    palette = np.stack([p0, p1, p2, p3], axis=1)
    idx = np.stack([(packed["bits"] >> (2 * p)) & 3 for p in range(16)], axis=1)
    px = np.take_along_axis(palette, idx[:, :, None].astype(np.int64), axis=1)
    bw, bh = w // 4, h // 4
    return np.clip(px.reshape(bh, bw, 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(h, w, 3) + 0.5, 0, 255).astype(np.uint8)


def colour_levels(srgb8, min_side=4):
    """BC1 mips of an sRGB picture, each halved in linear light."""
    levels = []
    linear = srgb_to_linear(srgb8)
    current = srgb8
    while True:
        h, w, _ = current.shape
        levels.append((w, h, bc1_encode(current)))
        if min(w, h) // 2 < min_side:
            break
        linear = halve(linear)
        current = to_srgb8(linear)
    return levels


def bgra_levels(rgb_float, renormalise=False, min_side=4):
    """BGRA8 mips of a linear 0..1 picture (H x W x 3); renormalise keeps normal map texels unit length."""
    levels = []
    current = rgb_float
    while True:
        h, w, _ = current.shape
        q = np.clip(current * 255.0 + 0.5, 0, 255).astype(np.uint8)
        bgra = np.empty((h, w, 4), np.uint8)
        bgra[..., 0] = q[..., 2]
        bgra[..., 1] = q[..., 1]
        bgra[..., 2] = q[..., 0]
        bgra[..., 3] = 255
        levels.append((w, h, bgra.tobytes()))
        if min(w, h) // 2 < min_side:
            break
        current = halve(current)
        if renormalise:
            n = current * 2.0 - 1.0
            n /= np.maximum(np.linalg.norm(n, axis=2, keepdims=True), 1e-6)
            current = n * 0.5 + 0.5
    return levels


# ---- Noise ---------------------------------------------------------------------------------------------------------

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

    def fbm_part(self, p, first, last, octaves, scale, gain=0.5, lacunarity=2.03):
        """Octaves first..last-1 of an fbm of `octaves` octaves, normalised as the whole sum is: the broad octaves can be
        painted at a low resolution and the fine ones added at full resolution."""
        norm = sum(gain ** o for o in range(octaves))
        total = np.zeros(p[0].shape, dtype=np.float32)
        for o in range(first, last):
            s = scale * lacunarity ** o
            total += (gain ** o) * self.value(p[0] * s + o * 17.1, p[1] * s - o * 9.3, p[2] * s + o * 5.7)
        return total / norm

    def fbm(self, p, octaves, scale, gain=0.5, lacunarity=2.03):
        return self.fbm_part(p, 0, octaves, octaves, scale, gain, lacunarity)

    def ridged(self, p, octaves, scale, gain=0.5, lacunarity=2.1):
        """Sharp crests (1 - |noise|, squared, each octave weighted by the one before): mountain ranges."""
        total = np.zeros(p[0].shape, dtype=np.float32)
        weight = np.ones(p[0].shape, dtype=np.float32)
        norm = 0.0
        for o in range(octaves):
            s = scale * lacunarity ** o
            n = 1.0 - np.abs(self.value(p[0] * s + o * 3.1, p[1] * s + o * 7.7, p[2] * s - o * 1.3))
            n = n * n * weight
            weight = np.clip(n * 1.6, 0.0, 1.0)
            total += n * gain ** o
            norm += gain ** o
        return total / norm


def directions(row0, rows, width, height):
    """Unit vectors (3 x rows x width) for the pixel centres of rows row0..row0+rows of an equirectangular picture;
    also latitude in radians."""
    v = (np.arange(row0, row0 + rows, dtype=np.float32) + 0.5) / height
    u = (np.arange(width, dtype=np.float32) + 0.5) / width
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


class Upsampler:
    """Bilinear lookups of low resolution equirectangular fields at the pixel centres of a bigger picture (wraps east to
    west, clamps at the poles)."""

    def __init__(self, low_w, low_h, high_w, high_h):
        x = (np.arange(high_w, dtype=np.float32) + 0.5) / high_w * low_w - 0.5
        x0 = np.floor(x).astype(np.int32)
        self.fx = (x - x0).astype(np.float32)
        self.x0 = np.mod(x0, low_w)
        self.x1 = np.mod(x0 + 1, low_w)
        self.low_h = low_h
        self.high_h = high_h

    def rows(self, field, row0, rows):
        y = (np.arange(row0, row0 + rows, dtype=np.float32) + 0.5) / self.high_h * self.low_h - 0.5
        y0 = np.floor(y).astype(np.int32)
        fy = (y - y0).astype(np.float32)[:, None]
        y1 = np.clip(y0 + 1, 0, self.low_h - 1)
        y0 = np.clip(y0, 0, self.low_h - 1)
        a = field[..., y0, :]
        b = field[..., y1, :]
        top = a[..., self.x0] + (a[..., self.x1] - a[..., self.x0]) * self.fx
        bottom = b[..., self.x0] + (b[..., self.x1] - b[..., self.x0]) * self.fx
        return top + (bottom - top) * fy


def blur_wrap(field, radius, passes=3):
    """Box blur of a 2D equirectangular field, wrapping east to west, clamped at the poles (a few passes: near gaussian)."""
    out = field.astype(np.float32)
    k = 2 * radius + 1
    for _ in range(passes):
        padded = np.concatenate([out[:, -radius:], out, out[:, :radius]], axis=1)
        c = np.cumsum(np.pad(padded, ((0, 0), (1, 0))), axis=1)
        out = (c[:, k:] - c[:, :-k]) / k
        padded = np.pad(out, ((radius, radius), (0, 0)), mode="edge")
        c = np.cumsum(np.pad(padded, ((1, 0), (0, 0))), axis=0)
        out = (c[k:] - c[:-k]) / k
    return out.astype(np.float32)


# ---- The planet ----------------------------------------------------------------------------------------------------

SEA = 0.02  # height of the shore


class Planet:
    def __init__(self):
        self.land = Noise3(6650)
        self.detail = Noise3(711)
        self.moist = Noise3(4242)
        self.cloud = Noise3(9001)
        self.warp = Noise3(313)
        self.ridge = Noise3(1234)
        self.river = Noise3(555)
        self.grain = Noise3(808)
        rng = np.random.default_rng(97)
        # Storms: spiral swirls in the clouds, more of them in the mid latitudes and the tropics.
        lat = np.radians(rng.uniform(12.0, 58.0, 9)) * rng.choice([-1.0, 1.0], 9)
        lon = rng.uniform(0.0, 2.0 * np.pi, 9)
        self.storms = np.stack([np.cos(lat) * np.cos(lon), np.cos(lat) * np.sin(lon), np.sin(lat)], axis=1).astype(np.float32)
        self.storm_size = rng.uniform(0.07, 0.16, 9).astype(np.float32)
        self.storm_turn = (rng.uniform(2.5, 4.5, 9) * np.sign(lat)).astype(np.float32)  # spin with the hemisphere

    def broad(self, d, lat):
        """The fields painted at low resolution: warp, the continents' broad octaves, moisture and the cloud swirls."""
        f = {}
        warp = np.stack([self.warp.fbm(d, 4, 1.5), self.warp.fbm(d + 3.3, 4, 1.5), self.warp.fbm(d - 7.1, 4, 1.5)]) * 0.35
        f["warp"] = warp
        f["land"] = self.land.fbm_part(d + warp, 0, 6, 10, 1.3)
        f["detail"] = self.detail.fbm_part(d, 0, 4, 7, 5.0)
        moist = self.moist.fbm(d, 6, 2.2) * 0.7 + 0.45 - 0.55 * smooth(0.25, 0.0, np.abs(np.abs(lat) - 0.42))  # dry belts
        moist += 0.25 * smooth(0.25, 0.0, np.abs(lat))  # wet equator
        f["moist"] = moist
        f["range"] = self.ridge.fbm(d + warp * 0.5, 4, 1.8)  # where the mountain ranges run
        cd = self.swirl(d)
        cw = np.stack([self.cloud.fbm(cd * 2.0, 5, 1.0), self.cloud.fbm(cd * 2.0 + 5.2, 5, 1.0), self.cloud.fbm(cd * 2.0 - 2.7, 5, 1.0)])
        stretched = cd + 0.45 * cw
        stretched[2] *= 1.8  # zonal streaks
        f["cloud_p"] = stretched
        f["cloud"] = self.cloud.fbm_part(stretched, 0, 5, 10, 2.6)
        return f

    def swirl(self, d):
        """Turns directions round each storm's centre, more near the middle: the clouds spiral in."""
        out = d.copy()
        for c, size, turn in zip(self.storms, self.storm_size, self.storm_turn):
            cosang = out[0] * c[0] + out[1] * c[1] + out[2] * c[2]
            dist = np.arccos(np.clip(cosang, -1.0, 1.0))
            angle = turn * np.exp(-(dist / size) ** 2) * (1.0 - np.exp(-(dist / (size * 0.18)) ** 2) * 0.6)
            ca, sa = np.cos(angle), np.sin(angle)
            cross = np.stack([c[1] * out[2] - c[2] * out[1], c[2] * out[0] - c[0] * out[2], c[0] * out[1] - c[1] * out[0]])
            dot = cosang
            out = out * ca + cross * sa + np.stack([c[0] * dot, c[1] * dot, c[2] * dot]) * (1.0 - ca)
        return out

    def paint(self):
        t0 = time.time()
        # 1. The broad fields at a quarter resolution.
        low = {}
        for row0 in range(0, LOW_H, CHUNK):
            d, lat = directions(row0, CHUNK, LOW_W, LOW_H)
            part = self.broad(d, lat)
            for k, v in part.items():
                if k not in low:
                    low[k] = np.zeros((v.shape[0], LOW_H, LOW_W) if v.ndim == 3 else (LOW_H, LOW_W), np.float32)
                low[k][..., row0:row0 + CHUNK, :] = v
        # Coasts (cities gather there): how much sea is near, from the broad land.
        land_low = (low["land"] + 0.35 * low["detail"] > SEA).astype(np.float32)
        near_sea = 1.0 - blur_wrap(land_low, 6)
        print("planet broad fields", round(time.time() - t0, 1), "s", flush=True)

        up = Upsampler(LOW_W, LOW_H, PLANET_W, PLANET_H)
        colour = np.zeros((PLANET_H, PLANET_W, 3), np.uint8)
        height_half = np.zeros((HALF_H, HALF_W), np.float32)
        cover_half = np.zeros((HALF_H, HALF_W), np.float32)
        water_half = np.zeros((HALF_H, HALF_W), np.float32)
        ice_half = np.zeros((HALF_H, HALF_W), np.float32)
        habit_half = np.zeros((HALF_H, HALF_W), np.float32)

        deep = np.array([0.006, 0.022, 0.075], np.float32)
        ocean = np.array([0.012, 0.050, 0.140], np.float32)
        shelf = np.array([0.030, 0.150, 0.230], np.float32)
        lagoon = np.array([0.070, 0.300, 0.330], np.float32)
        beach = np.array([0.520, 0.460, 0.330], np.float32)
        jungle = np.array([0.028, 0.095, 0.030], np.float32)
        forest = np.array([0.045, 0.120, 0.040], np.float32)
        grass = np.array([0.150, 0.210, 0.070], np.float32)
        savanna = np.array([0.330, 0.290, 0.130], np.float32)
        desert = np.array([0.600, 0.440, 0.250], np.float32)
        red_desert = np.array([0.520, 0.270, 0.130], np.float32)
        tundra = np.array([0.240, 0.230, 0.180], np.float32)
        boreal = np.array([0.040, 0.080, 0.055], np.float32)
        rock = np.array([0.280, 0.240, 0.205], np.float32)
        dark_rock = np.array([0.120, 0.105, 0.095], np.float32)
        snow = np.array([0.860, 0.890, 0.930], np.float32)
        river_blue = np.array([0.020, 0.070, 0.120], np.float32)
        cloud_white = np.array([0.900, 0.910, 0.930], np.float32)
        cloud_grey = np.array([0.560, 0.600, 0.660], np.float32)

        for row0 in range(0, PLANET_H, CHUNK):
            d, lat = directions(row0, CHUNK, PLANET_W, PLANET_H)
            warp = up.rows(low["warp"], row0, CHUNK)
            dw = d + warp
            h = up.rows(low["land"], row0, CHUNK) + self.land.fbm_part(dw, 6, 10, 10, 1.3)
            h += 0.35 * (up.rows(low["detail"], row0, CHUNK) + self.detail.fbm_part(d, 4, 7, 7, 5.0))
            coast = up.rows(near_sea, row0, CHUNK)
            # Mountain ranges: ridged crests along the bands where `range` is high, inland.
            ranges = smooth(-0.3, 0.1, up.rows(low["range"], row0, CHUNK))
            crest = self.ridge.ridged(dw, 6, 4.0)
            inland = smooth(SEA, SEA + 0.10, h)
            fold = smooth(0.42, 0.88, crest) * ranges * inland  # the crest lines themselves, sparse
            h += fold * 0.32 + crest * ranges * inland * 0.08
            h += 0.010 * self.grain.fbm(d, 3, 90.0) * inland  # the small ups and downs
            land = h > SEA
            alt = np.clip((h - SEA) / 0.55, 0.0, 1.0)
            depth = np.clip((SEA - h) / 0.35, 0.0, 1.0)

            # Climate: warm at the equator, cold at the poles and up high; moisture from the broad field.
            temp = np.cos(lat) ** 1.4 - alt * 0.75 + 0.05 * self.detail.fbm(d, 3, 9.0)
            moist = up.rows(low["moist"], row0, CHUNK) + 0.12 * self.moist.fbm_part(d, 2, 6, 6, 9.0) + 0.25 * coast * (temp > 0.4)

            # Sea: shallow shelves by the shore (turquoise in the warm seas), deep blue far out.
            water = mix(shelf, ocean, smooth(0.0, 0.22, depth))
            water = mix(water, deep, smooth(0.25, 0.9, depth))
            water = mix(water, lagoon, smooth(0.03, 0.0, depth) * smooth(0.55, 0.85, temp))
            water *= (1.0 + 0.10 * self.grain.fbm(d * 1.0 + 4.0, 3, 24.0))[..., None]

            # Land: biomes by temperature and moisture, then rock and snow up high.
            wet = smooth(0.30, 0.75, moist)
            hot = smooth(0.55, 0.85, temp)
            green = mix(grass, mix(forest, jungle, hot), smooth(0.45, 0.85, moist))
            dry = mix(savanna, mix(desert, red_desert, smooth(0.0, 0.5, self.grain.fbm(d + 9.0, 4, 6.0))), smooth(0.25, 0.05, moist))
            ground = mix(dry, green, wet)
            cold = smooth(0.42, 0.22, temp)
            ground = mix(ground, mix(tundra, boreal, smooth(0.35, 0.7, moist)), cold * smooth(0.12, 0.25, temp))
            ground = mix(ground, beach, smooth(0.012, 0.0, h - SEA) * (1.0 - cold))
            steep = smooth(0.1, 0.6, fold)
            ground = mix(ground, rock, smooth(0.40, 0.75, alt) * 0.9 + steep * 0.35)
            ground = mix(ground, dark_rock, smooth(0.35, 0.05, fold) * smooth(0.3, 0.6, crest) * ranges * inland * 0.5)
            peaks = smooth(0.55, 0.9, alt) * (0.35 + 0.65 * smooth(0.25, 0.8, fold)) * (0.8 + 0.2 * self.grain.fbm(d - 3.0, 3, 70.0))
            ground = mix(ground, snow, np.clip(smooth(0.16, 0.04, temp) + peaks * 0.85, 0.0, 1.0))
            # Rivers: thin dark winding lines on the wetter lowlands.
            rp = dw * 1.0
            rv = np.abs(self.river.fbm(rp, 6, 5.0))
            river = smooth(0.007, 0.0015, rv) * smooth(0.25, 0.5, moist) * smooth(0.45, 0.15, alt) * smooth(SEA + 0.005, SEA + 0.03, h)
            ground = mix(ground, river_blue, river * 0.85)
            # Relief painted in as well (crests light, the folds between them dark), so the ranges read from orbit
            # whatever the light; and a broad mottling so no plain is one flat colour.
            tone = 1.0 + 0.16 * self.detail.fbm(d, 4, 22.0) + 0.08 * self.grain.fbm(d, 3, 140.0)
            tone *= 0.9 + 0.35 * smooth(0.2, 0.9, fold) - 0.2 * smooth(0.45, 0.2, crest) * ranges * inland
            tone *= 1.0 + 0.12 * self.moist.fbm(d + 5.0, 3, 3.0)
            ground = ground * tone[..., None]
            surface = np.where(land[..., None], ground, water)
            ice = smooth(1.20, 1.34, np.abs(lat) + 0.10 * self.detail.fbm(d, 5, 7.0))
            ice_tex = 1.0 - 0.10 * np.abs(self.grain.fbm(d, 4, 60.0))
            surface = mix(surface, snow * ice_tex[..., None], ice)

            # Clouds: the broad swirls plus fine wisps; their shadows darken the ground just beside them.
            cp = up.rows(low["cloud_p"], row0, CHUNK)
            c = up.rows(low["cloud"], row0, CHUNK) + self.cloud.fbm_part(cp, 5, 10, 10, 2.6)
            c += 0.18 * np.cos(np.abs(lat) * 6.0) - 0.05
            cover = smooth(0.0, 0.45, c)
            cover = np.clip(cover * (0.85 + 0.3 * smooth(0.2, 0.0, np.abs(lat))), 0.0, 1.0)
            wisp = smooth(-0.1, 0.3, self.cloud.fbm(d * 1.0 + 2.0, 5, 14.0))
            cover = np.clip(cover * (0.8 + 0.2 * wisp) + 0.10 * wisp * smooth(0.0, 0.3, cover), 0.0, 1.0)
            surface *= (1.0 - 0.35 * smooth(0.15, 0.8, cover))[..., None]
            cloud_tone = mix(cloud_grey * np.ones_like(cover)[..., None], cloud_white, smooth(0.3, 0.95, cover))
            final = mix(surface, cloud_tone, smooth(0.0, 1.0, cover) ** 0.9)
            colour[row0:row0 + CHUNK] = to_srgb8(final)

            # Halved fields for the relief, roughness and lights.
            hr = row0 // 2
            relief = np.where(land, h - SEA, 0.0).astype(np.float32) * (1.0 - ice * 0.6)
            height_half[hr:hr + CHUNK // 2] = halve(relief[..., None])[..., 0]
            cover_half[hr:hr + CHUNK // 2] = halve(cover[..., None])[..., 0]
            water_half[hr:hr + CHUNK // 2] = halve((~land).astype(np.float32)[..., None])[..., 0]
            ice_half[hr:hr + CHUNK // 2] = halve(ice[..., None])[..., 0]
            habit = land * smooth(0.25, 0.55, temp) * smooth(0.10, 0.40, moist) * smooth(0.45, 0.10, alt) * (1.0 - ice)
            habit = habit * (0.35 + 0.65 * coast) + river * 0.6 * land
            habit_half[hr:hr + CHUNK // 2] = halve(habit.astype(np.float32)[..., None])[..., 0]
            if row0 % 512 == 0:
                print("planet rows", row0 + CHUNK, "of", PLANET_H, round(time.time() - t0, 1), "s", flush=True)

        write_r3tx(os.path.join(OUT, "planet_color.r3tx"), FORMAT_BC1, colour_levels(colour))
        self.preview = colour[::8, ::8].copy()
        del colour
        self.write_normals(height_half, cover_half)
        self.write_roughness(water_half, cover_half, ice_half)
        self.write_lights(habit_half, cover_half)
        print("planet done", round(time.time() - t0, 1), "s", flush=True)

    @staticmethod
    def write_normals(height, cover):
        """Tangent-space relief: x along the sphere's east (its UV u), y = normal x east (north), as TitleStage.cpp
        gives the mesh its tangents. The height is softened a little first (no single-texel grain), and slopes are
        scaled so the steepest ranges lean about 20 degrees: the relief shows along the terminator without speckling
        the limb, seen edge-on."""
        height = blur_wrap(height, 1, passes=2)
        h, w = height.shape
        lat = (np.pi * 0.5 - (np.arange(h, dtype=np.float32) + 0.5) / h * np.pi)[:, None]
        step_x = np.maximum(np.cos(lat), 0.05) * 2.0 * np.pi / w
        step_y = np.pi / h
        east = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) / (2.0 * step_x)
        down = np.empty_like(height)
        down[1:-1] = (height[2:] - height[:-2]) / (2.0 * step_y)
        down[0] = down[1]
        down[-1] = down[-2]
        north = -down
        slope = np.sqrt(east ** 2 + north ** 2)
        scale = 0.36 / max(float(np.percentile(slope[slope > 0], 99.5)), 1e-6)
        flat = 1.0 - 0.85 * smooth(0.1, 0.8, cover)
        nx = -east * scale * flat
        ny = -north * scale * flat
        nz = np.ones_like(nx)
        n = np.stack([nx, ny, nz], axis=2)
        n /= np.linalg.norm(n, axis=2, keepdims=True)
        write_r3tx(os.path.join(OUT, "planet_normal.r3tx"), FORMAT_BGRA8, bgra_levels(n * 0.5 + 0.5, renormalise=True))

    @staticmethod
    def write_roughness(water, cover, ice):
        small = halve(np.stack([water, cover, ice], axis=2))
        water_s, cover_s, ice_s = small[..., 0], small[..., 1], small[..., 2]
        rough = 0.88 + (0.30 - 0.88) * water_s
        rough = rough + (0.55 - rough) * ice_s
        rough = rough + (0.95 - rough) * smooth(0.05, 0.6, cover_s)
        rgb = np.stack([rough, rough, rough], axis=2)
        write_r3tx(os.path.join(OUT, "planet_rough.r3tx"), FORMAT_BGRA8, bgra_levels(rgb))

    @staticmethod
    def write_lights(habit, cover):
        """City lights: towns scattered where people would live (warm, wet enough, low, by the coasts and rivers), each
        a bright core in a glow of suburbs, faint lights along the countryside; under thick cloud they fade."""
        h, w = habit.shape
        rng = np.random.default_rng(4040)
        lights = np.zeros((h, w), np.float32)
        weights = habit.ravel().astype(np.float64)
        rowlat = np.cos(np.pi * 0.5 - (np.arange(h) + 0.5) / h * np.pi)
        weights *= np.repeat(np.abs(rowlat), w)  # equal area
        total = weights.sum()
        if total > 0:
            count = 5200
            picks = rng.choice(len(weights), size=count, p=weights / total)
            ys, xs = np.divmod(picks, w)
            size = (rng.pareto(2.2, count) * 0.6 + 0.5).clip(0.5, 6.0)
            bright = (0.25 + 0.75 * rng.random(count)) * np.sqrt(size)
            for radius in range(1, 7):
                sel = size * 1.2 >= radius - 0.5
                for dy in range(-radius, radius + 1):
                    for dx in range(-radius, radius + 1):
                        r2 = dx * dx + dy * dy
                        if r2 > radius * radius or (radius > 1 and r2 <= (radius - 1) * (radius - 1)):
                            continue
                        fall = np.exp(-r2 / (2.0 * (size[sel] * 0.55) ** 2))
                        np.add.at(lights, ((ys[sel] + dy).clip(0, h - 1), (xs[sel] + dx) % w), bright[sel] * fall * 0.6)
        speckle = Noise3(77)
        for row0 in range(0, h, CHUNK):
            d, _ = directions(row0, CHUNK, w, h)
            n = speckle.fbm(d, 3, 160.0)
            lights[row0:row0 + CHUNK] += habit[row0:row0 + CHUNK] * smooth(0.15, 0.45, n) * 0.18
        lights *= 1.0 - 0.8 * smooth(0.2, 0.9, cover)
        lights = np.clip(lights, 0.0, 1.0)
        warm = np.array([1.0, 0.60, 0.26], np.float32)
        rgb = lights[..., None] * warm
        write_r3tx(os.path.join(OUT, "planet_lights.r3tx"), FORMAT_BC1, colour_levels(to_srgb8(rgb)))


# ---- The sky -------------------------------------------------------------------------------------------------------

class Sky:
    """Linear light: the band ~0.05-0.15, its core ~0.4, background ~0.002, stars up to ~1.5."""

    def __init__(self):
        self.band_noise = Noise3(1977)
        self.dust_noise = Noise3(2049)
        self.neb_noise = Noise3(88)
        normal = np.array([0.18, -0.62, 0.76], np.float32)
        self.normal = normal / np.linalg.norm(normal)
        core = np.array([0.80, 0.35, 0.19], np.float32)
        core -= self.normal * np.dot(core, self.normal)  # the bulge lies in the band
        self.core = core / np.linalg.norm(core)
        self.stars, self.bright, self.colours = self.catalogue()

    def light(self, d, fine):
        """Diffuse light of the galaxy for directions d (3 x ...); fine adds the small star clouds a crisp view shows."""
        n, core = self.normal, self.core
        b = d[0] * n[0] + d[1] * n[1] + d[2] * n[2]  # distance from the band's plane
        to_core = np.arccos(np.clip(d[0] * core[0] + d[1] * core[1] + d[2] * core[2], -1.0, 1.0))
        clump = np.clip(self.band_noise.fbm(d, 6, 3.5) * 1.6 + 0.45, 0.0, 1.4)
        grain = 0.6 + 0.4 * np.clip(self.band_noise.fbm(d + 4.0, 3, 48.0) * 2.0 + 0.5, 0.0, 1.0)  # unresolved star clouds
        if fine:
            grain *= 0.75 + 0.5 * np.clip(self.band_noise.fbm(d - 6.0, 4, 220.0) + 0.5, 0.0, 1.0)
        texture = clump ** 1.6 * grain
        band = np.exp(-(b / 0.13) ** 2) * texture * 0.055 + np.exp(-(b / 0.045) ** 2) * texture * 0.05 + np.exp(-(b / 0.4) ** 2) * 0.006
        bulge = np.exp(-(to_core / 0.38) ** 2) * np.exp(-(b / 0.2) ** 2) * 0.18 * (0.65 + 0.35 * grain)
        light = band + bulge
        lane_warp = np.stack([self.dust_noise.fbm(d, 3, 2.0), self.dust_noise.fbm(d + 2.0, 3, 2.0), self.dust_noise.fbm(d - 2.0, 3, 2.0)]) * 0.25
        filaments = self.dust_noise.fbm(d + lane_warp, 8 if fine else 6, 7.0)
        dust = smooth(-0.15, 0.3, filaments) * np.exp(-((b + 0.012 * self.band_noise.fbm(d, 3, 6.0)) / 0.06) ** 2)
        light *= 1.0 - 0.88 * dust
        warm = np.clip(bulge / 0.32, 0.0, 1.0)
        tint = mix(np.array([0.75, 0.85, 1.0], np.float32) * np.ones_like(b)[..., None], np.array([1.0, 0.82, 0.6], np.float32), warm)
        rgb = light[..., None] * tint
        # Nebulae: soft magenta and teal clouds near the band.
        n1 = smooth(0.15, 0.6, self.neb_noise.fbm(d, 7 if fine else 5, 3.0)) * np.exp(-(b / 0.35) ** 2)
        n2 = smooth(0.2, 0.65, self.neb_noise.fbm(d + 11.0, 7 if fine else 5, 2.5)) * np.exp(-(b / 0.5) ** 2)
        rgb += n1[..., None] * np.array([0.050, 0.010, 0.040], np.float32)
        rgb += n2[..., None] * np.array([0.005, 0.030, 0.040], np.float32)
        rgb += np.array([0.0008, 0.0010, 0.0018], np.float32)
        return rgb

    def catalogue(self):
        """Stars: uniform on the sphere plus more in the band; most faint, a few bright. Colours from red dwarfs to blue
        giants (more of the white-yellow ones)."""
        rng = np.random.default_rng(6650)
        count = 260000
        v = rng.normal(size=(count, 3)).astype(np.float32)
        v /= np.linalg.norm(v, axis=1)[:, None]
        in_band = rng.normal(size=(count // 2, 3)).astype(np.float32)
        in_band -= np.outer(in_band @ self.normal, self.normal) * (1.0 - rng.random(count // 2)[:, None] * 0.25).astype(np.float32)
        in_band /= np.linalg.norm(in_band, axis=1)[:, None]
        stars = np.concatenate([v, in_band])
        bright = (rng.pareto(1.7, size=len(stars)) * 0.05 + 0.008).astype(np.float32)
        bright = np.clip(bright, 0.0, 1.8)
        temp = rng.beta(2.2, 2.0, len(stars)).astype(np.float32)  # 0 red, 1 blue
        red = np.array([1.0, 0.62, 0.38], np.float32)
        white = np.array([1.0, 0.95, 0.88], np.float32)
        blue = np.array([0.66, 0.78, 1.0], np.float32)
        colours = np.where((temp < 0.5)[:, None], red + (white - red) * (temp / 0.5)[:, None], white + (blue - white) * ((temp - 0.5) / 0.5)[:, None])
        return stars, bright, colours.astype(np.float32)

    def paint_sphere(self):
        image = np.zeros((GALAXY_H, GALAXY_W, 3), dtype=np.float32)
        for row0 in range(0, GALAXY_H, CHUNK):
            d, _ = directions(row0, CHUNK, GALAXY_W, GALAXY_H)
            image[row0:row0 + CHUNK] = self.light(d, False)
        theta = np.arccos(np.clip(self.stars[:, 2], -1.0, 1.0))
        phi = np.mod(np.arctan2(self.stars[:, 1], self.stars[:, 0]), 2.0 * np.pi)
        px = np.clip((phi / (2.0 * np.pi) * GALAXY_W).astype(np.int32), 0, GALAXY_W - 1)
        py = np.clip((theta / np.pi * GALAXY_H).astype(np.int32), 0, GALAXY_H - 1)
        np.add.at(image, (py, px), self.colours * self.bright[:, None] * 0.5)
        write_png(os.path.join(OUT, "galaxy.png"), to_srgb8(image))

    def view_to_sky(self, d_stage):
        """Stage directions (x ahead, y right, z up) to the galaxy sphere's own (it is turned by TsGalaxyTurn)."""
        a = math.radians(VIEW_TURN_DEGREES)
        x = d_stage[0] * math.cos(a) - d_stage[1] * math.sin(a)
        y = d_stage[0] * math.sin(a) + d_stage[1] * math.cos(a)
        return np.stack([x, y, d_stage[2]])

    def paint_view(self):
        """The window's sky: a flat picture at x = 1 in front of the eye, y from -VIEW_HALF_X (left) to +VIEW_HALF_X,
        z from +VIEW_HALF_Y (top row) to -VIEW_HALF_Y."""
        image = np.zeros((VIEW_H, VIEW_W, 3), dtype=np.float32)
        ty = ((np.arange(VIEW_W, dtype=np.float32) + 0.5) / VIEW_W * 2.0 - 1.0) * VIEW_HALF_X
        for row0 in range(0, VIEW_H, CHUNK):
            rows = min(CHUNK, VIEW_H - row0)
            tz = (1.0 - (np.arange(row0, row0 + rows, dtype=np.float32) + 0.5) / VIEW_H * 2.0) * VIEW_HALF_Y
            y = np.broadcast_to(ty[None, :], (rows, VIEW_W))
            z = np.broadcast_to(tz[:, None], (rows, VIEW_W))
            x = np.ones_like(y)
            d = np.stack([x, y, z])
            d /= np.linalg.norm(d, axis=0, keepdims=True)
            image[row0:row0 + rows] = self.light(self.view_to_sky(d), True)
        print("sky view band done", flush=True)

        # The catalogue's stars in view, plus a dense dust of faint ones (thicker in the band) only this crisp view shows.
        rng = np.random.default_rng(1313)
        extra = 90000
        e = rng.normal(size=(extra * 30, 3)).astype(np.float32)
        e /= np.linalg.norm(e, axis=1)[:, None]
        a = math.radians(VIEW_TURN_DEGREES)
        stage = np.stack([e[:, 0] * math.cos(-a) - e[:, 1] * math.sin(-a), e[:, 0] * math.sin(-a) + e[:, 1] * math.cos(-a), e[:, 2]], axis=1)
        ok = stage[:, 0] > 0.5
        ok &= np.abs(stage[:, 1] / np.maximum(stage[:, 0], 1e-6)) < VIEW_HALF_X * 1.02
        ok &= np.abs(stage[:, 2] / np.maximum(stage[:, 0], 1e-6)) < VIEW_HALF_Y * 1.02
        e = e[ok]
        b = e @ self.normal
        keep = rng.random(len(e)) < (0.25 + 0.75 * np.exp(-(b / 0.12) ** 2))
        e = e[keep][:extra]
        faint = (rng.pareto(2.5, len(e)) * 0.006 + 0.0035).astype(np.float32).clip(0.0, 0.08)
        temp = rng.random(len(e)).astype(np.float32)
        fcol = np.stack([0.85 + 0.15 * (1 - temp), 0.88 + 0.07 * np.ones_like(temp), 0.8 + 0.2 * temp], axis=1).astype(np.float32)
        stars = np.concatenate([self.stars, e])
        bright = np.concatenate([self.bright, faint])
        colours = np.concatenate([self.colours, fcol])

        stage = np.stack([stars[:, 0] * math.cos(-a) - stars[:, 1] * math.sin(-a), stars[:, 0] * math.sin(-a) + stars[:, 1] * math.cos(-a), stars[:, 2]], axis=1)
        front = stage[:, 0] > 0.3
        stage, bright, colours = stage[front], bright[front], colours[front]
        fx = (stage[:, 1] / stage[:, 0] / VIEW_HALF_X * 0.5 + 0.5) * VIEW_W - 0.5
        fy = (0.5 - stage[:, 2] / stage[:, 0] / VIEW_HALF_Y * 0.5) * VIEW_H - 0.5
        inside = (fx > -4) & (fx < VIEW_W + 3) & (fy > -4) & (fy < VIEW_H + 3)
        fx, fy, bright, colours = fx[inside], fy[inside], bright[inside], colours[inside]
        print("stars in view", len(fx), flush=True)
        # Each star a small round gaussian placed to a fraction of a texel (its peak is its brightness); the bright ones
        # a little wider with a faint halo, so they read bigger without blurring.
        ix = np.floor(fx).astype(np.int32)
        iy = np.floor(fy).astype(np.int32)
        sx = fx - ix
        sy = fy - iy
        sigma = (0.52 + 0.30 * np.clip(np.log10(bright / 0.01 + 1.0), 0.0, 2.0)).astype(np.float32)
        for dy in range(-2, 4):
            for dx in range(-2, 4):
                r2 = (dx - sx) ** 2 + (dy - sy) ** 2
                w = np.exp(-r2 / (2.0 * sigma ** 2))
                yy, xx = iy + dy, ix + dx
                ok = (yy >= 0) & (yy < VIEW_H) & (xx >= 0) & (xx < VIEW_W) & (w > 0.002)
                np.add.at(image, (yy[ok], xx[ok]), colours[ok] * (bright[ok] * w[ok])[:, None])
        big = np.nonzero(bright > 0.25)[0]
        for i in big:
            r = 9
            y0, x0 = iy[i], ix[i]
            ys = np.arange(max(0, y0 - r), min(VIEW_H, y0 + r + 1))
            xs = np.arange(max(0, x0 - r), min(VIEW_W, x0 + r + 1))
            if len(ys) == 0 or len(xs) == 0:
                continue
            gy, gx = np.meshgrid(ys - fy[i], xs - fx[i], indexing="ij")
            halo = np.exp(-(gx ** 2 + gy ** 2) / (2.0 * 3.0 ** 2)) * 0.035 * bright[i]
            image[ys[0]:ys[-1] + 1, xs[0]:xs[-1] + 1] += halo[..., None] * colours[i]
        dither = (rng.random((VIEW_H, VIEW_W, 1)).astype(np.float32) - 0.5) * 0.0006
        write_png(os.path.join(OUT, "sky_view.png"), to_srgb8(image + dither))


def main():
    global PLANET_W, PLANET_H, LOW_W, LOW_H, HALF_W, HALF_H, ROUGH_W, ROUGH_H
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
    os.makedirs(OUT, exist_ok=True)
    if "quick" in argv:  # a quarter of the planet's size, for trying looks
        PLANET_W, PLANET_H, LOW_W, LOW_H, HALF_W, HALF_H, ROUGH_W, ROUGH_H = 2048, 1024, 512, 256, 1024, 512, 512, 256
    only = [a for a in argv if a in ("planet", "galaxy")]
    if not only or "galaxy" in only:
        sky = Sky()
        sky.paint_view()
        sky.paint_sphere()
    if not only or "planet" in only:
        Planet().paint()


if __name__ == "__main__":
    main()
