"""Run the real menu on a synthetic offscreen Vulkan image.

Usage: py -I tests/menu_preview_test.py [--cc clang++] [--deps DIR] [--out DIR]
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
# The Age of Decomp toolchain: AOD_TOOLCHAIN_DIR, else the repository's own
# (<project>/engine/source -> <project>/.cache/toolchain/windows). --cc and --deps pick any other.
TOOLCHAIN = Path(os.environ.get("AOD_TOOLCHAIN_DIR") or ROOT.parent.parent / ".cache" / "toolchain" / "windows")
DEFAULT_CC = TOOLCHAIN / "llvm/bin/clang++.exe"
DEFAULT_DEPS = TOOLCHAIN / "deps" if os.name == "nt" else Path("/usr")

# Rows of the Controls page above the first binding row with the preview's default devices (the keyboard and one
# Xbox pad): 2 devices, 5 player rows, the buttons the game shows, the menus' confirm button (with a gamepad), motion
# controls (greyed: an Xbox pad sends no gyro), rumble strength and curve, trigger rumble, dead zone, mouse, and the
# bindings' player row (the Button layout row is gone since 0.2.0).
CONTROLS_TO_BINDS = 16
ROW_HOVER = (700, 280)
CONTINUE = (1660, 968)
FULLSCREEN_SEGMENT = (1635, 223)
RAIL_SOUND = (250, 334)
SLIDER_START = (1370, 223)
SLIDER_END = (1580, 223)
SEGMENT_HOVER = (1635, 223)


def build(cc, deps, out):
    windows = os.name == "nt"
    sources = [
        "tests/menu_preview.cpp", "tests/menu_preview_stubs.cpp",
        "runtime/ui/menu.cpp", "runtime/ui/option_rows.cpp", "runtime/ui/binds_page.cpp",
        "runtime/ui_text.c", "runtime/cfg.c", "runtime/input_map.c",
        "runtime/platform/common.c", "runtime/platform/win32.c" if windows else "runtime/platform/linux.c",
        *("runtime/third_party/imgui/" + f + ".cpp" for f in
          ("imgui", "imgui_draw", "imgui_tables", "imgui_widgets", "backends/imgui_impl_vulkan")),
    ]
    common = ["-O2", "-g", "-Wall", "-fno-strict-aliasing", "-march=x86-64-v3",
              *(["-D_WIN32_WINNT=0x0A00"] if windows else []), "-I", str(ROOT / "runtime"),
              "-I", str(ROOT / "runtime/third_party/imgui"),
              "-I", str(deps / "include"), "-I", str(deps / "include/vma")]
    objdir = out / "objects"
    objdir.mkdir(parents=True, exist_ok=True)
    objects = []
    log = []
    for i, source in enumerate(sources):
        obj = objdir / f"{i}.o"
        command = [str(cc), *common, *(["-std=c++20"] if source.endswith(".cpp") else ["-x", "c", "-std=gnu11"]),
                   "-c", str(ROOT / source), "-o", str(obj)]
        result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace")
        log.append("$ " + subprocess.list2cmdline(command) + "\n" + result.stdout + result.stderr)
        if result.returncode:
            (out / "build.log").write_text("\n".join(log), encoding="utf-8")
            print(result.stderr, file=sys.stderr)
            raise RuntimeError(f"compile failed: {source}")
        objects.append(str(obj))
    exe = out / ("menu-preview.exe" if windows else "menu-preview")
    libs = (["-lvulkan-1", "-lpsapi", "-lbcrypt", "-luser32", "-lgdi32",
             "-lwinmm", "-lpdh", "-lrstrtmgr", "-lshell32", "-lole32",
             "-l:libc++.a", "-l:libc++abi.a", "-l:libunwind.a", "-l:libwinpthread.a"]
            if windows else ["-lvulkan", "-lSDL3", "-ldl", "-lpthread", "-lm"])
    command = [str(cc), *objects, "-o", str(exe), "-L", str(deps / "lib"), *libs]
    result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace")
    log.append("$ " + subprocess.list2cmdline(command) + "\n" + result.stdout + result.stderr)
    (out / "build.log").write_text("\n".join(log), encoding="utf-8")
    if result.returncode:
        print(result.stderr, file=sys.stderr)
        raise RuntimeError("link failed")
    return exe


def scenarios():
    yield "text", "text-check\n"
    yield "keyboard", """
open
key DownArrow tap
key RightArrow tap
expect WINDOW_SIZE=1366x768
shot keyboard-focus
expect-color 3fc6ff 8 100
expect-color e7b75a 8 100
key E tap
shot keyboard-tab-game
key Q tap
key RightArrow tap
expect FULLSCREEN=1
key Escape tap
expect-open 0
frames 2
expect-ini WINDOW_SIZE=1366x768
expect-ini FULLSCREEN=1
"""
    yield "gamepad", """
