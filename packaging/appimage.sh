#!/usr/bin/env bash
# Bundle a clean Linux build and its Nix runtime closure as an Uncharted AppImage.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
[[ -f out/bb-probe && -f out/bb-gpu-capabilities && -f out/gpu/libbbgpu.so && -f out/VERSION ]] ||
    { echo 'Build first inside shell.nix: bash build.sh' >&2; exit 1; }
command -v nix >/dev/null || { echo 'Need Nix with nix-command and flakes enabled.' >&2; exit 1; }
command -v nix-instantiate >/dev/null || { echo 'Need nix-instantiate.' >&2; exit 1; }
version=$(git describe --tags --match 'v*' --always)
if ! git diff-index --quiet --ignore-submodules=dirty HEAD --; then
    echo 'Commit source changes before packaging a release build.' >&2
    exit 1
fi
if [[ $(cat out/VERSION) != "$version" ]]; then
    echo "out/VERSION does not match this checkout ($version); rebuild first." >&2
    exit 1
fi
root=$PWD
# The ELF RUNPATHs identify Nix store libraries that the bundler must retain.
{
    echo '['
    for elf in out/bb-probe out/gpu/libbbgpu.so; do
        readelf -d "$elf" | sed -n 's/.*\[\(.*\)\]/\1/p' | tr ':' '\n'
    done | grep -o '^/nix/store/[^/]*' | sort -u | grep -v -- '-nix-shell$' | sed 's/.*/  "&"/'
    echo ']'
} > packaging/runtime-paths.nix
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
(cd "$work" && nix bundle --impure --bundler github:ralismark/nix-appimage \
    --expr "import $root/packaging {}")
mkdir -p dist
asset="Uncharted-${version}-x86_64.AppImage"
built=$(find "$work" -maxdepth 1 -name '*.AppImage' -print -quit)
[[ -n $built ]] || { echo 'nix bundle produced no AppImage.' >&2; exit 1; }
install -m755 "$built" "dist/$asset"
(cd dist && sha256sum "$asset" > "$asset.sha256" && sha256sum --check "$asset.sha256")
nixpkgs=$(nix-instantiate --eval --raw -E '(import <nixpkgs> {}).lib.version')
nixpkgs_path=$(nix-instantiate --eval --raw -E 'builtins.toString <nixpkgs>')
printf 'Uncharted %s\ncommit %s\nnixpkgs %s\nnixpkgs_path %s\n' \
    "$version" "$(git rev-parse HEAD)" "$nixpkgs" "$nixpkgs_path" > "dist/$asset.build.txt"
ls -lh "dist/$asset"
