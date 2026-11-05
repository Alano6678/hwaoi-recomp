"""Command line; reports never imply game boot or complete instruction support."""
from __future__ import annotations
import argparse
from collections import Counter
import json
from pathlib import Path
import struct
import sys
from .nup import Nup, extract_exefs, FormatError, require
from .nso import Nso


def emit_report(report, output=None):
    text = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
    if output:
        path = Path(output)
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("x", encoding="utf-8") as stream:
            stream.write(text)
        print(f"Report: {path.resolve()}")
    else:
        print(text)


def analyze_module(nso: Nso, with_coverage=False):
    report = nso.summary()
    symbols = nso.symbols()
    report["defined_function_symbols"] = sum(s["type"] == 2 and s["section"] != 0 for s in symbols)
    report["imported_symbols"] = [s["name"] for s in symbols if s["section"] == 0 and s["name"]]
    if with_coverage:
        from .a64 import instruction_support
        counts = Counter()
        samples = []
        for index, (word,) in enumerate(struct.iter_unpack("<I", nso.text.data)):
            family = instruction_support(word)
            counts[family or "unsupported"] += 1
            if family is None and len(samples) < 32:
                samples.append({"address": hex(nso.text.memory_offset + index * 4), "word": f"{word:08x}"})
        report["linear_word_classification"] = {"word_count": len(nso.text.data) // 4,
            "counts": dict(counts.most_common()), "unsupported_samples": samples,
            "warning": "Linear .text scan includes embedded data/padding; this is NOT reachable-code or game compatibility coverage."}
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description="HWR static recompilation workbench (not a playable PC port)")
    sub = parser.add_subparsers(dest="command", required=True)
    info = sub.add_parser("inspect", help="Validate NUP tables and inspect metadata without bulk extraction")
    info.add_argument("input")
    info.add_argument("--report")
    extract = sub.add_parser("extract-exefs", help="Extract verified executable members into a NEW directory")
    extract.add_argument("input")
    extract.add_argument("output")
    extract.add_argument("--package", type=int, help="Explicit package; default overlays latest update on base")
    extract.add_argument("--report")
    analyze = sub.add_parser("analyze", help="Verify and report NSO segments; no linking or execution")
    analyze.add_argument("input", help="NSO file or directory containing NSO modules")
    analyze.add_argument("--coverage", action="store_true")
    analyze.add_argument("--report")
    dis = sub.add_parser("disassemble", help="Disassemble an explicit module-relative text range")
    dis.add_argument("input")
    dis.add_argument("--address", type=lambda x: int(x, 0), required=True)
    dis.add_argument("--size", type=lambda x: int(x, 0), required=True)
    trans = sub.add_parser("translate", help="Translate a bounded NSO function to C, rejecting unsupported words")
    trans.add_argument("input")
    trans.add_argument("--address", type=lambda x: int(x, 0), required=True)
    trans.add_argument("--size", type=lambda x: int(x, 0), required=True)
    trans.add_argument("--name", default="hwr_function")
    trans.add_argument("--output", required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "inspect":
            with Nup(args.input) as archive:
                emit_report(archive.summary(), args.report)
        elif args.command == "extract-exefs":
            with Nup(args.input) as archive:
                emit_report(extract_exefs(archive, Path(args.output), args.package), args.report)
        elif args.command == "analyze":
            path = Path(args.input)
            candidates = sorted(path.iterdir()) if path.is_dir() else [path]
            modules = []
            for member in candidates:
                if not member.is_file():
                    continue
                with member.open("rb") as stream:
                    magic = stream.read(4)
                if magic == b"NSO0":
                    modules.append(analyze_module(Nso(member), args.coverage))
            require(bool(modules), "no NSO0 modules found")
            emit_report({"modules": modules, "game_execution": "not attempted"}, args.report)
        elif args.command == "disassemble":
            require(args.size <= 0x10000, "disassembly selection exceeds 64 KiB limit")
            import capstone
            nso = Nso(args.input)
            code = nso.code(args.address, args.size)
            disassembler = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_LITTLE_ENDIAN)
            disassembler.skipdata = True
            for address, size, mnemonic, operands in disassembler.disasm_lite(code, args.address):
                print(f"{address:016x}  {mnemonic:10s} {operands}")
        elif args.command == "translate":
            from .a64 import translate
            require(args.size <= 0x10000, "translation selection exceeds 64 KiB prototype limit")
            nso = Nso(args.input)
            code = nso.code(args.address, args.size)
            source = translate(code, args.address, args.name)
            path = Path(args.output)
            path.parent.mkdir(parents=True, exist_ok=True)
            with path.open("x", encoding="utf-8") as stream:
                stream.write(source)
            emit_report({"output": str(path.resolve()), "module_sha256": nso.sha256,
                         "build_id": nso.build_id, "address": hex(args.address), "size": args.size,
                         "status": "bounded function translated, NOT a game executable"})
    except (FormatError, OSError, ValueError, ImportError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    return 0
