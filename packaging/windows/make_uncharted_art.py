"""Draws the launcher's pictures: a golden-hour scene for each game (u1: island ruins at sunset,
u2: Himalayan monastery at dawn, u3: desert ruins), the collection banner (an old map with the
route between them) and the brass compass emblem. Original artwork made only from shapes and
noise, no game images or logos. Needs Pillow.
usage: make_uncharted_art.py launcher/covers"""
import math
import random
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter

CARD = (620, 660)   # the cover tiles
BANNER = (1920, 540)
SS = 2              # drawn at twice the size, scaled down


def hexrgb(color):
    return tuple(int(color[i:i + 2], 16) for i in (1, 3, 5))


def mix(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def gradient(size, stops):
    """Vertical gradient; stops are (position 0..1, '#rrggbb')."""
    w, h = size
    column = Image.new('RGB', (1, h))
    px = column.load()
    stops = [(p, hexrgb(c)) for p, c in stops]
    for y in range(h):
        t = y / max(h - 1, 1)
        for (p0, c0), (p1, c1) in zip(stops, stops[1:]):
            if p0 <= t <= p1:
                px[0, y] = mix(c0, c1, (t - p0) / max(p1 - p0, 1e-6))
                break
        else:
            px[0, y] = stops[-1][1] if t > stops[-1][0] else stops[0][1]
    return column.resize(size)


def noise1d(n, seed, octaves=5, roughness=0.5, base=6):
    """Fractal value noise, n samples in about -1..1."""
    rng = random.Random(seed)
    out = [0.0] * n
    amp, cells, total = 1.0, base, 0.0
    for _ in range(octaves):
        knots = [rng.uniform(-1, 1) for _ in range(cells + 2)]
        for x in range(n):
            f = x / n * cells
            i = int(f)
            t = (1 - math.cos((f - i) * math.pi)) / 2
            out[x] += amp * (knots[i] * (1 - t) + knots[i + 1] * t)
        total += amp
        amp *= roughness
        cells *= 2
    return [v / total for v in out]


def ridge(w, y0, amp, seed, **kw):
    return [y0 - amp * v for v in noise1d(w, seed, **kw)]


def layer_mask(size, ys):
    w, h = size
    mask = Image.new('L', size, 0)
    ImageDraw.Draw(mask).polygon([(0, h)] + [(x, ys[x]) for x in range(w)] + [(w, h)], fill=255)
    return mask


def paint(image, mask, top, bottom=None, y0=None, y1=None):
    """Fill mask with a vertical top->bottom colour (between y0 and y1)."""
    w, h = image.size
    if bottom is None:
        fill = Image.new('RGB', image.size, hexrgb(top))
    else:
        y0 = 0 if y0 is None else y0
        y1 = h if y1 is None else y1
        fill = gradient(image.size, [(0, top), (max(y0 / h, 0), top), (min(y1 / h, 1), bottom), (1, bottom)])
    image.paste(fill, (0, 0), mask)


def rim(image, mask, color, width, alpha):
    """Light along the upper edge of a silhouette (the side the low sun catches)."""
    shifted = ImageChops.offset(mask, 0, width)
    edge = ImageChops.subtract(mask, shifted).filter(ImageFilter.GaussianBlur(width / 2))
    image.paste(Image.new('RGB', image.size, hexrgb(color)), (0, 0), edge.point(lambda v: int(v * alpha)))


def glow(image, center, radius, color, strength):
    w, h = image.size
    mask = Image.new('L', image.size, 0)
    d = ImageDraw.Draw(mask)
    cx, cy = center
    for i in range(24, 0, -1):
        r = radius * i / 24
        d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=int(255 * strength * (1 - i / 24) ** 1.6))
    mask = mask.filter(ImageFilter.GaussianBlur(radius / 10))
    return Image.composite(Image.new('RGB', image.size, hexrgb(color)), image, mask)


