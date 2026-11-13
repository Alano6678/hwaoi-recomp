#!/bin/bash
# Linux build of HWAoI Recomp from your own extracted game files: C generation, compile and link, then the managed
# shader translator. Everything it writes stays under the ignored work/ folder. docs/hwaoi/linux.md lists the packages.
#   tools/linux/build.sh [exefs dir] [romfs dir]      (defaults: work/game/exefs, work/game/romfs)
set -eo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
PY=.venv/bin/python
[ -x "$PY" ] || { echo "no .venv: python -m venv .venv && .venv/bin/pip install -r tools/requirements-hwaoi.txt" >&2; exit 1; }
EXEFS=${1:-work/game/exefs} ROMFS=${2:-work/game/romfs}
[ -d "$EXEFS" ] && [ -d "$ROMFS" ] || { echo "missing game files: $EXEFS, $ROMFS" >&2; exit 1; }
GEN=work/generated/hwaoi196608-full OUT=work/build/hwaoi196608
echo "== generate"
"$PY" tools/hwaoi.py --exefs "$EXEFS" generate --display-settings --out "$GEN" --limit 0 --jobs 0
echo "== compile and link"
"$PY" tools/build_game.py --version hwaoi196608 --gen "$GEN" --out "$OUT" --expected "$GEN/expected.json" \
    --exefs "$EXEFS" --romfs "$ROMFS" --profile work/saves/hwaoi196608 --full-build
echo "== shader translator"
if [ -e "$OUT/xlate-managed-irbanks" ]; then
    echo "kept: $OUT/xlate-managed-irbanks"
else
    # --online: the first build restores the .NET packages from nuget.org into work/toolchain/NuGet.
    "$PY" tools/build_xlate_hwaoi.py --out "$OUT/xlate-managed-irbanks" --no-vendor-reference --online
fi
echo "== done: tools/linux/play.sh"
