# SPDX-License-Identifier: GPL-2.0-or-later
"""Pictures for the Uncharted launcher (Pillow).

- card_scene() / banner_scene(): the three cover posters in launcher/art (u1, u2, u3).
  card_scene shows the whole poster. banner_scene blurs it into a wide header.
  If a file is missing, card_scene falls back to a painted scene;
- cover(): the collection's box art from the player's own game folder (sce_sys/icon0.png);
- steam_artwork(): Steam library pictures for the non-Steam shortcut, made from the same files of
  the player's game folder (cover, wide header, hero banner).
"""
import math
import random
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont


def _gradient(size, stops):
    """Vertical gradient; stops are (position 0..1, (r, g, b))."""
    w, h = size
    column = Image.new('RGB', (1, h))
    pixels = column.load()
    for y in range(h):
        t = y / max(h - 1, 1)
        for (p0, c0), (p1, c1) in zip(stops, stops[1:]):
            if p0 <= t <= p1:
                f = (t - p0) / max(p1 - p0, 1e-6)
                pixels[0, y] = tuple(int(a + (b - a) * f) for a, b in zip(c0, c1))
                break
    return column.resize((w, h))


def _ridge(w, h, base, amplitude, seed, roughness=0.55, points=9):
    """A random mountain or hill outline (midpoint displacement) as polygon points."""
    rng = random.Random(seed)
    heights = [rng.uniform(-1, 1) for _ in range(points)]
    line = []
    for i in range(points - 1):
        a, b = heights[i], heights[i + 1]
        for k in range(16):
            f = k / 16
            v = a + (b - a) * (3 * f * f - 2 * f * f * f)
            v += rng.uniform(-1, 1) * roughness * 0.15
            line.append(v)
    line.append(heights[-1])
    step = w / (len(line) - 1)
    return [(i * step, base - v * amplitude) for i, v in enumerate(line)] + [(w, h), (0, h)]


def _glow(image, center, radius, color, strength=0.9):
    """A soft light added to the picture (screen blend)."""
    layer = Image.new('RGB', image.size, (0, 0, 0))
    ImageDraw.Draw(layer).ellipse((center[0] - radius, center[1] - radius, center[0] + radius,
                                   center[1] + radius), fill=color)
    layer = layer.filter(ImageFilter.GaussianBlur(radius * 0.8))
    return Image.blend(image, ImageChops.screen(image, layer), strength)


def _frond(d, x, y, angle, length, width, color):
    """A palm frond: a drooping stem with leaflets on both sides."""
    previous = (x, y)
    for k in range(1, 25):
        f = k / 24
        px = x + math.cos(angle) * length * f
        py = y + math.sin(angle) * length * f + length * 0.45 * f * f
        d.line((previous, (px, py)), fill=color, width=max(1, int(width * (1 - f * 0.7))))
        dx, dy = px - previous[0], py - previous[1]
        norm = math.hypot(dx, dy) or 1
        leaf = length * 0.32 * (1 - f) ** 0.6 * (0.6 + 0.4 * math.sin(f * math.pi))
        for side in (-1, 1):
            nx, ny = -dy / norm * side, dx / norm * side
            tip = (px + (nx * 0.8 + dx / norm * 0.5) * leaf, py + (ny * 0.8 + dy / norm * 0.5) * leaf + leaf * 0.35)
            d.line(((px, py), tip), fill=color, width=max(1, int(width * 0.55)))
        previous = (px, py)


def _jungle(w, h):
    """Drake's Fortune: overgrown ruins in a warm jungle evening."""
    img = _gradient((w, h), [(0, (24, 58, 64)), (0.45, (120, 132, 92)), (0.62, (226, 178, 98)),
                             (1, (40, 46, 30))])
    img = _glow(img, (w * 0.68, h * 0.58), h * 0.25, (255, 200, 120))
    d = ImageDraw.Draw(img)
    d.polygon(_ridge(w, h, h * 0.62, h * 0.08, 3), fill=(58, 80, 58))
    # stepped temple
    cx, base = w * 0.36, h * 0.66
    for i, (tw, th) in enumerate([(0.30, 0.06), (0.23, 0.06), (0.16, 0.06), (0.09, 0.07)]):
        top = base - (i + 1) * th * h
        d.rectangle((cx - tw * w / 2, top, cx + tw * w / 2, top + th * h + 1), fill=(46, 58, 42))
    d.rectangle((cx - 0.012 * w, base - 0.31 * h, cx + 0.012 * w, base - 0.26 * h), fill=(30, 36, 28))
    d.polygon(_ridge(w, h, h * 0.74, h * 0.06, 8), fill=(30, 46, 30))
    # palm trees in the foreground
    for px, py, scale, lean in ((w * 0.07, h * 0.30, 1.0, 0.25), (w * 0.95, h * 0.22, 1.15, -0.2)):
        d.line(((px - lean * w * 0.1, h), (px, py)), fill=(14, 24, 15), width=int(h * 0.025 * scale))
        for i in range(8):
            angle = -math.pi / 2 + (i - 3.5) * 0.55 + lean
            _frond(d, px, py, angle, h * 0.34 * scale, h * 0.012 * scale, (14, 24, 15))
    d.rectangle((0, h * 0.86, w, h), fill=(18, 28, 18))
    return img


