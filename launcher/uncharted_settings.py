# SPDX-License-Identifier: GPL-2.0-or-later
"""Console-style settings lists for the launcher's pages (Tk).

Each setting is one row: its name on the left, its value on the right. One row is lit at a time
(under the pointer, or moved with the arrow keys): a gold edge and border. Enter or a click opens
a choice in a pop-up list, flips an on/off setting or runs an action; Left/Right step through a
choice's values. The lit row's description shows under the list, so the rows stay short."""


class SettingsList:
    def __init__(self, tk, ttk, parent, colors, px, fonts):
        self.tk, self.colors, self.px, self.fonts = tk, colors, px, fonts
        surface = colors['surface']
        self.outer = tk.Frame(parent, bg=surface)
        # The lit row's description, under the list.
        self.help = tk.Label(self.outer, text='', bg=surface, fg=colors['muted'], font=fonts['hint'],
                             anchor='nw', justify='left', wraplength=px(720), height=3)
        self.help.pack(side='bottom', fill='x', padx=px(22), pady=(px(10), px(6)))
        tk.Frame(self.outer, bg=colors['line'], height=1).pack(side='bottom', fill='x', padx=px(20))
        body = tk.Frame(self.outer, bg=surface)
        body.pack(fill='both', expand=True)
        self.canvas = tk.Canvas(body, bg=surface, highlightthickness=0, bd=0)
        self.bar = ttk.Scrollbar(body, orient='vertical', command=self.canvas.yview)
        self.canvas.configure(yscrollcommand=self.bar.set)
        self.canvas.pack(side='left', fill='both', expand=True)
        self.inner = tk.Frame(self.canvas, bg=surface)
        self.window = self.canvas.create_window(0, 0, window=self.inner, anchor='nw')
        self.inner.bind('<Configure>', self._fit)
        self.canvas.bind('<Configure>', self._fit)
        self.rows, self.lit, self.popup = [], None, None
        # The drop-down stays where it opened: a click beside the rows or scrolling closes it.
        for widget in (self.canvas, self.inner, self.help):
            widget.bind('<Button-1>', lambda _e: self.popup and self.popup.close(), add='+')
        self.canvas.configure(yscrollcommand=self._scrolled)

    def _scrolled(self, first, last):
        if self.popup:
            self.popup.close()
        self.bar.set(first, last)

    def _fit(self, _event=None):
        canvas, inner = self.canvas, self.inner
        canvas.itemconfigure(self.window, width=canvas.winfo_width() - self.px(20) * 2)
        canvas.coords(self.window, self.px(20), 0)
        canvas.configure(scrollregion=(0, 0, canvas.winfo_width(), inner.winfo_reqheight() + self.px(12)))
        if inner.winfo_reqheight() > canvas.winfo_height():
            self.bar.pack(side='right', fill='y')
        else:
            self.bar.pack_forget()
            canvas.yview_moveto(0)

    # ---- building ------------------------------------------------------------------------------
    def heading(self, text, top=22):
        self.tk.Label(self.inner, text=text.upper(), bg=self.colors['surface'], fg=self.colors['gold_dim'],
                      font=self.fonts['heading'], anchor='w').pack(fill='x', padx=self.px(4),
                                                                  pady=(self.px(top), self.px(6)))

    def note(self, text):
        self.tk.Label(self.inner, text=text, bg=self.colors['surface'], fg=self.colors['muted'],
                      font=self.fonts['hint'], anchor='w', justify='left', wraplength=self.px(700)).pack(
            fill='x', padx=self.px(4), pady=(0, self.px(8)))

    def add(self, row):
        row.frame.pack(fill='x', pady=(0, max(1, self.px(2))))
        self.rows.append(row)
        for widget in row.hover_widgets():
            widget.bind('<Enter>', lambda _e, r=row: self.light(r), add='+')
            widget.bind('<Button-1>', lambda _e, r=row: self.click(r), add='+')
        if self.lit is None:
            self.light(row)
        return row

    # ---- lighting and keys ---------------------------------------------------------------------
    def light(self, row, scroll=False):
        if self.lit is row:
            return
        if self.lit:
            self.lit.show_lit(False)
        self.lit = row
        row.show_lit(True)
        self.help.configure(text=row.hint or '')
        if scroll:
            self.scroll_to(row)

    def click(self, row):
        if self.popup:  # a click outside the drop-down closes it
            self.popup.close()
            return
        self.light(row)
        row.frame.focus_set()  # arrow keys go to the list, not to a text box clicked before
        row.activate()

    def scroll_to(self, row):
        self.inner.update_idletasks()
        total = max(1, self.inner.winfo_reqheight() + self.px(12))
        top, bottom = row.frame.winfo_y(), row.frame.winfo_y() + row.frame.winfo_height()
        view_top, view_bottom = (v * total for v in self.canvas.yview())
        if top < view_top:
            self.canvas.yview_moveto(max(0, top - self.px(40)) / total)
        elif bottom > view_bottom:
            self.canvas.yview_moveto((bottom + self.px(20) - (view_bottom - view_top)) / total)

    def key(self, action):
        """Arrow keys and Enter from the launcher; returns 'break' when handled, 'top' for Up on
        the first row (the launcher's menu row takes the focus)."""
        if self.popup:
            return self.popup.key(action)
        if not self.rows:
            return None
        if action == 'enter':
            if self.lit:
                self.lit.activate()
            return 'break'
        rows, columns = action
        if rows:
            index = self.rows.index(self.lit) if self.lit in self.rows else 0
            if index + rows < 0:
                return 'top'
            self.light(self.rows[min(len(self.rows) - 1, max(0, index + rows))], scroll=True)
        elif self.lit:
            self.lit.step(columns)
        return 'break'

    def back(self):
        """Esc: closes an open pop-up; False when there is none."""
        if self.popup:
            self.popup.close()
            return True
        return False


