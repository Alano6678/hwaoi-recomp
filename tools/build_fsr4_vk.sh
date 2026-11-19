#!/usr/bin/env bash
# Build fsr4-vk on this machine and put amd_fidelityfx_upscaler_vk.so next to the game, for HWAOI_TEMPORAL=fsr on Linux.
# Ported from totk-pc-recomp (branch linux-fsr4-vk, tools/build_fsr4_vk.sh).
#
#   tools/build_fsr4_vk.sh --dest <game folder> [--work <build folder>] [--jobs N]
#
# fsr4-vk (https://github.com/Astyyyyy/fsr4-vk) is an independent open-source Vulkan implementation of AMD's FSR 4.1.1
# (FP8 and INT8 networks). It is licensed GPL-3.0, so this project never copies, links or ships it: the game loads the
# finished .so at run time through the plain ffx-api C interface (runtime/vk_ffx.c), and this script builds that file
# from upstream's source on your own machine. The network weights and shaders are extracted from AMD's own
# amd_fidelityfx_upscaler_dx12.dll of FidelityFX SDK 2.3.0 (version 4.1.1.2740, which AMD's licence lists as MIT).
#
# What it does: clone upstream at the pinned commit below with its submodules; download AMD's SDK 2.3.0 sample package
# (about 127 MB), take that one DLL out of it and check its SHA-256; run upstream's tools/setup.py on it and its
# tools/build/linux.sh; copy the .so into --dest. Needs git, curl, cmake >= 3.22.1, a C++20 compiler, llvm-objdump, and
# Python 3.14 (or any Python 3.10+ with the zstandard package). Plan for several GB of disk and some tens of minutes.
set -euo pipefail

PIN=b0125ea9798633750f83ebabbc00279bcb49d8dd   # upstream main of 2026-10-26: the commit this was written against
REPO=https://github.com/Astyyyyy/fsr4-vk.git
SDK_ZIP_URL=https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/download/v2.3.0/FidelityFX-Samples-v2.3.0-prebuilt.zip
SDK_DLL_MEMBER=Samples/Upscalers/FidelityFX_FSR/dx12/x64/Release/amd_fidelityfx_upscaler_dx12.dll
SDK_DLL_SHA256=d0dcccc74a43c44ba435b7a369b456e0970d8a4464e4bd683119b374f2c9fb46
OUT_NAME=amd_fidelityfx_upscaler_vk.so

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dest="" work="$here/work/fsr4-vk-build" jobs="$(nproc 2>/dev/null || echo 4)"
while [ $# -gt 0 ]; do
    case "$1" in
        --dest) dest="${2:?--dest needs a folder}"; shift 2;;
        --work) work="${2:?--work needs a folder}"; shift 2;;
        --jobs) jobs="${2:?--jobs needs a number}"; shift 2;;
        -h|--help) sed -n '2,19p' "$0"; exit 0;;
        *) echo "unknown argument: $1 (see --help)" >&2; exit 2;;
    esac
done
[ -n "$dest" ] || { echo "usage: tools/build_fsr4_vk.sh --dest <game folder>   (the folder with the game executable, e.g. work/build/hwaoi196608)" >&2; exit 2; }
[ -d "$dest" ] || { echo "no such folder: $dest" >&2; exit 2; }

# ---- prerequisites, all checked before anything big is downloaded
need() { command -v "$1" >/dev/null 2>&1 || { echo "missing: $1 ($2)" >&2; missing=1; }; }
missing=0
need git "to fetch fsr4-vk"; need curl "to download AMD's SDK package"; need cmake "version 3.22.1 or newer"
need llvm-objdump "part of LLVM; the setup tool reads the DLL with it"
command -v c++ >/dev/null 2>&1 || command -v g++ >/dev/null 2>&1 || command -v clang++ >/dev/null 2>&1 \
    || { echo "missing: a C++20 compiler (g++ or clang++)" >&2; missing=1; }
py=""
for cand in "$here/.venv-py314/bin/python" python3.14 python3; do
    if command -v "$cand" >/dev/null 2>&1 || [ -x "$cand" ]; then py="$cand"; break; fi