open
padtap DOWN
padtap UP
padtap A
expect FULLSCREEN=1
shot gamepad-focus
expect-color 3fc6ff 8 100
padtap R
shot gamepad-tab-game
stick 0 -1
frames 2
shot gamepad-stick-down
stick 0 0
padtap B
expect-open 0
"""
    yield "mouse-controls", f"""
open
click {FULLSCREEN_SEGMENT[0]} {FULLSCREEN_SEGMENT[1]}
expect FULLSCREEN=1
click {RAIL_SOUND[0]} {RAIL_SOUND[1]}
shot mouse-rail-sound
drag {SLIDER_START[0]} {SLIDER_START[1]} {SLIDER_END[0]} {SLIDER_END[1]}
expect VOLUME=285
shot mouse-slider
wheel -3
shot mouse-wheel
"""
    yield "mouse", f"""
open
mouse {ROW_HOVER[0]} {ROW_HOVER[1]}
shot mouse-hover
click {CONTINUE[0]} {CONTINUE[1]}
expect-open 0
"""
    yield "remap", """
open
key E tap
key E tap
key E tap
""" + "key DownArrow tap\n" * CONTROLS_TO_BINDS + """key Enter tap
key K tap
sleep 500
expect BIND_MENU=key:k, pad:default
expect-ini BIND_MENU=key:k, pad:default
"""
    yield "quit", "open\n" + "key DownArrow tap\n" * 30 + """
key Enter tap
expect-quit 1
"""
    yield "resize-and-cycle", """
open
size 1280x800
lang ru
shot resized-ru
close
key F10 tap
expect PERF_OVERLAY=fps
key F10 tap
expect PERF_OVERLAY=full
"""
    yield "remap-pad", """
open
key E tap
key E tap
key E tap
""" + "key DownArrow tap\n" * CONTROLS_TO_BINDS + """
key RightArrow tap
key RightArrow tap
key Enter tap
padtap A
sleep 500
expect BIND_MENU=key:default, pad:east
expect-ini BIND_MENU=key:default, pad:east
"""


def restart_scenarios():
    """The Restart button: shown while a setting read at start differs from its value at start, gone when it is back,
    and only when the launcher restarts the game (AOD_RESTART_EXIT); pressing it ends with the restart."""
    yield "restart-button", """
open
frames 2
expect-status 0
set OUTPUT_RES=2560x1440
frames 2
expect-status 2
shot restart-shown
set OUTPUT_RES=auto
frames 2
expect-status 0
shot restart-gone
set AUDIO_LATENCY=high
frames 2
expect-status 2
""" + "key DownArrow tap\n" * 30 + """
key LeftArrow tap
key Enter tap
expect-restart 1
expect-ini AUDIO_LATENCY=high
""", {"AOD_RESTART_EXIT": "75"}, 1920, 1080, "en"
    yield "restart-unavailable", """
open
frames 2
set OUTPUT_RES=2560x1440
frames 2
expect-status 2
shot restart-unavailable
""" + "key DownArrow tap\n" * 30 + """
key Enter tap
expect-restart 0
expect-quit 1
""", {"AOD_RESTART_EXIT": ""}, 1920, 1080, "en"
    # The narrowest bar: the longest words (Russian) at 720p, with the Restart button beside Quit and Continue.
    for lang in ("ru", "it"):
        yield "restart-" + lang + "-720p", """
open
frames 2
set OUTPUT_RES=2560x1440
frames 2
expect-status 2
shot restart-""" + lang + """-720p
""", {"AOD_RESTART_EXIT": "75"}, 1280, 720, lang


def shots():
    sizes = [(1280, 720, 96), (1280, 800, 96), (1920, 1080, 96),
             (2560, 1440, 96), (3440, 1440, 96), (3840, 2160, 96), (1280, 720, 144)]
    for lang in ("en", "ru", "it"):
        for w, h, dpi in sizes:
            suffix = "-dpi144" if dpi != 96 else ""
            commands = ["open", "frames 3"]
            for tab in range(6):
                commands.append(f"shot {lang}-{w}x{h}{suffix}-tab{tab}")
                if tab != 5:
                    commands.append("key E tap")
            yield f"shots-{lang}-{w}x{h}{suffix}", "\n".join(commands), w, h, dpi, lang
    yield "overlay", """
