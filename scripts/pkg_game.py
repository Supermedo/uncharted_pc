"""Prepares Uncharted: The Nathan Drake Collection (CUSA02320) from its PS4 packages.

Usage: python scripts/pkg_game.py prepare --base <file.pkg> [--update <file.pkg>] --root <folder>
"""
import argparse
from collections import namedtuple
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import sys
import traceback

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pkg_info

TITLE = "CUSA02320"
PROFILES = {
    TITLE: {
        "name": "Uncharted: The Nathan Drake Collection",
        "content_id": "UP9000-CUSA02320_00-UNCHARTEDTRILOGY",
        "versions": {
            "1.00": {"app_ver": "01.00", "sfo_version": "01.00", "extracted_bytes": 62818556843,
                     "eboot_sha256": "8953b6ed5cf4d1d26aa3aa4cd1e0c72817ea155f6a5774fb17f6fec7d787f953"},
            "1.02": {"app_ver": "01.02", "sfo_version": "01.01", "extracted_bytes": 64982787751,
                     "eboot_sha256": "dc017b45da8978bb11a0a11d940e96d98fa07f3c066a3debba043714432212f8"},
        },
        # sce_sys/param.sfo is not in the package file system; it comes from the PKG metadata.
        "required_files": (
            "eboot.bin", "big2-ps4_Shipping.elf", "big3-ps4_Shipping.elf", "sce_discmap.plt",
            "sce_module/libc.prx", "sce_module/libSceFios2.prx", "sce_sys/param.sfo", "shareddata/chunks.txt",
            "u1data/data.psarc", "u2data/data.psarc", "u3data/data.psarc",
        ),
    },
}

LAYER_MARKER = ".pkg_layer.json"
VIEW_MARKER = ".pkg_view.json"
FREE_SPACE_MARGIN = 2 << 30
NOT_EXTRACTED = "the game has not been extracted yet"

Package = namedtuple("Package", "path info fingerprint")


class PackageError(Exception):
    """A package the user has to choose again; the message is shown as it is."""


class PrepareError(Exception):
    """The game folder cannot be prepared; the message is shown as it is."""


class Selection:
    def __init__(self, title, version, base, update=None):
        self.title, self.version, self.base, self.update = title, version, base, update

    @property
    def profile(self):
        return PROFILES[self.title]

    @property
    def sources(self):
        return {"base": self.base.fingerprint, "update": self.update.fingerprint if self.update else None}


def human(size):
    for unit in ("B", "KB", "MB", "GB"):
        if size < 1024 or unit == "GB":
            return f"{size:.1f} {unit}"
        size /= 1024


def read_json(path):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def write_json(path, value):
    path = Path(path)
    temp = path.with_name(path.name + ".tmp")
    temp.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
    os.replace(temp, path)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def is_file_nonempty(path):
    return path.is_file() and path.stat().st_size > 0


def fingerprint(path, info):
    stat = os.stat(path)
    return {"size": stat.st_size, "mtime_ns": stat.st_mtime_ns,
            "content_id": info["content_id"], "app_ver": info["app_ver"]}


def read_package(path, label, title):
    path = Path(path)
    if not path.is_file():
        raise PackageError(f"The {label} package was not found:\n{path}")
    try:
        info = pkg_info.inspect(str(path))
    except (OSError, ValueError, IndexError, struct.error) as error:
        raise PackageError(f"{path.name} is not a readable PS4 package.") from error
    info["kind"] = pkg_info.classify(info)
    info["title_id"] = pkg_info.title_id(info)
    info["app_ver"] = info["param_sfo"].get("APP_VER")
    if info["title_id"] != title:
        raise PackageError(f"{path.name} is for {info['title_id'] or 'an unknown title'}. "
                           f"The launcher supports {title} ({PROFILES[title]['name']}) only.")
    return Package(path, info, fingerprint(path, info))


def require_fake(package):
    if not package.info["fake_pkg"]:
        raise PackageError(f"{package.path.name} is not a fake PKG. The launcher currently supports "
                           "only the fake-PKG format, not original (retail) packages.")


def select_packages(base, update=None, title=TITLE):
    """The base package and the optional update, checked against the title's profile.
    Raises PackageError with the reason when they cannot be used."""
    profile = PROFILES[title]
    if not base:
        raise PackageError(f"Choose the base package: the 1.00 game ({title}).")
    base_package = read_package(base, "base", title)
    info = base_package.info
    if info["kind"] != "base":
        raise PackageError(f"{base_package.path.name} is not a base game package ({info['kind']}).")
    if info["content_id"] != profile["content_id"]:
        raise PackageError(f"{base_package.path.name} has content ID {info['content_id']}; "
                           f"the launcher supports {profile['content_id']} only.")
    base_version = profile["versions"]["1.00"]["app_ver"]
    if info["app_ver"] != base_version:
        raise PackageError(f"The base package is version {info['app_ver']}; {title} needs {base_version}.")
    require_fake(base_package)
    if not update:
        return Selection(title, "1.00", base_package)

    update_package = read_package(update, "update", title)
    info = update_package.info
    if info["kind"] != "patch":
        raise PackageError(f"{update_package.path.name} is not an update package ({info['kind']}).")
    if info["content_id"] != base_package.info["content_id"]:
        raise PackageError("The base and update packages are not for the same game: content IDs "
                           f"{base_package.info['content_id']} and {info['content_id']}.")
    update_version = profile["versions"]["1.02"]["app_ver"]
    if info["app_ver"] != update_version:
        raise PackageError(f"The update package is version {info['app_ver']}; {title} needs {update_version}.")
    require_fake(update_package)
    return Selection(title, "1.02", base_package, update_package)


