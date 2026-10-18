#!/usr/bin/env bash
# Copies the third-party licenses named in packaging/windows/licenses/MANIFEST.txt
# into a package's licenses/ folder, and fails when a license for code that is
# linked or redistributed is missing. Run from packaging/windows/package.sh, or
# standalone against a working tree to see what would be collected:
#
#   bash packaging/windows/check_licenses.sh
#   bash packaging/windows/check_licenses.sh --stage out/stage/uncharted-windows
#
# Entries are copied. An entry whose source is absent is fatal, except for one
# marked `optional` (the DLSS bridge ships only when it has been built) and for
# an MSYS2 package with no license file on this machine, which is reported so
# that a trimmed toolchain does not break packaging.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
manifest=packaging/windows/licenses/MANIFEST.txt
[[ -f $manifest ]] || { echo "Missing $manifest" >&2; exit 1; }

# MSYS2 keeps package licenses under <prefix>/share/licenses/<package>/LICENSE.
msys_prefix=${MSYS2_ROOT:-/c/msys64}/clang64

stage=''
while [[ $# -gt 0 ]]; do
    case $1 in
        --stage) stage=${2:?--stage needs a directory}; shift 2;;
        *) echo "Unknown argument: $1" >&2; exit 2;;
    esac
done
[[ -n $stage ]] || stage=out/stage/uncharted-windows/licenses

missing=()
optional_missing=()
copied=0

while IFS='|' read -r name source kind; do
    # Skip comments and blank lines.
    [[ -z ${name// } || $name == \#* ]] && continue
    [[ -z ${source// } ]] && continue

    if [[ $kind == pkg ]]; then
        # MSYS2 names the license file inconsistently between packages (LICENSE,
        # LICENSE.txt, COPYING), and the package directory is sometimes capitalised
        # differently from the package name, so match case-insensitively.
        found=$(find "$msys_prefix/share/licenses" -maxdepth 2 -ipath "*/$source/*" \
                    -type f \( -iname 'LICENSE*' -o -iname 'COPYING*' -o -iname 'NOTICE*' \) \
                    2>/dev/null | head -1)
        # Not every MSYS2 package ships a license file. That is reported but
        # never fatal, so packaging still works on a trimmed toolchain.
        if [[ -z $found ]]; then
            optional_missing+=("$name (no license under $msys_prefix/share/licenses for $source)")
            continue
        fi
    else
        if [[ ! -f $source ]]; then
            # Optional components (the DLSS bridge) are only shipped when built.
            # Everything else is required: package.sh runs after build.sh, which
            # already needs the submodules.
            if [[ $kind == optional ]]; then
                optional_missing+=("$name ($source not present)")
            else
                missing+=("$name ($source)")
            fi
            continue
        fi
        found=$source
    fi

    mkdir -p "$stage"
    cp -f "$found" "$stage/$name"
    copied=$((copied + 1))
done < "$manifest"

echo "Licenses: $copied copied to $stage"
for entry in "${optional_missing[@]+"${optional_missing[@]}"}"; do
    echo "  not shipped: $entry" >&2
done
if [[ ${#missing[@]} -gt 0 ]]; then
    echo "Missing required licenses:" >&2
    for entry in "${missing[@]}"; do echo "  $entry" >&2; done
    echo "Fix the paths in $manifest, or run 'git submodule update --init --recursive'." >&2
    exit 1
fi