def rays(image, center, color, count, seed, strength=0.16):
    w, h = image.size
    rng = random.Random(seed)
    mask = Image.new('L', image.size, 0)
    d = ImageDraw.Draw(mask)
    cx, cy = center
    for _ in range(count):
        a = rng.uniform(0, math.tau)
        spread = rng.uniform(0.01, 0.035)
        far = max(w, h) * 1.5
        d.polygon([(cx, cy), (cx + far * math.cos(a - spread), cy + far * math.sin(a - spread)),
                   (cx + far * math.cos(a + spread), cy + far * math.sin(a + spread))],
                  fill=int(255 * rng.uniform(0.4, 1.0)))
    mask = mask.filter(ImageFilter.GaussianBlur(w / 60))
    falloff = Image.new('L', image.size, 0)
    fd = ImageDraw.Draw(falloff)
    for i in range(30, 0, -1):
        r = max(w, h) * 0.9 * i / 30
        fd.ellipse((cx - r, cy - r, cx + r, cy + r), fill=int(255 * (1 - i / 30)))
    mask = ImageChops.multiply(mask, falloff.filter(ImageFilter.GaussianBlur(w / 30)))
    return Image.composite(Image.new('RGB', image.size, hexrgb(color)), image,
                           mask.point(lambda v: int(v * strength)))


def fog(image, y, height, color, alpha, seed):
    """A soft, uneven band of haze centred on y."""
    w, h = image.size
    mask = Image.new('L', image.size, 0)
    d = ImageDraw.Draw(mask)
    wobble = noise1d(w, seed, octaves=3, base=4)
    for x in range(0, w, 2):
        c = y + wobble[x] * height * 0.4
        d.line((x, c - height / 2, x, c + height / 2), fill=255, width=2)
    mask = mask.filter(ImageFilter.GaussianBlur(height / 2.5))
    image.paste(Image.new('RGB', image.size, hexrgb(color)), (0, 0), mask.point(lambda v: int(v * alpha)))


def birds(image, count, box, color, seed, size=1.0):
    rng = random.Random(seed)
    d = ImageDraw.Draw(image)
    x0, y0, x1, y1 = box
    for _ in range(count):
        x, y, s = rng.uniform(x0, x1), rng.uniform(y0, y1), rng.uniform(5, 11) * size
        lift = rng.uniform(0.2, 0.6) * s
        d.line([(x - s, y - lift), (x - s * 0.4, y - lift * 0.4), (x, y)], fill=hexrgb(color), width=max(2, int(s / 4)))
        d.line([(x, y), (x + s * 0.4, y - lift * 0.4), (x + s, y - lift)], fill=hexrgb(color), width=max(2, int(s / 4)))


def finish(image, size, grain=10, vignette=0.55, seed=1):
    """Film grain, a vignette and the bottom darkened for the titles, then scaled down."""
    w, h = image.size
    noise = Image.effect_noise((w, h), 64).point(lambda v: v - 128 + 128)
    image = Image.blend(image, Image.merge('RGB', (noise, noise, noise)), grain / 255)
    mask = Image.new('L', image.size, 0)
    ImageDraw.Draw(mask).ellipse((-w * 0.25, -h * 0.2, w * 1.25, h * 1.15), fill=255)
    mask = mask.filter(ImageFilter.GaussianBlur(w / 8)).point(lambda v: int((255 - v) * vignette))
    image = Image.composite(Image.new('RGB', image.size, (6, 4, 3)), image, mask)
    return image.resize(size, Image.LANCZOS)


# ---- shapes ---------------------------------------------------------------------------------

def palm(d, x, y, height, lean, color, seed, fronds=8):
    rng = random.Random(seed)
    pts = []
    for i in range(21):
        t = i / 20
        pts.append((x + lean * height * t * t, y - height * t))
    for i in range(20):
        w = height * (0.03 - 0.014 * i / 20)
        d.line([pts[i], pts[i + 1]], fill=color, width=max(2, int(w)))
    tx, ty = pts[-1]
    for k in range(fronds):
        a = math.radians(-180 + 360 * k / fronds + rng.uniform(-14, 14))
        length = height * rng.uniform(0.3, 0.42)
        droop = rng.uniform(0.35, 0.75)
        spine = []
        for i in range(17):
            t = i / 16
            spine.append((tx + math.cos(a) * length * t,
                          ty + math.sin(a) * length * t * 0.45 + droop * length * t * t))
        upper, lower = [], []
        for i, (sx, sy) in enumerate(spine):
            nx, ny = (spine[min(i + 1, 16)][0] - spine[max(i - 1, 0)][0],
                      spine[min(i + 1, 16)][1] - spine[max(i - 1, 0)][1])
            n = math.hypot(nx, ny) or 1
            t = i / 16
            wid = height * 0.045 * math.sin(math.pi * min(1, t * 1.15)) * rng.uniform(0.75, 1.15)
            upper.append((sx + ny / n * wid * 0.5, sy - nx / n * wid * 0.5))
            lower.append((sx - ny / n * wid, sy + nx / n * wid))
        d.polygon(upper + lower[::-1], fill=color)
    d.ellipse((tx - height * 0.025, ty - height * 0.02, tx + height * 0.025, ty + height * 0.03), fill=color)

