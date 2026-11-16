"""Adds the game to Steam as a non-Steam shortcut (Steam's userdata/<id>/config/shortcuts.vdf).

shortcuts.vdf is Valve's binary KeyValues: a type byte (0 = map, 1 = string, 2 = int32,
7 = uint64), a NUL-terminated key, then the value; 8 ends a map. The file holds one map,
"shortcuts", whose entries are maps named "0", "1", ...
"""
import os
from pathlib import Path
import struct
import time
import zlib

MAP, STRING, INT32, UINT64, END = 0, 1, 2, 7, 8


def _cstring(data, at):
    end = data.index(b'\0', at)
    return data[at:end].decode('utf-8', errors='replace'), end + 1


def parse(data, at=0):
    """A map as a list of (type, key, value); nested maps are lists too. Returns (items, next)."""
    items = []
    while at < len(data):
        kind = data[at]
        at += 1
        if kind == END:
            return items, at
        key, at = _cstring(data, at)
        if kind == MAP:
            value, at = parse(data, at)
        elif kind == STRING:
            value, at = _cstring(data, at)
        elif kind == INT32:
            value, at = struct.unpack_from('<i', data, at)[0], at + 4
        elif kind == UINT64:
            value, at = struct.unpack_from('<Q', data, at)[0], at + 8
        else:
            raise ValueError(f'unknown field type {kind} in shortcuts.vdf')
        items.append((kind, key, value))
    return items, at


def dump(items):
    out = bytearray()
    for kind, key, value in items:
        out += bytes([kind]) + key.encode('utf-8') + b'\0'
        if kind == MAP:
            out += dump(value) + bytes([END])
        elif kind == STRING:
            out += str(value).encode('utf-8') + b'\0'
        elif kind == INT32:
            out += struct.pack('<i', value)
        elif kind == UINT64:
            out += struct.pack('<Q', value)
    return bytes(out)


def shortcut_id(exe, name):
    """The id Steam gives a non-Steam game: CRC32 of the quoted exe and the name, top bit set."""
    return (zlib.crc32((exe + name).encode('utf-8')) & 0xffffffff) | 0x80000000


def entry(name, exe, start_dir, icon, options=''):
    quoted = f'"{exe}"'
    appid = shortcut_id(quoted, name)
    return [
        (INT32, 'appid', struct.unpack('<i', struct.pack('<I', appid))[0]),
        (STRING, 'AppName', name), (STRING, 'Exe', quoted), (STRING, 'StartDir', f'"{start_dir}"'),
        (STRING, 'icon', icon), (STRING, 'ShortcutPath', ''), (STRING, 'LaunchOptions', options),
        (INT32, 'IsHidden', 0), (INT32, 'AllowDesktopConfig', 1), (INT32, 'AllowOverlay', 1),
        (INT32, 'OpenVR', 0), (INT32, 'Devkit', 0), (STRING, 'DevkitGameID', ''),
        (INT32, 'DevkitOverrideAppID', 0), (INT32, 'LastPlayTime', 0), (STRING, 'FlatpakAppID', ''),
        (MAP, 'tags', []),
    ]


def add_shortcut(vdf_path, name, exe, start_dir, icon, options=''):
    """Adds the shortcut to one shortcuts.vdf, or updates the entry that starts the same exe.
    Returns 'added' or 'updated'. A copy of the old file is kept as shortcuts.vdf.bak."""
    vdf_path = Path(vdf_path)
    data = vdf_path.read_bytes() if vdf_path.exists() else b''
    root = parse(data)[0] if data else []
    shortcuts = next((value for kind, key, value in root if kind == MAP and key.lower() == 'shortcuts'), None)
    if shortcuts is None:
        shortcuts = []
        root.append((MAP, 'shortcuts', shortcuts))
    new = entry(name, exe, start_dir, icon, options)
    target = f'"{exe}"'.lower()
    for index, (kind, key, value) in enumerate(shortcuts):
        fields = {k.lower(): v for _t, k, v in value} if kind == MAP else {}
        if str(fields.get('exe', '')).lower() == target:
            # Keep what Steam or the player added (play time, tags).
            kept = {k.lower() for k in ('LastPlayTime', 'tags')}
            old = {k.lower(): (t, k, v) for t, k, v in value}
            shortcuts[index] = (MAP, key, [old[k.lower()] if k.lower() in kept and k.lower() in old else (t, k, v)
                                            for t, k, v in new])
            result = 'updated'
            break
    else:
        shortcuts.append((MAP, str(len(shortcuts)), new))
        result = 'added'
    if data:
        vdf_path.with_name(vdf_path.name + '.bak').write_bytes(data)
    vdf_path.parent.mkdir(parents=True, exist_ok=True)
    vdf_path.write_bytes(dump(root) + bytes([END]))
    return result


