#!/usr/bin/env bash
# Linux launcher for the source tree and the packaged AppImage.

PORT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ ${1:-} == --help || -z ${BB_GAME_DIR:-} ]]; then
    echo "Usage: BB_GAME_DIR=/path/to/CUSA02320 ${APPIMAGE:-$0} [probe options]"
    echo "Set BB_EXE=u1, u2 or u3 to select a game. Supply your own decrypted game files."
    [[ ${1:-} == --help ]] && exit 0
    exit 2
fi
GAME="$BB_GAME_DIR"
EXE_NAME="${BB_EXE:-u1}"

case "$EXE_NAME" in
    u1) EXE="eboot.bin" ;;
    u2) EXE="big2-ps4_Shipping.elf" ;;
    u3) EXE="big3-ps4_Shipping.elf" ;;
    *)  EXE="$EXE_NAME" ;;
esac

DATA_ROOT="${BB_DATA_DIR:-$PORT}"
USER_DIR="${BB_USER_DIR:-$DATA_ROOT/user}"
OUT_ROOT="$DATA_ROOT/out"
OUT="$OUT_ROOT/${EXE%.*}"
PROBE="$PORT/out/bb-probe"
[[ -x $PORT/bin/bb-probe ]] && PROBE="$PORT/bin/bb-probe"

if [ ! -f "$GAME/eboot.bin" ]; then
    echo "ERRO: não achei eboot.bin em $GAME" >&2
    exit 1
fi
if [ ! -x "$PROBE" ]; then
    echo "ERRO: bb-probe não encontrado em $PROBE" >&2
    exit 1
fi

mkdir -p "$OUT" "$USER_DIR"

export PYTHONUTF8=1
export PYTHONIOENCODING=utf-8
export BB_CONFIG="${BB_CONFIG:-$DATA_ROOT/bbport.ini}"
export BB_INPUT_CONFIG="${BB_INPUT_CONFIG:-$DATA_ROOT/input.ini}"
if [[ -n ${BB_DATA_DIR:-} ]]; then
    export BB_FSR4_DIR="${BB_FSR4_DIR:-$DATA_ROOT/fsr4_shaders}"
    export BB_FSR411_DIR="${BB_FSR411_DIR:-$DATA_ROOT/fsr4_411}"
fi
export BB_GPU_USER_DIR="$USER_DIR"
export BB_VBLANK_HZ="${BB_VBLANK_HZ:-60}"
export BB_VELOCITY_PS="${BB_VELOCITY_PS:-f4b18264}"
export BB_UPSCALE_BEFORE_CS="${BB_UPSCALE_BEFORE_CS:-aec539f0}"
export BB_HUDLESS_PS="${BB_HUDLESS_PS:-484cdbf5}"
export BB_LOADEXEC_FILE="$OUT_ROOT/loadexec.txt"

prepare_all() {
    echo "Preparando $EXE ..."
    python3 "$PORT/scripts/prepare.py" "$GAME" --out "$OUT" --exe "$EXE"
    python3 "$PORT/scripts/link_modules.py" "$GAME" --out "$OUT" --exe "$EXE"
    python3 "$PORT/scripts/content_profile.py" "$GAME" --out "$OUT" --sku "${BB_CONTENT_SKU:-full}"
}

prepare_all

while true; do
    rm -f "$BB_LOADEXEC_FILE"
    "$PROBE" "$OUT/boot-linked.bin" \
        --content-profile "$OUT/content.bin" \
        --app0 "$GAME" \
        --user "$USER_DIR" \
        --timeout "${BB_TIMEOUT:-0}" \
        "$@"
    code=$?

    if [ "$code" -ne 101 ] || [ ! -f "$BB_LOADEXEC_FILE" ]; then
        exit "$code"
    fi

    NEW_EXE=$(basename "$(head -n 1 "$BB_LOADEXEC_FILE" | tr -d '\r')")
    if [ -z "$NEW_EXE" ]; then
        echo "ERRO: loadexec.txt vazio" >&2
        exit 1
    fi
    echo ">>> Trocando para $NEW_EXE"
    EXE="$NEW_EXE"
    OUT="$OUT_ROOT/${EXE%.*}"
    mkdir -p "$OUT"
    prepare_all
done