def tower_ruin(d, x, base, width, height, color, seed):
    """A broken fortress tower with merlons and dark arched openings cut out."""
    rng = random.Random(seed)
    d.rectangle((x, base - height, x + width, base), fill=color)
    step = width / 7
    for i in range(7):
        if i % 2 == 0 and rng.random() > 0.2:
            d.rectangle((x + i * step, base - height - step * 0.9, x + (i + 1) * step, base - height), fill=color)
    # broken corner
    d.polygon([(x + width * 0.62, base - height - 2), (x + width + 1, base - height - 2),
               (x + width + 1, base - height * 0.72)], fill=None)


def arch_window(d, cx, top, w, h, color):
    d.rectangle((cx - w / 2, top + w / 2, cx + w / 2, top + h), fill=color)
    d.ellipse((cx - w / 2, top, cx + w / 2, top + w), fill=color)


def pillar(d, x, base, width, height, color, broken, seed):
    rng = random.Random(seed)
    d.rectangle((x - width * 0.65, base - width * 0.5, x + width * 0.65, base), fill=color)
    d.rectangle((x - width / 2, base - height, x + width / 2, base), fill=color)
    if broken:
        pts = [(x - width / 2, base - height)]
        for i in range(5):
            pts.append((x - width / 2 + width * (i + 0.5) / 5, base - height - rng.uniform(-height * 0.06, height * 0.08)))
        pts.append((x + width / 2, base - height))
        d.polygon(pts + [(x + width / 2, base - height + 4), (x - width / 2, base - height + 4)], fill=color)
    else:
        d.rectangle((x - width * 0.7, base - height - width * 0.45, x + width * 0.7, base - height), fill=color)


# ---- scenes ---------------------------------------------------------------------------------