def backup(vdf_path, data, now=None):
    """A copy of the file as it was, named by the time of this write (shortcuts.vdf.<time>.bak):
    every write keeps its own."""
    stamp = time.strftime('%Y%m%d-%H%M%S', time.localtime(now))
    path = vdf_path.with_name(f'{vdf_path.name}.{stamp}.bak')
    n = 2
    while path.exists():
        path = vdf_path.with_name(f'{vdf_path.name}.{stamp}-{n}.bak')
        n += 1
    path.write_bytes(data)
    return path


def set_shortcuts(vdf_path, wanted, replaces=None, now=None):
    """Adds or updates several shortcuts that may start the same exe: wanted is a list of
    (name, exe, start_dir, icon, options), matched to existing entries by exe and launch options
    together. replaces(fields) marks old entries to remove (fields: lower-case keys); their slots
    take the new entries, so no other entry changes. Returns (results, backup path or None)."""
    vdf_path = Path(vdf_path)
    data = vdf_path.read_bytes() if vdf_path.exists() else b''
    root = parse(data)[0] if data else []
    shortcuts = next((value for kind, key, value in root if kind == MAP and key.lower() == 'shortcuts'), None)
    if shortcuts is None:
        shortcuts = []
        root.append((MAP, 'shortcuts', shortcuts))

    def fields(value):
        return {k.lower(): v for _t, k, v in value}

    def same(value, exe, options):
        f = fields(value)
        return str(f.get('exe', '')).lower() == f'"{exe}"'.lower() and str(f.get('launchoptions', '')) == options

    kept = ('lastplaytime', 'tags')  # what Steam or the player added
    free = [i for i, (kind, _key, value) in enumerate(shortcuts)
            if kind == MAP and replaces and replaces(fields(value))
            and not any(same(value, w[1], w[4]) for w in wanted)]
    results = []
    for name, exe, start_dir, icon, options in wanted:
        new = entry(name, exe, start_dir, icon, options)
        index = next((i for i, (kind, _key, value) in enumerate(shortcuts)
                      if kind == MAP and same(value, exe, options)), None)
        if index is not None:
            old = {k.lower(): (t, k, v) for t, k, v in shortcuts[index][2]}
            new = [old[k.lower()] if k.lower() in kept and k.lower() in old else (t, k, v) for t, k, v in new]
            shortcuts[index] = (MAP, shortcuts[index][1], new)
            results.append('updated')
        elif free:
            index = free.pop(0)
            shortcuts[index] = (MAP, shortcuts[index][1], new)
            results.append('replaced')
        else:
            numbers = [int(key) for _kind, key, _value in shortcuts if key.isdigit()]
            shortcuts.append((MAP, str(max(numbers) + 1 if numbers else 0), new))
            results.append('added')
    if free:  # more old entries than new ones: drop the rest and number the entries again
        for index in reversed(free):
            del shortcuts[index]
        shortcuts[:] = [(kind, str(n), value) for n, (kind, _key, value) in enumerate(shortcuts)]
    saved = backup(vdf_path, data, now) if data else None
    vdf_path.parent.mkdir(parents=True, exist_ok=True)
    vdf_path.write_bytes(dump(root) + bytes([END]))
    return results, saved


def steam_folder():
    """Steam's install folder from the registry, or None."""
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r'Software\Valve\Steam') as key:
            path = Path(winreg.QueryValueEx(key, 'SteamPath')[0])
    except OSError:
        return None
    return path if path.is_dir() else None


def user_configs(steam):
    """userdata/<account>/config folders of the accounts that used Steam on this PC."""
    users = Path(steam) / 'userdata'
    return [d / 'config' for d in sorted(users.iterdir()) if d.is_dir() and d.name.isdigit() and d.name != '0'] \
        if users.is_dir() else []


def steam_running():
    import subprocess
    result = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq steam.exe', '/NH'], capture_output=True,
                            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    return b'steam.exe' in result.stdout.lower()


if __name__ == '__main__':  # self-test on a temporary file
    import tempfile
    with tempfile.TemporaryDirectory() as folder:
        vdf = Path(folder) / 'shortcuts.vdf'
        print(add_shortcut(vdf, 'Bloodborne', r'C:\Games\bb\Play Bloodborne.exe', r'C:\Games\bb', 'x.exe'))
        print(add_shortcut(vdf, 'Other', r'C:\Other\o.exe', r'C:\Other', ''))
        print(add_shortcut(vdf, 'Bloodborne', r'C:\Games\bb\Play Bloodborne.exe', r'C:\Games\bb', 'y.exe'))
        root = parse(vdf.read_bytes())[0]
        print([[f for f in e[2] if f[1] in ('AppName', 'icon', 'appid')] for e in root[0][2]])
        assert dump(root) + bytes([END]) == vdf.read_bytes()
    print(os.path.basename(__file__), 'ok')
