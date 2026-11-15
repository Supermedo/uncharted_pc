#!/bin/bash
# Steam Deck: starts the native Linux build (the Steam Deck AppImage) with handheld settings.
# Add this script to Steam as a non-Steam game (docs/STEAM_DECK.md); it opens the collection menu,
# or one game directly with u1/u2/u3 in the shortcut's launch options.
# Set GAME_DIR to your decrypted dump (the folder with eboot.bin, big2-ps4_Shipping.elf, u1data, ...).
GAME_DIR=${BB_GAME_DIR:-"$HOME/Games/CUSA02344"}
APPIMAGE=${BB_APPIMAGE:-"$HOME/Applications/Uncharted-SteamDeck-x86_64.AppImage"}

# Steam preloads its overlay (it needs the host libGL, which the self-contained AppImage lacks) and
# its runtime library path: both stop the AppImage before it starts. Steam Input does not need them.
unset LD_PRELOAD LD_LIBRARY_PATH
export BB_DATA_DIR=${BB_DATA_DIR:-$HOME/.local/share/uncharted-native}
export BB_GAME_DIR=$GAME_DIR BB_EXE=${1:-u1}
export BB_INPUT_CONFIG=$BB_DATA_DIR/input.ini BB_LANGUAGE=${BB_LANGUAGE:-1} BB_FULLSCREEN=1 BB_PRESENT_MODE=Mailbox
# 1280x800: the Deck screen's own size and 16:10 shape (the games draw a taller view, no bars or
# scaling); 120 Hz game timing (a late frame waits 8 ms more instead of 16).
export BB_VBLANK_HZ=120 BB_WINDOW_SIZE=1280x800 BB_GAME_RES=1280x800 BB_FSR1=1 BB_RCAS=1 BB_RCAS_ATTENUATION=500
export BB_UPSCALER=off BB_FRAME_GEN=off BB_BUTTON_PROMPTS=auto
export BB_JOB_SLEEP=1
mkdir -p "$BB_DATA_DIR"
exec "$APPIMAGE" > "$BB_DATA_DIR/last_run.log" 2>&1
