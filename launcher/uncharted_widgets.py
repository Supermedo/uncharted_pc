"""Launcher widgets with a modern look on Tk: toggle switches, a slider and pill buttons. Edges
are anti-aliased: the shapes are drawn with PIL at four times the size and scaled down (Tk's
canvas draws them jagged)."""
import math
from PIL import Image, ImageDraw, ImageTk

SCALE = 4
_cache = {}


def _rgb(color):
    return tuple(int(color[i:i + 2], 16) for i in (1, 3, 5))


def _photo(key, build):
    if key not in _cache:
        _cache[key] = ImageTk.PhotoImage(build())
    return _cache[key]


def rounded_image(width, height, radius, fill, background, outline=None):
    """A rounded rectangle (anti-aliased) on `background`."""
    def build():
        w, h, r = width * SCALE, height * SCALE, radius * SCALE
        image = Image.new('RGB', (w, h), _rgb(background))
        draw = ImageDraw.Draw(image)
        if outline:
            draw.rounded_rectangle((0, 0, w - 1, h - 1), r, fill=_rgb(outline))
            draw.rounded_rectangle((SCALE, SCALE, w - 1 - SCALE, h - 1 - SCALE), max(0, r - SCALE), fill=_rgb(fill))
        else:
            draw.rounded_rectangle((0, 0, w - 1, h - 1), r, fill=_rgb(fill))
        return image.resize((width, height), Image.LANCZOS)
    return _photo(('round', width, height, radius, fill, background, outline), build)


def circle_image(diameter, fill, background, outline=None):
    def build():
        d = diameter * SCALE
        image = Image.new('RGB', (d, d), _rgb(background))
        draw = ImageDraw.Draw(image)
        draw.ellipse((0, 0, d - 1, d - 1), fill=_rgb(outline or fill))
        if outline:
            draw.ellipse((SCALE, SCALE, d - 1 - SCALE, d - 1 - SCALE), fill=_rgb(fill))
        return image.resize((diameter, diameter), Image.LANCZOS)
    return _photo(('circle', diameter, fill, background, outline), build)


def _corner(radius, fill, background, outline, which):
    """One anti-aliased corner of a card (radius x radius), with its border."""
    def build():
        r = radius * SCALE
        image = Image.new('RGB', (2 * r, 2 * r), _rgb(background))
        draw = ImageDraw.Draw(image)
        draw.ellipse((0, 0, 2 * r - 1, 2 * r - 1), fill=_rgb(outline))
        draw.ellipse((SCALE, SCALE, 2 * r - 1 - SCALE, 2 * r - 1 - SCALE), fill=_rgb(fill))
        box = {'nw': (0, 0, r, r), 'ne': (r, 0, 2 * r, r), 'sw': (0, r, r, 2 * r), 'se': (r, r, 2 * r, 2 * r)}[which]
        return image.crop(box).resize((radius, radius), Image.LANCZOS)
    return _photo(('corner', radius, fill, background, outline, which), build)