def scene_u1(size):
    """Drake's Fortune: an island fortress at sunset over the sea, palms in front."""
    w, h = size
    horizon = h * 0.6
    img = gradient(size, [(0, '#16233a'), (0.28, '#4a3a55'), (0.45, '#b4593b'), (0.6, '#f0a85a'),
                          (0.61, '#3c5560'), (1, '#0b1418')])
    sun = (w * 0.6, horizon - h * 0.04)
    img = glow(img, sun, w * 0.9, '#ffb866', 0.55)
    img = glow(img, sun, w * 0.1, '#fff1c8', 0.95)
    img = rays(img, sun, '#ffd28a', 40, 11)
    # sun on the water
    d = ImageDraw.Draw(img)
    rng = random.Random(3)
    for i in range(160):
        y = rng.uniform(horizon + 4, h * 0.95)
        t = (y - horizon) / (h - horizon)
        half = w * (0.02 + 0.12 * t) * rng.uniform(0.3, 1)
        x = sun[0] + rng.gauss(0, w * 0.04 * (1 + t * 2))
        d.line((x - half, y, x + half, y), fill=mix(hexrgb('#ffcf87'), hexrgb('#3c5560'), t * 0.7), width=max(1, int(h * 0.003)))
    # far islands
    far = layer_mask(size, ridge(w, horizon + 2, h * 0.05, 5, base=3))
    far = ImageChops.multiply(far, Image.new('L', size, 255).crop((0, 0, w, h)))
    cut = Image.new('L', size, 0)
    ImageDraw.Draw(cut).rectangle((0, 0, w, horizon + 3), fill=255)
    paint(img, ImageChops.multiply(far, cut), '#7a4c4c')
    fog(img, horizon, h * 0.03, '#f2b073', 0.35, 4)
    # the fortress island on the right
    cliff_y = [horizon + 4 - max(0, (x - w * 0.52) / (w * 0.48)) ** 0.45 * h * 0.2
               - h * 0.03 * v for x, v in zip(range(w), noise1d(w, 9, base=8))]
    cliff = layer_mask(size, cliff_y)
    keep = Image.new('L', size, 0)
    ImageDraw.Draw(keep).rectangle((w * 0.5, 0, w, horizon + h * 0.02), fill=255)
    cliff = ImageChops.multiply(cliff, keep)
    d = ImageDraw.Draw(cliff)
    tower_ruin(d, w * 0.7, cliff_y[int(w * 0.72)] + 6, w * 0.09, h * 0.16, 255, 2)
    tower_ruin(d, w * 0.84, cliff_y[int(w * 0.86)] + 6, w * 0.07, h * 0.1, 255, 3)
    d.rectangle((w * 0.66, cliff_y[int(w * 0.7)] - h * 0.05, w * 0.95, cliff_y[int(w * 0.7)] + 4), fill=255)
    paint(img, cliff, '#3a2a33')
    rim(img, cliff, '#ff9c5a', 3, 0.7)
    d = ImageDraw.Draw(img)
    for cx in (0.735, 0.765, 0.875):
        arch_window(d, w * cx, cliff_y[int(w * 0.72)] - h * 0.13, w * 0.012, h * 0.03, hexrgb('#1a1219'))
    birds(img, 7, (w * 0.18, h * 0.22, w * 0.55, h * 0.4), '#2b1d24', 8, size=w / 620)
    # the jungle shore in front
    shore = layer_mask(size, ridge(w, h * 0.86, h * 0.05, 21, base=5))
    paint(img, shore, '#16201b', '#070b09', h * 0.8, h)
    rim(img, shore, '#b4693f', 3, 0.5)
    leaves = Image.new('L', size, 0)
    d = ImageDraw.Draw(leaves)
    rng = random.Random(34)
    for _ in range(70):
        x = rng.uniform(-0.05, 1.05) * w
        r = rng.uniform(0.03, 0.08) * w
        y = h * 0.87 - rng.uniform(0, 0.05) * h
        d.ellipse((x - r, y - r * 0.7, x + r, y + r), fill=255)
    palm(d, w * 0.1, h * 1.02, h * 0.62, 0.32, 255, 31, 9)
    palm(d, w * 0.25, h * 1.02, h * 0.44, 0.45, 255, 32, 8)
    palm(d, w * 0.95, h * 1.02, h * 0.5, -0.35, 255, 33, 8)
    paint(img, leaves, '#0d1210')
    rim(img, leaves, '#c06a3c', 2, 0.35)
    return img


def peaks(w, base, tall, count, seed):
    """Sharp mountain peaks: overlapping triangles with a jagged noise edge."""
    rng = random.Random(seed)
    tops = [(w * (i + rng.uniform(0.1, 0.9)) / count, tall * rng.uniform(0.55, 1.0),
             w * rng.uniform(0.18, 0.32)) for i in range(count)]
    jag = noise1d(w, seed + 3, octaves=6, roughness=0.6, base=12)
    ys = []
    for x in range(w):
        lift = max(t * max(0.0, 1 - abs(x - px) / half) ** 1.15 for px, t, half in tops)
        ys.append(base - lift - tall * 0.06 * jag[x])
    return ys