def game_root(data_dir, title=TITLE):
    return Path(data_dir) / "game" / title


def view_dir(data_dir, title=TITLE):
    return game_root(data_dir, title) / "prepared"


def layer_action(layer, source):
    """'ready': the layer holds this package's finished unpack; 'resume': an unpack of it stopped; else 'fresh'."""
    marker = read_json(Path(layer) / LAYER_MARKER)
    if not marker or marker.get("source") != source:
        return "fresh"
    return "ready" if marker.get("complete") else "resume"


def remove_owned(folder, marker):
    """Deletes a folder this launcher made (it holds `marker`, or is empty); refuses anything else."""
    folder = Path(folder)
    if not folder.exists():
        return
    if (folder / marker).is_file():
        shutil.rmtree(folder)
    elif folder.is_dir() and not any(folder.iterdir()):
        folder.rmdir()
    else:
        raise PrepareError(f"{folder} holds files the launcher did not make. Move or delete that folder, "
                           "then extract the game again.")


def ensure_writable(folder):
    folder = Path(folder)
    try:
        folder.mkdir(parents=True, exist_ok=True)
        probe = folder / ".pkg_write_test"
        probe.write_bytes(b"")
        probe.unlink()
    except OSError as error:
        raise PrepareError(f"The extracted game folder is not available:\n{folder}\n"
                           f"{error.strerror or error}") from error


def ensure_space(folder, needed, label):
    free = shutil.disk_usage(folder).free
    if free < needed + FREE_SPACE_MARGIN:
        raise PrepareError(f"Not enough free space for the {label} package: about {human(needed)} needed, "
                           f"{human(free)} free on {Path(folder).anchor or folder}.")


def load_extractor():
    try:
        import pkg_extract
    except ImportError as error:
        raise PrepareError("PKG support needs the cryptography package (pip install cryptography).") from error
    return pkg_extract


def progress_printer(label, log):
    last = [-1]

    def report(done, total, rate):
        percent = done * 100 // max(total, 1)
        if percent >= last[0] + 5 or percent == 100 and last[0] != 100:
            last[0] = percent
            log(f"  {label}: {percent:3d}%  {human(done)} of {human(total)}  {rate:.0f} MB/s")
    return report


def unpack_layer(package, layer, label, log):
    action = layer_action(layer, package.fingerprint)
    if action == "ready":
        log(f"{label}: already extracted ({layer})")
        return
    extractor = load_extractor()
    if action == "fresh":
        remove_owned(layer, LAYER_MARKER)
    layer.mkdir(parents=True, exist_ok=True)
    write_json(layer / LAYER_MARKER, {"source": package.fingerprint, "complete": False})
    try:
        _total, remaining = extractor.plan(str(package.path), layer)
    except Exception as error:
        raise PrepareError(f"Reading the {label} package failed: {error}") from error
    ensure_space(layer, remaining, label)
    log(f"{label}: unpacking {package.path.name} -> {layer}")
    try:
        extractor.extract(str(package.path), layer, progress_printer(label, log))
    except Exception as error:
        raise PrepareError(f"Unpacking the {label} package failed: {error}") from error
    write_json(layer / LAYER_MARKER, {"source": package.fingerprint, "complete": True})
    log(f"{label}: done")


def _link(source, dest):
    try:
        os.link(source, dest)
        return "link"
    except OSError:
        shutil.copy2(source, dest)
        return "copy"


def _lay(source, dest, stats):
    """Lays `source` over the folder `dest`: files replace, folders merge (names ignore case, as on the console)."""
    existing = {entry.name.casefold(): entry for entry in dest.iterdir()}
    for item in sorted(source.iterdir()):
        if item.name == LAYER_MARKER or item.name.endswith((".part", ".tmp")):
            continue
        match = existing.get(item.name.casefold())
        if match is not None and match.is_dir() != item.is_dir():
            if match.is_dir():
                shutil.rmtree(match)
            else:
                match.unlink()
            stats["replaced"] += 1
            match = None
        if item.is_dir():
            target = match if match is not None else dest / item.name
            target.mkdir(exist_ok=True)
            _lay(item, target, stats)
            continue
        if match is not None:
            match.unlink()
            stats["replaced"] += 1
        stats[_link(item, dest / item.name)] += 1