class Row:
    """A setting's row: the name on the left, the value or controls in `right`."""

    def __init__(self, owner, title, hint=None):
        tk, colors, px = owner.tk, owner.colors, owner.px
        self.owner, self.hint, self.colors = owner, hint, colors
        bg = colors['card']
        self.frame = tk.Frame(owner.inner, bg=bg, highlightthickness=1, highlightbackground=bg,
                              highlightcolor=bg, takefocus=0)
        self.mark = tk.Frame(self.frame, bg=bg, width=max(2, px(3)))
        self.mark.pack(side='left', fill='y')
        self.title = tk.Label(self.frame, text=title, bg=bg, fg='#d8d2c6', font=owner.fonts['row'], anchor='w')
        self.title.pack(side='left', padx=(px(14), px(12)), pady=px(11))
        self.right = tk.Frame(self.frame, bg=bg)
        self.right.pack(side='right', padx=(0, px(16)))

    def hover_widgets(self):
        return [self.frame, self.mark, self.title, self.right]

    def show_lit(self, lit):
        edge = self.colors['gold_dim'] if lit else self.colors['card']
        self.frame.configure(highlightbackground=edge, highlightcolor=edge)
        self.mark.configure(bg=self.colors['gold'] if lit else self.colors['card'])
        self.title.configure(fg=self.colors['text'] if lit else '#d8d2c6')

    def activate(self):
        pass

    def step(self, direction):
        pass

    def value_label(self, text=''):
        label = self.owner.tk.Label(self.right, text=text, bg=self.colors['card'], fg='#b9b2a5',
                                    font=self.owner.fonts['value'], anchor='e')
        label.pack(side='left')
        return label

    def chevron(self):
        icons = self.owner.fonts.get('icon')
        label = self.owner.tk.Label(self.right, text='\ue76c' if icons else '›', bg=self.colors['card'],
                                    fg=self.colors['muted'], font=icons or self.owner.fonts['value'])
        label.pack(side='left', padx=(self.owner.px(12), 0))
        return label


class ChoiceRow(Row):
    """One of a list of values: Enter opens the list, Left/Right step through it."""

    def __init__(self, owner, title, variable, options, hint=None):
        super().__init__(owner, title, hint)
        self.var, self.options = variable, options
        self.values = [v for v, _t in options]
        if variable.get() not in self.values:
            variable.set(self.values[0])
        self.value = self.value_label()
        self.extra = [self.value, self.chevron()]
        variable.trace_add('write', lambda *_a: self.show())
        self.show()

    def hover_widgets(self):
        return super().hover_widgets() + self.extra

    def show(self):
        value = self.var.get()
        self.value.configure(text=dict(self.options).get(value, value))

    def activate(self):
        ChoicePopup(self)

    def step(self, direction):
        index = self.values.index(self.var.get()) if self.var.get() in self.values else 0
        self.var.set(self.values[min(len(self.values) - 1, max(0, index + direction))])