set PERF_OVERLAY=full
sleep 100
shot overlay-full
set PERF_OVERLAY=fps
sleep 650
shot overlay-fps
""", 1920, 1080, 96, "en"


def run(exe, out, name, script, w=1920, h=1080, dpi=96, lang="en", hook=False, extra_env=None, block_ini=False):
    scratch = Path(tempfile.mkdtemp(prefix=name + "-", dir=out / "runs"))
    local_exe = scratch / exe.name
    shutil.copy2(exe, local_exe)
    script_path = scratch / "scenario.txt"
    script_path.write_text(script, encoding="utf-8")
    command = [str(local_exe), "--size", f"{w}x{h}", "--dpi", str(dpi),
               "--lang", lang, "--script", str(script_path), "--out", str(out)]
    env = os.environ.copy()
    if extra_env:
        env.update(extra_env)
    if block_ini:
        (scratch / "hwaoi.ini").mkdir()
    if hook:
        hook_dir = scratch / "hook"
        hook_dir.mkdir()
        env["TOTK_MENU_TEST"] = "5:10"
        env["TOTK_MENU_TEST_DIR"] = str(hook_dir)
    try:
        result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=300, env=env)
        log = "$ " + subprocess.list2cmdline(command) + "\n" + result.stdout + result.stderr
        (scratch / "run.log").write_text(log, encoding="utf-8")
        if result.returncode:
            print(f"FAIL {name}: exit {result.returncode}; {scratch / 'run.log'}", flush=True)
            print(result.stderr, file=sys.stderr)
            return False
        if hook and len(list(hook_dir.glob("menu-test-*.png"))) != 6:
            print(f"FAIL {name}: expected six hook PNGs in {hook_dir}", flush=True)
            return False
    except subprocess.TimeoutExpired as e:
        (scratch / "run.log").write_text(str(e), encoding="utf-8")
        print(f"FAIL {name}: timeout; {scratch / 'run.log'}", flush=True)
        return False
    print(f"PASS {name}", flush=True)
    return True


def states():
    controls = "open\n" + "key E tap\n" * 3 + "key DownArrow tap\n" * CONTROLS_TO_BINDS
    for lang, w, h in (("en", 1920, 1080), ("it", 1280, 720)):
        suffix = f"{lang}-{w}x{h}"
        yield (f"state-bindings-{suffix}",
               controls + f"""
expect-capture 0
shot state-bindings-{suffix}
key Enter tap
expect-capture 1
expect-popup 1
shot state-capture-{suffix}
key Escape tap
sleep 500
expect-capture 0
""", w, h, 96, lang, None, False)
    advanced = "open\nkey Q tap\n"
    yield ("state-advanced-buttons",
           advanced + "key DownArrow tap\n" * 35 + """
expect-open 1
shot state-advanced-buttons
key Enter tap
expect-popup 1
shot state-reset-confirm
key Escape tap
expect-popup 0
expect-open 1
""", 1920, 1080, 96, "en", None, False)
    yield ("state-env-fixed", """
open
expect-env FULLSCREEN=1
expect-env-name FULLSCREEN=HWAOI_FULLSCREEN
key LeftArrow tap
expect FULLSCREEN=1
shot state-env-fixed
""", 1920, 1080, 96, "en", {"HWAOI_FULLSCREEN": "1"}, False)
    yield ("state-env-controls", """
open
key DownArrow tap
expect-env WINDOW_SIZE=1366x768
key LeftArrow tap
expect WINDOW_SIZE=1366x768
key DownArrow tap
key DownArrow tap
key DownArrow tap
key DownArrow tap
key DownArrow tap
key DownArrow tap
expect-env SHARPNESS=50
key RightArrow tap
expect SHARPNESS=50
shot state-env-controls
key E tap
key E tap
key E tap
key DownArrow tap
key DownArrow tap
key DownArrow tap
key DownArrow tap
key DownArrow tap
key DownArrow tap
key DownArrow tap
key DownArrow tap
expect-env MOTION=1
key LeftArrow tap
expect MOTION=1
shot state-env-toggle
""", 1920, 1080, 96, "en", {
               "HWAOI_WINDOW_SIZE": "1366x768", "HWAOI_SHARPNESS": "50", "HWAOI_MOTION": "1",
               "AOD_PREVIEW_PADS": "pro2"
           }, False)
    yield ("state-saved", """
open
key RightArrow tap
expect FULLSCREEN=1
sleep 500
expect-status 1
expect-ini FULLSCREEN=1
shot state-saved
""", 1920, 1080, 96, "en", None, False)
    yield ("state-restart", """
open
key DownArrow tap
key DownArrow tap
key DownArrow tap
key RightArrow tap
expect GPU_INDEX=0
expect-status 2
shot state-restart
""", 1920, 1080, 96, "en", None, False)
    yield ("state-restart-dpi144", advanced + """
