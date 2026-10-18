"""Launcher widgets with a modern look on Tk: rounded cards, toggle switches, a slider and pill
buttons. Edges are anti-aliased: the shapes are drawn with PIL at four times the size and scaled
down (Tk's canvas draws them jagged)."""
import time

import math
from PIL import Image, ImageDraw, ImageTk

SCALE = 4
TOGGLE_SLIDE_MS = 160
TOGGLE_FRAME_MS = 16
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


class Card:
    """A rounded panel; widgets go into `inner` (a frame, grid columns 0 = label, 1 = control)."""

    def __init__(self, tk, parent, colors, px, title=None, font=None):
        self.colors, self.px = colors, px
        self.radius = px(12)
        self.pad = px(20)
        bg, card = colors['surface'], colors['card']
        self.canvas = tk.Canvas(parent, bg=bg, highlightthickness=0, bd=0, height=10)
        self.inner = tk.Frame(self.canvas, bg=card)
        self.inner.columnconfigure(1, weight=1)
        self.window = self.canvas.create_window(self.pad, self.pad - px(4), window=self.inner, anchor='nw')
        self.canvas.bind('<Configure>', self._draw)
        self.inner.bind('<Configure>', self._fit)
        self._fit_pending = False
        self.refit()
        if title:
            tk.Label(self.inner, text=title.upper(), bg=card, fg=colors['gold'], font=font, anchor='w').grid(
                row=0, column=0, columnspan=4, sticky='w', pady=(0, px(6)))

    def refit(self):
        """Measure the content again once Tk is idle (the inner frame only reports its size while it
        is shown, so new content cannot wait for that)."""
        if not self._fit_pending:
            self._fit_pending = True
            self.canvas.after_idle(self._fit)

    def _fit(self, _event=None):
        self._fit_pending = False
        height = self.inner.winfo_reqheight() + 2 * self.pad - self.px(6)
        if int(self.canvas['height']) != height:
            self.canvas.configure(height=height)

    def _draw(self, _event=None):
        c, colors = self.canvas, self.colors
        w, h, r = c.winfo_width(), c.winfo_height(), self.radius
        if w < 2 * r + 2 or h < 2 * r + 2:
            return
        c.itemconfigure(self.window, width=w - 2 * self.pad)
        self.refit()  # text wraps differently at a new width
        c.delete('bg')
        fill, line, bg = colors['card'], colors['line'], colors['surface']
        c.create_rectangle(r, 0, w - r, h, fill=fill, outline='', tags='bg')
        c.create_rectangle(0, r, w, h - r, fill=fill, outline='', tags='bg')
        c.create_line(r, 0, w - r, 0, fill=line, tags='bg')
        c.create_line(r, h - 1, w - r, h - 1, fill=line, tags='bg')
        c.create_line(0, r, 0, h - r, fill=line, tags='bg')
        c.create_line(w - 1, r, w - 1, h - r, fill=line, tags='bg')
        for which, x, y, anchor in (('nw', 0, 0, 'nw'), ('ne', w, 0, 'ne'), ('sw', 0, h, 'sw'), ('se', w, h, 'se')):
            c.create_image(x, y, image=_corner(r, fill, bg, line, which), anchor=anchor, tags='bg')
        c.tag_lower('bg')


def _smoothstep(t):
    return t * t * (3 - 2 * t)


class Toggle:
    """An on/off switch bound to a BooleanVar; the knob slides to the other end on each change."""

    def __init__(self, tk, parent, variable, colors, px, background=None):
        self.var, self.colors = variable, colors
        self.w, self.h = px(40), px(22)
        self.margin = px(3)
        self.knob = self.h - 2 * self.margin
        self.bg = background or colors['card']
        self.canvas = tk.Canvas(parent, width=self.w, height=self.h, bg=self.bg, highlightthickness=0, bd=0,
                                cursor='hand2')
        self.on = bool(self.var.get())
        self._from = self._to = self._rest(self.on)
        self._started, self._job = 0.0, None
        self._photo = ImageTk.PhotoImage(self._frame(self._to))
        self.canvas.create_image(0, 0, anchor='nw', image=self._photo)
        self.canvas.bind('<Button-1>', lambda _e: self.var.set(not self.var.get()))
        self.canvas.bind('<Destroy>', lambda _e: self._stop())
        self.var.trace_add('write', lambda *_a: self._switch())

    def _rest(self, on):
        return self.w - self.margin - self.knob if on else self.margin

    def _position(self, now):
        t = min(1.0, max(0.0, (now - self._started) / (TOGGLE_SLIDE_MS / 1000.0)))
        if t >= 1.0:
            return self._to
        return self._from + (self._to - self._from) * _smoothstep(t)

    def _frame(self, x):
        """Pill and knob in one image: a separate knob image would show square corners past the ends."""
        colors, s = self.colors, SCALE
        track = colors['gold'] if self.on else colors['track']
        knob = '#14110a' if self.on else colors['knob']
        image = Image.new('RGB', (self.w * s, self.h * s), _rgb(self.bg))
        draw = ImageDraw.Draw(image)
        draw.rounded_rectangle((0, 0, self.w * s - 1, self.h * s - 1), self.h * s // 2, fill=_rgb(track))
        left, top, d = round(x * s), self.margin * s, self.knob * s
        draw.ellipse((left, top, left + d - 1, top + d - 1), fill=_rgb(knob))
        return image.resize((self.w, self.h), Image.LANCZOS)

    def _show(self, x):
        self._photo.paste(self._frame(x))

    def _switch(self):
        now = time.monotonic()
        self._from = self._position(now)
        self.on = bool(self.var.get())
        self._to = self._rest(self.on)
        self._started = now
        self._show(self._from)
        if self._job is None:
            self._job = self.canvas.after(TOGGLE_FRAME_MS, self._step)

    def _step(self):
        self._job = None
        now = time.monotonic()
        self._show(self._position(now))
        if now - self._started < TOGGLE_SLIDE_MS / 1000.0:
            self._job = self.canvas.after(TOGGLE_FRAME_MS, self._step)

    def _stop(self):
        if self._job is not None:
            self.canvas.after_cancel(self._job)
            self._job = None

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
