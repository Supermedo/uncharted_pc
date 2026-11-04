"""Launcher widgets with a modern look on Tk: rounded cards, toggle switches, a slider and pill
buttons. Edges are anti-aliased: the shapes are drawn with PIL at four times the size and scaled
down (Tk's canvas draws them jagged)."""
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
                 log=False, entry=False, suffix='Ã—'):
        self.var, self.low, self.high, self.step, self.fmt, self.log = variable, low, high, step, fmt, log
        self.suffix = suffix
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
            self.label = tk.Label(self.frame, bg=colors['card'], fg=colors['muted'], font=font, text='Ã—')
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
            self.label.configure(text=self.fmt.format(self.value()) + self.suffix)

    def grid(self, **kw):
        self.frame.grid(**kw)

    def lift(self):
        self.frame.lift()


class ControllerDiagram:
    """Persistent event handlers; selecting a control never rebuilds the layout."""

    def __init__(self, tk, parent, colors, px, font, translate=lambda text: text, on_select=None):
        self.tk, self.colors, self.px, self.font, self._ = tk, colors, px, font, translate
        self.on_select = on_select
        self.frame = tk.Frame(parent, bg=colors['card'])
        self.frame.columnconfigure(0, weight=1)
        self.canvas = tk.Canvas(self.frame, width=px(320), height=px(400), bg=colors['card'],
                                highlightthickness=1, highlightbackground=colors['card'],
                                highlightcolor=colors['gold'], bd=0, takefocus=True)
        self.canvas.grid(row=0, column=0, sticky='ew')
        self.profile, self.name, self.connected = 'generic', '', False
        self.bindings, self.labels, self.available, self.action_titles = {}, {}, None, {}
        self.images, self.selected, self.targets, self.hit_regions = {}, None, {}, {}
        self.width, self.pending_width, self.resize_job = 680, 680, None
        self.callouts, self.rings, self.buttons, self.assignment_rows = [], {}, {}, {}
        self.show_all = tk.BooleanVar(value=False)
        self.expand = tk.Checkbutton(self.frame, text=self._('Show all assignments'),
            variable=self.show_all, command=self.toggle_assignments, font=font,
            bg=colors['card'], fg=colors['text'], selectcolor=colors['card_hi'],
            activebackground=colors['card'], activeforeground=colors['gold'],
            highlightthickness=0, anchor='w')
        self.expand.grid(row=1,column=0,sticky='w',pady=px(8))
        self.button_frame = tk.Frame(self.frame,bg=colors['card'])
        groups = [ ('Face buttons',('a','b','x','y')),
            ('Shoulders and triggers',('leftshoulder','rightshoulder','lefttrigger','righttrigger')),
            ('Sticks and D-pad',('leftstick','rightstick','dpup','dpdown','dpleft','dpright')),
            ('Menu and extra buttons',('back','start','touchpad','misc1','misc2','misc3','misc4','misc5','misc6')),
            ('Rear buttons',('paddle2','paddle1','paddle4','paddle3')) ]
        self.group_frames, self.assignment_headers = [], []
        for title,keys in groups:
            group=tk.Frame(self.button_frame,bg=colors['card'])
            group.columnconfigure(0,weight=1)
            tk.Label(group,text=self._(title),font=font,bg=colors['card'],fg=colors['gold'],
                     anchor='w').grid(row=0,column=0,sticky='ew',pady=(px(12),px(8)))
            headings=tk.Frame(group,bg=colors['card'])
            headings.grid(row=1,column=0,sticky='ew',pady=(0,px(6)))
            for column,title in enumerate(('Your controller button','Game button (PS4)')):
                headings.columnconfigure(column,weight=1,uniform='heading')
                label=tk.Label(headings,text=self._(title),font=font,bg=colors['card'],fg=colors['muted'],
                               anchor='w',justify='left')
                label.grid(row=0,column=column,sticky='ew',padx=px(10))
                self.assignment_headers.append(label)
            for row,key in enumerate(keys,2):
                entry=tk.Frame(group,bg=colors['card'],highlightthickness=1,
                               highlightbackground=colors['line'])
                entry.grid(row=row,column=0,sticky='ew',pady=(0,px(3)))
                entry.columnconfigure(0,weight=1,uniform='assignment')
                entry.columnconfigure(2,weight=1,uniform='assignment')
                button=tk.Button(entry,anchor='w',font=('Segoe UI Symbol',11),relief='flat',bd=0,
                    bg=colors['card'],fg=colors['text'],activebackground=colors['card_hi'],
                    activeforeground=colors['gold'],highlightcolor=colors['gold'],
                    highlightbackground=colors['card'],highlightthickness=1,padx=px(8),pady=px(6),
                    command=lambda k=key:self.select(k),takefocus=True)
                button.grid(row=0,column=0,sticky='nsew')
                arrow=tk.Label(entry,text='→',font=font,bg=colors['card'],fg=colors['muted'])
                arrow.grid(row=0,column=1,padx=px(4))
                target=tk.Label(entry,anchor='w',justify='left',font=('Segoe UI Symbol',11),
                                bg=colors['card'],fg=colors['text'],padx=px(8),pady=px(6))
                target.grid(row=0,column=2,sticky='nsew')
                for widget in (target,arrow):
                    widget.bind('<Button-1>',lambda _e,k=key:self.select(k))
                    widget.configure(cursor='hand2')
                self.buttons[key]=button
                self.assignment_rows[key]=(entry,arrow,target)
            self.group_frames.append((group,keys))
        for column in (0,1):
            self.button_frame.columnconfigure(column,weight=1,uniform='groups')
        self.canvas.bind('<Configure>',self.resize)
        self.canvas.bind('<Button-1>',self.pick)
        self.canvas.bind('<Motion>',self.hover)
        self.canvas.bind('<Leave>',lambda _e:self.highlight(None,'hover'))
        self.canvas.bind('<KeyPress>',self.keypress)
        self.canvas.bind('<Destroy>',self.destroy)
        self.draw()

    def destroy(self, _event=None):
        if self.resize_job is not None:
            self.canvas.after_cancel(self.resize_job)
            self.resize_job=None

    def resize(self, event):
        self.pending_width=max(320,event.width/(self.px(680)/680))
        if self.resize_job is None and abs(self.pending_width-self.width)>2:
            self.resize_job=self.canvas.after(40,self.finish_resize)

    def finish_resize(self):
        self.resize_job=None
        if abs(self.pending_width-self.width)>2:
            self.width=self.pending_width
            self.draw()

    def set_controller(self, profile, name, connected=True, bindings=None, labels=None,
                       available=None, action_titles=None):
        state=(profile,name,connected,bindings or {},labels or {},available,action_titles or {})
        current=(self.profile,self.name,self.connected,self.bindings,self.labels,self.available,self.action_titles)
        if state==current:
            return
        self.profile,self.name,self.connected,self.bindings,self.labels,self.available,self.action_titles=state
        self.draw()

    def select(self, control):
        if control not in self.hit_regions and control not in self.visible_controls:
            return
        self.selected=control
        self.highlight(control,'selected')
        if self.on_select:
            self.on_select(control)

    def highlight(self, control, kind):
        if kind=='selected':
            for key,(entry,arrow,target) in self.assignment_rows.items():
                bg=self.colors['card_hi'] if key==control else self.colors['card']
                entry.configure(bg=bg,highlightbackground=self.colors['gold'] if key==control else self.colors['line'])
                arrow.configure(bg=bg)
                target.configure(bg=bg)
                self.buttons[key].configure(bg=bg,highlightbackground=bg)
        item=self.rings.get(kind)
        if not item:
            return
        region=self.hit_regions.get(control)
        if region:
            x,y,r=region
            self.canvas.coords(item,*[self.px(v) for v in (x-r-3,y-r-3,x+r+3,y+r+3)])
            self.canvas.itemconfigure(item,state='normal')
        else:
            self.canvas.itemconfigure(item,state='hidden')

    def show_pressed(self, controls):
        self.canvas.delete('pressed')
        for control in set(controls):
            region=self.hit_regions.get(control)
            if region:
                x,y,r=region
                self.canvas.create_oval(*[self.px(v) for v in (x-r-6,y-r-6,x+r+6,y+r+6)],
                                        outline='#67c9f0',width=3,tags='pressed')

    def hit_test(self, event):
        scale=self.px(680)/680
        x,y=event.x/scale,event.y/scale
        for box,key in self.callouts:
            x1,y1,x2,y2=box
            if x1<=x<=x2 and y1<=y<=y2:
                return key
        hits=[((x-cx)**2+(y-cy)**2,key) for key,(cx,cy,r) in self.hit_regions.items()
              if (x-cx)**2+(y-cy)**2<=r*r]
        return min(hits)[1] if hits else None

    def pick(self,event):
        control=self.hit_test(event)
        if control:
            self.select(control)

    def hover(self,event):
        control=self.hit_test(event)
        self.highlight(control,'hover')
        self.canvas.configure(cursor='hand2' if control else '')

    def keypress(self,event):
        keys=list(self.hit_regions)
        if not keys:
            return
        if event.keysym in ('Left','Up','Right','Down','Home','End'):
            index=keys.index(self.selected) if self.selected in keys else -1
            index=0 if event.keysym=='Home' else len(keys)-1 if event.keysym=='End' else (
                index+(-1 if event.keysym in ('Left','Up') else 1))%len(keys)
            self.select(keys[index])
            return 'break'
        if event.keysym in ('Return','space'):
            self.select(self.selected or keys[0])
            return 'break'

    def assignments(self,control):
        return [self.action_titles.get(action,action) for action,value in self.bindings.items()
                if control in value.split('|')]

    def toggle_assignments(self):
        if self.show_all.get():
            self.button_frame.grid(row=2,column=0,sticky='ew',pady=(0,self.px(10)))
        else:
            self.button_frame.grid_remove()

    def draw(self):
        from uncharted_controller_art import fitted_controller_art, MODELS
        c,colors,px=self.canvas,self.colors,self.px
        rear=self.profile in ('steam_deck','steam_controller','steam') or bool(
            self.available and any(k.startswith('paddle') for k in self.available))
        c.delete('all')
        self.targets,self.hit_regions,self.callouts={},{},[]
        c.create_text(px(12),px(10),text=self._('Offline preview') if not self.connected else self.name[:48],
                      anchor='nw',width=px(self.width*.5-20),fill=colors['muted'],font=self.font)
        c.create_text(px(self.width-12),px(10),text=dict(MODELS).get(self.profile,'Generic'),
                      anchor='ne',width=px(self.width*.45),fill=colors['gold'],font=self.font)
        header_bottom=c.bbox('all')[3]/(px(680)/680)
        used_images={}
        def artwork(back,width,y):
            key=(self.profile,px(width),back,colors['card'],colors['gold'])
            if key not in self.images:
                im,controls=fitted_controller_art(self.profile,px(width),back,colors['card'],colors['gold'])
                self.images[key]=(ImageTk.PhotoImage(im),controls,im.height)
            photo,controls,height=self.images[key]
            used_images[key]=self.images[key]
            x=(self.width-width)/2
            c.create_image(px(x),px(y),image=photo,anchor='nw')
            dpi=px(680)/680
            for control,(cx,cy,r) in controls.items():
                self.targets[control]=(x+cx/dpi,y+cy/dpi)
                self.hit_regions[control]=(x+cx/dpi,y+cy/dpi,max(8,r/dpi))
            return y+height/dpi
        compact=self.width<500
        body_width=min(560,self.width-(24 if compact else 230))
        bottom=artwork(False,body_width,max(54,header_bottom+16))
        def callout(control,x,y,anchor,width):
            available=self.available is None or control in self.available
            assigned=self.assignments(control)
            detail=assigned[0]+(' +'+str(len(assigned)-1) if len(assigned)>1 else '') if assigned else self._('Not assigned')
            label=self.labels.get(control,control)
            item=c.create_text(px(x),px(y),anchor=anchor,width=px(width),text=label+'\n'+detail,
                fill=colors['text'] if available else colors['muted'],font=('Segoe UI Symbol',11))
            box=c.bbox(item)
            if box:
                self.callouts.append((tuple(v/(px(680)/680) for v in box),control))
        if not compact:
            sides = ((0,('lefttrigger','leftshoulder','start')),(1,('righttrigger','misc4','rightshoulder'))) \
                if self.profile == 'gamecube' else \
                ((0,('leftshoulder','lefttrigger','back')),(1,('rightshoulder','righttrigger','start')))
            for side,keys in sides:
                for row,control in enumerate(keys):
                    if self.profile == 'gamecube' and control.startswith('misc') and \
                            (self.available is None or control not in self.available):
                        continue
                    callout(control,12 if side==0 else self.width-12,65+row*74,'nw' if side==0 else 'ne',100)
            bottom=max(bottom,290)
        if rear:
            c.create_text(px(self.width/2),px(bottom+24),text=self._('Rear view (turned over)'),
                          fill=colors['muted'],font=self.font)
            bottom=artwork(True,min(340,self.width-(24 if compact else 230)),bottom+48)
            if not compact:
                for control in ('paddle1','paddle3','paddle2','paddle4'):
                    x,y=self.targets[control]
                    callout(control,12 if x<self.width/2 else self.width-12,y-16,
                            'nw' if x<self.width/2 else 'ne',100)
        self.images=used_images
        c.configure(height=px(bottom+18))
        self.rings={kind:c.create_oval(0,0,1,1,outline=colors['gold'],width=2 if kind=='selected' else 1,
                                     dash=() if kind=='selected' else (3,2),state='hidden')
                    for kind in ('hover','selected')}
        self.visible_controls=set(self.hit_regions)|((self.available or set())-{'guide'})
        if self.selected not in self.visible_controls:
            self.selected=None
        self.highlight(self.selected,'selected')
        columns=2 if self.width>=780 else 1
        wrap=px(max(100,(self.width/columns-64)/2))
        for label in self.assignment_headers:
            label.configure(wraplength=wrap)
        group_index=0
        for group,keys in self.group_frames:
            visible=[key for key in keys if key in self.visible_controls]
            if visible:
                column=group_index%columns
                group.grid(row=group_index//columns,column=column,columnspan=3-columns,sticky='new',
                           padx=(0,px(16) if columns==2 and column==0 else 0),pady=(0,px(8)))
                group_index+=1
            else:
                group.grid_remove()
            for key in keys:
                button=self.buttons[key]
                entry,arrow,target=self.assignment_rows[key]
                if key not in visible:
                    entry.grid_remove()
                    continue
                available=self.available is None or key in self.available
                assigned=' / '.join(self.assignments(key)) or self._('Not assigned')
                if not available:
                    assigned+=' · '+self._('Not exposed')
                fg=colors['text'] if available else colors['muted']
                button.configure(text=self.labels.get(key,key),fg=fg,wraplength=wrap)
                target.configure(text=assigned,fg=fg,wraplength=wrap)
                entry.grid()

    def grid(self,**kw):
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
