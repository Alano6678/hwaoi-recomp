"""Local NUP-to-native build workflow used by the portable compiler window."""
from __future__ import annotations
import argparse
import contextlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import traceback

SOURCE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SOURCE / "tools"))
from hwrecomp.nup import Nup, extract_exefs
import hwaoi

STATE = "hwaoi-compiler.json"
RESOLUTIONS = ("1280x720", "1920x1080", "2560x1440", "3840x2160")


def require(ok, message):
    if not ok:
        raise ValueError(message)


def save(path, value):
    pending = path.with_suffix(path.suffix + ".part")
    pending.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    pending.replace(path)


def bundle_check(bundle):
    """Verify the exact shipped file set, including the compiler and translator."""
    bundle = Path(bundle).resolve(strict=True)
    manifest = json.loads((bundle / "bundle.json").read_text(encoding="utf-8"))
    require(manifest.get("format") == "hwaoi-compiler-v1", "Unsupported compiler bundle")
    for relative, expected in manifest["files"].items():
        path = (bundle / relative).resolve()
        require(path.is_relative_to(bundle) and path.is_file(), f"Missing bundle file: {relative}")
        require(hwaoi.sha256(path) == expected, f"Bundle file changed: {relative}. Extract the ZIP again.")
    for relative in ("python/python.exe", "llvm/bin/clang.exe", "bin/ninja.exe",
                     "bin/zlib1.dll", "translator/totk_xlate.exe", "translator/totk_xlate.id",
                     "deps/include/vma/vk_mem_alloc.h", "source/tools/hwaoi.py"):
        require(relative in manifest["files"], f"Incomplete bundle: {relative}")
    return manifest


def inspect_nup(path):
    """Reject the wrong title/build before extracting any executable member."""
    profile = json.loads((SOURCE / "config/games/hwaoi-update-196608.json").read_text(encoding="utf-8"))
    expected = {**profile["modules"], "main.npdm": profile["npdm_sha256"]}
    with Nup(path) as archive:
        packages, files = archive.exefs()
        require(not any(p.kind in (2, 3) for p in archive.packages), "DLC/addon packages are not supported")
        require(packages[0].title_id == int(profile["title_id"], 16), "This is not the supported HWAoI title")
        require(packages[-1].version == profile["ncm_version"], "Unsupported game update (requires NCM 196608)")
        require({name: member.sha256 for name, (_, member) in files.items()} == expected,
                "Game modules do not match the supported build. No patches were applied.")
        return {"header": archive.header_sha256, "toc": archive.toc_sha256, "size": archive.file_size}


def fresh_output(output, bundle, nup):
    output, bundle, nup = Path(output).resolve(), Path(bundle).resolve(), Path(nup).resolve()
    require(not output.is_relative_to(bundle) and not bundle.is_relative_to(output),
            "Choose an output folder outside the extracted compiler folder")
    require(not nup.is_relative_to(output), "The output folder must not contain the source NUP")
    require(not output.exists() or (output.is_dir() and not any(output.iterdir())),
            "Output folder is not empty. Choose a new empty folder; existing files are never overwritten.")
    parent = output
    while not parent.exists():
        parent = parent.parent
    require(shutil.disk_usage(parent).free >= 20 << 30, "At least 20 GiB free is required on the output drive")
    return output


def clean_environment(bundle):
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("HWAOI_", "DKB_", "TOTK_", "PYTHON"))}
    env["PYTHONUTF8"] = "1"
    env["PYTHONNOUSERSITE"] = "1"
    # No dependency on tools installed on the build author's PC or on the user's PATH.
    env["PATH"] = os.pathsep.join(str(p) for p in (
        bundle / "llvm/bin", bundle / "bin", Path(os.environ.get("SystemRoot", "C:/Windows")) / "System32"))
    return env