def _peaks(w, h, base, height, seed, count):
    """Sharp mountain outline: straight slopes between random peaks and valleys."""
    rng = random.Random(seed)
    points, x = [], -w * 0.1
    while x < w * 1.1:
        points.append((x, base - rng.uniform(0.0, 0.35) * height))
        x += w / count * rng.uniform(0.25, 0.75)
        points.append((x, base - rng.uniform(0.45, 1.0) * height))
        x += w / count * rng.uniform(0.25, 0.75)
        if rng.random() < 0.5:  # a shoulder below the summit
            points.append((x, base - rng.uniform(0.3, 0.6) * height))
            x += w / count * rng.uniform(0.15, 0.35)
    return points


def _mountains(w, h):
    """Among Thieves: cold Himalayan peaks."""
    img = _gradient((w, h), [(0, (18, 30, 58)), (0.5, (88, 116, 156)), (0.75, (170, 188, 210)),
                             (1, (214, 222, 234))])
    img = _glow(img, (w * 0.24, h * 0.26), h * 0.12, (250, 246, 230), 0.8)
    d = ImageDraw.Draw(img)
    for base, height, seed, count, rock, snow in (
            (0.66, 0.56, 7, 2.5, (104, 120, 150), (236, 242, 250)),
            (0.82, 0.36, 12, 3.5, (60, 72, 98), (206, 216, 232))):
        outline = _peaks(w, h, h * base, h * height, seed, count)
        d.polygon(outline + [(w * 1.2, h), (-w * 0.2, h)], fill=snow)
        snowline = h * (base - height * 0.55)
        rock_line = [(x, y + max(0.0, snowline - y) * 0.7) for x, y in outline]
        d.polygon(rock_line + [(w * 1.2, h), (-w * 0.2, h)], fill=rock)
    d.polygon(_ridge(w, h, h * 0.9, h * 0.04, 21, roughness=0.3), fill=(28, 36, 54))
    rng = random.Random(5)
    for _ in range(int(w * h / 6000)):
        x, y = rng.uniform(0, w), rng.uniform(0, h)
        r = rng.uniform(0.5, 1.4) * h / 360
        d.ellipse((x - r, y - r, x + r, y + r), fill=(232, 238, 246))
    return img


def _desert(w, h):
    """Drake's Deception: dunes under a hot sky."""
    img = _gradient((w, h), [(0, (122, 60, 28)), (0.4, (226, 150, 72)), (0.6, (246, 214, 150)),
                             (1, (150, 86, 40))])
    img = _glow(img, (w * 0.72, h * 0.42), h * 0.2, (255, 220, 150), 0.9)
    d = ImageDraw.Draw(img)
    d.ellipse((w * 0.72 - h * 0.07, h * 0.42 - h * 0.07, w * 0.72 + h * 0.07, h * 0.42 + h * 0.07),
              fill=(255, 244, 212))
    for base, amp, seed, color in ((0.62, 0.05, 2, (214, 150, 86)), (0.72, 0.07, 6, (186, 118, 60)),
                                   (0.84, 0.08, 14, (140, 84, 40)), (0.95, 0.06, 23, (96, 56, 28))):
        d.polygon(_ridge(w, h, h * base, h * amp, seed, roughness=0.2, points=5), fill=color)
    # a distant caravan
    for i in range(5):
        x = w * (0.18 + i * 0.035)
        y = h * 0.605 - (i % 2) * h * 0.004
        d.ellipse((x - h * 0.014, y - h * 0.01, x + h * 0.014, y + h * 0.008), fill=(70, 40, 20))
        d.line((x + h * 0.012, y - h * 0.004, x + h * 0.022, y - h * 0.022), fill=(70, 40, 20),
               width=max(1, int(h * 0.006)))
    return img


