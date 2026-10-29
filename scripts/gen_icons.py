#!/usr/bin/env python3
"""Quantize res/icons/*.png -> romfs/icons/*.png (PNG-8, ~3KB each)."""
import glob, os, sys
from PIL import Image

src_dir = 'res/icons'
dst_dir = 'romfs/icons'
os.makedirs(dst_dir, exist_ok=True)
n = 0
for p in sorted(glob.glob(src_dir + '/*.png')):
    try:
        im = Image.open(p).convert('RGBA')
    except Exception as e:
        print('skip', p, e); continue
    q = im.quantize(colors=64, method=Image.FASTOCTREE)
    q.save(os.path.join(dst_dir, os.path.basename(p)), optimize=True)
    n += 1
total = sum(os.path.getsize(f) for f in glob.glob(dst_dir + '/*.png'))
print(f"{n} icons, {total/1e6:.1f} MB")
