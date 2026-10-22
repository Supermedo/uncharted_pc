#!/usr/bin/env bash
# Linux entry point for Uncharted: The Nathan Drake Collection.
# Usage: BB_GAME_DIR=/path/to/Game ./run-linux.sh [bb-probe arguments]
#        ./run-linux.sh /path/to/Game [bb-probe arguments]
set -euo pipefail
cd -- "$(dirname -- "$0")"

if [[ $# -gt 0 && -z ${BB_GAME_DIR:-} && -f $1/eboot.bin ]]; then
    export BB_GAME_DIR=$1
    shift
fi
export BB_GAME_DIR=${BB_GAME_DIR:-../CUSA03173}
if [[ ! -f $BB_GAME_DIR/eboot.bin ]]; then
    echo "No eboot.bin in $BB_GAME_DIR (set BB_GAME_DIR or pass the game folder)." >&2
    exit 1
fi

if [[ -z ${BB_PROBE:-} ]]; then
    if [[ -x out/bb-probe ]]; then
        BB_PROBE=$PWD/out/bb-probe
    elif [[ -x bin/bb-probe ]]; then
        BB_PROBE=$PWD/bin/bb-probe
    else
        echo 'bb-probe not built: run "bash build.sh" first.' >&2
        exit 1
    fi
fi
export BB_PROBE

libs=()
for dir in out/lib out/gpu bin/lib; do
    [[ -d $dir ]] && libs+=("$PWD/$dir")
done
if ((${#libs[@]})); then
    LD_LIBRARY_PATH=$(IFS=:; echo "${libs[*]}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}")
    export LD_LIBRARY_PATH
fi

exec "${PYTHON:-python3}" run.py "$@"
