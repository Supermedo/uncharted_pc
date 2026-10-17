# SPDX-License-Identifier: GPL-2.0-or-later
"""PS3-style media for the launcher: each game's video and music while its cover is hovered, and
a looping header video.

The files are the player's own and live in the art folders next to the covers (never in the
repository or a release, like the covers):
- u1_video.mp4, u2_video.*, u3_video.*: played inside that game's cover while it is hovered;
- background.*: the header video while no cover is hovered;
- u1_music.mp3, u2_music.*, u3_music.*: played while the pointer rests on that game's cover;
- u1/, u2/, u3/ holding a PS3 disc's PS3_GAME files as they are: ICON1.PAM (the XMB video)
  and SND0.AT3 (the XMB music), used when the named files above are missing.
Video and compressed music need PyAV (pip install av); without it the header keeps its still
picture and only PCM .wav music plays. Music goes through winsound (looped from a WAV decoded
once into a cache folder), faded in and out with the process's waveOut volume.
"""
import ctypes
import hashlib
import math
from pathlib import Path
import queue
import tempfile
import threading
import time

from uncharted_art import art_directories

VIDEO_EXTENSIONS = ('.mp4', '.webm', '.mkv', '.mov', '.m4v', '.avi', '.pam')
AUDIO_EXTENSIONS = ('.mp3', '.ogg', '.opus', '.flac', '.m4a', '.aac', '.wav', '.at9', '.at3')
MAX_MUSIC_SECONDS = 600
DEFAULT_CACHE = Path(tempfile.gettempdir()) / 'uncharted-launcher-media'
PACK_START = b'\x00\x00\x01\xba'  # MPEG program stream pack header


def have_av():
    try:
        import av  # noqa: F401
        return True
    except ImportError:
        return False


def find_media(stem, extensions):
    for directory in art_directories():
        for extension in extensions:
            path = directory / f'{stem}{extension}'
            if path.is_file():
                return path
    return None


def find_ps3(key, name):
    """A PS3_GAME file (ICON1.PAM, SND0.AT3) copied into the art folder's u1/, u2/ or u3/."""
    for directory in art_directories():
        for candidate in (name, name.lower()):
            path = directory / key / candidate
            if path.is_file():
                return path
    return None


def clip_for(key):
    """The video played inside a hovered cover."""
    if not have_av():
        return None
    return find_media(f'{key}_video', VIDEO_EXTENSIONS) or find_ps3(key, 'ICON1.PAM')


def header_video():
    """The video in the header while no cover is hovered."""
    return find_media('background', VIDEO_EXTENSIONS) if have_av() else None


def music_for(key):
    return find_media(f'{key}_music', AUDIO_EXTENSIONS) or find_ps3(key, 'SND0.AT3')


def _cache_name(source, suffix):
    stat = source.stat()
    digest = hashlib.sha1(f'{source.resolve()}|{stat.st_size}|{stat.st_mtime_ns}'.encode()).hexdigest()[:16]
    return f'{digest}{suffix}'


def playable_video(source, cache_dir):
    """A PS3 PAMF (.pam) is an MPEG program stream behind its own header (usually 0x800 bytes):
    the stream from its first pack header, cached as .mpg, is what FFmpeg reads."""
    if source.suffix.lower() != '.pam':
        return source
    target = Path(cache_dir) / _cache_name(source, '.mpg')
    if not target.is_file():
        data = source.read_bytes()
        start = data.find(PACK_START, 0, 1 << 20)
        if start < 0:
            raise ValueError(f'{source.name}: no MPEG program stream inside')
        target.parent.mkdir(parents=True, exist_ok=True)
        partial = target.with_suffix('.part')
        partial.write_bytes(data[start:])
        partial.replace(target)
    return target


