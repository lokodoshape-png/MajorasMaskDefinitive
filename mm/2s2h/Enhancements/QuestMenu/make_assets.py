#!/usr/bin/env python3
"""Draws the quest menu's PNG art into mm/assets/custom/overlays/questmenu/.

Everything here is original art made from simple shapes and noise, so nothing from the game or other packs is
redistributed. The files end up in 2ship.o2r under overlays/questmenu/. Run it again after changing a drawing:
    python3 mm/2s2h/Enhancements/QuestMenu/make_assets.py
"""

import math
import os
import random

from PIL import Image, ImageDraw, ImageFilter

OUT = os.path.join(os.path.dirname(__file__), "..", "..", "..", "assets", "custom", "overlays", "questmenu")
SS = 4  # supersampling

GOLD = (244, 204, 98)
GOLD_DARK = (150, 105, 40)
GOLD_LIGHT = (255, 240, 180)
OUTLINE = (40, 20, 10)
PURPLE_DEEP = (28, 10, 44)
PURPLE = (58, 26, 86)
PURPLE_LIGHT = (110, 60, 140)
WINE = (104, 30, 48)


def canvas(w, h):
    return Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))


def finish(img, name):
    w, h = img.size
    img = img.resize((w // SS, h // SS), Image.LANCZOS)
    img.save(os.path.join(OUT, name), optimize=True)


def s(v):
    return int(round(v * SS))


def gold_band(d, box, width, radius):
    x0, y0, x1, y1 = box
    d.rounded_rectangle([s(x0), s(y0), s(x1), s(y1)], s(radius), outline=OUTLINE + (255,), width=s(width + 3))
    d.rounded_rectangle([s(x0 + 1), s(y0 + 1), s(x1 - 1), s(y1 - 1)], s(radius), outline=GOLD_DARK + (255,),
                        width=s(width + 1))
    d.rounded_rectangle([s(x0 + 1.5), s(y0 + 1.5), s(x1 - 1.5), s(y1 - 1.5)], s(radius), outline=GOLD + (255,),
                        width=s(width - 1))
    hi = width * 0.35
    d.rounded_rectangle([s(x0 + hi), s(y0 + hi), s(x1 - hi), s(y1 - hi)], s(radius), outline=GOLD_LIGHT + (200,),
                        width=s(1.2))


def diamond(d, cx, cy, r, fill, inner=None):
    d.polygon([(s(cx), s(cy - r)), (s(cx + r), s(cy)), (s(cx), s(cy + r)), (s(cx - r), s(cy))], fill=fill,
              outline=OUTLINE + (255,))
    if inner:
        r2 = r * 0.45
        d.polygon([(s(cx), s(cy - r2)), (s(cx + r2), s(cy)), (s(cx), s(cy + r2)), (s(cx - r2), s(cy))], fill=inner)


def curl(d, cx, cy, r, start, end, width=2.0, col=GOLD):
    d.arc([s(cx - r), s(cy - r), s(cx + r), s(cy + r)], start, end, fill=OUTLINE + (255,), width=s(width + 1.6))
    d.arc([s(cx - r), s(cy - r), s(cx + r), s(cy + r)], start, end, fill=col + (255,), width=s(width))


def frame():
    # 192x192, sliced 64 px on each side: an ornate gold border with filigree corners.
    W = 192
    img = canvas(W, W)
    d = ImageDraw.Draw(img)
    gold_band(d, (10, 10, W - 10, W - 10), 9, 14)
    d.rounded_rectangle([s(22), s(22), s(W - 22), s(W - 22)], s(8), outline=GOLD_DARK + (220,), width=s(1.5))
    for (cx, cy, a) in ((14, 14, 0), (W - 14, 14, 90), (W - 14, W - 14, 180), (14, W - 14, 270)):
        diamond(d, cx, cy, 11, GOLD + (255,), PURPLE_LIGHT + (255,))
        # two curls running along each edge from the corner
        dx = 1 if cx < W / 2 else -1
        dy = 1 if cy < W / 2 else -1
        curl(d, cx + dx * 26, cy + dy * 4, 8, 0, 360, 1.6)
        curl(d, cx + dx * 4, cy + dy * 26, 8, 0, 360, 1.6)
        d.ellipse([s(cx + dx * 26 - 2.5), s(cy + dy * 4 - 2.5), s(cx + dx * 26 + 2.5), s(cy + dy * 4 + 2.5)],
                  fill=GOLD_LIGHT + (255,))
        d.ellipse([s(cx + dx * 4 - 2.5), s(cy + dy * 26 - 2.5), s(cx + dx * 4 + 2.5), s(cy + dy * 26 + 2.5)],
                  fill=GOLD_LIGHT + (255,))
    # small studs at the middle of each edge
    for (cx, cy) in ((W / 2, 14), (W / 2, W - 14), (14, W / 2), (W - 14, W / 2)):
        diamond(d, cx, cy, 6, GOLD + (255,), WINE + (255,))
    finish(img, "frame.png")


def button(name, top, bottom, glow):
    # 96x96, sliced 28 px.
    W = 96
    img = canvas(W, W)
    if glow:
        g = Image.new("RGBA", img.size, (0, 0, 0, 0))
        gd = ImageDraw.Draw(g)
        gd.rounded_rectangle([s(6), s(6), s(W - 6), s(W - 6)], s(16), outline=(255, 210, 90, 200), width=s(6))
        g = g.filter(ImageFilter.GaussianBlur(s(4)))
        img.alpha_composite(g)
    fill = Image.new("RGBA", img.size, (0, 0, 0, 0))
    grad = Image.new("RGBA", img.size)
    gp = grad.load()
    for y in range(img.size[1]):
        t = y / img.size[1]
        c = tuple(int(top[i] * (1 - t) + bottom[i] * t) for i in range(3)) + (255,)
        for x in range(img.size[0]):
            gp[x, y] = c
    mask = Image.new("L", img.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle([s(10), s(10), s(W - 10), s(W - 10)], s(12), fill=255)
    fill.paste(grad, (0, 0), mask)
    img.alpha_composite(fill)
    d = ImageDraw.Draw(img)
    if glow:
        gold_band(d, (10, 10, W - 10, W - 10), 4.5, 12)
    else:
        d.rounded_rectangle([s(10), s(10), s(W - 10), s(W - 10)], s(12), outline=(170, 120, 190, 200), width=s(2))
    # top sheen
    d.rounded_rectangle([s(16), s(14), s(W - 16), s(30)], s(6), fill=(255, 255, 255, 22))
    finish(img, name)


def noise_layer(size, scale, seed):
    rnd = random.Random(seed)
    small = Image.new("L", (max(1, size[0] // scale), max(1, size[1] // scale)))
    small.putdata([rnd.randint(0, 255) for _ in range(small.size[0] * small.size[1])])
    return small.resize(size, Image.BICUBIC)


def parchment():
    W, H = 512, 512
    base = Image.new("RGB", (W, H), (240, 226, 196))
    for scale, amount, seed in ((64, 26, 1), (16, 14, 2), (4, 8, 3)):
        n = noise_layer((W, H), scale, seed)
        dark = Image.new("RGB", (W, H), (150, 110, 60))
        base = Image.composite(dark, base, n.point(lambda v, a=amount: int(v * a / 255)))
    # fibres
    d = ImageDraw.Draw(base)
    rnd = random.Random(9)
    for _ in range(260):
        x, y = rnd.uniform(0, W), rnd.uniform(0, H)
        ang = rnd.uniform(-0.4, 0.4)
        ln = rnd.uniform(8, 40)
        d.line([(x, y), (x + math.cos(ang) * ln, y + math.sin(ang) * ln)], fill=(205, 182, 140), width=1)
    # burnt, uneven edges
    edge = Image.new("L", (W, H), 0)
    ed = ImageDraw.Draw(edge)
    for i in range(40):
        ed.rectangle([i, i, W - 1 - i, H - 1 - i], outline=int(255 * (1 - i / 40) ** 2))
    n = noise_layer((W, H), 8, 5)
    edge = Image.composite(edge, Image.new("L", (W, H), 0), n.point(lambda v: 255 if v > 60 else 140))
    brown = Image.new("RGB", (W, H), (120, 72, 30))
    base = Image.composite(brown, base, edge.point(lambda v: int(v * 0.8)))
    img = base.convert("RGBA")
    a = Image.new("L", (W, H), 255)
    ad = ImageDraw.Draw(a)
    rnd = random.Random(11)
    for i in range(0, W, 3):
        for (x, y) in ((i, 0), (i, H - 1), (0, i), (W - 1, i)):
            r = rnd.uniform(1, 4)
            ad.ellipse([x - r, y - r, x + r, y + r], fill=0)
    img.putalpha(a)
    img.save(os.path.join(OUT, "parchment.png"), optimize=True)


def background():
    # 256x256, tiles: deep purple with a faint gold lattice.
    W = 256
    img = Image.new("RGB", (W, W), PURPLE_DEEP)
    n = noise_layer((W, W), 16, 21)
    lighter = Image.new("RGB", (W, W), (52, 22, 72))
    img = Image.composite(lighter, img, n.point(lambda v: int(v * 0.5)))
    big = img.resize((W * SS, W * SS), Image.BICUBIC).convert("RGBA")
    d = ImageDraw.Draw(big)
    step = 64
    for k in range(-W, 2 * W, step):
        d.line([(s(k), s(0)), (s(k + W), s(W))], fill=(244, 204, 98, 26), width=s(1.2))
        d.line([(s(k), s(W)), (s(k + W), s(0))], fill=(244, 204, 98, 26), width=s(1.2))
    for x in range(0, W + 1, step):
        for y in range(0, W + 1, step):
            for (cx, cy) in ((x, y), (x + step / 2, y + step / 2)):
                r = 3
                d.polygon([(s(cx), s(cy - r)), (s(cx + r), s(cy)), (s(cx), s(cy + r)), (s(cx - r), s(cy))],
                          fill=(244, 204, 98, 40))
    big.resize((W, W), Image.LANCZOS).save(os.path.join(OUT, "background.png"), optimize=True)


def banner():
    W, H = 768, 96
    img = canvas(W, H)
    d = ImageDraw.Draw(img)
    pts = [(40, 12), (W - 40, 12), (W - 6, 48), (W - 40, 84), (40, 84), (6, 48)]
    d.polygon([(s(x), s(y)) for x, y in pts], fill=PURPLE + (255,))
    d.polygon([(s(x), s(y)) for x, y in [(42, 14), (W - 42, 14), (W - 22, 36), (22, 36)]], fill=(110, 50, 130, 255))
    d.line([(s(x), s(y)) for x, y in pts + [pts[0]]], fill=OUTLINE + (255,), width=s(7), joint="curve")
    d.line([(s(x), s(y)) for x, y in pts + [pts[0]]], fill=GOLD + (255,), width=s(4), joint="curve")
    for side in (-1, 1):
        cx = W / 2 + side * (W / 2 - 80)
        curl(d, cx, 48, 12, 0, 360, 2.4)
        curl(d, cx - side * 22, 48, 7, 0, 360, 2.0)
        diamond(d, cx + side * 30, 48, 7, GOLD + (255,), WINE + (255,))
    finish(img, "banner.png")


def divider():
    W, H = 512, 24
    img = canvas(W, H)
    d = ImageDraw.Draw(img)
    for x0, x1 in ((16, W / 2 - 22), (W / 2 + 22, W - 16)):
        d.line([(s(x0), s(12)), (s(x1), s(12))], fill=GOLD_DARK + (255,), width=s(3))
        d.line([(s(x0), s(11.5)), (s(x1), s(11.5))], fill=GOLD + (255,), width=s(1.4))
    diamond(d, W / 2, 12, 9, GOLD + (255,), WINE + (255,))
    diamond(d, W / 2 - 30, 12, 4, GOLD + (255,))
    diamond(d, W / 2 + 30, 12, 4, GOLD + (255,))
    finish(img, "divider.png")


def corner():
    W = 96
    img = canvas(W, W)
    d = ImageDraw.Draw(img)
    curl(d, 30, 30, 18, 180, 450, 3)
    curl(d, 54, 22, 9, 90, 360, 2.2)
    curl(d, 22, 54, 9, 0, 270, 2.2)
    d.line([(s(30), s(12)), (s(88), s(12))], fill=GOLD + (255,), width=s(2.4))
    d.line([(s(12), s(30)), (s(12), s(88))], fill=GOLD + (255,), width=s(2.4))
    diamond(d, 30, 30, 8, GOLD + (255,), PURPLE_LIGHT + (255,))
    finish(img, "corner.png")


# ----- icons, 128x128

def icon_canvas():
    return canvas(128, 128)


def poly(d, pts, fill, width=3):
    d.polygon([(s(x), s(y)) for x, y in pts], fill=fill, outline=OUTLINE + (255,))
    d.line([(s(x), s(y)) for x, y in pts + [pts[0]]], fill=OUTLINE + (255,), width=s(width), joint="curve")


def icon_main():
    img = icon_canvas()
    d = ImageDraw.Draw(img)
    for (x, y) in ((64, 14), (34, 66), (94, 66)):
        poly(d, [(x, y), (x + 30, y + 50), (x - 30, y + 50)], GOLD + (255,))
        poly(d, [(x, y + 10), (x + 18, y + 40), (x - 18, y + 40)], (255, 228, 140, 255), 1)
    finish(img, "cat_main.png")


def icon_side():
    img = icon_canvas()
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([s(26), s(22), s(102), s(106)], s(6), fill=(240, 226, 196, 255), outline=OUTLINE + (255,),
                        width=s(3))
    for cy in (22, 106):
        d.ellipse([s(18), s(cy - 10), s(110), s(cy + 10)], fill=(214, 186, 138, 255), outline=OUTLINE + (255,),
                  width=s(3))
    for y in (44, 58, 72, 86):
        d.line([(s(38), s(y)), (s(90), s(y))], fill=(120, 80, 50, 255), width=s(3))
    diamond(d, 92, 92, 10, WINE + (255,))
    finish(img, "cat_side.png")


def icon_masks():
    img = icon_canvas()
    d = ImageDraw.Draw(img)
    # heart-shaped mask with spikes
    for i in range(9):
        a = math.radians(200 + i * 17.5)
        x, y = 64 + math.cos(a) * 50, 60 + math.sin(a) * 46
        poly(d, [(64 + math.cos(a - 0.12) * 34, 60 + math.sin(a - 0.12) * 30), (x, y),
                 (64 + math.cos(a + 0.12) * 34, 60 + math.sin(a + 0.12) * 30)], (250, 190, 60, 255), 2)
    d.ellipse([s(24), s(30), s(104), s(102)], fill=(150, 60, 170, 255), outline=OUTLINE + (255,), width=s(3))
    poly(d, [(64, 110), (34, 82), (94, 82)], (150, 60, 170, 255), 2)
    for ex in (46, 82):
        d.ellipse([s(ex - 13), s(52), s(ex + 13), s(78)], fill=(250, 190, 60, 255), outline=OUTLINE + (255,),
                  width=s(2))
        d.ellipse([s(ex - 6), s(58), s(ex + 6), s(72)], fill=(60, 170, 60, 255))
        d.ellipse([s(ex - 2.5), s(62), s(ex + 2.5), s(68)], fill=(20, 20, 20, 255))
    finish(img, "cat_masks.png")


def icon_people():
    img = icon_canvas()
    d = ImageDraw.Draw(img)
    for (cx, col) in ((46, (214, 170, 120)), (82, (238, 196, 140))):
        d.ellipse([s(cx - 16), s(26), s(cx + 16), s(58)], fill=col + (255,), outline=OUTLINE + (255,), width=s(3))
        d.pieslice([s(cx - 30), s(60), s(cx + 30), s(130)], 180, 360, fill=PURPLE_LIGHT + (255,),
                   outline=OUTLINE + (255,), width=s(3))
    finish(img, "cat_people.png")


def icon_places():
    img = icon_canvas()
    d = ImageDraw.Draw(img)
    # owl statue
    poly(d, [(30, 110), (98, 110), (88, 40), (64, 18), (40, 40)], (170, 160, 150, 255))
    poly(d, [(40, 40), (52, 22), (56, 38)], (150, 140, 130, 255), 2)
    poly(d, [(88, 40), (76, 22), (72, 38)], (150, 140, 130, 255), 2)
    for ex in (52, 76):
        d.ellipse([s(ex - 11), s(42), s(ex + 11), s(64)], fill=(250, 220, 120, 255), outline=OUTLINE + (255,),
                  width=s(2))
        d.ellipse([s(ex - 4), s(49), s(ex + 4), s(57)], fill=(30, 20, 20, 255))
    poly(d, [(64, 62), (70, 72), (64, 80), (58, 72)], (230, 170, 70, 255), 2)
    for y in (88, 98):
        d.arc([s(46), s(y - 8), s(82), s(y + 8)], 20, 160, fill=(120, 110, 100, 255), width=s(3))
    finish(img, "cat_places.png")


def icon_items():
    img = icon_canvas()
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([s(18), s(58), s(110), s(108)], s(4), fill=(150, 90, 40, 255), outline=OUTLINE + (255,),
                        width=s(3))
    d.pieslice([s(18), s(28), s(110), s(88)], 180, 360, fill=(170, 105, 48, 255), outline=OUTLINE + (255,),
               width=s(3))
    for x in (34, 94):
        d.rectangle([s(x - 5), s(42), s(x + 5), s(108)], fill=GOLD + (255,), outline=OUTLINE + (255,), width=s(2))
    d.rectangle([s(18), s(56), s(110), s(64)], fill=GOLD + (255,), outline=OUTLINE + (255,), width=s(2))
    d.rounded_rectangle([s(54), s(56), s(74), s(80)], s(3), fill=GOLD_LIGHT + (255,), outline=OUTLINE + (255,),
                        width=s(2))
    finish(img, "cat_items.png")


def icon_records():
    img = icon_canvas()
    d = ImageDraw.Draw(img)
    poly(d, [(64, 34), (110, 24), (110, 100), (64, 110)], (240, 226, 196, 255))
    poly(d, [(64, 34), (18, 24), (18, 100), (64, 110)], (226, 208, 170, 255))
    for i in range(4):
        y = 44 + i * 14
        d.line([(s(28), s(y - 2)), (s(56), s(y + 4))], fill=(120, 80, 50, 255), width=s(2.4))
    # a music note on the right page
    d.ellipse([s(76), s(78), s(92), s(92)], fill=(110, 50, 160, 255))
    d.line([(s(90), s(84)), (s(90), s(46))], fill=(110, 50, 160, 255), width=s(4))
    d.line([(s(90), s(46)), (s(102), s(54))], fill=(110, 50, 160, 255), width=s(4))
    finish(img, "cat_records.png")


def state_icons():
    # locked: padlock
    img = canvas(64, 64)
    d = ImageDraw.Draw(img)
    d.arc([s(18), s(8), s(46), s(40)], 180, 360, fill=OUTLINE + (255,), width=s(9))
    d.arc([s(18), s(8), s(46), s(40)], 180, 360, fill=(170, 170, 180, 255), width=s(5))
    d.line([(s(20), s(24)), (s(20), s(30))], fill=(170, 170, 180, 255), width=s(5))
    d.line([(s(44), s(24)), (s(44), s(30))], fill=(170, 170, 180, 255), width=s(5))
    d.rounded_rectangle([s(12), s(28), s(52), s(58)], s(5), fill=GOLD + (255,), outline=OUTLINE + (255,),
                        width=s(3))
    d.ellipse([s(28), s(36), s(36), s(44)], fill=OUTLINE + (255,))
    d.line([(s(32), s(42)), (s(32), s(50))], fill=OUTLINE + (255,), width=s(3))
    finish(img, "state_locked.png")

    # available: gold exclamation in a circle
    img = canvas(64, 64)
    d = ImageDraw.Draw(img)
    d.ellipse([s(6), s(6), s(58), s(58)], fill=(70, 30, 100, 255), outline=GOLD + (255,), width=s(4))
    d.rounded_rectangle([s(28), s(14), s(36), s(40)], s(3), fill=GOLD_LIGHT + (255,))
    d.ellipse([s(27), s(44), s(37), s(54)], fill=GOLD_LIGHT + (255,))
    finish(img, "state_available.png")

    # in progress: hourglass
    img = canvas(64, 64)
    d = ImageDraw.Draw(img)
    d.ellipse([s(6), s(6), s(58), s(58)], fill=(40, 60, 110, 255), outline=GOLD + (255,), width=s(4))
    poly(d, [(20, 16), (44, 16), (32, 32)], (240, 226, 196, 255), 2)
    poly(d, [(32, 32), (44, 48), (20, 48)], (240, 226, 196, 255), 2)
    poly(d, [(25, 44), (39, 44), (32, 36)], (230, 180, 70, 255), 1)
    d.line([(s(18), s(15)), (s(46), s(15))], fill=GOLD + (255,), width=s(3))
    d.line([(s(18), s(49)), (s(46), s(49))], fill=GOLD + (255,), width=s(3))
    finish(img, "state_progress.png")

    # done: green check on a gold seal
    img = canvas(64, 64)
    d = ImageDraw.Draw(img)
    pts = []
    for i in range(24):
        a = math.radians(i * 15)
        r = 28 if i % 2 == 0 else 24
        pts.append((32 + math.cos(a) * r, 32 + math.sin(a) * r))
    poly(d, pts, GOLD + (255,), 2)
    d.ellipse([s(13), s(13), s(51), s(51)], fill=(60, 150, 60, 255), outline=OUTLINE + (255,), width=s(2))
    d.line([(s(21), s(33)), (s(29), s(42)), (s(44), s(22))], fill=(20, 60, 20, 255), width=s(9), joint="curve")
    d.line([(s(21), s(33)), (s(29), s(42)), (s(44), s(22))], fill=(230, 255, 220, 255), width=s(5), joint="curve")
    finish(img, "state_done.png")

    # objective marks: empty / ticked boxes
    for done in (False, True):
        img = canvas(48, 48)
        d = ImageDraw.Draw(img)
        d.rounded_rectangle([s(6), s(6), s(42), s(42)], s(6), fill=(255, 248, 230, 255), outline=(110, 70, 40, 255),
                            width=s(3))
        if done:
            d.line([(s(12), s(25)), (s(21), s(34)), (s(38), s(12))], fill=(20, 70, 20, 255), width=s(8),
                   joint="curve")
            d.line([(s(12), s(25)), (s(21), s(34)), (s(38), s(12))], fill=(84, 214, 70, 255), width=s(4),
                   joint="curve")
        finish(img, "mark_done.png" if done else "mark_todo.png")


# ----- book layout (closer to the concept art): a carved purple frame, an open book, plaques and tiles.

STONE_DARK = (34, 14, 50)
STONE = (66, 30, 92)
STONE_LIGHT = (104, 58, 136)


def vgradient(size, top, bottom):
    w, h = size
    g = Image.new("RGBA", size)
    gp = g.load()
    for y in range(h):
        t = y / max(1, h - 1)
        c = tuple(int(top[i] * (1 - t) + bottom[i] * t) for i in range(3)) + (255,)
        for x in range(w):
            gp[x, y] = c
    return g


def stone_fill(size, seed, base=STONE, light=STONE_LIGHT, dark=STONE_DARK):
    w, h = size
    img = Image.new("RGB", size, base)
    img = Image.composite(Image.new("RGB", size, light), img, noise_layer(size, 24 * SS, seed).point(lambda v: int(v * 0.45)))
    img = Image.composite(Image.new("RGB", size, dark), img, noise_layer(size, 6 * SS, seed + 1).point(lambda v: int(v * 0.35)))
    return img.convert("RGBA")


def zigzag_band(d, x0, x1, y, h, col):
    # A carved band of chevrons and diamonds, like the tribal patterns on the frame of the concept.
    step = h * 1.6
    x = x0
    while x < x1 - step:
        d.line([(s(x), s(y + h)), (s(x + step / 2), s(y)), (s(x + step), s(y + h))], fill=col, width=s(2.2))
        diamond(d, x + step / 2, y + h * 0.62, h * 0.22, col)
        x += step


def book_frame():
    # 256x256, sliced 88 px: thick carved purple stone with gold trims and a chevron band; the middle is empty.
    W = 256
    B = 70
    img = canvas(W, W)
    stone = stone_fill(img.size, 31)
    mask = Image.new("L", img.size, 0)
    md = ImageDraw.Draw(mask)
    md.rounded_rectangle([s(4), s(4), s(W - 4), s(W - 4)], s(26), fill=255)
    md.rounded_rectangle([s(B), s(B), s(W - B), s(W - B)], s(10), fill=0)
    img.paste(stone, (0, 0), mask)
    d = ImageDraw.Draw(img)
    # bevel: light top-left, dark bottom-right
    d.rounded_rectangle([s(6), s(6), s(W - 6), s(W - 6)], s(24), outline=(150, 100, 180, 160), width=s(2))
    # chevron band in the middle of the border, top and bottom (repeated by the slicing)
    band = Image.new("RGBA", img.size, (0, 0, 0, 0))
    bd = ImageDraw.Draw(band)
    for y in (24, W - 46):
        zigzag_band(bd, 30, W - 30, y, 20, (30, 10, 44, 255))
    for x in (24, W - 46):
        rot = Image.new("RGBA", (img.size[1], img.size[0]), (0, 0, 0, 0))
        rd = ImageDraw.Draw(rot)
        zigzag_band(rd, 30, W - 30, x, 20, (30, 10, 44, 255))
        band.alpha_composite(rot.transpose(Image.Transpose.ROTATE_90).transpose(Image.Transpose.FLIP_TOP_BOTTOM))
    band = band.filter(ImageFilter.GaussianBlur(s(0.4)))
    hi = band.point(lambda v: v)
    img.alpha_composite(band)
    # gold trims
    gold_band(d, (4, 4, W - 4, W - 4), 5, 26)
    gold_band(d, (B - 6, B - 6, W - B + 6, W - B + 6), 5, 12)
    # corner medallions
    for cx, cy in ((22, 22), (W - 22, 22), (22, W - 22), (W - 22, W - 22)):
        d.ellipse([s(cx - 15), s(cy - 15), s(cx + 15), s(cy + 15)], fill=OUTLINE + (255,))
        d.ellipse([s(cx - 13), s(cy - 13), s(cx + 13), s(cy + 13)], fill=GOLD_DARK + (255,))
        d.ellipse([s(cx - 11), s(cy - 11), s(cx + 11), s(cy + 11)], fill=GOLD + (255,))
        diamond(d, cx, cy, 8, WINE + (255,), GOLD_LIGHT + (255,))
    finish(img, "book_frame.png")


def page_left():
    # Dark purple page with soft texture and faint corner ornaments.
    W, H = 512, 512
    base = stone_fill((W, H), 41, base=(54, 22, 74), light=(78, 38, 104), dark=(30, 10, 44))
    vign = Image.new("L", (W, H), 0)
    vd = ImageDraw.Draw(vign)
    for i in range(60):
        vd.rectangle([i, i, W - 1 - i, H - 1 - i], outline=int(150 * (1 - i / 60) ** 2))
    base = Image.composite(Image.new("RGBA", (W, H), (14, 4, 22, 255)), base, vign)
    img = base.resize((W * SS, H * SS), Image.BICUBIC)
    d = ImageDraw.Draw(img)
    for cx, cy, a0 in ((34, 34, 180), (W - 34, 34, 270), (34, H - 34, 90), (W - 34, H - 34, 0)):
        curl(d, cx, cy, 14, a0, a0 + 270, 1.6, col=(150, 105, 40))
    finish(img, "page_left.png")


def page_right():
    # Parchment with the gutter shadow on its left edge, as a right-hand page.
    src = Image.open(os.path.join(OUT, "parchment.png")).convert("RGBA")
    W, H = src.size
    shade = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    sp = shade.load()
    for x in range(70):
        a = int(150 * (1 - x / 70) ** 2)
        for y in range(H):
            sp[x, y] = (60, 30, 10, a)
    src.alpha_composite(shade)
    src.save(os.path.join(OUT, "page_right.png"), optimize=True)


def spine():
    W, H = 48, 512
    img = canvas(W, H)
    g = Image.new("RGBA", (s(W), s(H)))
    gp = g.load()
    for x in range(s(W)):
        t = abs(x / s(W) - 0.5) * 2
        c = (int(20 + 50 * (1 - t)), int(8 + 20 * (1 - t)), int(30 + 50 * (1 - t)), 255)
        for y in range(s(H)):
            gp[x, y] = c
    img.alpha_composite(g)
    d = ImageDraw.Draw(img)
    for y in range(40, H, 108):
        d.rounded_rectangle([s(10), s(y), s(W - 10), s(y + 16)], s(6), fill=GOLD_DARK + (255,), outline=OUTLINE + (255,), width=s(1.5))
        d.line([(s(13), s(y + 5)), (s(W - 13), s(y + 5))], fill=GOLD_LIGHT + (200,), width=s(1.5))
    finish(img, "spine.png")


def title_plaque():
    W, H = 640, 112
    img = canvas(W, H)
    d = ImageDraw.Draw(img)
    pts = [(70, 20), (W - 70, 20), (W - 14, 56), (W - 70, 92), (70, 92), (14, 56)]
    fill = vgradient((s(W), s(H)), (118, 52, 136), (54, 18, 70))
    mask = Image.new("L", img.size, 0)
    ImageDraw.Draw(mask).polygon([(s(x), s(y)) for x, y in pts], fill=255)
    img.paste(fill, (0, 0), mask)
    d.line([(s(x), s(y)) for x, y in pts + [pts[0]]], fill=OUTLINE + (255,), width=s(9), joint="curve")
    d.line([(s(x), s(y)) for x, y in pts + [pts[0]]], fill=GOLD_DARK + (255,), width=s(6), joint="curve")
    d.line([(s(x), s(y)) for x, y in pts + [pts[0]]], fill=GOLD + (255,), width=s(3), joint="curve")
    inner = [(78, 28), (W - 78, 28), (W - 28, 56), (W - 78, 84), (78, 84), (28, 56)]
    d.line([(s(x), s(y)) for x, y in inner + [inner[0]]], fill=(244, 204, 98, 120), width=s(1.4), joint="curve")
    for side in (-1, 1):
        cx = W / 2 + side * (W / 2 - 60)
        curl(d, cx, 40, 10, 0, 360, 2.2)
        curl(d, cx, 72, 10, 0, 360, 2.2)
        curl(d, cx + side * 18, 56, 6, 0, 360, 1.8)
    finish(img, "title_plaque.png")


def emblem():
    # A horned heart-shaped mask with big eyes, drawn from shapes: the crest over the title.
    W, H = 200, 150
    img = canvas(W, H)
    d = ImageDraw.Draw(img)
    cx, cy = W / 2, 84
    spikes = [(-1.0, -0.15), (-0.92, 0.25), (-0.7, 0.62), (-0.35, 0.86), (0.35, 0.86), (0.7, 0.62), (0.92, 0.25), (1.0, -0.15),
              (-0.42, -1.0), (0.42, -1.0)]
    for sx, sy in spikes:
        ang = math.atan2(sy, sx)
        bx, by = cx + sx * 40, cy + sy * 34
        tip = (cx + sx * 40 + math.cos(ang) * 30, cy + sy * 34 + math.sin(ang) * 30)
        perp = (-math.sin(ang) * 8, math.cos(ang) * 8)
        col = (90, 170, 60) if sy > 0.5 else (240, 200, 90)
        poly(d, [(bx + perp[0], by + perp[1]), tip, (bx - perp[0], by - perp[1])], col + (255,), 2)
    # heart-shaped face
    face = [(cx, cy + 42)]
    for i in range(0, 181, 6):
        a = math.radians(i)
        face.append((cx - 46 * math.sin(a) - 6 * math.sin(2 * a), cy - 30 * math.cos(a) * 0.9 - 8))
    face2 = [(2 * cx - x, y) for x, y in reversed(face[1:])]
    poly(d, face + face2, (120, 40, 140, 255), 3)
    d.polygon([(s(cx - 30), s(cy - 4)), (s(cx), s(cy + 34)), (s(cx + 30), s(cy - 4)), (s(cx), s(cy + 10))], fill=(190, 40, 50, 255))
    for side in (-1, 1):
        ex = cx + side * 20
        d.ellipse([s(ex - 15), s(cy - 26), s(ex + 15), s(cy + 4)], fill=OUTLINE + (255,))
        d.ellipse([s(ex - 13), s(cy - 24), s(ex + 13), s(cy + 2)], fill=(250, 150, 40, 255))
        d.ellipse([s(ex - 8), s(cy - 19), s(ex + 8), s(cy - 3)], fill=(255, 220, 60, 255))
        d.ellipse([s(ex - 4.5), s(cy - 15.5), s(ex + 4.5), s(cy - 6.5)], fill=(90, 200, 70, 255))
        d.ellipse([s(ex - 2), s(cy - 13), s(ex + 2), s(cy - 9)], fill=(10, 10, 10, 255))
    d.line([(s(cx), s(cy - 34)), (s(cx), s(cy + 8))], fill=OUTLINE + (255,), width=s(2))
    finish(img, "emblem.png")


def keycap():
    W = 64
    img = canvas(W, W)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([s(4), s(8), s(W - 4), s(W - 2)], s(10), fill=(40, 34, 48, 255))
    fill = vgradient((s(W), s(W)), (196, 190, 206), (120, 112, 134))
    mask = Image.new("L", img.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle([s(6), s(4), s(W - 6), s(W - 10)], s(9), fill=255)
    img.paste(fill, (0, 0), mask)
    d.rounded_rectangle([s(6), s(4), s(W - 6), s(W - 10)], s(9), outline=(60, 50, 70, 255), width=s(2))
    d.rounded_rectangle([s(10), s(7), s(W - 10), s(16)], s(4), fill=(255, 255, 255, 60))
    finish(img, "keycap.png")


def hint_plaque():
    W, H = 760, 96
    img = canvas(W, H)
    d = ImageDraw.Draw(img)
    pts = [(46, 10), (W - 46, 10), (W - 8, 48), (W - 46, 86), (46, 86), (8, 48)]
    fill = vgradient((s(W), s(H)), (78, 34, 98), (40, 14, 56))
    mask = Image.new("L", img.size, 0)
    ImageDraw.Draw(mask).polygon([(s(x), s(y)) for x, y in pts], fill=255)
    img.paste(fill, (0, 0), mask)
    d.line([(s(x), s(y)) for x, y in pts + [pts[0]]], fill=OUTLINE + (255,), width=s(7), joint="curve")
    d.line([(s(x), s(y)) for x, y in pts + [pts[0]]], fill=GOLD + (255,), width=s(3.4), joint="curve")
    for cx in (66, W - 66):
        t = 18
        for ox, oy in ((0, -t * 0.5), (-t * 0.5, t * 0.37), (t * 0.5, t * 0.37)):
            x, y = cx + ox, 50 + oy
            poly(d, [(x, y - t * 0.43), (x + t * 0.5, y + t * 0.43), (x - t * 0.5, y + t * 0.43)], GOLD + (255,), 1.5)
    finish(img, "hint_plaque.png")


def tile(name, current):
    W = 96
    img = canvas(W, W)
    if current:
        g = Image.new("RGBA", img.size, (0, 0, 0, 0))
        ImageDraw.Draw(g).rounded_rectangle([s(6), s(6), s(W - 6), s(W - 6)], s(18), outline=(255, 210, 90, 230), width=s(7))
        img.alpha_composite(g.filter(ImageFilter.GaussianBlur(s(3.5))))
    top, bottom = ((150, 80, 170), (70, 26, 92)) if current else ((236, 214, 168), (196, 160, 104))
    fill = vgradient(img.size, top, bottom)
    mask = Image.new("L", img.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle([s(10), s(10), s(W - 10), s(W - 10)], s(14), fill=255)
    img.paste(fill, (0, 0), mask)
    d = ImageDraw.Draw(img)
    gold_band(d, (10, 10, W - 10, W - 10), 4 if current else 3, 14)
    finish(img, name)


def portrait_ring():
    W = 160
    img = canvas(W, W)
    d = ImageDraw.Draw(img)
    d.ellipse([s(6), s(6), s(W - 6), s(W - 6)], outline=OUTLINE + (255,), width=s(12))
    d.ellipse([s(8), s(8), s(W - 8), s(W - 8)], outline=GOLD_DARK + (255,), width=s(9))
    d.ellipse([s(10), s(10), s(W - 10), s(W - 10)], outline=GOLD + (255,), width=s(5))
    d.ellipse([s(12), s(12), s(W - 12), s(W - 12)], outline=GOLD_LIGHT + (200,), width=s(1.4))
    for a in range(0, 360, 90):
        x = W / 2 + math.cos(math.radians(a + 45)) * (W / 2 - 10)
        y = W / 2 + math.sin(math.radians(a + 45)) * (W / 2 - 10)
        diamond(d, x, y, 7, GOLD + (255,), WINE + (255,))
    finish(img, "portrait_ring.png")


def clock_box():
    W, H = 280, 60
    img = canvas(W, H)
    d = ImageDraw.Draw(img)
    pts = [(24, 6), (W - 24, 6), (W - 6, 30), (W - 24, 54), (24, 54), (6, 30)]
    fill = vgradient((s(W), s(H)), (70, 40, 110), (28, 14, 60))
    mask = Image.new("L", img.size, 0)
    ImageDraw.Draw(mask).polygon([(s(x), s(y)) for x, y in pts], fill=255)
    img.paste(fill, (0, 0), mask)
    d.line([(s(x), s(y)) for x, y in pts + [pts[0]]], fill=OUTLINE + (255,), width=s(6), joint="curve")
    d.line([(s(x), s(y)) for x, y in pts + [pts[0]]], fill=(170, 150, 220, 255), width=s(2.6), joint="curve")
    finish(img, "clock_box.png")


# ----- The QUESTS entry on the pause menu's quest page (under the Bombers' Notebook, z_kaleido_collect.c).
# These are game Texture resources written directly (no extension), in the resource format's version 1, which carries
# a scale: the pause menu loads them at their N64 size (what the texture coordinates of the quads read) and the
# renderer samples the 4x larger image behind it, like an HD texture pack. Sizes are N64 texels; the art is HD x4.

import struct

HD = 4
PAPER = (232, 208, 156)
PAPER_DARK = (170, 128, 76)
INK = (70, 36, 22)
RED = (200, 34, 28)
RED_DARK = (112, 14, 16)
MASK_PURPLE = (96, 40, 120)
MASK_ORANGE = (236, 120, 40)
SYS_FONTS = "/usr/share/fonts"


def font(path_in_repo=None, system=None, size=16):
    from PIL import ImageFont
    for p in ([os.path.join(OUT, "..", "..", "fonts", path_in_repo)] if path_in_repo else []) + (
            [os.path.join(SYS_FONTS, system)] if system else []):
        if os.path.exists(p):
            return ImageFont.truetype(p, size)
    return ImageFont.load_default()


def write_texture(name, img, w, h):
    """img (RGBA, w*HD x h*HD) as a plain RGBA32 texture at its full size. The game loads it with
    gDPLoadTextureTile and texture coordinates in its own texels (a scaled Texture v1 came out garbled)."""
    img = img.convert("RGBA")
    assert img.size == (w * HD, h * HD), (name, img.size)
    img.save(os.path.join(OUT, name + ".rgba32.png"))
    if os.path.exists(os.path.join(OUT, name)):
        os.remove(os.path.join(OUT, name))
    preview = os.environ.get("QUEST_PREVIEW")
    if preview:
        img.save(os.path.join(preview, name + ".png"))


def hd_canvas(w, h, ss=2):
    return Image.new("RGBA", (w * HD * ss, h * HD * ss), (0, 0, 0, 0)), HD * ss


def down(img, w, h):
    return img.resize((w * HD, h * HD), Image.LANCZOS)


def grain(img, seed, strength=0.10):
    rng = random.Random(seed)
    noise = Image.new("L", (img.size[0] // 4, img.size[1] // 4))
    noise.putdata([int(128 + rng.uniform(-1, 1) * 127) for _ in range(noise.size[0] * noise.size[1])])
    noise = noise.resize(img.size, Image.BICUBIC).filter(ImageFilter.GaussianBlur(img.size[0] / 200))
    r, g, bl, a = img.split()
    out = []
    for ch in (r, g, bl):
        out.append(Image.composite(ch.point(lambda v: int(v * (1 - strength))), ch, noise.point(lambda v: 255 - v)))
    return Image.merge("RGBA", out + [a])


def mask_emblem(d, cx, cy, r, u):
    """A small heart-shaped mask with spikes and staring eyes (original drawing, in the spirit of the game)."""
    for i in range(-3, 4):
        ang = math.radians(90 + i * 24)
        x0, y0 = cx + math.cos(ang) * r * 0.55, cy - math.sin(ang) * r * 0.40
        x1, y1 = cx + math.cos(ang) * r * 1.25, cy - math.sin(ang) * r * 0.95
        d.polygon([(x0 - r * 0.10 * u / u, y0), (x1, y1), (x0 + r * 0.10, y0)], fill=MASK_ORANGE + (255,),
                  outline=OUTLINE + (255,))
    pts = []
    for k in range(64):
        t = k / 64 * 2 * math.pi
        x = 16 * math.sin(t) ** 3
        y = 13 * math.cos(t) - 5 * math.cos(2 * t) - 2 * math.cos(3 * t) - math.cos(4 * t)
        pts.append((cx + x / 16 * r, cy - y / 16 * r * 0.9 + r * 0.1))
    d.polygon(pts, fill=MASK_PURPLE + (255,), outline=OUTLINE + (255,))
    for sx in (-1, 1):
        ex, ey = cx + sx * r * 0.38, cy - r * 0.02
        d.ellipse([ex - r * 0.26, ey - r * 0.20, ex + r * 0.26, ey + r * 0.20], fill=(255, 210, 60, 255),
                  outline=OUTLINE + (255,), width=max(1, int(r * 0.06)))
        d.ellipse([ex - r * 0.09, ey - r * 0.09, ex + r * 0.09, ey + r * 0.09], fill=(200, 30, 20, 255))
    d.polygon([(cx - r * 0.3, cy + r * 0.42), (cx, cy + r * 0.66), (cx + r * 0.3, cy + r * 0.42)],
              fill=(230, 210, 120, 255), outline=OUTLINE + (255,))


def scroll_art(w, h, seed, dark=0.0):
    img, u = hd_canvas(w, h)
    d = ImageDraw.Draw(img)
    x0, x1 = 5.0 * u, (w - 5.0) * u
    y0, y1 = 3.6 * u, (h - 3.6) * u
    lw = max(1, int(0.55 * u))
    # the sheet
    d.rectangle([x0, y0, x1, y1], fill=PAPER + (255,), outline=OUTLINE + (255,), width=lw)
    # rolled ends: a tube at the top and at the bottom, darker underneath
    for (yy, flip) in ((y0, 1), (y1, -1)):
        d.rounded_rectangle([x0 - 2.2 * u, yy - 2.0 * u, x1 + 2.2 * u, yy + 2.0 * u], radius=2.0 * u,
                            fill=PAPER_DARK + (255,), outline=OUTLINE + (255,), width=lw)
        d.rounded_rectangle([x0 - 1.6 * u, yy - 1.4 * u, x1 + 1.6 * u, yy + 0.2 * u * flip], radius=1.4 * u,
                            fill=(244, 226, 182, 255))
        for sx, ex in ((x0 - 2.2 * u, x0 - 0.8 * u), (x1 + 0.8 * u, x1 + 2.2 * u)):
            d.ellipse([sx, yy - 2.0 * u, ex, yy + 2.0 * u], fill=GOLD_DARK + (255,), outline=OUTLINE + (255,),
                      width=lw)
    # the mask at the top of the sheet
    mask_emblem(d, (x0 + x1) / 2, y0 + 5.2 * u, 3.4 * u, u)
    # the checklist: boxes with checks and lines of writing
    for i in range(3):
        yy = y0 + (10.4 + i * 3.6) * u
        bx = x0 + 2.0 * u
        d.rectangle([bx, yy - 1.1 * u, bx + 2.2 * u, yy + 1.1 * u], fill=(250, 240, 214, 255),
                    outline=INK + (255,), width=max(1, int(0.4 * u)))
        if i < 2:
            d.line([(bx + 0.4 * u, yy), (bx + 1.0 * u, yy + 0.8 * u), (bx + 2.6 * u, yy - 1.4 * u)],
                   fill=RED + (255,), width=max(1, int(0.55 * u)))
        d.line([(bx + 3.4 * u, yy), (x1 - 2.4 * u - (i == 2) * 3 * u, yy)], fill=INK + (220,),
               width=max(1, int(0.45 * u)))
    # the wax seal with ribbons, bottom right
    sx, sy, sr = x1 - 1.2 * u, y1 - 3.0 * u, 2.6 * u
    d.polygon([(sx - 1.6 * u, sy), (sx - 2.4 * u, sy + 4.4 * u), (sx - 1.0 * u, sy + 3.6 * u), (sx - 0.2 * u, sy + 4.6 * u),
               (sx + 0.2 * u, sy)], fill=RED_DARK + (255,), outline=OUTLINE + (255,))
    d.polygon([(sx + 0.2 * u, sy), (sx + 0.8 * u, sy + 4.6 * u), (sx + 1.8 * u, sy + 3.6 * u), (sx + 2.8 * u, sy + 4.2 * u),
               (sx + 1.8 * u, sy)], fill=RED + (255,), outline=OUTLINE + (255,))
    d.ellipse([sx - sr, sy - sr, sx + sr, sy + sr], fill=GOLD + (255,), outline=OUTLINE + (255,), width=lw)
    d.ellipse([sx - sr * 0.72, sy - sr * 0.72, sx + sr * 0.72, sy + sr * 0.72], fill=RED + (255,))
    d.polygon([(sx, sy - sr * 0.45), (sx + sr * 0.42, sy + sr * 0.3), (sx - sr * 0.42, sy + sr * 0.3)],
              fill=GOLD_LIGHT + (255,))
    img = grain(img, seed)
    if dark:
        r, g, bl, a = img.split()
        f = lambda v: int(v * (1 - dark))
        img = Image.merge("RGBA", (r.point(f), g.point(f), bl.point(f), a))
    return down(img, w, h)


def select_frame(w, h):
    """Gold corner brackets with a warm glow, drawn around the scroll while it is selected."""
    img, u = hd_canvas(w, h)
    glow = Image.new("RGBA", img.size, (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    gd.ellipse([7 * u, 6 * u, (w - 7) * u, (h - 6) * u], fill=(255, 206, 96, 120))
    glow = glow.filter(ImageFilter.GaussianBlur(4.5 * u))
    img = Image.alpha_composite(img, glow)
    d = ImageDraw.Draw(img)
    L, t = 6.0 * u, 1.3 * u
    for (cx, cy, sx, sy) in ((2 * u, 2 * u, 1, 1), ((w - 2) * u, 2 * u, -1, 1), (2 * u, (h - 2) * u, 1, -1),
                             ((w - 2) * u, (h - 2) * u, -1, -1)):
        for col, grow in ((OUTLINE, 0.6 * u), (GOLD_DARK, 0.25 * u), (GOLD, 0)):
            d.polygon([(cx - sx * grow, cy - sy * grow), (cx + sx * (L + grow), cy - sy * grow),
                       (cx + sx * (L + grow), cy + sy * (t + grow)), (cx + sx * (t + grow), cy + sy * (t + grow)),
                       (cx + sx * (t + grow), cy + sy * (L + grow)), (cx - sx * grow, cy + sy * (L + grow))],
                      fill=col + (255,))
        d.line([(cx + sx * 0.5 * u, cy + sy * 0.4 * u), (cx + sx * (L - 0.6 * u), cy + sy * 0.4 * u)],
               fill=GOLD_LIGHT + (255,), width=max(1, int(0.35 * u)))
    return down(img, w, h)


def new_mark_art(w, h):
    img, u = hd_canvas(w, h)
    d = ImageDraw.Draw(img)
    d.ellipse([0.4 * u, 0.4 * u, (w - 0.4) * u, (h - 0.4) * u], fill=OUTLINE + (255,))
    d.ellipse([1.0 * u, 1.0 * u, (w - 1.0) * u, (h - 1.0) * u], fill=GOLD + (255,))
    d.ellipse([1.7 * u, 1.7 * u, (w - 1.7) * u, (h - 1.7) * u], fill=(226, 26, 20, 255))
    d.ellipse([2.6 * u, 2.2 * u, 4.4 * u, 3.8 * u], fill=(255, 180, 160, 255))
    return down(img, w, h)


def quest_card(w, h):
    """The card beside the scroll: a parchment plaque with the mask, QUESTS and what it holds."""
    img, u = hd_canvas(w, h)
    d = ImageDraw.Draw(img)
    pts = []
    rng = random.Random(7)
    for k in range(40):  # a torn-paper outline
        t = k / 40
        if t < 0.25:
            x, y = 1.5 + (w - 3) * t / 0.25, 1.5
        elif t < 0.5:
            x, y = w - 1.5, 1.5 + (h - 3) * (t - 0.25) / 0.25
        elif t < 0.75:
            x, y = w - 1.5 - (w - 3) * (t - 0.5) / 0.25, h - 1.5
        else:
            x, y = 1.5, h - 1.5 - (h - 3) * (t - 0.75) / 0.25
        pts.append(((x + rng.uniform(-0.5, 0.5)) * u, (y + rng.uniform(-0.5, 0.5)) * u))
    shadow = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ImageDraw.Draw(shadow).polygon([(x + 0.8 * u, y + 1.0 * u) for x, y in pts], fill=(30, 16, 6, 150))
    img = Image.alpha_composite(img, shadow.filter(ImageFilter.GaussianBlur(0.8 * u)))
    d = ImageDraw.Draw(img)
    d.polygon(pts, fill=PAPER + (255,), outline=PAPER_DARK + (255,))
    d.line(pts + [pts[0]], fill=(150, 104, 56, 255), width=max(1, int(0.5 * u)))
    img = grain(img, 9, 0.14)
    d = ImageDraw.Draw(img)
    mask_emblem(d, w * u / 2, 6.0 * u, 3.0 * u, u)
    title = font("Alegreya-Bold.ttf", None, int(10.5 * u))
    tw = d.textlength("QUESTS", font=title)
    d.text((w * u / 2 - tw / 2, 9.0 * u), "QUESTS", font=title, fill=(70, 20, 60, 255))
    d.line([(w * u * 0.2, 21.4 * u), (w * u * 0.8, 21.4 * u)], fill=(150, 104, 56, 255), width=max(1, int(0.35 * u)))
    sub = font(None, "truetype/crosextra/Caladea-Italic.ttf", int(4.6 * u))
    line = "Missões, objetivos e eventos"
    sw = d.textlength(line, font=sub)
    d.text((w * u / 2 - sw / 2, 22.2 * u), line, font=sub, fill=(60, 34, 30, 255))
    return down(img, w, h)


def panel_name(w, h, text):
    """The pause menu's name line (bottom panel): white letters with a dark rim, like the item names. The panel's
    combiner tints it (white stays the name color, black becomes the panel's dark)."""
    img, u = hd_canvas(w, h)
    d = ImageDraw.Draw(img)
    f = font(None, "opentype/inter/InterDisplay-Bold.otf", int(11.5 * u))
    tw = d.textlength(text, font=f)
    while tw > (w - 6) * u:
        f = font(None, "opentype/inter/InterDisplay-Bold.otf", int(f.size * 0.94))
        tw = d.textlength(text, font=f)
    box = f.getbbox(text)
    x = w * u / 2 - tw / 2
    y = h * u / 2 - (box[1] + box[3]) / 2
    d.text((x, y), text, font=f, fill=(255, 255, 255, 255), stroke_width=int(1.1 * u), stroke_fill=(0, 0, 0, 255))
    return down(img, w, h)


def pause_icon():
    # The slot's quad reads 28x26 texels; the frame and the halo are larger quads around it.
    write_texture("gQuestScrollTex", scroll_art(28, 26, 21), 28, 26)
    write_texture("gQuestScrollPressedTex", scroll_art(28, 26, 21, dark=0.22), 28, 26)
    write_texture("gQuestScrollFrameTex", select_frame(40, 38), 40, 38)
    write_texture("gQuestScrollNewTex", new_mark_art(10, 10), 10, 10)
    write_texture("gQuestCardTex", quest_card(64, 32), 64, 32)
    write_texture("gQuestNameTex", panel_name(128, 16, "Diário de Missões"), 128, 16)
    for old in ("gQuestMenuPauseIconTex.rgba32.png", "gQuestBookTex.rgba32.png", "gQuestBookPressedTex.rgba32.png",
                "gQuestBookOpen1Tex.rgba32.png", "gQuestBookOpen2Tex.rgba32.png", "gQuestBookHaloTex.rgba32.png",
                "gQuestBookNewTex.rgba32.png", "gQuestBookNameTex.ia8.png", "gQuestBookHintTex.ia8.png"):
        if os.path.exists(os.path.join(OUT, old)):
            os.remove(os.path.join(OUT, old))


def book_assets():
    book_frame()
    page_left()
    page_right()
    spine()
    title_plaque()
    emblem()
    keycap()
    hint_plaque()
    tile("tile.png", False)
    tile("tile_current.png", True)
    portrait_ring()
    clock_box()


def main():
    os.makedirs(OUT, exist_ok=True)
    frame()
    button("button.png", (86, 44, 112), (48, 20, 70), False)
    button("button_selected.png", (130, 66, 140), (78, 30, 84), True)
    parchment()
    background()
    banner()
    divider()
    corner()
    icon_main()
    icon_side()
    icon_masks()
    icon_people()
    icon_places()
    icon_items()
    icon_records()
    state_icons()
    book_assets()
    pause_icon()


if __name__ == "__main__":
    main()
