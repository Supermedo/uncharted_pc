#!/usr/bin/env bash
# Adaptação Linux do run.py do uncharted_pc (Windows)

PORT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GAME="${BB_GAME_DIR:?Defina BB_GAME_DIR com a pasta do jogo (com eboot.bin)}"
EXE_NAME="${BB_EXE:-u1}"

case "$EXE_NAME" in
    u1) EXE="eboot.bin" ;;
    u2) EXE="big2-ps4_Shipping.elf" ;;
    u3) EXE="big3-ps4_Shipping.elf" ;;
    *)  EXE="$EXE_NAME" ;;
esac

USER_DIR="${BB_USER_DIR:-$PORT/user}"
OUT_ROOT="$PORT/out"
OUT="$OUT_ROOT/${EXE%.*}"
PROBE="$PORT/out/bb-probe"

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
export BB_CONFIG="${BB_CONFIG:-$PORT/bbport.ini}"
export BB_INPUT_CONFIG="${BB_INPUT_CONFIG:-$PORT/input.ini}"
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