class VideoClip:
    """Decodes a video on a helper thread, in real time and looped, into pictures of `size`;
    `finish` (run on that thread) shades or shapes each one. The Tk side takes them with latest().
    `cover` fills the box (cut at the edges, the header); otherwise, and for clips too small to
    fill it sharply, the whole clip is shown in its own shape over `fill(size)` (a picture, e.g.
    the dimmed cover) or, without one, over a blurred fill made from itself."""

    def __init__(self, path, size, finish, cache_dir=DEFAULT_CACHE, cover=True, fill=None):
        self.path, self.size, self.finish, self.cache_dir, self.cover = path, size, finish, cache_dir, cover
        self.fill = fill
        self.feather = {}
        self.frames = queue.Queue(maxsize=1)
        self.stopped, self.active = threading.Event(), threading.Event()
        self.active.set()
        self.failed = False
        threading.Thread(target=self._run, daemon=True).start()

    def resize(self, size):
        self.size = size

    def pause(self, paused):
        (self.active.clear if paused else self.active.set)()

    def stop(self):
        self.stopped.set()
        self.active.set()

    def latest(self):
        try:
            return self.frames.get_nowait()
        except queue.Empty:
            return None

    def _picture(self, frame):
        """The frame for the box. Filling it (`cover`) cuts the edges; a small clip (a PS3
        ICON1.PAM is 320×176) would turn soft that way, so it keeps its shape like a clip in a
        cover does: whole, over a blurred fill made from itself, its edges feathered into it."""
        from PIL import Image, ImageChops, ImageFilter
        w, h = self.size
        if self.cover and frame.width >= w * 0.6:
            scale = max(w / frame.width, h / frame.height)
            sw, sh = max(w, math.ceil(frame.width * scale)), max(h, math.ceil(frame.height * scale))
            image = frame.reformat(width=sw, height=sh, format='rgb24').to_image()
            left, top = (sw - w) // 2, max(0, min(sh - h, round(sh * 0.42 - h / 2)))
            return self.finish(image.crop((left, top, left + w, top + h)))
        scale = min(w / frame.width, h / frame.height)
        cw, ch = min(w, round(frame.width * scale)), min(h, round(frame.height * scale))
        clip = frame.reformat(width=cw, height=ch, format='rgb24', interpolation='LANCZOS').to_image()
        if self.fill:
            picture = self.fill((w, h)).copy()
            picture.paste(clip, ((w - cw) // 2, (h - ch) // 2))
            return self.finish(picture)
        fill = clip.resize((max(1, cw // 12), max(1, ch // 12)), Image.BILINEAR)
        fill = fill.filter(ImageFilter.GaussianBlur(1.5)).resize((w, h), Image.BILINEAR)  # blurred: stretching is unseen
        fill = Image.blend(fill, Image.new('RGB', fill.size, (8, 7, 6)), 0.35)
        if self.feather.get('size') != clip.size:
            def ramp(n, room):
                edge = max(8, n // 10) if room else 1  # only the edges that meet the fill
                return [min(255, 255 * min(i, n - 1 - i) // edge) if room else 255 for i in range(n)]
            across, down = Image.new('L', (cw, 1)), Image.new('L', (1, ch))
            across.putdata(ramp(cw, cw < w))
            down.putdata(ramp(ch, ch < h))
            mask = ImageChops.darker(across.resize(clip.size), down.resize(clip.size))
            self.feather = {'size': clip.size, 'mask': mask}
        fill.paste(clip, ((w - cw) // 2, (h - ch) // 2), self.feather['mask'])
        return self.finish(fill)

    def _run(self):
        try:
            import av
            source = playable_video(self.path, self.cache_dir)
            while not self.stopped.is_set():
                with av.open(str(source)) as container:
                    stream = container.streams.video[0]
                    stream.thread_type = 'AUTO'
                    begin = None
                    for frame in container.decode(stream):
                        if self.stopped.is_set():
                            return
                        if not self.active.is_set():
                            paused = time.monotonic()
                            self.active.wait()
                            if begin is not None:
                                begin += time.monotonic() - paused
                        when = float(frame.time or 0.0)
                        if begin is None:
                            begin = time.monotonic() - when
                        late = begin + when - time.monotonic()
                        if late < -0.25:
                            continue  # behind: drop frames rather than drift
                        picture = self._picture(frame)
                        wait = begin + when - time.monotonic()
                        if wait > 0:
                            time.sleep(wait)
                        try:
                            self.frames.get_nowait()
                        except queue.Empty:
                            pass
                        self.frames.put(picture)
        except Exception:  # an unreadable file: the still picture stays
            self.failed = True


def _cached_wav(source, cache_dir):
    """The music as 16-bit stereo WAV (what winsound plays), decoded once per file version."""
    if source.suffix.lower() == '.wav' and not have_av():
        return source
    target = cache_dir / _cache_name(source, '.wav')
    if target.is_file():
        return target
    import av
    import wave
    cache_dir.mkdir(parents=True, exist_ok=True)
    partial = target.with_suffix('.part')
    rate = 44100
    with av.open(str(source)) as container, wave.open(str(partial), 'wb') as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(rate)
        resampler = av.AudioResampler(format='s16', layout='stereo', rate=rate)
        written = 0
        for frame in container.decode(container.streams.audio[0]):
            for chunk in resampler.resample(frame):
                out.writeframes(bytes(chunk.planes[0])[:chunk.samples * 4])
                written += chunk.samples
            if written > MAX_MUSIC_SECONDS * rate:
                break
        for chunk in resampler.resample(None):
            out.writeframes(bytes(chunk.planes[0])[:chunk.samples * 4])
    partial.replace(target)
    return target


class HoverMusic:
    """A game's music while the pointer rests on its cover: starts after a short pause, fades in,
    fades out when the pointer leaves. All calls come from the Tk thread."""

    DELAY_MS, FADE_MS, VOLUME = 350, 700, 0.7

    def __init__(self, root, cache_dir):
        self.root, self.cache_dir = root, Path(cache_dir)
        self.wanted = self.playing = None
        self.pending = self.fading = None
        self.level = 0.0
        self.ready = queue.Queue()

    def hover(self, key):
        if self.pending:
            self.root.after_cancel(self.pending)
            self.pending = None
        self.wanted = key
        if key == self.playing:
            self._fade_to(self.VOLUME)
            return
        if key is None:
            self._fade_to(0.0, then=self._silence)
            return
        source = music_for(key)
        if not source:
            self._fade_to(0.0, then=self._silence)
            return
        if self.playing:
            self._fade_to(0.0)  # the previous cover's music fades while the next one gets ready
        self.pending = self.root.after(self.DELAY_MS, lambda: self._prepare(key, source))

    def stop(self):
        self.wanted = None
        if self.pending:
            self.root.after_cancel(self.pending)
            self.pending = None
        self._silence()

    def _prepare(self, key, source):
        self.pending = None

        def work():
            try:
                self.ready.put((key, _cached_wav(source, self.cache_dir)))
            except Exception:
                self.ready.put((key, None))
        threading.Thread(target=work, daemon=True).start()
        self._collect()

    def _collect(self):
        try:
            key, wav = self.ready.get_nowait()
        except queue.Empty:
            self.root.after(50, self._collect)
            return
        if wav and key == self.wanted:
            import winsound
            self._set_volume(0.0)
            winsound.PlaySound(str(wav), winsound.SND_FILENAME | winsound.SND_ASYNC | winsound.SND_LOOP |
                               winsound.SND_NODEFAULT)
            self.playing = key
            self._fade_to(self.VOLUME)

    def _silence(self):
        import winsound
        winsound.PlaySound(None, 0)
        self.playing = None
        self._set_volume(0.0)

    def _set_volume(self, level):
        self.level = level
        v = max(0, min(0xFFFF, int(level * 0xFFFF)))
        try:
            ctypes.windll.winmm.waveOutSetVolume(None, v | (v << 16))
        except (AttributeError, OSError):
            pass

    def _fade_to(self, target, then=None):
        if self.fading:
            self.root.after_cancel(self.fading)
            self.fading = None
        steps = 14
        step = (target - self.level) / steps

        def tick(n=steps):
            self.fading = None
            if n <= 1:
                self._set_volume(target)
                if then:
                    then()
                return
            self._set_volume(self.level + step)
            self.fading = self.root.after(self.FADE_MS // steps, lambda: tick(n - 1))
        tick()