class Toggle:
    """An on/off switch bound to a BooleanVar."""

    def __init__(self, tk, parent, variable, colors, px, background=None):
        self.var, self.colors = variable, colors
        self.w, self.h = px(40), px(22)
        self.bg = background or colors['card']
        self.canvas = tk.Canvas(parent, width=self.w, height=self.h, bg=self.bg, highlightthickness=0, bd=0,
                                cursor='hand2')
        self.canvas.bind('<Button-1>', lambda _e: self.var.set(not self.var.get()))
        self.var.trace_add('write', lambda *_a: self.draw())
        self.draw()

    def draw(self):
        c, colors, on = self.canvas, self.colors, bool(self.var.get())
        c.delete('all')
        track = colors['gold'] if on else colors['track']
        c.create_image(0, 0, anchor='nw', image=rounded_image(self.w, self.h, self.h // 2, track, self.bg))
        d = self.h - 6
        knob = circle_image(d, '#14110a' if on else colors['knob'], track)
        c.create_image(self.w - 3 - d if on else 3, 3, anchor='nw', image=knob)

    def grid(self, **kw):
        self.canvas.grid(**kw)

    def pack(self, **kw):
        self.canvas.pack(**kw)


class Slider:
    """A horizontal slider bound to a StringVar holding a number; shows the value next to it.
    log=True spaces the range logarithmically (a wide range stays fine to set near its low end);
    entry=True makes the value a box the number can be typed into (clamped to the range)."""

    def __init__(self, tk, parent, variable, low, high, step, colors, px, font, fmt='{:.2f}', width=320,
                 log=False, entry=False):
        self.var, self.low, self.high, self.step, self.fmt, self.log = variable, low, high, step, fmt, log
        self.colors, self.px = colors, px
        self.frame = tk.Frame(parent, bg=colors['card'])
        self.w, self.h = px(width), px(26)
        self.canvas = tk.Canvas(self.frame, width=self.w, height=self.h, bg=colors['card'], highlightthickness=0,
                                bd=0, cursor='hand2')
        self.canvas.pack(side='left')
        if entry:
            box = tk.Entry(self.frame, textvariable=variable, width=6, font=font, bg=colors['card_hi'], fg=colors['text'],
                           insertbackground=colors['text'], relief='flat', highlightthickness=1,
                           highlightbackground=colors['card_hi'], highlightcolor=colors['gold'])
            box.pack(side='left', padx=(px(12), 0), ipady=2)
            for sequence in ('<Return>', '<FocusOut>'):
                box.bind(sequence, lambda _e: self.var.set(self.fmt.format(self.value())))
            self.label = tk.Label(self.frame, bg=colors['card'], fg=colors['muted'], font=font, text='×')
        else:
            self.label = tk.Label(self.frame, bg=colors['card'], fg=colors['text'], font=font, width=6, anchor='w')
        self.label.pack(side='left', padx=(px(12) if not entry else px(4), 0))
        self.entry = entry
        for sequence in ('<Button-1>', '<B1-Motion>'):
            self.canvas.bind(sequence, self._pick)
        self.var.trace_add('write', lambda *_a: self.draw())
        self.draw()

    def value(self):
        try:
            return min(self.high, max(self.low, float(self.var.get())))
        except ValueError:
            return self.low

    def _pick(self, event):
        r = self.h // 2
        f = min(1.0, max(0.0, (event.x - r) / max(1, self.w - 2 * r)))
        if self.log:
            v = self.low * (self.high / self.low) ** f
            v = round(v, 2) if v < 1 else round(v, 1) if v < 10 else round(v)
        else:
            v = self.low + f * (self.high - self.low)
            v = round(round(v / self.step) * self.step, 4)
        self.var.set(self.fmt.format(min(self.high, max(self.low, v))))

    def draw(self):
        c, colors = self.canvas, self.colors
        c.delete('all')
        r, y = self.h // 2, self.h // 2
        t = max(2, self.px(4))
        f = math.log(self.value() / self.low) / math.log(self.high / self.low) if self.log             else (self.value() - self.low) / (self.high - self.low)
        x = r + f * (self.w - 2 * r)
        c.create_line(r, y, self.w - r, y, fill=colors['track'], width=t, capstyle='round')
        c.create_line(r, y, x, y, fill=colors['gold'], width=t, capstyle='round')
        d = self.px(18)
        c.create_image(x, y, image=circle_image(d, colors['knob_on'], colors['card'], colors['gold']))
        if not self.entry:
            self.label.configure(text=self.fmt.format(self.value()) + '×')

    def grid(self, **kw):
        self.frame.grid(**kw)

    def lift(self):
        self.frame.lift()


class PillButton:
    """A rounded button; kind 'primary' (gold), 'secondary' or 'ghost'. configure(state=, text=)."""

    def __init__(self, tk, parent, text, command, colors, px, font, kind='secondary', background=None,
                 padding=(18, 9), icon=None, icon_font=None):
        self.tk, self.colors, self.px, self.font, self.kind = tk, colors, px, font, kind
        self.command, self.text, self.icon, self.icon_font = command, text, icon, icon_font
        self.bg = background or colors['card']
        self.padding = padding
        self.state, self.hover = 'normal', False
        self.canvas = tk.Canvas(parent, bg=self.bg, highlightthickness=0, bd=0, cursor='hand2')
        self.canvas.bind('<Enter>', lambda _e: self._hover(True))
        self.canvas.bind('<Leave>', lambda _e: self._hover(False))
        self.canvas.bind('<ButtonRelease-1>', self._click)
        self.draw()

    def _hover(self, inside):
        self.hover = inside
        self.draw()

    def _click(self, event):
        if self.state == 'normal' and 0 <= event.x < self.canvas.winfo_width() and \
                0 <= event.y < self.canvas.winfo_height() and self.command:
            self.command()

    def configure(self, **kw):
        if 'state' in kw:
            self.state = kw['state']
        if 'text' in kw:
            self.text = kw['text']
        self.draw()

    config = configure

    def draw(self):
        import tkinter.font as tkfont
        c, colors = self.canvas, self.colors
        font = tkfont.Font(font=self.font)
        label = self.text
        text_w = font.measure(label)
        icon_w = 0
        if self.icon:
            icon_w = tkfont.Font(font=self.icon_font).measure(self.icon) + self.px(10)
        w = text_w + icon_w + 2 * self.px(self.padding[0])
        h = font.metrics('linespace') + 2 * self.px(self.padding[1])
        c.configure(width=w, height=h, cursor='hand2' if self.state == 'normal' else 'arrow')
        c.delete('all')
        disabled = self.state != 'normal'
        if self.kind == 'primary':
            fill = colors['gold_hi'] if self.hover and not disabled else colors['gold']
            fg = '#16120a'
            if disabled:
                fill, fg = colors['track'], colors['muted']
        elif self.kind == 'ghost':
            fill = colors['card_hi'] if self.hover and not disabled else self.bg
            fg = colors['muted'] if disabled else (colors['text'] if self.hover else colors['muted'])
        else:
            fill = colors['button_hi'] if self.hover and not disabled else colors['button']
            fg = colors['muted'] if disabled else colors['text']
        c.create_image(0, 0, anchor='nw', image=rounded_image(w, h, h // 2, fill, self.bg))
        x = w // 2 - (text_w + icon_w) // 2
        if self.icon:
            c.create_text(x, h // 2, text=self.icon, anchor='w', fill=fg, font=self.icon_font)
            x += icon_w
        c.create_text(x, h // 2, text=label, anchor='w', fill=fg, font=self.font)

    def pack(self, **kw):
        self.canvas.pack(**kw)

    def grid(self, **kw):
        self.canvas.grid(**kw)

    def lift(self):
        self.canvas.lift()
