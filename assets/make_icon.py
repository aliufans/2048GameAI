# -*- coding: utf-8 -*-
"""生成 2048 应用图标：2×2 方块拼成的 {2,0 / 4,8}（和游戏里的方块同款配色）。

用法：
    python make_icon.py            # 需要 Pillow；生成 assets/game2048.ico + 各尺寸预览
之后重新编译资源即可让 exe 用上新图标：
    zig rc app.rc app.res          # 或 windres app.rc -O coff -o app.res
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ICO = os.path.join(HERE, "game2048.ico")

BOARD = (0xBB, 0xAD, 0xA0)      # 背板（和游戏棋盘同色）
FONT_BOLD = r"C:\Windows\Fonts\arialbd.ttf"
FONT_FALLBACK = "DejaVuSans-Bold.ttf"

# 2×2 的四个方块：值 → (底色, 文字色)
CELLS = [
    (2, (0xEE, 0xE4, 0xDA), (0x77, 0x6E, 0x65)),
    (0, (0xED, 0xC2, 0x2E), (0xFF, 0xFF, 0xFF)),
    (4, (0xED, 0xE0, 0xC8), (0x77, 0x6E, 0x65)),
    (8, (0xF2, 0xB1, 0x79), (0xFF, 0xFF, 0xFF)),
]


def load_font(px):
    for path in (FONT_BOLD, FONT_FALLBACK):
        try:
            return ImageFont.truetype(path, px)
        except OSError:
            continue
    return ImageFont.load_default()


def rounded(draw, box, radius, fill):
    draw.rounded_rectangle(box, radius=radius, fill=fill)


def render(size, with_digits=None):
    if with_digits is None:
        with_digits = size >= 32          # 小尺寸画字也是糊，只留色块
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # 背板
    rounded(d, [0, 0, size - 1, size - 1], max(2, int(size * 0.18)), BOARD)

    pad = max(1, int(size * 0.055))
    gap = max(1, int(size * 0.05))
    tile = (size - 2 * pad - gap) // 2
    rad = max(1, int(tile * 0.16))

    for i, (val, bg, fg) in enumerate(CELLS):
        col, row = i % 2, i // 2
        x0 = pad + col * (tile + gap)
        y0 = pad + row * (tile + gap)
        rounded(d, [x0, y0, x0 + tile - 1, y0 + tile - 1], rad, bg)
        if with_digits:
            f = load_font(int(tile * 0.62))
            t = str(val)
            box = f.getbbox(t)
            tw, th = box[2] - box[0], box[3] - box[1]
            d.text((x0 + (tile - tw) / 2 - box[0], y0 + (tile - th) / 2 - box[1]),
                   t, font=f, fill=fg)
    return img


def main():
    sizes = [16, 24, 32, 48, 64, 128, 256]
    imgs = [render(s) for s in sizes]
    imgs[-1].save(ICO, format="ICO", sizes=[(s, s) for s in sizes])
    print("生成图标:", ICO, os.path.getsize(ICO), "字节")

    for s in sizes:
        render(s).save(os.path.join(HERE, "icon-%d.png" % s))

    sheet = Image.new("RGBA", (700, 300), (0xFA, 0xF8, 0xEF, 255))
    x = 20
    for s in sizes:
        ic = Image.open(ICO)
        ic.size = (s, s)
        ic = ic.convert("RGBA")
        sheet.paste(ic, (x, 270 - s), ic)
        x += s + 16
    sheet.save(os.path.join(HERE, "icon-preview.png"))
    print("预览图:", os.path.join(HERE, "icon-preview.png"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
