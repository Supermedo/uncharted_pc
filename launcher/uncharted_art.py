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
_BANNER = None
_CUSTOM_ART = {}
_CUSTOM_CROPS = {}


def set_custom_art(paths, crops=None):
    """Use local files chosen in the launcher, without copying or packaging them."""
    global _BANNER, _CUSTOM_ART, _CUSTOM_CROPS
    custom = {name: str(path) for name, path in paths.items() if path}
    crop_values = crops or {}
    if custom != _CUSTOM_ART or crop_values != _CUSTOM_CROPS:
        _CUSTOM_ART = custom
        _CUSTOM_CROPS = crop_values
        _LOADED.clear()
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
    custom = _CUSTOM_ART.get(name)
    if custom:
        try:
            with Image.open(custom) as image:
                return image.convert('RGB')
        except OSError:
            pass
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


def crop_image(image, size, crop=None):
    """Fill ``size`` using a saved zoom and focal point from the artwork editor."""
    crop = crop or {}
    zoom = max(1.0, min(3.0, float(crop.get('zoom', 1.0))))
    focus = (max(0.0, min(1.0, float(crop.get('x', 0.5)))),
             max(0.0, min(1.0, float(crop.get('y', 0.5)))))
    w, h = size
    scale = max(w / image.width, h / image.height) * zoom
    scaled = image.resize((math.ceil(image.width * scale), math.ceil(image.height * scale)), Image.LANCZOS)
    left = max(0, min(scaled.width - w, round(focus[0] * scaled.width - w / 2)))
    top = max(0, min(scaled.height - h, round(focus[1] * scaled.height - h / 2)))
    return scaled.crop((left, top, left + w, top + h))


def _fit(image, width, height, background=(9, 9, 9)):
    """The whole picture inside width×height, centred. Nothing is cropped."""
    scale = min(width / image.width, height / image.height)
    resized = image.resize((max(1, round(image.width * scale)), max(1, round(image.height * scale))),
                           Image.LANCZOS)
    canvas = Image.new('RGB', (width, height), background)
    canvas.paste(resized, ((width - resized.width) // 2, (height - resized.height) // 2))
    return canvas


def card_scene(key, width, height):
    """Cover the poster area with the bundled or user-selected image."""
    width, height = max(width, 8), max(height, 8)
    image = _load_scene(key)
    if image is None:
        image = SCENES[key](width * 2, height * 2).resize((width, height), Image.LANCZOS)
    elif POSTERS[key] in _CUSTOM_ART:
        return crop_image(image, (width, height), _CUSTOM_CROPS.get(POSTERS[key]))
    return _cover(image, (width, height), (0.5, 0.5))


def _rounded_mask(size, radius):
    mask = Image.new('L', size, 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, size[0] - 1, size[1] - 1), radius=radius, fill=255)
    return mask


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
                radius=16, page=(18, 17, 15)):
    """A launcher tile: drop shadow, rounded cover, caption strip, gold ring when selected."""
    width, height = max(width, 48), max(height, 80)
    text_w = max(24, width - 28)
    try:
        from uncharted_fonts import font_path
        bold = font_path('Cinzel-SemiBold.ttf') or font_path('Cinzel-Bold.ttf')
        regular = font_path('Cinzel-Regular.ttf')
    except OSError:
        bold = regular = None
    title_font = _fit_font(bold, title, text_w, max(12, min(18, width // 16)))
    sub_font = _fit_font(regular, subtitle, text_w, max(10, min(13, width // 22)))
    _tw, title_h = _ink(title_font, title)
    _sw, sub_h = _ink(sub_font, subtitle)
    gap = max(8, width // 36)
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
    clip = _rounded_mask((width, poster_h), radius)
    framed = Image.new('RGBA', (width, poster_h), (0, 0, 0, 255))
    framed.paste(poster, (0, 0))
    framed.putalpha(clip)
    shadow = Image.new('RGBA', (width + 8, poster_h + 8), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle((0, 0, width + 7, poster_h + 7), radius=radius + 2, fill=(0, 0, 0, 110))
    shadow = shadow.filter(ImageFilter.GaussianBlur(max(6, width // 40)))
    canvas.alpha_composite(shadow, (pad + 4, pad + 6))
    canvas.alpha_composite(framed, (pad, pad))
    if selected:
        ring = ImageDraw.Draw(canvas)
        for offset, alpha in ((0, 255), (1, 140)):
            ring.rounded_rectangle((pad - 2 + offset, pad - 2 + offset, pad + width + 1 - offset,
                                    pad + poster_h + 1 - offset), radius=radius + 1,
                                   outline=(231, 195, 106, alpha), width=2)
    elif hover:
        ImageDraw.Draw(canvas).rounded_rectangle((pad, pad, pad + width - 1, pad + poster_h - 1),
                                                 radius=radius, outline=(120, 105, 72, 200), width=2)
    foot = Image.new('RGBA', (width, caption), (0, 0, 0, 0))
    ImageDraw.Draw(foot).rounded_rectangle((0, 0, width - 1, caption - 1), radius=max(8, radius - 4),
                                           fill=(page[0], page[1], page[2], 245))
    canvas.alpha_composite(foot, (pad, pad + poster_h))
    draw = ImageDraw.Draw(canvas)
    title_color = (231, 195, 106) if selected else (246, 241, 230)
    sub_color = (231, 195, 106) if selected else (154, 145, 127)
    title_x = pad + max(8, (width - title_font.getlength(title)) / 2)
    sub_x = pad + max(8, (width - sub_font.getlength(subtitle)) / 2)
    title_y = pad + poster_h + 8
    sub_y = title_y + title_h + gap
    draw.text((title_x, title_y), title, fill=title_color, font=title_font, anchor='lt')
    draw.text((sub_x, sub_y), subtitle, fill=sub_color, font=sub_font, anchor='lt')
    flat = Image.new('RGB', canvas.size, page)
    flat.paste(canvas, mask=canvas.split()[3])
    return flat


def collection_banner(width, height, page=(5, 5, 5)):
    """Legacy of Thieves-style header art, cropped to width×height with a soft bottom fade."""
    global _BANNER
    if _BANNER is None:
        _BANNER = load_art(COLLECTION_BANNER) or False
    if not _BANNER:
        return None
    size = (max(width, 8), max(height, 8))
    if COLLECTION_BANNER in _CUSTOM_ART:
        image = crop_image(_BANNER, size, _CUSTOM_CROPS.get(COLLECTION_BANNER))
    else:
        image = _cover(_BANNER, size, (0.50, 0.42))
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


def steam_artwork(game_dir, grid_dir, appid, custom_art='', crops=None):
    """Steam library pictures for the shortcut with this id: cover (p), header, hero.
    Returns the files written; nothing when the game folder has no art."""
    art = None
    if custom_art:
        try:
            with Image.open(custom_art) as image:
                art = image.convert('RGB')
        except OSError:
            pass
    custom = art is not None
    art = art or cover(game_dir)
    if art is None:
        return []
    grid_dir = Path(grid_dir)
    grid_dir.mkdir(parents=True, exist_ok=True)
    if custom:
        crops = crops or {}
        pictures = {f'{appid}p.png': crop_image(art, (600, 900), crops.get('portrait')),
                    f'{appid}.png': crop_image(art, (920, 430), crops.get('header')),
                    f'{appid}_hero.png': crop_image(art, (1920, 620), crops.get('hero'))}
    else:
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
