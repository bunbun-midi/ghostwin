#!/usr/bin/env python3
"""Pack the RGBA renders from make_icon.exe into res/ghostwin.ico (each size
drawn natively, not scaled) and a preview PNG. Needs Pillow."""
import sys
from PIL import Image

src, ico, preview = sys.argv[1], sys.argv[2], sys.argv[3]
sizes = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
imgs = [Image.frombytes("RGBA", (s, s), open(f"{src}/icon-{s}.rgba", "rb").read()) for s in sizes]
big = imgs[-1]
big.save(ico, format="ICO", sizes=[(s, s) for s in sizes], append_images=imgs[:-1])

# preview: every size on a dark and a light strip
w = sum(sizes) + 12 * (len(sizes) + 1)
sheet = Image.new("RGBA", (w, (256 + 24) * 2), (0, 0, 0, 0))
for row, bg in enumerate([(32, 32, 36, 255), (243, 243, 243, 255)]):
    y0 = row * (256 + 24)
    sheet.paste(Image.new("RGBA", (w, 256 + 24), bg), (0, y0))
    x = 12
    for im in imgs:
        sheet.alpha_composite(im, (x, y0 + 12 + 256 - im.size[1]))
        x += im.size[0] + 12
sheet.save(preview)