SCENES = {'u1': _jungle, 'u2': _mountains, 'u3': _desert}
POSTERS = {'u1': 'u1.jpg', 'u2': 'u2.jpg', 'u3': 'u3.jpg'}
COLLECTION_BANNER = 'collection_banner.jpg'
_LOADED = {}
_BACKGROUNDS = {}
_BANNER = None


def art_directories():
    """Where covers are looked for, first match per file wins: the art folder next to
    Uncharted.exe (frozen) and launcher/art, the player's own covers that are never distributed;
    then the covers that come with the port (launcher/covers, bundled as covers)."""
    here = Path(__file__).resolve().parent
    bundle = Path(getattr(sys, '_MEIPASS', here))
    own = [Path(sys.executable).resolve().parent / 'art'] if getattr(sys, 'frozen', False) else []
    return own + [here / 'art', bundle / 'covers', here / 'covers']


def load_art(name):
    """The first `name` in art_directories() as RGB, or None (painted scene / dump art instead)."""
    for directory in art_directories():
        try:
            with Image.open(directory / name) as image:
                return image.convert('RGB')
        except OSError:
            continue
    return None


def _load_scene(key):
    if key not in _LOADED:
        _LOADED[key] = load_art(POSTERS[key])
    return _LOADED[key]


def _cover(image, size, focus=(0.5, 0.42)):
    """Scale image so it covers size, keeping `focus` (0..1, 0..1) in frame."""
    w, h = size
    scale = max(w / image.width, h / image.height)
    scaled = image.resize((math.ceil(image.width * scale), math.ceil(image.height * scale)), Image.LANCZOS)
    fx, fy = focus
    left = max(0, min(scaled.width - w, round(fx * scaled.width - w / 2)))
    top = max(0, min(scaled.height - h, round(fy * scaled.height - h / 2)))
    return scaled.crop((left, top, left + w, top + h))


