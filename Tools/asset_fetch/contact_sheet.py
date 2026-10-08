"""Builds RawAssets/contact_sheet_<category>.png: base colour of every imported texture set with its name underneath.

Set list/categories are read (as data, via ast) from Scripts/import_textures.py.
Usage: .venv-contact/bin/python -I contact_sheet.py
"""
import ast
import glob
import os
import re

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
RAW = os.path.join(ROOT, "RawAssets")
TILE, PAD, COLS, LABEL_H = 384, 16, 4, 44
BASECOLOR = {"polyhaven": ["{s}_diff_*", "{s}_diffuse_*"], "ambientcg": ["{s}_*_Color.*"]}

src = open(os.path.join(ROOT, "Scripts", "import_textures.py"), encoding="utf-8").read()
sets = ast.literal_eval(re.search(r"SETS = (\[.*?\n\])", src, re.S).group(1))

font = ImageFont.load_default(size=22)
small = ImageFont.load_default(size=16)

by_cat = {}
for cat, source, name in sets:
    by_cat.setdefault(cat, []).append((source, name))

for cat, items in by_cat.items():
    rows = -(-len(items) // COLS)
    w = PAD + COLS * (TILE + PAD)
    h = PAD + rows * (TILE + LABEL_H + PAD)
    sheet = Image.new("RGB", (w, h), (24, 24, 24))
    draw = ImageDraw.Draw(sheet)
    for i, (source, name) in enumerate(items):
        x = PAD + (i % COLS) * (TILE + PAD)
        y = PAD + (i // COLS) * (TILE + LABEL_H + PAD)
        files = [f for p in BASECOLOR[source] for f in sorted(glob.glob(os.path.join(RAW, source, name, p.format(s=name))))]
        res = ""
        if files:
            with Image.open(files[0]) as im:
                res = "{}x{}".format(*im.size)
                im = im.convert("RGB")
                im.thumbnail((TILE, TILE), Image.LANCZOS)
                sheet.paste(im, (x, y))
        draw.text((x, y + TILE + 4), name, font=font, fill=(240, 240, 240))
        draw.text((x, y + TILE + 26), "{} {}".format(source, res), font=small, fill=(150, 150, 150))
    out = os.path.join(RAW, "contact_sheet_{}.png".format(cat.lower()))
    sheet.save(out, optimize=True)
    print(out, sheet.size, len(items))