def build_view(layers, view, description):
    """Creates `view` from the layer folders, base first. Hard links, so no game data is copied."""
    remove_owned(view, VIEW_MARKER)
    view.mkdir(parents=True)
    write_json(view / VIEW_MARKER, {"complete": False, "description": description,
                                    "layers": [str(layer) for layer in layers]})
    stats = {"link": 0, "copy": 0, "replaced": 0}
    for layer in layers:
        _lay(layer, view, stats)
    return stats


def view_problems(view, selection):
    """(missing files, mismatches) of a prepared game folder for this selection."""
    profile = selection.profile
    version = profile["versions"][selection.version]
    missing = [name for name in profile["required_files"] if not is_file_nonempty(view / name)]
    try:
        sfo = pkg_info.parse_sfo((view / "sce_sys" / "param.sfo").read_bytes())
    except (OSError, ValueError, IndexError, struct.error):
        sfo = {}
    mismatches = []
    identity = {"TITLE_ID": selection.title, "CONTENT_ID": profile["content_id"],
                "APP_VER": version["app_ver"], "VERSION": version["sfo_version"]}
    for key, wanted in identity.items():
        if sfo.get(key) != wanted:
            mismatches.append(f"sce_sys/param.sfo {key} is {sfo.get(key) or 'missing'}, not {wanted}")
    eboot = view / "eboot.bin"
    if is_file_nonempty(eboot) and sha256(eboot) != version["eboot_sha256"]:
        mismatches.append(f"eboot.bin is not the {selection.version} build")
    return missing, mismatches


def finish_view(view, selection, stats):
    marker = read_json(view / VIEW_MARKER) or {}
    write_json(view / VIEW_MARKER, {**marker, "complete": True, "title": selection.title,
                                    "version": selection.version, "sources": selection.sources, "stats": stats})


def extracted_bytes(selection):
    """Disk space the extracted layers of this selection take (measured from the packages, see PROFILES)."""
    return selection.profile["versions"][selection.version]["extracted_bytes"]


def check_prepared(root, selection):
    """None when the extracted game under `root` is ready for `selection`; otherwise the reason it is not."""
    view = Path(root) / "prepared"
    if not view.is_dir():
        return NOT_EXTRACTED
    marker = read_json(view / VIEW_MARKER)
    if not marker or not marker.get("complete"):
        return "the last extraction did not finish"
    if marker.get("version") != selection.version or marker.get("sources") != selection.sources:
        return "the packages changed since the game was extracted"
    missing, mismatches = view_problems(view, selection)
    if missing or mismatches:
        return "the extracted files are incomplete or changed"
    return None


def prepare(selection, root, log=print):
    """Unpacks what the selection needs into `root`, lays the update over the base and checks the result.
    Returns the prepared game folder."""
    root = Path(root)
    ensure_writable(root)
    view = root / "prepared"
    if check_prepared(root, selection) is None:
        log(f"already extracted: {view}")
        return view
    layers = [("base", selection.base)]
    if selection.update:
        layers.append(("update", selection.update))
    dirs = []
    for label, package in layers:
        layer = root / "extracted" / label
        unpack_layer(package, layer, label, log)
        dirs.append(layer)
    stats = build_view(dirs, view, f"{selection.title} {selection.version}")
    missing, mismatches = view_problems(view, selection)
    if missing:
        raise PrepareError("Required game files are missing after extraction: " + ", ".join(missing))
    if mismatches:
        raise PrepareError("The extracted game does not match the packages: " + "; ".join(mismatches))
    finish_view(view, selection, stats)
    log(f"extracted: {view} ({selection.title} {selection.version}: {stats['link']} linked, "
        f"{stats['copy']} copied, {stats['replaced']} replaced by the update)")
    return view


def report_failure(error):
    cause = error.__cause__
    if cause is not None:
        print(f"cause: {type(cause).__name__}: {cause}", flush=True)
    print("error: " + str(error).replace("\n", " "), flush=True)
    return 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    prepare_command = commands.add_parser("prepare", help="unpack the packages into the extracted game folder")
    prepare_command.add_argument("--base", required=True)
    prepare_command.add_argument("--update")
    prepare_command.add_argument("--root", required=True, type=Path)
    prepare_command.add_argument("--title", default=TITLE, choices=sorted(PROFILES))
    args = parser.parse_args(argv)
    try:
        selection = select_packages(args.base, args.update, args.title)
        prepare(selection, args.root, lambda text: print(text, flush=True))
        return 0
    except (PackageError, PrepareError) as error:
        return report_failure(error)
    except Exception as error:
        traceback.print_exc(file=sys.stdout)
        return report_failure(PrepareError("Extraction failed; see the log for details."))


if __name__ == "__main__":
    sys.exit(main())