def _fit(image, width, height, background=(9, 9, 9)):
    """The whole picture inside width×height, centred. Nothing is cropped; the strips left beside
    or above it show a darkened, blurred copy of the picture instead of a flat colour."""
    scale = min(width / image.width, height / image.height)
    resized = image.resize((max(1, round(image.width * scale)), max(1, round(image.height * scale))),
                           Image.LANCZOS)
    if resized.size == (width, height):
        return resized
    canvas = _cover(image.resize((max(1, image.width // 8), max(1, image.height // 8)), Image.BILINEAR),
                    (width, height), (0.5, 0.5)).filter(ImageFilter.GaussianBlur(max(6, width // 24)))
    canvas = Image.blend(canvas, Image.new('RGB', canvas.size, background), 0.45)
    canvas.paste(resized, ((width - resized.width) // 2, (height - resized.height) // 2))
    return canvas


def poster_aspect(key):
    """Width / height of the game's cover picture, or None when the painted scene stands in."""
    image = _load_scene(key)
    return image.width / image.height if image is not None else None


def card_scene(key, width, height):
    """The whole cover poster fitted to the card, or a painted scene if the file is missing."""
    width, height = max(width, 8), max(height, 8)
    image = _load_scene(key)
    if image is None:
        image = SCENES[key](width * 2, height * 2).resize((width, height), Image.LANCZOS)
    return _fit(image, width, height)


def _tracked(draw, xy, text, font, fill, tracking):
    """Text with extra space between the letters (`tracking` pixels); returns its width."""
    x, y = xy
    for char in text:
        draw.text((x, y), char, font=font, fill=fill)
        x += font.getlength(char) + tracking
    return x - xy[0] - tracking


def _tracked_width(text, font, tracking):
    return sum(font.getlength(char) for char in text) + tracking * (len(text) - 1)


def brand_heading(width, dpi=1.0):
    """The sidebar heading, logo-style: UNCHARTED in spaced Cinzel capitals with a gold gradient,
    a short gold rule, THE NATHAN DRAKE COLLECTION in small spaced capitals. RGBA, `width` wide
    at most."""
    from uncharted_fonts import font_path
    cinzel = font_path('Cinzel-Bold.ttf')
    small = font_path('MPLUS1p-Medium.ttf') or font_path('Cinzel-Regular.ttf')
    title, line = 'UNCHARTED', 'THE NATHAN DRAKE COLLECTION'
    size = round(30 * dpi)
    while True:
        big = ImageFont.truetype(str(cinzel), size) if cinzel else ImageFont.load_default()
        tracking = size * 0.10
        if _tracked_width(title, big, tracking) <= width or size <= 12:
            break
        size -= 1
    title_w = _tracked_width(title, big, tracking)
    sub_size = max(8, round(8.5 * dpi * 96 / 72))
    while True:
        sub = ImageFont.truetype(str(small), sub_size) if small else ImageFont.load_default()
        sub_tracking = sub_size * 0.16
        if _tracked_width(line, sub, sub_tracking) <= title_w or sub_size <= 7:
            break
        sub_size -= 1
    ascent, descent = big.getmetrics()
    sub_ascent, sub_descent = sub.getmetrics()
    rule_y = ascent + descent + round(6 * dpi)
    height = rule_y + round(10 * dpi) + sub_ascent + sub_descent
    image = Image.new('RGBA', (math.ceil(title_w) + 2, height), (0, 0, 0, 0))
    # The capitals as a mask, filled with a vertical gold gradient.
    mask = Image.new('L', image.size, 0)
    _tracked(ImageDraw.Draw(mask), (0, 0), title, big, 255, tracking)
    gold = _gradient(image.size, [(0.0, (250, 226, 160)), (0.55, (231, 195, 106)), (1.0, (176, 138, 62))])
    image.paste(gold, (0, 0), mask)
    draw = ImageDraw.Draw(image)
    rule_w = round(28 * dpi)
    draw.rectangle((0, rule_y, rule_w, rule_y + max(1, round(dpi))), fill=(168, 137, 66, 255))
    _tracked(draw, (0, rule_y + round(8 * dpi)), line, sub, (154, 146, 132, 255), sub_tracking)
    return image


def rounded_mask(size, radius, scale=4):
    """Rounded-rectangle mask with smooth (anti-aliased) corners: drawn larger, then reduced."""
    w, h = size
    big = Image.new('L', (w * scale, h * scale), 0)
    ImageDraw.Draw(big).rounded_rectangle((0, 0, w * scale - 1, h * scale - 1), radius=radius * scale, fill=255)
    return big.resize(size, Image.LANCZOS)


def _ring(size, box, radius, color, width, scale=4):
    """A smooth rounded outline as an RGBA layer of `size` (drawn larger, then reduced)."""
    layer = Image.new('RGBA', (size[0] * scale, size[1] * scale), (0, 0, 0, 0))
    ImageDraw.Draw(layer).rounded_rectangle(tuple(v * scale for v in box), radius=radius * scale,
                                            outline=color, width=max(1, round(width * scale)))
    return layer.resize(size, Image.LANCZOS)


def _fit_font(path, text, max_width, size, minimum=8):
    """Largest face of `path` whose `text` stays within `max_width` pixels."""
    if not path:
        return ImageFont.load_default()
    size = max(minimum, size)
    font = ImageFont.truetype(str(path), size)
    while size > minimum and font.getlength(text) > max_width:
        size -= 1
        font = ImageFont.truetype(str(path), size)
    return font


def _ink(font, text):
    box = font.getbbox(text or ' ')
    return box[2] - box[0], box[3] - box[1]


def poster_tile(key, width, height, title, subtitle, *, selected=False, hover=False, dimmed=False,
                radius=16, page=(18, 17, 15), caption=None, transparent=False):
    """A launcher tile: drop shadow, rounded cover, caption strip, gold ring when selected.

    With `caption` (pixels) the strip is left empty for the caller to write the names in as
    system text (ClearType, sharper than text drawn into the picture). The picture's
    info['caption'] is the strip's (left, top, width, height). `transparent` keeps it RGBA, so a
    background behind the tile shows around it; `page` then only colours the caption strip."""
    width, height = max(width, 48), max(height, 80)
    text_w = max(24, width - 28)
    try:
        from uncharted_fonts import font_path
        bold = font_path('MPLUS1p-Medium.ttf') or font_path('Cinzel-SemiBold.ttf')
        regular = font_path('MPLUS1p-Regular.ttf') or font_path('Cinzel-Regular.ttf')
    except OSError:
        bold = regular = None
    title_font = _fit_font(bold, title, text_w, max(12, min(18, width // 16)))
    sub_font = _fit_font(regular, subtitle, text_w, max(10, min(13, width // 22)))
    _tw, title_h = _ink(title_font, title)
    _sw, sub_h = _ink(sub_font, subtitle)
    gap = max(8, width // 36)
    draw_text = caption is None
    if draw_text:
        caption = title_h + gap + sub_h + 18
        if caption > height // 3:
            caption = max(title_h + gap + sub_h + 12, height // 3)
    poster_h = height - caption
    pad = max(10, width // 28)
    canvas = Image.new('RGBA', (width + pad * 2, height + pad * 2), (0, 0, 0, 0))
    poster = card_scene(key, width, poster_h)
    if dimmed:
        poster = Image.blend(poster, Image.new('RGB', poster.size, (12, 12, 12)), 0.52)
    if selected:
        poster = Image.blend(poster, Image.new('RGB', poster.size, (255, 220, 150)), 0.06)
    clip = rounded_mask((width, poster_h), radius)
    framed = Image.new('RGBA', (width, poster_h), (0, 0, 0, 255))
    framed.paste(poster, (0, 0))
    framed.putalpha(clip)
    # A soft shadow straight under the cover (no offset block showing beside its corners).
    shadow = Image.new('RGBA', canvas.size, (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle((pad + 2, pad + 5, pad + width - 3, pad + poster_h + 2),
                                             radius=radius, fill=(0, 0, 0, 120))
    canvas.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(max(5, pad * 0.7))))
    canvas.alpha_composite(framed, (pad, pad))
    if selected:
        canvas.alpha_composite(_ring(canvas.size, (pad - 2, pad - 2, pad + width + 1, pad + poster_h + 1),
                                     radius + 2, (231, 195, 106, 255), 2.2))
    elif hover:
        canvas.alpha_composite(_ring(canvas.size, (pad, pad, pad + width - 1, pad + poster_h - 1),
                                     radius, (150, 132, 92, 220), 2))
    if not transparent:  # the names sit on the page itself when the tile is see-through
        foot = Image.new('RGBA', (width, caption), (0, 0, 0, 0))
        ImageDraw.Draw(foot).rounded_rectangle((0, 0, width - 1, caption - 1), radius=max(8, radius - 4),
                                               fill=(page[0], page[1], page[2], 245))
        canvas.alpha_composite(foot, (pad, pad + poster_h))
    if draw_text:
        draw = ImageDraw.Draw(canvas)
        title_color = (231, 195, 106) if selected else (246, 241, 230)
        sub_color = (231, 195, 106) if selected else (154, 145, 127)
        title_x = pad + max(8, (width - title_font.getlength(title)) / 2)
        sub_x = pad + max(8, (width - sub_font.getlength(subtitle)) / 2)
        title_y = pad + poster_h + 8
        sub_y = title_y + title_h + gap
        draw.text((title_x, title_y), title, fill=title_color, font=title_font, anchor='lt')
        draw.text((sub_x, sub_y), subtitle, fill=sub_color, font=sub_font, anchor='lt')
    if transparent:
        canvas.info.update(caption=(pad, pad + poster_h, width, caption), poster=(pad, pad, width, poster_h))
        return canvas
    flat = Image.new('RGB', canvas.size, page)
    flat.paste(canvas, mask=canvas.split()[3])
    flat.info.update(caption=(pad, pad + poster_h, width, caption), poster=(pad, pad, width, poster_h))
    return flat


def _game_background(key):
    """The player's background picture for a game: u1_background.jpg (.jpeg, .png, .webp) in an
    art folder, else a PS3 disc's PIC1.PNG copied into the art folder's u1/."""
    if key not in _BACKGROUNDS:
        names = [f'{key}_background{ext}' for ext in ('.jpg', '.jpeg', '.png', '.webp')]
        names += [f'{key}/PIC1.PNG', f'{key}/pic1.png']
        _BACKGROUNDS[key] = next((image for image in map(load_art, names) if image is not None), None)
    return _BACKGROUNDS[key]


def has_background(key):
    """Whether the player has a background picture for the game in an art folder."""
    return _game_background(key) is not None


def game_backdrop(key, width, height, page):
    """The game's background picture over the top of the Play page: scaled to the page width,
    a little darker, and faded into the page colour over its lower part. None without one."""
    image = _game_background(key)
    if image is None:
        return None
    width, height = max(width, 8), max(height, 8)
    image = _cover(image, (width, height), (0.5, 0.3))
    image = Image.blend(image, Image.new('RGB', image.size, (6, 5, 4)), 0.22)
    fade = Image.new('L', (1, height))
    pixels = fade.load()
    for y in range(height):
        t = min(1.0, max(0.0, (y / max(height - 1, 1) - 0.42) / 0.58))
        pixels[0, y] = int(255 * t * t * (3 - 2 * t))  # smooth from 42 % of the height to the end
    return Image.composite(Image.new('RGB', image.size, page), image, fade.resize((width, height)))


_CONTOURS = {}
_ICONS = {}


def _smooth(t):
    t = min(1.0, max(0.0, t))
    return t * t * (3 - 2 * t)


def _contours(size, color=(231, 195, 106), alpha=30, seed=7):
    """Faint map contour lines (RGBA), the home screen's own touch of the series' treasure maps:
    irregular rings around a few hills, drawn larger and reduced so they stay smooth."""
    if size in _CONTOURS:
        return _CONTOURS[size]
    w, h = size
    scale = 2
    lines = Image.new('L', (w * scale, h * scale), 0)
    draw = ImageDraw.Draw(lines)
    rnd = random.Random(seed)
    span = max(w, h) * scale
    for cx, cy, rings, step in ((0.80, 0.26, 10, 0.040), (0.10, 0.92, 7, 0.050), (1.02, 1.0, 6, 0.055)):
        phases = [rnd.uniform(0, 2 * math.pi) for _ in range(3)]
        amps = [rnd.uniform(0.06, 0.12), rnd.uniform(0.03, 0.07), rnd.uniform(0.015, 0.035)]
        for ring in range(1, rings + 1):
            radius = ring * step * span
            points = []
            for i in range(240):
                a = i / 240 * 2 * math.pi
                k = 1 + sum(amps[j] * math.sin((j + 2) * a + phases[j] + ring * 0.4) for j in range(3))
                points.append((cx * w * scale + radius * k * math.cos(a),
                               cy * h * scale + radius * k * 0.78 * math.sin(a)))
            draw.line(points + [points[0]], fill=255, width=scale)
    mask = lines.resize(size, Image.LANCZOS).point(lambda v: v * alpha // 255)
    layer = Image.new('RGBA', size, color + (0,))
    layer.putalpha(mask)
    _CONTOURS.clear()  # one window size at a time
    _CONTOURS[size] = layer
    return layer


def home_backdrop(key, width, height, page, dim=0.0):
    """The home screen's background for the focused game, over the whole window: the player's
    background picture (u1_background.jpg …), else the game's cover softened, else the painted
    scene. Darker at the top (menu icons), along the
    bottom (names, Start) and on the left, with faint map contour lines over it. `dim` (0..1)
    darkens it further towards the page colour (behind a settings page)."""
    width, height = max(width, 8), max(height, 8)
    image = _game_background(key)
    sharp = image is not None
    if image is None:
        image = _load_scene(key)
    if image is None:
        image = SCENES[key](width // 2, height // 2)
    image = _cover(image, (width, height), (0.5, 0.35))
    if not sharp:
        image = image.filter(ImageFilter.GaussianBlur(max(2, width // 360)))
    image = Image.blend(image, Image.new('RGB', image.size, (6, 5, 4)), 0.25)
    column = Image.new('L', (1, height))
    pixels = column.load()
    for y in range(height):
        t = y / max(height - 1, 1)
        pixels[0, y] = int(255 * max(0.55 * (1 - _smooth(t / 0.22)), 0.94 * _smooth((t - 0.40) / 0.60)))
    row = Image.new('L', (width, 1))
    pixels = row.load()
    for x in range(width):
        pixels[x, 0] = int(255 * 0.5 * (1 - _smooth(x / max(width - 1, 1) / 0.62)))
    shade = ImageChops.lighter(column.resize((width, height)), row.resize((width, height)))
    image = Image.composite(Image.new('RGB', image.size, page), image, shade)
    if dim:
        image = Image.blend(image, Image.new('RGB', image.size, page), dim)
    image = image.convert('RGBA')
    image.alpha_composite(_contours((width, height)))
    return image.convert('RGB')


def _tile_art(key):
    """A game's square tile picture: the player's art/u1_icon.png (.jpg), else its cover."""
    if key not in _ICONS:
        _ICONS[key] = load_art(f'{key}_icon.png') or load_art(f'{key}_icon.jpg')
    return _ICONS[key] or _load_scene(key)


def home_tile(key, side, *, focused=False, hover=False, dimmed=False):
    """A square home-screen tile (RGBA): the game's picture with slightly rounded corners and a
    soft shadow; a gold ring when focused, a faint one under the pointer. info['poster'] is the
    picture's (left, top, width, height) in the image."""
    side = max(side, 24)
    pad = max(8, side // 12)
    radius = max(4, side // 12)
    image = _tile_art(key)
    if image is None:
        image = SCENES[key](side * 2, side * 2)
    art = _cover(image, (side, side), (0.5, 0.38))
    if dimmed:
        art = Image.blend(art, Image.new('RGB', art.size, (12, 12, 12)), 0.55)
    return _tile_frame(art, side, pad, radius, focused, hover)


def home_blank_tile(side, *, focused=False, hover=False):
    """A tile without a picture (Settings): dark glass for an icon drawn over it."""
    side = max(side, 24)
    pad = max(8, side // 12)
    art = Image.new('RGBA', (side, side), (22, 23, 26, 215))
    return _tile_frame(art, side, pad, max(4, side // 12), focused, hover)


def _tile_frame(art, side, pad, radius, focused, hover):
    canvas = Image.new('RGBA', (side + pad * 2, side + pad * 2), (0, 0, 0, 0))
    shadow = Image.new('RGBA', canvas.size, (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle((pad + 2, pad + 5, pad + side - 3, pad + side + 2),
                                             radius=radius, fill=(0, 0, 0, 150))
    canvas.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(max(4, pad * 0.6))))
    framed = art.convert('RGBA')
    mask = rounded_mask((side, side), radius)
    framed.putalpha(ImageChops.multiply(framed.getchannel('A'), mask))
    canvas.alpha_composite(framed, (pad, pad))
    if focused:
        width = max(2.5, side / 70)
        canvas.alpha_composite(_ring(canvas.size, (pad - 3, pad - 3, pad + side + 2, pad + side + 2),
                                     radius + 3, (231, 195, 106, 255), width))
    elif hover:
        canvas.alpha_composite(_ring(canvas.size, (pad, pad, pad + side - 1, pad + side - 1),
                                     radius, (210, 200, 180, 170), 1.6))
    canvas.info['poster'] = (pad, pad, side, side)
    return canvas


def pill_image(width, height, fill, ring=None):
    """A smooth rounded button face (RGBA) for canvas buttons."""
    image = Image.new('RGBA', (width, height), fill)
    image.putalpha(ImageChops.multiply(image.getchannel('A'), rounded_mask((width, height), height // 2)))
    if ring:
        image.alpha_composite(_ring((width, height), (0, 0, width - 1, height - 1), height // 2, ring, 1.5))
    return image


def glow_dot(size, alpha=70):
    """A soft round highlight (RGBA) behind a focused or hovered menu icon."""
    image = Image.new('RGBA', (size, size), (255, 255, 255, 0))
    mask = Image.new('L', (size * 4, size * 4), 0)
    ImageDraw.Draw(mask).ellipse((0, 0, size * 4 - 1, size * 4 - 1), fill=alpha)
    image.putalpha(mask.resize((size, size), Image.LANCZOS))
    return image


def collection_banner(width, height, page=(5, 5, 5)):
    """Legacy of Thieves-style header art, cropped to width×height with a soft bottom fade."""
    global _BANNER
    if _BANNER is None:
        _BANNER = load_art(COLLECTION_BANNER) or False
    if not _BANNER:
        return None
    image = _cover(_BANNER, (max(width, 8), max(height, 8)), (0.50, 0.42))
    w, h = image.size
    shade = Image.new('L', (1, h))
    pixels = shade.load()
    for y in range(h):
        t = y / max(h - 1, 1)
        pixels[0, y] = 0 if t < 0.55 else int(min(1.0, (t - 0.55) / 0.45) ** 1.1 * 200)
    dark = Image.new('RGB', image.size, page)
    return Image.composite(dark, image, shade.resize((w, h)))


def hero_backdrop(key, width, height, page=(10, 10, 10)):
    """Wide cinematic header from the selected cover: blur, vignette, gold wash."""
    image = _load_scene(key)
    if image is None:
        return Image.new('RGB', (width, height), page)
    image = _cover(image, (width, height), (0.48, 0.38))
    image = image.filter(ImageFilter.GaussianBlur(max(14, width // 55)))
    image = Image.blend(image, Image.new('RGB', image.size, (6, 5, 4)), 0.45)
    vignette = Image.new('L', (width, height), 0)
    draw = ImageDraw.Draw(vignette)
    draw.ellipse((-width * 0.15, -height * 0.35, width * 1.15, height * 1.25), fill=255)
    vignette = vignette.filter(ImageFilter.GaussianBlur(max(20, width // 30)))
    dark = Image.new('RGB', image.size, (4, 3, 2))
    image = Image.composite(dark, image, vignette.point(lambda v: int((255 - v) * 0.55)))
    gold = Image.new('RGB', (width, 1), (231, 195, 106))
    gold = gold.resize((width, height))
    gold_mask = Image.linear_gradient('L').resize((width, height)).point(lambda v: int(v * 0.08))
    image = Image.composite(gold, image, gold_mask)
    fade = Image.linear_gradient('L').resize((width, height))
    fade = fade.point(lambda v: 0 if v < 100 else min(255, (v - 100) * 255 // 120))
    return Image.composite(Image.new('RGB', image.size, page), image, fade)


def banner_scene(key, width, height, page=(16, 15, 14)):
    """The selected cover, blurred to fill the header, darker behind the title and faded into
    the page along the bottom. None when the poster is missing."""
    image = _load_scene(key)
    if image is None:
        return None
    image = _cover(image, (width, height)).filter(ImageFilter.GaussianBlur(max(12, width // 70)))
    image = Image.blend(image, Image.new('RGB', image.size, (8, 7, 6)), 0.38)
    mask = Image.new('L', (width, 1))
    pixels = mask.load()
    for x in range(width):
        t = x / max(width - 1, 1)
        pixels[x, 0] = 0 if t > 0.72 else int(190 * (1 - t / 0.72) ** 1.05)
    image = Image.composite(Image.new('RGB', image.size, (8, 7, 6)), image, mask.resize((width, height)))
    fade = Image.linear_gradient('L').resize((width, height))
    fade = fade.point(lambda v: 0 if v < 130 else min(255, (v - 130) * 255 // 100))
    return Image.composite(Image.new('RGB', image.size, page), image, fade)


def cover(game_dir):
    """The collection's box art (sce_sys/icon0.png) of the game folder, or None."""
    path = Path(game_dir or '.') / 'sce_sys' / 'icon0.png'
    try:
        with Image.open(path) as image:
            return image.convert('RGB')
    except OSError:
        return None


def _fill(image, size):
    """The image scaled to cover size, centred."""
    w, h = size
    scale = max(w / image.width, h / image.height)
    scaled = image.resize((math.ceil(image.width * scale), math.ceil(image.height * scale)), Image.LANCZOS)
    left, top = (scaled.width - w) // 2, (scaled.height - h) // 2
    return scaled.crop((left, top, left + w, top + h))


def _framed(art, size):
    """The square box art centred on a blurred, darkened copy of itself."""
    background = _fill(art, size).filter(ImageFilter.GaussianBlur(max(size) / 30))
    background = Image.blend(background, Image.new('RGB', size, (10, 8, 6)), 0.35)
    side = min(size)
    background.paste(art.resize((side, side), Image.LANCZOS), ((size[0] - side) // 2, (size[1] - side) // 2))
    return background


def steam_artwork(game_dir, grid_dir, appid):
    """Steam library pictures for the shortcut with this id: cover (p), header, hero.
    Returns the files written; nothing when the game folder has no art."""
    art = cover(game_dir)
    if art is None:
        return []
    grid_dir = Path(grid_dir)
    grid_dir.mkdir(parents=True, exist_ok=True)
    pictures = {f'{appid}p.png': _framed(art, (600, 900)), f'{appid}.png': _framed(art, (920, 430))}
    try:
        with Image.open(Path(game_dir) / 'sce_sys' / 'pic1.png') as wide:
            pictures[f'{appid}_hero.png'] = _fill(wide.convert('RGB'), (1920, 620))
    except OSError:
        pictures[f'{appid}_hero.png'] = _framed(art, (1920, 620))
    written = []
    for name, picture in pictures.items():
        picture.save(grid_dir / name)
        written.append(grid_dir / name)
    return written


if __name__ == '__main__':  # preview: python uncharted_art.py out.png [game folder]
    import sys
    sheet = Image.new('RGB', (3 * 400, 225 + 300))
    for i, key in enumerate(SCENES):
        sheet.paste(card_scene(key, 400, 225), (i * 400, 0))
    if len(sys.argv) > 2 and cover(sys.argv[2]):
        sheet.paste(_framed(cover(sys.argv[2]), (200, 300)), (0, 225))
        sheet.paste(_framed(cover(sys.argv[2]), (640, 300)), (210, 225))
    sheet.save(sys.argv[1])
