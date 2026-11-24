#!/usr/bin/env python3
"""Cuts a ChatGPT icon sheet (a strict N x N grid of icons on a transparent background) into one PNG per icon.

    python3 tools/slice_icon_sheet.py sheet.png order_cancel order_deconstruct ... [--grid 3] [--size 256]
    python3 tools/slice_icon_sheet.py --all <folder>    # every sheet in sheets.json, from <folder>/rim3d_icons_<sheet>.png

Each cell's icon is found by its alpha, cropped to a square with a little margin, scaled to --size pixels and written to
assets/ui/icons/<name>.png. Names go left to right, top to bottom; "-" skips a cell. See assets/ui/icons/sheets.json for
the prompts and the names of every sheet, and docs/design/ui-layout.md ("Icons").
"""
import argparse
import json
import os
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("sheet", nargs="?")
    parser.add_argument("names", nargs="*")
    parser.add_argument("--all", metavar="FOLDER")
    parser.add_argument("--grid", type=int, default=3)
    parser.add_argument("--size", type=int, default=256)
    parser.add_argument("--out", default=os.path.join(ROOT, "assets", "ui", "icons"))
    args = parser.parse_args()
    if args.all:
        with open(os.path.join(ROOT, "assets", "ui", "icons", "sheets.json"), encoding="utf-8") as f:
            sheets = json.load(f)["sheets"]
        for entry in sheets:
            path = os.path.join(args.all, f"rim3d_icons_{entry['sheet']}.png")
            if not os.path.exists(path):
                print(f"sheet {entry['sheet']}: {path} missing", file=sys.stderr)
                continue
            slice_sheet(path, entry["names"], args.grid, args.size, args.out)
    elif args.sheet and args.names:
        slice_sheet(args.sheet, args.names, args.grid, args.size, args.out)
    else:
        parser.error("give a sheet and its names, or --all FOLDER")


def slice_sheet(path, names, n, size, out):
    sheet = Image.open(path).convert("RGBA")
    width, height = sheet.size
    if len(names) != n * n:
        sys.exit(f"need {n * n} names, got {len(names)}")
    alpha = sheet.getchannel("A").point(lambda a: 255 if a > 24 else 0)
    os.makedirs(out, exist_ok=True)
    for index, name in enumerate(names):
        if name == "-":
            continue
        row, col = divmod(index, n)
        cell = (round(col * width / n), round(row * height / n), round((col + 1) * width / n), round((row + 1) * height / n))
        box = alpha.crop(cell).getbbox()
        if not box:
            print(f"{name}: empty cell", file=sys.stderr)
            continue
        left, top, right, bottom = box[0] + cell[0], box[1] + cell[1], box[2] + cell[0], box[3] + cell[1]
        if left <= cell[0] + 1 or top <= cell[1] + 1 or right >= cell[2] - 1 or bottom >= cell[3] - 1:
            print(f"{name}: icon touches its cell edge, check it", file=sys.stderr)
        side = max(right - left, bottom - top) * 1.06
        cx, cy = (left + right) / 2, (top + bottom) / 2
        square = (round(cx - side / 2), round(cy - side / 2), round(cx + side / 2), round(cy + side / 2))
        icon = Image.new("RGBA", (square[2] - square[0], square[3] - square[1]), (0, 0, 0, 0))
        # Only this cell's pixels: a neighbour's edge that pokes into the margin is left out.
        clip = (max(square[0], cell[0]), max(square[1], cell[1]), min(square[2], cell[2]), min(square[3], cell[3]))
        icon.paste(sheet.crop(clip), (clip[0] - square[0], clip[1] - square[1]))
        icon = icon.convert("RGBa").resize((size, size), Image.LANCZOS).convert("RGBA")  # premultiplied: no dark rims
        icon.save(os.path.join(out, name + ".png"), optimize=True)
        print(f"{name}: {right - left}x{bottom - top} px in the sheet")


if __name__ == "__main__":
    main()
