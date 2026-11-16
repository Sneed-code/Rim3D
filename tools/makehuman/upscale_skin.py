# Upscales a skin colour map 4x with Real-ESRGAN (x4plus) and brings it to the wanted size (4096 by default), for the
# creator's "4K skin" (docs/design/makehuman.md). Runs with a Python that has torch and spandrel (ComfyUI's env):
#   <python> tools/makehuman/upscale_skin.py <in.png> <out.png> [--model game/Saved/Models/upscale/RealESRGAN_x4plus.pth]
#        [--size 4096]
# Tiled (512 px tiles, 32 px overlap) so it fits any GPU.
import argparse
import os

import numpy as np
import torch
from PIL import Image
from spandrel import ModelLoader

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

parser = argparse.ArgumentParser()
parser.add_argument("src")
parser.add_argument("out")
parser.add_argument("--model", default=os.path.join(REPO, "game", "Saved", "Models", "upscale", "RealESRGAN_x4plus.pth"))
parser.add_argument("--size", type=int, default=4096)
parser.add_argument("--tile", type=int, default=512)
args = parser.parse_args()

device = "cuda" if torch.cuda.is_available() else "cpu"
model = ModelLoader().load_from_file(args.model).to(device).eval()
scale = model.scale
img = Image.open(args.src).convert("RGB")
w, h = img.size
src = torch.from_numpy(np.asarray(img).astype(np.float32) / 255.0).permute(2, 0, 1).unsqueeze(0)
out = torch.zeros((1, 3, h * scale, w * scale))
weight = torch.zeros((1, 1, h * scale, w * scale))
tile, pad = args.tile, 32
with torch.no_grad():
    for y in range(0, h, tile):
        for x in range(0, w, tile):
            x0, y0 = max(x - pad, 0), max(y - pad, 0)
            x1, y1 = min(x + tile + pad, w), min(y + tile + pad, h)
            part = model(src[:, :, y0:y1, x0:x1].to(device)).float().cpu().clamp(0, 1)
            # Keep the tile's middle (its own area) so the seams between tiles fall in the overlap.
            ix0, iy0 = (x - x0) * scale, (y - y0) * scale
            ix1, iy1 = ix0 + min(tile, w - x) * scale, iy0 + min(tile, h - y) * scale
            out[:, :, y * scale:y * scale + (iy1 - iy0), x * scale:x * scale + (ix1 - ix0)] = part[:, :, iy0:iy1, ix0:ix1]
result = Image.fromarray((out[0].permute(1, 2, 0).numpy() * 255.0 + 0.5).astype(np.uint8))
if result.size != (args.size, args.size):
    result = result.resize((args.size, args.size), Image.LANCZOS)
os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
result.save(args.out)
print("[upscale_skin]", args.src, w, "->", result.size[0], "with", os.path.basename(args.model), "on", device)