def run_tool(bundle, output, *arguments):
    command = [str(bundle / "python/python.exe"), "-E", "-s", str(output / "tools/hwaoi.py"), *map(str, arguments)]
    print("+ " + subprocess.list2cmdline(command), flush=True)
    process = subprocess.Popen(command, cwd=output, env=clean_environment(bundle), stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
    for line in process.stdout:
        print(line, end="", flush=True)
    process.stdout.close()
    code = process.wait()
    require(code == 0, f"Build step failed (exit {code}). See compiler.log for details.")


class Tee:
    def __init__(self, stream, log):
        self.stream, self.log = stream, log
    def write(self, value):
        if self.stream:
            self.stream.write(value)
            self.stream.flush()
        self.log.write(value)
        self.log.flush()
    def flush(self):
        self.log.flush()


def build(bundle, nup, output, jobs=2, resolution="1920x1080", unlock=False):
    require(os.name == "nt" and platform.machine().lower() in ("amd64", "x86_64"), "Windows x64 is required")
    require(1 <= jobs <= 8 and resolution in RESOLUTIONS, "Invalid build settings")
    bundle = Path(bundle).resolve(strict=True)
    nup = Path(nup).resolve(strict=True)
    print("[1/6] Checking compiler files and game identity...", flush=True)
    manifest = bundle_check(bundle)
    identity = inspect_nup(nup)
    before = nup.stat()
    output = fresh_output(output, bundle, nup)
    shutil.copytree(bundle / "source", output, dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    state = {"format": "hwaoi-local-build-v1", "status": "incomplete", "source_revision": manifest["source_revision"],
             "nup": str(nup), "nup_identity": identity, "resolution": resolution, "rate_unlock": bool(unlock)}
    save(output / STATE, state)
    with (output / "compiler.log").open("w", encoding="utf-8") as log, contextlib.redirect_stdout(Tee(sys.stdout, log)):
        try:
            exefs = output / "work/game/exefs"
            print("[2/6] Extracting and hashing executable modules...", flush=True)
            with Nup(nup) as archive:
                extract_exefs(archive, exefs)
            hwaoi.verify(exefs)
            print("[3/6] Preparing the read-only game resource cache...", flush=True)
            common = ["--exefs", exefs]
            run_tool(bundle, output, *common, "prepare-romfs", "--nup", nup)
            print("[4/6] Translating ARM64 game code to C...", flush=True)
            run_tool(bundle, output, *common, "generate", "--rate-unlock" if unlock else "--display-settings",
                     "--out", "work/generated/hwaoi196608-full", "--limit", "0", "--jobs", jobs)
            print("[5/6] Compiling and linking the native Windows game (this can take a while)...", flush=True)
            run_tool(bundle, output, *common, "build", "--romfs", "work/game/romfs", "--deps", bundle / "deps",
                     "--toolchain", bundle / "llvm", "--ninja", bundle / "bin/ninja.exe", "--jobs", jobs)
            print("[6/6] Installing the shader translator and launch settings...", flush=True)
            game = output / "work/build/hwaoi196608"
            shutil.copytree(bundle / "translator", game / "xlate-managed-irbanks")
            shutil.copy2(bundle / "bin/zlib1.dll", game / "zlib1.dll")
            (game / "hwaoi.ini").write_text("[settings]\noutput_res=" + resolution +
                                           "\nrender_scale=100\nfps_limit=60\n", encoding="utf-8")
            after = nup.stat()
            require((before.st_size, before.st_mtime_ns) == (after.st_size, after.st_mtime_ns), "Source NUP changed during build")
            state.update(status="complete", executable_sha256=hwaoi.sha256(game / "hwaoi.exe"))
            save(output / STATE, state)
            print("BUILD COMPLETE. Use Open compiled game in the compiler window.\n"
                  "Keep the NUP at its original location: the game reads resources from it.\n"
                  "This local output contains game-derived files and is not for redistribution.", flush=True)
        except Exception:
            traceback.print_exc(file=sys.stdout)
            raise
    return output


def launch(bundle, output):
    bundle, output = Path(bundle).resolve(strict=True), Path(output).resolve(strict=True)
    bundle_check(bundle)
    state = json.loads((output / STATE).read_text(encoding="utf-8"))
    require(state.get("format") == "hwaoi-local-build-v1" and state.get("status") == "complete", "Build is incomplete")
    require(inspect_nup(state["nup"]) == state["nup_identity"], "Source NUP has moved or changed")
    game = output / "work/build/hwaoi196608/hwaoi.exe"
    require(hwaoi.sha256(game) == state["executable_sha256"], "Compiled executable has changed; rebuild in a fresh folder")
    env = clean_environment(bundle)
    env["HWAOI_RATE_UNLOCK"] = "1" if state["rate_unlock"] else "0"
    command = [str(bundle / "python/python.exe"), "-E", "-s", str(output / "tools/hwaoi.py"), "launch",
               "--zlib", str(game.parent / "zlib1.dll"), "--no-captures"]
    completed = subprocess.run(command, cwd=output, env=env)
    require(completed.returncode == 0, "Game launch failed; see the launch report under work/reports")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("build", "launch", "check"))
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--nup", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--resolution", choices=RESOLUTIONS, default="1920x1080")
    parser.add_argument("--unlock", action="store_true")
    args = parser.parse_args()
    try:
        if args.action == "check":
            bundle_check(args.bundle)
            if args.nup:
                print(json.dumps(inspect_nup(args.nup)))
            print("Compiler bundle checks passed")
        elif args.action == "build":
            require(args.nup and args.output, "Choose a game NUP and output folder")
            build(args.bundle, args.nup, args.output, args.jobs, args.resolution, args.unlock)
        else:
            require(args.output, "Choose the completed output folder")
            launch(args.bundle, args.output)
        return 0
    except Exception as error:
        print(f"ERROR: {error}", file=sys.stderr, flush=True)
        return 2


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    raise SystemExit(main())