key RightArrow tap
expect ASYNC_PREEMPT=2
expect-status 2
shot state-restart-dpi144
""", 1280, 720, 144, "en", None, False)
    for lang in ("ru", "it"):
        yield (f"state-restart-{lang}-dpi144", advanced + f"""
key RightArrow tap
expect ASYNC_PREEMPT=2
expect-status 2
shot state-restart-{lang}-dpi144
""", 1280, 720, 144, lang, None, False)
    yield ("state-save-failed", """
open
key RightArrow tap
expect FULLSCREEN=1
sleep 500
expect-status 3
shot state-save-failed
expect-color ff5a4f 8 20
""", 1920, 1080, 96, "en", None, True)
    # Frame generation: asked for at 6x on a card that reaches 4x. The row says what is in use; its list is Off, 2x to
    # 4x and the 6x that is set (5x, which the card cannot do, is not offered); another value needs a restart; the
    # overlay shows the frames shown and the ones drawn.
    for lang in ("en", "it"):
        yield (f"state-frame-gen-{lang}", """
set FRAME_GEN=6
open
""" + "key DownArrow tap\n" * 8 + f"""
expect FRAME_GEN=6
shot state-frame-gen-{lang}
key LeftArrow tap
expect FRAME_GEN=4
key LeftArrow tap
expect FRAME_GEN=3
expect-status 2
close
set PERF_OVERLAY=fps
sleep 650
shot state-frame-gen-overlay-{lang}
""", 1920, 1080, 96, lang, {"MENU_PREVIEW_FG": "6"}, False)
    # Greyed rows: Window size in fullscreen ("in use" the screen's size, 2560x1440 in the stubs), and the HDR rows on a
    # screen without HDR (MENU_PREVIEW_NO_HDR=1). A greyed row takes the cursor (its note shows) but no change. The HDR
    # case goes down past the page's last row to the action bar, then up once onto that row.
    for lang in ("en", "it"):
        yield (f"state-greyed-window-{lang}", f"""
set FULLSCREEN=1
set WINDOW_SIZE=1920x1080
open
key DownArrow tap
key RightArrow tap
expect WINDOW_SIZE=1920x1080
shot state-greyed-window-{lang}
""", 1920, 1080, 96, lang, None, False)
        yield (f"state-greyed-hdr-{lang}", """
set HDR=1
set HDR_UI_NITS=200
open
""" + "key DownArrow tap\n" * 40 + f"""
key UpArrow tap
key RightArrow tap
expect HDR_UI_NITS=200
shot state-greyed-hdr-{lang}
""", 1920, 1080, 96, lang, {"MENU_PREVIEW_NO_HDR": "1"}, False)
    yield ("state-segment-hover", f"""
open
mouse {SEGMENT_HOVER[0]} {SEGMENT_HOVER[1]}
expect FULLSCREEN=0
shot state-segment-hover
""", 1920, 1080, 96, "en", None, False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=str(DEFAULT_CC) if DEFAULT_CC.is_file() else shutil.which("clang++"))
    parser.add_argument("--deps", type=Path, default=DEFAULT_DEPS)
    parser.add_argument("--out", type=Path, default=ROOT / "build/tests/menu-preview")
    parser.add_argument("--shots-only", action="store_true")
    args = parser.parse_args()
    if not args.cc:
        parser.error("pass --cc with clang++")
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out / "runs").mkdir(exist_ok=True)
    try:
        exe = build(args.cc, args.deps.resolve(), out)
    except RuntimeError as e:
        print(f"FAIL build: {e}; see {out / 'build.log'}", file=sys.stderr)
        return 1
    results = [run(exe, out, name, script, w, h, dpi, lang)
               for name, script, w, h, dpi, lang in shots()]
    for family, value in (("xbox", "3"), ("ps4", "5"), ("ps5", "6")):
        results.append(run(exe, out, "glyphs-" + family, "open\npadtap DOWN\nshot " + family + "-buttons\n",
                           extra_env={"AOD_PREVIEW_PAD_TYPE": value}))
    if not args.shots_only:
        results.extend(run(exe, out, name, script) for name, script in scenarios())
        results.append(run(exe, out, "menu-test-hook", "frames 80\nexpect-open 0\n", hook=True))
        results.extend(run(exe, out, name, script, w, h, 96, lang, extra_env=env)
                       for name, script, env, w, h, lang in restart_scenarios())
        results.extend(run(exe, out, name, script, w, h, dpi, lang, extra_env=env, block_ini=blocked)
                       for name, script, w, h, dpi, lang, env, blocked in states())
    return 0 if all(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
