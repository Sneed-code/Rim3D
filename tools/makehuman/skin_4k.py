# The creator's "4K skin" (docs/design/makehuman.md): from a character's 2K skin colour map (skin.png beside its FBX),
# a 4096 colour map with fine detail added (a sharp resize, then faint mottling and pores, so the skin's own tones and
# the pawn's skin colour and pigment are kept), a 4096 normal map from that detail, and a roughness map (pores and
# mottling a little rougher). The MakeHumanArt commandlet makes a second skin material from them, and the creator
# switches between the 2K and 4K skins.
#   python tools/makehuman/skin_4k.py <folder with skin.png> [--strength 1.0]
import argparse
import os

import numpy as np
from PIL import Image, ImageFilter

parser = argparse.ArgumentParser()
parser.add_argument("folder")
parser.add_argument("--size", type=int, default=4096)
parser.add_argument("--strength", type=float, default=1.0)
parser.add_argument("--seed", type=int, default=11)
args = parser.parse_args()
rng = np.random.default_rng(args.seed)
size = args.size

src = Image.open(os.path.join(args.folder, "skin.png")).convert("RGB")
base = np.asarray(src.resize((size, size), Image.LANCZOS)).astype(np.float32) / 255.0


def blurred_noise(sigma):
    """Gaussian-blurred white noise, normalised to about -1..1."""
    noise = Image.fromarray((rng.random((size, size)) * 255).astype(np.uint8))
    out = np.asarray(noise.filter(ImageFilter.GaussianBlur(sigma))).astype(np.float32)
    out -= out.mean()
    return out / max(out.std() * 3.0, 1e-6)


# Mottling: soft blotches at a few sizes (blood under the skin, freckle-like unevenness).
mottle = 0.5 * blurred_noise(24) + 0.3 * blurred_noise(9) + 0.2 * blurred_noise(3)
# Pores: small dark dips scattered over the skin.
pores = np.asarray(Image.fromarray((rng.random((size, size)) < 0.018).astype(np.uint8) * 255).filter(ImageFilter.GaussianBlur(1.1))).astype(np.float32) / 255.0
pores = pores / max(pores.max(), 1e-6)
k = args.strength
colour = base * (1.0 + 0.045 * k * mottle[..., None])
colour[..., 0] *= 1.0 + 0.012 * k * mottle             # blotches lean a touch red
colour *= (1.0 - 0.07 * k * pores)[..., None]
colour = np.clip(colour, 0.0, 1.0)
Image.fromarray((colour * 255.0 + 0.5).astype(np.uint8)).save(os.path.join(args.folder, "skin_4k.png"))

# Normal: the skin's own high frequencies (as make_skin_normal does) plus the pores as dents and the fine mottling.
lum = colour[..., 0] * 0.3 + colour[..., 1] * 0.59 + colour[..., 2] * 0.11
lum_img = Image.fromarray((lum * 255).astype(np.uint8))
blur = np.asarray(lum_img.filter(ImageFilter.GaussianBlur(20))).astype(np.float32) / 255.0
height = (lum - blur) / np.maximum(blur, 0.05) * 6.0 - pores * 1.6 * k + blurred_noise(1.5) * 0.3 * k
dx = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * 0.5
dy = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * 0.5
n = np.stack([-dx, dy, np.ones_like(dx)], axis=-1)
n /= np.linalg.norm(n, axis=-1, keepdims=True)
Image.fromarray(((n * 0.5 + 0.5) * 255.0 + 0.5).astype(np.uint8)).save(os.path.join(args.folder, "skin_normal_4k.png"))

# Roughness: mid-grey is the material's own value; pores and blotches a little rougher, the rest a little smoother.
rough = np.clip(0.5 + 0.18 * k * pores + 0.06 * k * mottle, 0.0, 1.0)
Image.fromarray((rough * 255.0 + 0.5).astype(np.uint8)).save(os.path.join(args.folder, "skin_rough_4k.png"))
print("[skin_4k]", args.folder, "->", size, "colour, normal and roughness")
