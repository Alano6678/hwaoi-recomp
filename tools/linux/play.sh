#!/bin/bash
# Starts the Linux build made by tools/linux/build.sh for normal play, with the settings tools/hwaoi.py
# launch_interactive passes on Windows, minus its diagnostic screenshots. Saves go to work/saves/hwaoi196608, the log
# to work/logs/play-<time>.log. Variables already set in the environment (HWAOI_FPS_LIMIT, SDL_VIDEODRIVER, ...) win.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
GAME=$ROOT/work/build/hwaoi196608
EXE=$GAME/hwaoi
[ -x "$EXE" ] || { echo "game not built: no $EXE (tools/linux/build.sh)" >&2; exit 1; }
[ -x "$GAME/xlate-managed-irbanks/totk_xlate" ] || { echo "shader translator missing in $GAME" >&2; exit 1; }
ZLIB=${HWAOI_ZLIB_DLL:-$(ldconfig -p | awk '/libz\.so\.1 .*x86-64/ {print $NF; exit}')}
[ -f "$ZLIB" ] || { echo "libz.so.1 not found; set HWAOI_ZLIB_DLL" >&2; exit 1; }
mkdir -p "$ROOT/work/saves/hwaoi196608" "$ROOT/work/logs"
export HWAOI_GPU=vulkan HWAOI_INPUT=${HWAOI_INPUT:-auto} HWAOI_RUMBLE=0 HWAOI_PROGRESS=1
export HWAOI_EXEFS=${HWAOI_EXEFS:-$ROOT/work/game/exefs} HWAOI_PROFILE_DIR=$ROOT/work/saves/hwaoi196608
export HWAOI_GSV_LOCAL_FRAME_POLICY=1 HWAOI_NATIVE_INFLATE=${HWAOI_NATIVE_INFLATE:-1} HWAOI_ZLIB_DLL=$ZLIB
export HWAOI_POOL_BACKPRESSURE=1 HWAOI_POOL_DIAG=0 HWAOI_CORE_STALL_DIAG=0
export HWAOI_IR_BANKS=1 HWAOI_XLATE=$GAME/xlate-managed-irbanks/totk_xlate
export HWAOI_PIPE_SYNC=0 HWAOI_PIPE_SKIP=strict HWAOI_PIPE_PREBUILD=1
export HWAOI_FPS_LIMIT=${HWAOI_FPS_LIMIT:-60}
# XWayland by default: on NVIDIA + Wayland, changing the window size or resolution from the menu ended the game with
# "wp_linux_drm_syncobj_surface_v1: error 3: Missing buffer". SDL_VIDEODRIVER=wayland goes back to native Wayland.
export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-x11}
cd "$GAME" && exec "$EXE" --play-experimental "$@" 2>&1 | tee "$ROOT/work/logs/play-$(date +%Y%m%d-%H%M%S).log"