class ToggleRow(Row):
    """On or off: Enter, a click or Left/Right flip it."""

    def __init__(self, owner, title, variable, hint=None, toggle=None):
        super().__init__(owner, title, hint)
        self.var = variable
        self.value = self.value_label()
        self.switch = toggle(self.right, variable, self.colors['card'])
        self.switch.pack(side='left', padx=(owner.px(12), 0))
        self.switch.canvas.unbind('<Button-1>')  # the row's click flips it (once)
        variable.trace_add('write', lambda *_a: self.show())
        self.show()

    def hover_widgets(self):
        return super().hover_widgets() + [self.value, self.switch.canvas]

    def show(self):
        self.value.configure(text='On' if self.var.get() else 'Off')

    def activate(self):
        self.var.set(not self.var.get())

    def step(self, direction):
        self.var.set(direction > 0)


class ActionRow(Row):
    """Runs a command (a button in a list): Enter or a click."""

    def __init__(self, owner, title, command, hint=None, value=''):
        super().__init__(owner, title, hint)
        self.command = command
        self.value = self.value_label(value)
        self.extra = [self.value, self.chevron()]

    def hover_widgets(self):
        return super().hover_widgets() + self.extra

    def activate(self):
        self.command()


class WidgetRow(Row):
    """A row holding its own controls (a text box, a slider, key buttons) on the right. Such
    controls are made before the row, as children of the list, and placed into it."""

    def __init__(self, owner, title, widget, hint=None):
        super().__init__(owner, title, hint)
        widget = getattr(widget, 'frame', widget)
        widget.pack(in_=self.right, side='left')
        widget.lift()
        self.widget = widget

    def activate(self):
        if self.widget.winfo_class() in ('Entry', 'TEntry'):
            self.widget.focus_set()


class ChoicePopup:
    """A drop-down list of a choice's values under its row (above it when there is no room); the
    current one is marked. A click outside it, Esc or scrolling closes it."""

    def __init__(self, row):
        owner = row.owner
        tk, colors, px = owner.tk, owner.colors, owner.px
        self.row, self.owner = row, owner
        box = tk.Frame(owner.outer, bg=colors['card'], highlightthickness=1, highlightbackground=colors['gold_dim'])
        self.cover = box
        tk.Frame(box, bg=colors['card'], height=px(4)).pack(fill='x')
        self.items = []
        current = row.var.get()
        for value, text in row.options:
            item = tk.Frame(box, bg=colors['card'], cursor='hand2')
            item.pack(fill='x', padx=px(10), pady=(0, 1))
            dot = tk.Label(item, text='●' if value == current else '○', bg=colors['card'],
                           fg=colors['gold'] if value == current else colors['muted'], font=owner.fonts['value'])
            dot.pack(side='left', padx=(px(12), px(10)), pady=px(7))
            label = tk.Label(item, text=text, bg=colors['card'], fg=colors['text'], font=owner.fonts['row'],
                             anchor='w')
            label.pack(side='left', fill='x', expand=True, padx=(0, px(24)))
            for widget in (item, dot, label):
                widget.bind('<Enter>', lambda _e, i=len(self.items): self.light(i))
                widget.bind('<Button-1>', lambda _e, v=value: self.pick(v))
            self.items.append((value, item, dot, label))
        tk.Frame(box, bg=colors['card'], height=px(4)).pack(fill='x')
        # Under the row, right-aligned with its value; above it, or as far up as needed, when the
        # list would run past the bottom.
        owner.outer.update_idletasks()
        ox, oy = owner.outer.winfo_rootx(), owner.outer.winfo_rooty()
        fx, fy = row.frame.winfo_rootx() - ox, row.frame.winfo_rooty() - oy
        width = max(box.winfo_reqwidth(), px(260))
        height = box.winfo_reqheight()
        room = owner.outer.winfo_height()
        x = max(0, fx + row.frame.winfo_width() - width)
        y = fy + row.frame.winfo_height() + 2
        if y + height > room:
            y = fy - height - 2 if fy - height - 2 >= 0 else max(0, room - height)
        box.place(x=x, y=y, width=width)
        box.lift()
        self.index = None
        self.light(next((i for i, item in enumerate(self.items) if item[0] == current), 0))
        owner.popup = self
        box.focus_set()

    def light(self, index):
        colors = self.owner.colors
        for i, (_value, item, dot, label) in enumerate(self.items):
            bg = colors['card_hi'] if i == index else colors['card']
            for widget in (item, dot, label):
                widget.configure(bg=bg)
        self.index = index

    def key(self, action):
        if action == 'enter':
            self.pick(self.items[self.index][0])
        elif action[0]:
            self.light(min(len(self.items) - 1, max(0, self.index + action[0])))
        return 'break'

    def pick(self, value):
        self.row.var.set(value)
        self.close()

    def close(self):
        self.cover.destroy()
        self.owner.popup = None
        self.row.frame.focus_set()
