"""HWAoI-specific verified entry point on top of the imported DKB recompiler.

No Bananza patch table, installer or game resources are used. Generation and
object compilation are separate from linking/boot. All derived output is local.
"""
from __future__ import annotations
import argparse
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PROFILE = ROOT / "config/games/hwaoi-update-196608.json"
DEFAULT_EXEFS = ROOT / "work/game/exefs"
DEFAULT_GENERATED = ROOT / "work/generated/hwaoi196608"
MODULES = ("main", "subsdk0", "sdk")
BUILD_NAME = "hwaoi196608"


def need(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def program_id(path):
    data = Path(path).read_bytes()
    need(len(data) >= 0x80 and data[:4] == b"META", "invalid main.npdm header")
    offset, size = struct.unpack_from("<II", data, 0x70)
    need(size >= 0x18 and offset <= len(data) and size <= len(data) - offset, "NPDM ACI0 outside file")
    need(data[offset:offset + 4] == b"ACI0", "NPDM has no ACI0 signature")
    return f"{struct.unpack_from('<Q', data, offset + 0x10)[0]:016X}"


def verify(exefs, profile_path=PROFILE):
    from hwrecomp.nso import Nso
    exefs = Path(exefs).resolve(strict=True)
    profile = json.loads(Path(profile_path).read_text(encoding="utf-8"))
    need(profile["title_id"] == "04003F40232DE000", "not the HWAoI title profile")
    need(program_id(exefs / "main.npdm") == profile["title_id"], "main.npdm title ID mismatch")
    need(sha256(exefs / "main.npdm") == profile["npdm_sha256"], "main.npdm SHA-256 mismatch")
    module_details, hashes = [], {}
    for name, expected in profile["modules"].items():
        need(name in (*MODULES, "rtld"), f"unexpected module name: {name}")
        actual = sha256(exefs / name)
        need(actual == expected, f"{name}: SHA-256 mismatch; refusing a different game/build")
        module = Nso(exefs / name)  # independent segment hash and bounds validation
        hashes[name] = actual
        module_details.append({"name": name, "sha256": actual, "build_id": module.build_id,
                               "image_size": module.image_end})
    need(set(hashes) == {*MODULES, "rtld"}, "profile must contain all four observed modules")
    hashes["main.npdm"] = sha256(exefs / "main.npdm")
    return {"title_id": profile["title_id"], "ncm_version": profile["ncm_version"],
            "display_version": profile.get("display_version"), "exefs": str(exefs),
            "exefs_sha256": hashes, "modules": module_details,
            "generated_modules": list(MODULES),
            "rtld_policy": "retained and hashed; DKB host linker replaces rtld execution (unverified for HWAoI)",
            "patches": "none; no Bananza/TotK addresses applied"}


def local_output(path, kind):
    result = Path(path).resolve()
    boundary = (ROOT / "work" / kind).resolve()
    need(result != boundary and result.is_relative_to(boundary), f"output must be a child of {boundary}")
    return result


def save_json(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(content, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def run(command, **kwargs):
    print("+", subprocess.list2cmdline([str(x) for x in command]), flush=True)
    env = dict(os.environ)
    env['PYTHONUTF8'] = '1'
    completed = subprocess.run([str(x) for x in command], cwd=ROOT, env=env, **kwargs)
    need(completed.returncode == 0, f"command failed with exit code {completed.returncode}")
    return completed


def prepare_generated(out, identity):
    marker = out / "hwaoi-input.json"
    if out.exists():
        need(marker.is_file(), "refusing to reuse an unmarked generation directory")
        old = json.loads(marker.read_text(encoding="utf-8"))
        need(old["exefs_sha256"] == identity["exefs_sha256"], "generation directory belongs to another input")
    else:
        out.mkdir(parents=True)
    save_json(marker, identity)


def generation_summary(out, limit):
    summary = []
    for name in MODULES:
        manifest = json.loads((out / name / "manifest.json").read_text(encoding="utf-8"))
        stats = manifest["stats"]
        summary.append({"module": name, "functions_discovered": stats["functions"],
                        "functions_emitted": manifest["function_count"],
                        "instructions": stats["instructions"], "unsupported": stats["unsupported"],
                        "traps": stats["traps"], "c_bytes": stats["c_bytes"],
                        "unsupported_census": manifest["unsupported_census"]})
    return {"stage": "C generation only", "function_limit_per_module": limit, "modules": summary,
            "all_discovered_functions_emitted": all(m["functions_discovered"] == m["functions_emitted"] for m in summary),
            "runtime_verified": False, "playable": False}


def compile_smoke(generated, out, clang, chunks):
    need(1 <= chunks <= 8, "smoke chunks must be in 1..8 per module")
    need(not out.exists(), "smoke output exists; choose a new directory")
    need(Path(clang).is_file(), f"compiler not found: {clang}")
    out.mkdir(parents=True)
    report = {"stage": "selected native object compilation (no linking/execution)", "objects": []}
    for module in MODULES:
        manifest = json.loads((generated / module / "manifest.json").read_text(encoding="utf-8"))
        for filename in manifest["chunks"][:chunks]:
            need(Path(filename).name == filename, "invalid generated chunk path")
            source = generated / module / filename
            destination = out / f"{module}-{source.stem}.o"
            command = [clang, "-std=gnu11", "-O1", "-fno-strict-aliasing", "-Wno-return-type",
                       "-Wno-unused-label", "-march=x86-64-v3", "-I", ROOT / "runtime", "-I", generated,
                       "-c", source, "-o", destination]
            run(command, timeout=180)
            report["objects"].append({"source": str(source), "object": str(destination),
                                      "size": destination.stat().st_size, "sha256": sha256(destination)})
    save_json(out / "compile-report.json", report)
    return report


def audit(exefs, identity):
    sys.path.insert(0, str(ROOT / "recomp"))
    from nso import Nso
    from layout import layout
    available = set()
    for path in (ROOT / "runtime").glob("*.c"):
        available.update(re.findall(r'HOST_OVERRIDE\(\s*"([^"\n]+)"', path.read_text(encoding="utf-8")))
    modules = {name: Nso(str(exefs / name)) for name in (*MODULES, "rtld")}
    exports = {}
    for name in MODULES:
        for symbol, info, section, value, size in modules[name].exports():
            if symbol and info >> 4 in (1, 2):
                exports.setdefault(symbol, []).append(name)
    records = []
    for name in MODULES:
        module = modules[name]
        imports = [{"name": s[0], "host_override_declared": s[0] in available,
                    "guest_export_providers": exports.get(s[0], []), "weak": s[1] >> 4 == 2}
                   for s in module.imports()]
        records.append({"module": name, "symbol_count": len(module.syms), "relocation_count": len(module.relocs),
                        "needed": module.needed, "imports": imports})
    return {**identity, "proposed_layout": [{"name": n, "base": hex(b), "size": s} for n, b, s in layout(exefs)],
            "linkage_inventory": records,
            "limitations": ["Textual host declarations are candidates, not verified ABI/behavior",
                            "Weak/global visibility, relocation execution and OS/GPU behavior not validated",
                            "No game boot performed"]}


def launch_interactive(args, identity):
    """Explicit user trial; retains logs and uses the user's local save profile."""
    need(os.name == "nt", "interactive launcher currently requires Windows")
    exe = local_output(args.exe, "build")
    need(exe.is_file(), f"runtime not built: {exe}")
    translator = exe.parent / "xlate-managed-irbanks/totk_xlate.exe"
    need(translator.is_file() and translator.with_suffix(".id").is_file(), "IR-bank shader translator is missing")
    zlib = args.zlib.resolve(strict=True)
    need(zlib.is_file(), "native zlib DLL is missing")
    report_dir = local_output(ROOT / "work/reports" / datetime.now().strftime("interactive-%Y%m%d-%H%M%S-%f"), "reports")
    report_dir.mkdir(parents=True)
    env = dict(os.environ)
    # Environment overrides take precedence over inherited ini/probe settings.
    options = {"HWAOI_GPU": "vulkan", "HWAOI_FULLSCREEN": "0", "HWAOI_WINDOW_SIZE": "1280x720",
               "HWAOI_INPUT": "auto", "HWAOI_RUMBLE": "0", "HWAOI_PIPE_SYNC": "1", "HWAOI_PROGRESS": "1",
               "HWAOI_EXEFS": str(args.exefs.resolve()),
               "HWAOI_PROFILE_DIR": str(ROOT / "work/saves/hwaoi196608"),
               "HWAOI_GSV_LOCAL_FRAME_POLICY": "1", "HWAOI_NATIVE_INFLATE": "1",
               "HWAOI_ZLIB_DLL": str(zlib), "HWAOI_POOL_BACKPRESSURE": "1", "HWAOI_POOL_DIAG": "1",
               "HWAOI_IR_BANKS": "1", "HWAOI_XLATE": str(translator), "HWAOI_TRACE": "sched,fs",
               "HWAOI_SHOTS": "60-1800:60", "HWAOI_SHOTS_DIR": str(report_dir / "presents"),
               "HWAOI_SHOTS_FULL": "1", "HWAOI_CORE_STALL_DIAG": "1",
               "DKB_SHADER_FAILURE_DUMP": str(report_dir / "shader-failures")}
    for setting in ("INPUT_SCRIPT", "INPUT_RECORD", "XLATE_ID"):
        for prefix in ("HWAOI_", "DKB_", "TOTK_"):
            env.pop(prefix + setting, None)
    env.update(options)
    report = {"stage": "interactive experimental launch", "executable": str(exe), "sha256": sha256(exe),
              "exefs_sha256": identity["exefs_sha256"], "runtime_options": options,
              "dependencies": {"translator": sha256(translator), "zlib": sha256(zlib)},
              "arguments": ["--play-experimental"], "report_directory": str(report_dir),
              "gameplay_verified": False}
    with (report_dir / "stdout.log").open("wb") as stdout, (report_dir / "stderr.log").open("wb") as stderr:
        proc = subprocess.Popen([str(exe), "--play-experimental"], cwd=exe.parent, env=env,
                                stdout=stdout, stderr=stderr, creationflags=subprocess.CREATE_NO_WINDOW)
    report["pid"] = proc.pid
    save_json(report_dir / "launch.json", report)
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--exefs", type=Path, default=DEFAULT_EXEFS)
    sub = parser.add_subparsers(dest="action", required=True)
    sub.add_parser("verify", help="Check title ID, exact module hashes and NSO segment hashes")
    inspect = sub.add_parser("audit", help="Inspect imports/relocations and proposed DKB module layout")
    inspect.add_argument("--report", type=Path, default=ROOT / "work/reports/hwaoi-linkage.json")
    romfs = sub.add_parser("prepare-romfs", help="Prepare verified compressed cache; raw resources stay in the NUP")
    romfs.add_argument("--nup", type=Path, required=True)
    romfs.add_argument("--out", type=Path, default=ROOT / "work/game/romfs")
    gen = sub.add_parser("generate", help="Run DKB's real generator with the HWAoI profile and NO game patches")
    gen.add_argument("--out", type=Path, default=DEFAULT_GENERATED)
    gen.add_argument("--limit", type=int, default=256, help="Functions per module; 0 explicitly requests all")
    gen.add_argument("--jobs", type=int, default=2)
    smoke = sub.add_parser("compile-smoke", help="Compile selected generated chunks, without claiming a game build")
    smoke.add_argument("--generated", type=Path, default=DEFAULT_GENERATED)
    smoke.add_argument("--out", type=Path, default=ROOT / "work/build/hwaoi-smoke")
    smoke.add_argument("--clang", type=Path, default=Path("F:/llvm-mingw/bin/clang.exe"))
    smoke.add_argument("--chunks", type=int, default=1)
    build = sub.add_parser("build", help="Link a complete generation with explicit runtime dependencies (experimental)")
    build.add_argument("--generated", type=Path, default=ROOT / "work/generated/hwaoi196608-full")
    build.add_argument("--out", type=Path, default=ROOT / "work/build/hwaoi196608")
    build.add_argument("--romfs", type=Path, required=True, help="Prepared romfs.bin/index.json directory; not the NUP")
    build.add_argument("--deps", type=Path, required=True, help="Existing include/lib/bin runtime dependency tree")
    build.add_argument("--toolchain", type=Path, default=Path("F:/llvm-mingw"))
    build.add_argument("--ninja", type=Path, default=Path("F:/Ninja/ninja.exe"))
    build.add_argument("--jobs", type=int, default=2)
    launch = sub.add_parser("launch", help="Open the experimental game with live input and local logs")
    launch.add_argument("--exe", type=Path, default=ROOT / "work/build/hwaoi196608/hwaoi.exe")
    launch.add_argument("--zlib", type=Path, default=Path("F:/GCC/ucrt64/bin/zlib1.dll"))
    args = parser.parse_args(argv)
    try:
        identity = verify(args.exefs)
        if args.action == "verify":
            report = identity
        elif args.action == "launch":
            report = launch_interactive(args, identity)
        elif args.action == "prepare-romfs":
            from hwaoi_romfs import prepare
            report = prepare(args.nup, local_output(args.out, "game"), identity)
        elif args.action == "audit":
            report = audit(args.exefs, identity)
            destination = local_output(args.report, "reports")
            save_json(destination, report)
            report = {"report": str(destination), "proposed_layout": report["proposed_layout"],
                      "modules": [{"module": m["module"], "symbols": m["symbol_count"],
                                   "relocations": m["relocation_count"], "imports": len(m["imports"])}
                                  for m in report["linkage_inventory"]], "runtime_verified": False}
        elif args.action == "generate":
            need(0 <= args.limit <= 1000000 and 1 <= args.jobs <= 8, "invalid limit/jobs")
            out = local_output(args.out, "generated")
            prepare_generated(out, identity)
            run([sys.executable, ROOT / "recomp/gen.py", "--exefs", args.exefs.resolve(), "--out", out,
                 "--modules", ",".join(MODULES), "--jobs", args.jobs, "--limit", args.limit])
            report = generation_summary(out, args.limit)
            save_json(out / "hwaoi-generation.json", report)
            save_json(out / "expected.json", identity)
        elif args.action in ("compile-smoke", "build"):
            generated = local_output(args.generated, "generated")
            old = json.loads((generated / "hwaoi-input.json").read_text(encoding="utf-8"))
            need(old["exefs_sha256"] == identity["exefs_sha256"], "generated input does not match current modules")
            if args.action == "compile-smoke":
                report = compile_smoke(generated, local_output(args.out, "build"), args.clang, args.chunks)
            else:
                complete = generation_summary(generated, 0)
                need(complete["all_discovered_functions_emitted"], "partial generation cannot be linked as a full game")
                need(all(m["unsupported"] == 0 for m in complete["modules"]), "unsupported instructions remain")
                need((args.romfs / "index.json").is_file(), "prepared HWAoI RomFS index is required")
                storage = json.loads((args.romfs / "index.json").read_text(encoding="utf-8"))
                need(storage.get("status") == "pass", "RomFS preparation is incomplete")
                need(storage.get("exefs_sha256") == identity["exefs_sha256"], "RomFS belongs to another ExeFS build")
                need((args.romfs / ("cache.bin" if storage.get("storage") == "nup-cache-v1" else "romfs.bin")).is_file(),
                     "prepared HWAoI RomFS storage is missing")
                need(args.deps.is_dir() and args.ninja.is_file(), "runtime dependency tree or Ninja missing")
                need(1 <= args.jobs <= 8, "build jobs must be in 1..8")
                out = local_output(args.out, "build")
                run([sys.executable, ROOT / "tools/build_game.py", "--version", BUILD_NAME,
                     "--gen", generated, "--out", out, "--expected", generated / "expected.json",
                     "--exefs", args.exefs.resolve(), "--romfs", args.romfs.resolve(),
                     "--profile", ROOT / "work/saves/hwaoi196608", "--toolchain", args.toolchain,
                     "--ninja", args.ninja, "--deps", args.deps, "--jobs", args.jobs, "--full-build"])
                report = {"stage": "linked experimental runtime", "output": str(out),
                          "game_boot_enabled": False, "experimental_interactive_command": "python tools/hwaoi.py launch",
                          "playable": False}
        print(json.dumps(report, ensure_ascii=False, indent=2))
        return 0
    except (OSError, ValueError, ImportError, subprocess.TimeoutExpired) as error:
        print(f"hwaoi: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    raise SystemExit(main())
