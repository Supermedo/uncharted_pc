#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Linux launcher for the native port of Uncharted: The Nathan Drake Collection (GTK4 / libadwaita).

Picks the game folder, the game to start and the main display options, runs run.py with them as
environment variables and shows its output. It shares settings (~/.config/uncharted-launcher/
settings.json) and the environment builder with the Windows launcher (uncharted_launcher.py);
controls, FSR and frame generation details are edited there or in the in-game menu.
"""
import json
import os
import signal
import subprocess
import sys
import threading
from pathlib import Path

import gi

gi.require_version('Gtk', '4.0')
gi.require_version('Adw', '1')
from gi.repository import Adw, Gio, GLib, Gtk  # noqa: E402

import uncharted_launcher as core  # noqa: E402

CHOICES = [('language', 'Language', core.LANGUAGES), ('present_mode', 'Present mode', core.PRESENT_MODES),
           ('frame_rate', 'Frame rate', core.FRAME_RATES), ('window_size', 'Window size', core.WINDOW_SIZES),
           ('scaler', 'Scaler', core.SCALERS), ('sharpening', 'Sharpening', core.SHARPENING),
           ('anti_aliasing', 'Anti-aliasing', core.ANTI_ALIASING),
           ('frame_generation', 'Frame generation', core.FRAME_GENERATION)]


class Window(Adw.ApplicationWindow):
    def __init__(self, app):
        super().__init__(application=app, title=core.NAME, default_width=720, default_height=820)
        self.settings = {**core.APP_DEFAULTS, **core.load_json(core.CONFIG_FILE, {})}
        self.process = None
        self.rows = {}

        page = Adw.PreferencesPage()
        group = Adw.PreferencesGroup(title='Game')
        self.folder = Adw.ActionRow(title='Game folder', subtitle=self.settings['game_dir'] or 'Not chosen')
        choose = Gtk.Button(label='Choose…', valign=Gtk.Align.CENTER)
        choose.connect('clicked', self.choose_folder)
        self.folder.add_suffix(choose)
        group.add(self.folder)
        self.game = self.combo(group, 'Game', [(k, f'{name} ({sub})') for k, name, sub, _e in core.GAMES], 'start')
        page.add(group)

        group = Adw.PreferencesGroup(title='Display')
        for key, title, options in CHOICES:
            self.rows[key] = self.combo(group, title, options, key)
        for key, title in (('fullscreen', 'Fullscreen'), ('show_fps', 'Show FPS')):
            row = Adw.SwitchRow(title=title, active=bool(self.settings[key]))
            row.connect('notify::active', lambda r, _p, k=key: self.settings.__setitem__(k, r.get_active()))
            group.add(row)
        page.add(group)

        group = Adw.PreferencesGroup(title='Advanced')
        self.extra = Adw.EntryRow(title='Extra environment (KEY=VALUE …)', text=self.settings['extra_env'])
        self.extra.connect('changed', lambda r: self.settings.__setitem__('extra_env', r.get_text()))
        group.add(self.extra)
        page.add(group)

        self.log = Gtk.TextView(editable=False, monospace=True, cursor_visible=False)
        scroll = Gtk.ScrolledWindow(child=self.log, min_content_height=200, vexpand=True)
        self.play = Gtk.Button(label='Play', css_classes=['suggested-action'])
        self.play.connect('clicked', self.toggle)
        bar = Adw.HeaderBar()
        bar.pack_start(self.play)
        box = Adw.ToolbarView(content=Gtk.Box(orientation=Gtk.Orientation.VERTICAL))
        box.add_top_bar(bar)
        box.get_content().append(page)
        box.get_content().append(scroll)
        page.set_vexpand(True)
        self.set_content(box)
        self.connect('close-request', self.on_close)

    def combo(self, group, title, options, key):
        row = Adw.ComboRow(title=title, model=Gtk.StringList.new([label for _v, label in options]))
        values = [v for v, _l in options]
        if self.settings.get(key) in values:
            row.set_selected(values.index(self.settings[key]))
        row.connect('notify::selected', lambda r, _p: self.settings.__setitem__(key, values[r.get_selected()]))
        group.add(row)
        return row

    def choose_folder(self, _button):
        dialog = Gtk.FileDialog(title='Game folder (with eboot.bin)')
        dialog.select_folder(self, None, self.folder_chosen)

    def folder_chosen(self, dialog, result):
        try:
            path = dialog.select_folder_finish(result).get_path()
        except GLib.Error:
            return
        self.settings['game_dir'] = path
        self.folder.set_subtitle(path)

    def append(self, text):
        buffer = self.log.get_buffer()
        buffer.insert(buffer.get_end_iter(), text)
        self.log.scroll_to_iter(buffer.get_end_iter(), 0, False, 0, 0)
        if buffer.get_line_count() > core.MAX_LOG_LINES:
            buffer.delete(buffer.get_start_iter(), buffer.get_iter_at_line(buffer.get_line_count() - core.MAX_LOG_LINES))
        return False

    def save(self):
        try:
            core.CONFIG_DIR.mkdir(parents=True, exist_ok=True)
            core.CONFIG_FILE.write_text(json.dumps(self.settings, indent=2), encoding='utf-8')
        except OSError as error:
            self.append(f'Could not save settings: {error}\n')

    def toggle(self, _button):
        if self.process:
            os.killpg(self.process.pid, signal.SIGTERM)
            return
        self.save()
        key = self.settings['start']
        if not core.game_available(self.settings, key):
            self.append('No game executable in the chosen folder: choose the folder with eboot.bin.\n')
            return
        self.log.get_buffer().set_text('')
        env = core.game_environment(self.settings)
        try:
            # A new session so Stop ends run.py and every game it chains to (U1 -> U2/U3).
            self.process = subprocess.Popen([os.path.join(core.PORT_DIR, 'run-linux.sh')], cwd=core.PORT_DIR, env=env,
                                            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                            stderr=subprocess.STDOUT, start_new_session=True)
        except OSError as error:
            self.append(f'Could not start: {error}\n')
            return
        self.play.set_label('Stop')
        threading.Thread(target=self.read, args=(self.process,), daemon=True).start()

    def read(self, process):
        for line in process.stdout:
            GLib.idle_add(self.append, line.decode('utf-8', 'replace'))
        code = process.wait()
        GLib.idle_add(self.finished, process, code)

    def finished(self, process, code):
        if self.process is process:
            self.process = None
            self.play.set_label('Play')
            self.append(f'\nThe game exited with code {code}.\n')
        return False

    def on_close(self, _window):
        self.save()
        if self.process:
            os.killpg(self.process.pid, signal.SIGTERM)
        return False


class App(Adw.Application):
    def __init__(self):
        super().__init__(application_id='io.github.supermedo.UnchartedLauncher', flags=Gio.ApplicationFlags.DEFAULT_FLAGS)

    def do_activate(self):
        (self.props.active_window or Window(self)).present()


if __name__ == '__main__':
    sys.exit(App().run(sys.argv))