def shade_side(image, mask, ys, color, alpha):
    """Darken the slopes that face away from the sun (falling to the right)."""
    w, h = image.size
    side = Image.new('L', image.size, 0)
    d = ImageDraw.Draw(side)
    k = max(4, w // 60)
    smooth = [sum(ys[max(0, x - k):x + k + 1]) / len(ys[max(0, x - k):x + k + 1]) for x in range(w)]
    for x in range(1, w):
        if smooth[x] > smooth[x - 1] + 0.2:
            d.line((x, ys[x], x, ys[x] + h * 0.22), fill=255)
    side = ImageChops.multiply(side.filter(ImageFilter.GaussianBlur(w / 120)), mask)
    image.paste(Image.new('RGB', image.size, hexrgb(color)), (0, 0), side.point(lambda v: int(v * alpha)))


def scene_u2(size):
    """Among Thieves: snow peaks at dawn, a monastery on the ridge, prayer flags."""
    w, h = size
    img = gradient(size, [(0, '#0b1426'), (0.3, '#283b60'), (0.5, '#8a7d93'), (0.62, '#f0b27a'), (1, '#e6a070')])
    sun = (w * 0.78, h * 0.52)
    img = glow(img, sun, w * 0.8, '#ffc489', 0.5)
    img = glow(img, sun, w * 0.07, '#fff3d6', 0.9)
    img = rays(img, sun, '#ffe2b0', 34, 12, 0.12)
    # three ranges of sharp peaks, each with a snow cap, paler with distance
    for seed, base, tall, count, rock, rock2, snow_c, haze in (
            (41, h * 0.55, h * 0.3, 4, '#8d93ad', '#6f7795', '#e4e2ec', 0.3),
            (42, h * 0.66, h * 0.24, 5, '#55607f', '#3a4462', '#cfd3e3', 0.2),
            (44, h * 0.76, h * 0.16, 6, '#2e3650', '#1d2338', '#b7bdd3', 0.12)):
        ys = peaks(w, base, tall, count, seed)
        m = layer_mask(size, ys)
        paint(img, m, rock, rock2, min(ys), base)
        depth = [h * 0.035 * (1.6 + v) for v in noise1d(w, seed + 7, octaves=4, base=10)]
        cap = ImageChops.subtract(m, layer_mask(size, [y + dd for y, dd in zip(ys, depth)]))
        img.paste(Image.new('RGB', size, hexrgb(snow_c)), (0, 0), cap)
        shade_side(img, m, ys, '#000000', 0.22)
        rim(img, m, '#ffd9b0', 3, 0.85)
        fog(img, base + h * 0.01, h * 0.07, '#c9b0b0', haze + 0.15, seed)
    # the near ridge with the monastery
    ys = [h * 0.9 - h * 0.12 * math.exp(-((x - w * 0.36) / (w * 0.32)) ** 2) - h * 0.02 * v
          for x, v in zip(range(w), noise1d(w, 46, base=6))]
    near = layer_mask(size, ys)
    d = ImageDraw.Draw(near)
    peak = min(range(w), key=lambda x: ys[x])
    base = ys[peak] + 8
    for i, (dx, bw, bh) in enumerate(((-0.17, 0.12, 0.07), (-0.06, 0.16, 0.12), (0.08, 0.12, 0.09), (0.18, 0.08, 0.05))):
        x0 = peak + dx * w
        top = base - bh * h
        inset = bw * w * 0.08
        d.polygon([(x0, base + 30), (x0 + inset, top), (x0 + bw * w - inset, top), (x0 + bw * w, base + 30)], fill=255)
        d.rectangle((x0 + inset - 3, top - h * 0.012, x0 + bw * w - inset + 3, top), fill=255)
    stupa_x = peak + 0.02 * w
    stupa_y = base - 0.12 * h - h * 0.025
    d.polygon([(stupa_x - w * 0.03, stupa_y + h * 0.012), (stupa_x + w * 0.03, stupa_y + h * 0.012),
               (stupa_x + w * 0.012, stupa_y - h * 0.015), (stupa_x - w * 0.012, stupa_y - h * 0.015)], fill=255)
    d.polygon([(stupa_x - 4, stupa_y - h * 0.015), (stupa_x + 4, stupa_y - h * 0.015), (stupa_x, stupa_y - h * 0.05)], fill=255)
    paint(img, near, '#171a28', '#07080d', base - h * 0.15, h)
    rim(img, near, '#ffbf87', 3, 0.7)
    d = ImageDraw.Draw(img)
    rng = random.Random(7)
    for i in range(10):  # lit windows
        x = peak + rng.uniform(-0.15, 0.25) * w
        y = base - rng.uniform(0.015, 0.06) * h
        d.rectangle((x, y, x + w * 0.006, y + h * 0.01), fill=hexrgb('#ffbe6b'))
    # prayer flags across the top
    flags = ['#2f6fb5', '#e8e2d4', '#c23a2c', '#3c8b4d', '#e3b23c']
    x0, y0, x1, y1 = -w * 0.05, h * 0.06, w * 1.05, h * 0.2
    pts = []
    for i in range(61):
        t = i / 60
        pts.append((x0 + (x1 - x0) * t, y0 + (y1 - y0) * t + h * 0.12 * math.sin(math.pi * t)))
    d.line(pts, fill=hexrgb('#1a1410'), width=max(2, int(w / 300)))
    for i in range(2, 60, 3):
        (ax, ay), (bx, by) = pts[i], pts[i + 1]
        c = mix(hexrgb(flags[(i // 3) % 5]), hexrgb('#5a4a52'), 0.35)
        fw, fh = (bx - ax) * 1.6, h * 0.045
        d.polygon([(ax, ay), (ax + fw, by), (ax + fw * 0.95, by + fh), (ax - fw * 0.05, ay + fh)], fill=c)
    # falling snow
    flakes = Image.new('L', size, 0)
    fd = ImageDraw.Draw(flakes)
    for _ in range(110):
        x, y, r = rng.uniform(0, w), rng.uniform(0, h), rng.uniform(0.8, 2.4) * w / 620
        fd.ellipse((x - r, y - r, x + r, y + r), fill=int(rng.uniform(120, 255)))
    img.paste(Image.new('RGB', size, (255, 250, 245)), (0, 0), flakes.filter(ImageFilter.GaussianBlur(0.8)))
    return img


def scene_u3(size):
    """Drake's Deception: dunes at sunset, broken pillars, a caravan on a far crest."""
    w, h = size
    img = gradient(size, [(0, '#2a1410'), (0.25, '#6e2c1c'), (0.45, '#c25a2a'), (0.6, '#f6c26b'), (1, '#c87a3a')])
    sun = (w * 0.42, h * 0.5)
    img = glow(img, sun, w * 1.0, '#ffb35e', 0.6)
    img = glow(img, sun, w * 0.12, '#fff0c0', 1.0)
    img = rays(img, sun, '#ffd590', 44, 13, 0.14)
    fog(img, h * 0.56, h * 0.05, '#ffd08a', 0.5, 50)
    layers = (('#c2703a', '#9a4f28', h * 0.6, h * 0.03, 51), ('#a5552b', '#6e3519', h * 0.68, h * 0.05, 52),
              ('#7a3a1d', '#3c1a0d', h * 0.78, h * 0.06, 53), ('#3d1d10', '#140906', h * 0.9, h * 0.07, 54))
    for i, (top, bottom, base, amp, seed) in enumerate(layers):
        ys = ridge(w, base, amp, seed, roughness=0.3, base=2, octaves=3)
        m = layer_mask(size, ys)
        paint(img, m, top, bottom, min(ys), base + h * 0.12)
        rim(img, m, '#ffd38a', 3 + i, 0.8 - i * 0.12)
        if i == 0:  # the caravan on the far crest
            d = ImageDraw.Draw(img)
            for k in range(5):
                cx = w * (0.66 + k * 0.035)
                cy = ys[int(cx)] - 1
                s = w * 0.012
                c = hexrgb('#4a2414')
                d.ellipse((cx - s, cy - s * 1.6, cx + s, cy - s * 0.6), fill=c)
                d.ellipse((cx - s * 0.4, cy - s * 2.1, cx + s * 0.3, cy - s * 1.1), fill=c)
                d.line((cx + s, cy - s * 1.2, cx + s * 1.6, cy - s * 2.2), fill=c, width=max(2, int(s * 0.4)))
                for lx in (-0.6, 0.6):
                    d.line((cx + s * lx, cy - s, cx + s * lx, cy), fill=c, width=max(1, int(s * 0.25)))
        if i == 1:  # the ruins of the lost city
            ruins = Image.new('L', size, 0)
            d = ImageDraw.Draw(ruins)
            for k, (px, ph, broken) in enumerate(((0.1, 0.3, False), (0.18, 0.22, True), (0.27, 0.34, False),
                                                  (0.35, 0.15, True), (0.83, 0.26, True), (0.92, 0.33, False))):
                x = w * px
                pillar(d, x, ys[int(x)] + 10, w * 0.035, h * ph, 255, broken, k)
            d.rectangle((w * 0.08, ys[int(w * 0.1)] - h * 0.31 - h * 0.025, w * 0.29,
                         ys[int(w * 0.1)] - h * 0.3 + h * 0.005), fill=255)
            paint(img, ruins, '#5a2a16', '#2a120a', h * 0.3, h * 0.7)
            rim(img, ruins, '#ffc27a', 2, 0.6)
            fog(img, h * 0.7, h * 0.05, '#e59a55', 0.35, 55)
    # blowing sand
    fog(img, h * 0.88, h * 0.04, '#c7804a', 0.3, 56)
    birds(img, 4, (w * 0.5, h * 0.25, w * 0.8, h * 0.38), '#3a1a10', 57, size=w / 620)
    return img


def banner(size):
    """An old map: parchment, coastlines, a compass rose and the route of the three games."""
    w, h = size
    rng = random.Random(77)
    img = gradient(size, [(0, '#3b2c1b'), (0.5, '#5a4329'), (1, '#2a1f13')])
    # stains and fibre
    blotch = Image.effect_noise((w // 24, h // 24), 60).resize(size, Image.BICUBIC).filter(ImageFilter.GaussianBlur(20))
    img = Image.composite(Image.new('RGB', size, hexrgb('#2a1d10')), img, blotch.point(lambda v: max(0, v - 120)))
    fibre = Image.effect_noise((w, h), 30).filter(ImageFilter.GaussianBlur(0.6))
    img = Image.blend(img, Image.merge('RGB', (fibre, fibre, fibre)), 0.05)
    # landmasses from fractal noise
    field = Image.new('L', size, 0)
    for octave, k in ((240, 0.62), (80, 0.28), (24, 0.1)):
        n = Image.effect_noise((max(2, w // octave), max(2, h // octave)), 70).resize(size, Image.BICUBIC)
        field = Image.blend(field, n, k) if field.getbbox() else n
    field = field.filter(ImageFilter.GaussianBlur(4))
    land = field.point(lambda v: 255 if v > 150 else 0).filter(ImageFilter.GaussianBlur(1.2)).point(lambda v: 255 if v > 127 else 0)
    paint(img, land, '#7a6040')
    coast = ImageChops.subtract(land.filter(ImageFilter.MaxFilter(5)), land)
    img.paste(Image.new('RGB', size, hexrgb('#24180c')), (0, 0), coast.point(lambda v: int(v * 0.8)))
    ripple = ImageChops.subtract(land.filter(ImageFilter.MaxFilter(15)), land.filter(ImageFilter.MaxFilter(11)))
    img.paste(Image.new('RGB', size, hexrgb('#2c2012')), (0, 0), ripple.point(lambda v: int(v * 0.35)))
    d = ImageDraw.Draw(img)
    # grid
    for x in range(0, w, w // 12):
        d.line((x, 0, x, h), fill=hexrgb('#6b5232'), width=1)
    for y in range(0, h, h // 4):
        d.line((0, y, w, y), fill=hexrgb('#6b5232'), width=1)
    # route: island -> mountains -> desert
    stops = [(w * 0.3, h * 0.66), (w * 0.55, h * 0.3), (w * 0.82, h * 0.56)]
    pts = []
    for (ax, ay), (bx, by) in zip(stops, stops[1:]):
        mx, my = (ax + bx) / 2, min(ay, by) - h * 0.18
        for i in range(60):
            t = i / 60
            pts.append(((1 - t) ** 2 * ax + 2 * (1 - t) * t * mx + t * t * bx,
                        (1 - t) ** 2 * ay + 2 * (1 - t) * t * my + t * t * by))
    pts.append(stops[-1])
    red = hexrgb('#b3362a')
    for i in range(0, len(pts) - 1, 3):
        d.line([pts[i], pts[i + 1]], fill=red, width=4)
    for x, y in stops:
        s = 14
        d.line((x - s, y - s, x + s, y + s), fill=red, width=6)
        d.line((x - s, y + s, x + s, y - s), fill=red, width=6)
        d.ellipse((x - s * 2, y - s * 2, x + s * 2, y + s * 2), outline=hexrgb('#24180c'), width=2)
    # little landmark glyphs
    ink = hexrgb('#2a1c0e')
    for k in range(4):  # mountains near the second stop
        x, y = stops[1][0] - 70 + k * 40, stops[1][1] - 28 + (k % 2) * 10
        d.line([(x - 18, y + 16), (x, y - 16), (x + 18, y + 16)], fill=ink, width=3)
    for k in range(3):  # palms near the first
        x, y = stops[0][0] - 60 + k * 22, stops[0][1] + 30
        d.line((x, y + 18, x + 4, y - 6), fill=ink, width=3)
        for a in (-150, -110, -60, -20):
            d.line((x + 4, y - 6, x + 4 + 14 * math.cos(math.radians(a)), y - 6 + 9 * math.sin(math.radians(a)) + 5), fill=ink, width=2)
    for k in range(3):  # dunes near the third
        x, y = stops[2][0] + 40 + k * 26, stops[2][1] + 26 + k * 6
        d.arc((x - 22, y - 10, x + 22, y + 14), 200, 340, fill=ink, width=3)
    # compass rose on the left
    rose = emblem(int(h * 0.62), plain=True)
    img.paste(rose, (int(w * 0.07), int(h * 0.12)), rose)
    # warm sunset light from the right and a dark lower edge for the titles
    img = glow(img, (w * 1.0, h * 0.2), w * 0.7, '#e09a52', 0.28)
    shade = gradient((1, h), [(0, '#000000'), (0.55, '#000000'), (1, '#ffffff')]).convert('L').resize(size)
    img = Image.composite(Image.new('RGB', size, (10, 7, 4)), img, shade.point(lambda v: int(v * 0.75)))
    return finish(img, size, grain=8, vignette=0.6)


def emblem(side, plain=False):
    """A brass compass rose on transparency."""
    s = side * 4
    image = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(image)
    c = s / 2
    brass, dark, light = hexrgb('#c9944a'), hexrgb('#5b3d1a'), hexrgb('#f4d48f')

    def at(angle, r):
        a = math.radians(angle - 90)
        return c + r * math.cos(a), c + r * math.sin(a)

    r = s * 0.46
    if not plain:
        d.ellipse((c - r, c - r, c + r, c + r), fill=dark + (255,))
        d.ellipse((c - r * 0.94, c - r * 0.94, c + r * 0.94, c + r * 0.94), fill=hexrgb('#1a120a') + (255,))
    d.ellipse((c - r * 0.86, c - r * 0.86, c + r * 0.86, c + r * 0.86), outline=brass + (255,), width=max(2, s // 120))
    d.ellipse((c - r * 0.8, c - r * 0.8, c + r * 0.8, c + r * 0.8), outline=brass + (200,), width=max(1, s // 300))
    for k in range(72):
        a = k * 5
        inner = r * (0.74 if k % 6 == 0 else 0.77)
        d.line([at(a, inner), at(a, r * 0.8)], fill=brass + (230,), width=max(1, s // 250))
    for k, (length, width) in enumerate(((0.72, 0.1), (0.5, 0.08)) * 4):
        a = k * 45
        tip, left, right = at(a, r * length), at(a - 90, r * width), at(a + 90, r * width)
        d.polygon([tip, left, (c, c)], fill=(light if k % 2 == 0 else brass) + (255,))
        d.polygon([tip, right, (c, c)], fill=(brass if k % 2 == 0 else dark) + (255,))
    d.ellipse((c - r * 0.07, c - r * 0.07, c + r * 0.07, c + r * 0.07), fill=light + (255,))
    n = at(0, r * 0.93)
    d.polygon([(n[0], n[1] - r * 0.06), (n[0] - r * 0.05, n[1] + r * 0.05), (n[0] + r * 0.05, n[1] + r * 0.05)],
              fill=hexrgb('#b3362a') + (255,))
    return image.resize((side, side), Image.LANCZOS)


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else 'launcher/covers')
    out.mkdir(parents=True, exist_ok=True)
    big = (CARD[0] * SS, CARD[1] * SS)
    for name, draw in (('u1', scene_u1), ('u2', scene_u2), ('u3', scene_u3)):
        finish(draw(big), CARD).save(out / f'{name}.jpg', quality=90)
    banner(BANNER).save(out / 'collection_banner.jpg', quality=90)
    emblem(256).save(out / 'emblem.png')
    print('wrote', ', '.join(p.name for p in sorted(out.iterdir())))


if __name__ == '__main__':
    main()
