#!/usr/bin/env bash
# Builds dist/uncharted-windows/ (and dist/uncharted-windows.zip): Uncharted.exe (the launcher,
# frozen with PyInstaller so players need no Python), Play Uncharted.exe, bb-probe.exe with the
# MSYS2 CLANG64 DLLs it needs, the preparation scripts and run.py. Run from an MSYS2 CLANG64
# shell after `bash build.sh`. Freezing uses a Windows Python 3.10+ (python.org; WINPYTHON
# overrides) and a private venv in out/pyenv with PyInstaller and Pillow.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
source ./msys2-env.sh
[[ -f out/bb-probe.exe && -f out/bb-gpu-capabilities.exe && -f out/bb-play.exe ]] || { echo 'Build first: bash build.sh' >&2; exit 1; }

# A Windows Python (not MSYS2's) for PyInstaller.
python=${WINPYTHON:-}
if [[ -z $python ]]; then
    for candidate in /c/Python3*/python.exe "${LOCALAPPDATA:-/c/Users/$USER/AppData/Local}"/Programs/Python/Python3*/python.exe; do
        [[ -x $candidate ]] && python=$candidate
    done
fi
[[ -n $python ]] || { echo 'Need a Windows Python 3 (python.org) or WINPYTHON=path\to\python.exe' >&2; exit 1; }
# Windows Python needs USERPROFILE (some MSYS2 shells start without it).
export USERPROFILE=${USERPROFILE:-$(cygpath -w "/c/Users/$(id -un)")}
if [[ ! -x out/pyenv/Scripts/python.exe ]]; then
    "$python" -m venv out/pyenv
fi
out/pyenv/Scripts/python.exe -m pip install -q --disable-pip-version-check pyinstaller pillow
# The scripts run inside Uncharted.exe (--script): the standard modules they import come along.
hidden=()
for module in argparse base64 collections hashlib json re shutil struct tempfile zlib ctypes.wintypes cheats \
              uncharted_art uncharted_fonts uncharted_widgets bbport_steam PIL.ImageTk; do
    hidden+=(--hidden-import "$module")
done
out/pyenv/Scripts/python.exe -m PyInstaller --noconfirm --clean --log-level WARN --windowed \
    --name Uncharted --icon "$(cygpath -w "$PWD/launcher/uncharted.ico")" --distpath out/pyi-dist \
    --workpath out/pyi-work --specpath out/pyi-work --paths "$(cygpath -w "$PWD/scripts")" \
    --paths "$(cygpath -w "$PWD/launcher")" \
    --add-data "$(cygpath -w "$PWD/launcher/fonts");fonts" \
    --add-data "$(cygpath -w "$PWD/launcher/covers");covers" \
    "${hidden[@]}" \
    "$(cygpath -w "$PWD/launcher/uncharted_launcher.py")"

# The package is assembled in a fresh staging folder and zipped from there; dist/uncharted-windows
# (a playable copy that may hold saves and settings) is only refreshed afterwards.
dest=out/stage/uncharted-windows
rm -rf -- out/stage
mkdir -p "$dest/bin" "$dest/launcher" "$dest/scripts"
cp -r out/pyi-dist/Uncharted/. "$dest/"
llvm-strip -o "$dest/Play Uncharted.exe" out/bb-play.exe
cp launcher/uncharted.ico launcher/uncharted.png "$dest/launcher/"
# The executables without debug information (out/ keeps the symbols for crash reports).
for exe in bb-probe.exe bb-gpu-capabilities.exe; do
    llvm-strip --strip-debug -o "$dest/bin/$exe" "out/$exe"
done
# Every DLL the executables load from the CLANG64 tree (SDL3, FFmpeg, Vulkan loader, ...).
ldd "$dest/bin/bb-probe.exe" "$dest/bin/bb-gpu-capabilities.exe" |
    awk '/\/clang64\/bin\// {print $3}' | sort -u | while read -r dll; do
        cp -u "$dll" "$dest/bin/"
    done
# Only what run.py runs to prepare a game (Bloodborne's patch and mod tools stay out).
cp scripts/prepare.py scripts/link_modules.py scripts/link_libc.py scripts/content_profile.py "$dest/scripts/"
cp run.py LICENSE "$dest/"
cp packaging/windows/README-Uncharted.txt "$dest/README.txt"
mkdir -p "$dest/licenses"
cp packaging/windows/licenses/*.txt "$dest/licenses/" 2>/dev/null || true
# FSR 4 (the v07 INT8 model) when its assets are in fsr4_shaders/ (tools/fetch_fsr4_assets.sh).
if [[ -d fsr4_shaders ]]; then cp -r fsr4_shaders "$dest/"; fi
# DLSS and DLSS Frame Generation (NVIDIA RTX): the MSVC-built bridge and NVIDIA's runtimes, next
# to bb-probe.exe (packaging/windows/build_dlss.sh). Without them those options stay unavailable.
if [[ -f out/bbport_dlss.dll && -f out/nvngx_dlss.dll ]]; then
    cp out/bbport_dlss.dll out/nvngx_dlss.dll "$dest/bin/"
    if [[ -f out/nvngx_dlssg.dll ]]; then cp out/nvngx_dlssg.dll "$dest/bin/"; fi
    cp out/NVIDIA-DLSS-LICENSE.txt "$dest/licenses/"
    cp gpu/dlss_bridge/LICENSE.txt "$dest/licenses/bbport_dlss-LICENSE.txt"
else
    echo "DLSS bridge not built (packaging/windows/build_dlss.sh): no DLSS in this package" >&2
fi
for license in gpu/third_party/wepoll/LICENSE:wepoll gpu/third_party/httplib/LICENSE:cpp-httplib; do
    if [[ -f ${license%%:*} ]]; then cp "${license%%:*}" "$dest/licenses/${license##*:}-LICENSE.txt"; fi
done
for package in protobuf abseil-cpp openssl miniupnpc zlib; do
    license=/clang64/share/licenses/$package/LICENSE
    if [[ -f $license ]]; then cp "$license" "$dest/licenses/$package-LICENSE.txt"; fi
done
find "$dest" -name __pycache__ -prune -exec rm -r {} +
mkdir -p dist
rm -f dist/uncharted-windows.zip
(cd out/stage && powershell -NoProfile -Command \
    "Compress-Archive -Path uncharted-windows -DestinationPath ../../dist/uncharted-windows.zip")

# Refresh dist/uncharted-windows, keeping what players create there (saves, settings), and only
# while nothing runs from it: deleting a running launcher's files breaks it.
play=dist/uncharted-windows
running=$(powershell -NoProfile -Command \
    "@(Get-Process | Where-Object { \$_.Path -like '$(cygpath -w "$PWD/$play")\\*' }).Count" | tr -d '\r')
if [[ ${running:-0} != 0 ]]; then
    echo "dist/uncharted-windows is in use ($running processes): not refreshed; the zip is ready." >&2
else
    mkdir -p "$play"
    find "$play" -mindepth 1 -maxdepth 1 ! -name user ! -name out ! -name art ! -name bbport.ini \
        ! -name input.ini ! -name last_run.log -exec rm -rf {} +
    cp -r "$dest/." "$play/"
fi
du -sh "$dest" dist/uncharted-windows.zip