done
[ -n "$py" ] || { echo "missing: python3" >&2; missing=1; }
if [ -n "$py" ]; then
    "$py" - <<'PY' || missing=1
import sys
if sys.version_info < (3, 10):
    sys.exit("Python 3.10 or newer is needed")
if sys.version_info < (3, 14):
    try:
        import zstandard   # noqa: F401
    except ImportError:
        sys.exit("this Python is older than 3.14: run  python3 -m pip install zstandard  or use Python 3.14")
PY
fi
cv="$(cmake --version 2>/dev/null | head -1 | grep -o '[0-9]*\.[0-9]*\.[0-9]*' || true)"
if [ -n "$cv" ] && [ "$(printf '%s\n3.22.1\n' "$cv" | sort -V | head -1)" != "3.22.1" ]; then
    echo "cmake $cv is older than 3.22.1" >&2; missing=1
fi
[ "$missing" = 0 ] || { echo "install the missing pieces and run again" >&2; exit 1; }

mkdir -p "$work"
work="$(cd "$work" && pwd)" dest="$(cd "$dest" && pwd)"   # absolute: the steps below run from inside the clone
src="$work/fsr4-vk"

# ---- 1. upstream at the pinned commit, with submodules
if [ ! -d "$src/.git" ]; then
    git init -q "$src"
    git -C "$src" remote add origin "$REPO"
fi
if [ "$(git -C "$src" rev-parse HEAD 2>/dev/null || true)" != "$PIN" ]; then
    echo "== fetching fsr4-vk $PIN"
    git -C "$src" fetch -q --depth 1 origin "$PIN"
    git -C "$src" checkout -q --force FETCH_HEAD
fi
echo "== submodules"
git -C "$src" submodule update --init --recursive --depth 1 -q

# ---- 2. AMD's DLL from the SDK 2.3.0 package
dll="$work/amd_fidelityfx_upscaler_dx12.dll"
if [ ! -f "$dll" ] || [ "$(sha256sum "$dll" | cut -d' ' -f1)" != "$SDK_DLL_SHA256" ]; then
    zip="$work/FidelityFX-Samples-v2.3.0-prebuilt.zip"
    if [ ! -f "$zip" ]; then
        echo "== downloading AMD's FidelityFX SDK 2.3.0 sample package (about 127 MB)"
        curl -fL --retry 3 -o "$zip.part" "$SDK_ZIP_URL"
        mv "$zip.part" "$zip"
    fi
    "$py" - "$zip" "$SDK_DLL_MEMBER" "$dll" <<'PY'
import shutil, sys, zipfile
zip_path, member, out = sys.argv[1:4]
with zipfile.ZipFile(zip_path) as z:
    try:
        src = z.open(member)
    except KeyError:
        sys.exit(f"{member} is not in {zip_path}")
    with src, open(out, 'wb') as f:
        shutil.copyfileobj(src, f)
PY
    got="$(sha256sum "$dll" | cut -d' ' -f1)"
    [ "$got" = "$SDK_DLL_SHA256" ] || { echo "SHA-256 of the extracted DLL is $got, expected $SDK_DLL_SHA256" >&2; rm -f "$dll"; exit 1; }
fi

# ---- 3. upstream's own asset setup and build
echo "== extracting the network and translating its shaders (fsr4-vk tools/setup.py)"
( cd "$src" && "$py" tools/setup.py --dll "$dll" )
echo "== building (fsr4-vk tools/build/linux.sh, $jobs jobs)"
( cd "$src" && FSR4_BUILD_JOBS="$jobs" bash tools/build/linux.sh )

built="$src/build/linux/$OUT_NAME"
[ -f "$built" ] || { echo "the build produced no $built" >&2; exit 1; }
cp -f "$built" "$dest/$OUT_NAME"
echo "== done: $dest/$OUT_NAME"
echo "   run with HWAOI_TEMPORAL=fsr   (HWAOI_FSR_MODEL = auto, fp8 or int8 picks the network)"
