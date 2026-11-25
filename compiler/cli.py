"""Headless compiler entry points used by RecompLauncher."""
from __future__ import annotations

import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from compiler import backend


def _backend_args(bundle: Path, action: str, output: Path, game: Path | None = None,
                  update: Path | None = None, dlc: Path | None = None,
                  jobs: int = 0, resolution: str = "1920x1080", unlock: bool = False) -> list[str]:
    args = ["backend.py", action, "--bundle", str(bundle), "--output", str(output)]
    if game:
        args += ["--nup", str(game)]
    if update:
        args += ["--update-nup", str(update)]
    if dlc:
        args += ["--dlc-nup", str(dlc)]
    if action == "build":
        args += ["--jobs", str(jobs), "--resolution", resolution]
        if unlock:
            args.append("--unlock")
    return args


def prepare(args: argparse.Namespace, bundle: Path) -> int:
    if not args.game or not args.out:
        raise ValueError("prepare requires --game and --out")
    print("progress 0.01 Checking the compiler bundle", flush=True)
    old = sys.argv
    try:
        sys.argv = _backend_args(bundle, "build", args.out, args.game, args.update, args.dlc,
                                 args.jobs, args.resolution, args.unlock)
        code = backend.main()
    finally:
        sys.argv = old
    if code == 0:
        print("progress 1.00 Build complete", flush=True)
    return code


def play(args: argparse.Namespace, bundle: Path) -> int:
    if not args.game_dir:
        raise ValueError("--play requires --game-dir")
    old = sys.argv
    try:
        sys.argv = _backend_args(bundle, "launch", args.game_dir, args.game, None, None)
        return backend.main()
    finally:
        sys.argv = old


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, default=Path(__file__).resolve().parents[2])
    sub = parser.add_subparsers(dest="command", required=True)

    cli = sub.add_parser("cli")
    cli_sub = cli.add_subparsers(dest="cli_command", required=True)
    prep = cli_sub.add_parser("prepare")
    prep.add_argument("--game", type=Path, required=True)
    prep.add_argument("--out", type=Path, required=True)
    prep.add_argument("--update", type=Path)
    prep.add_argument("--dlc", type=Path)
    prep.add_argument("--jobs", type=int, default=0)
    prep.add_argument("--resolution", default="1920x1080")
    prep.add_argument("--unlock", action="store_true")

    play_parser = sub.add_parser("play")
    play_parser.add_argument("--game-dir", type=Path, required=True)
    play_parser.add_argument("--game", type=Path)

    args = parser.parse_args(argv)
    bundle = args.bundle.resolve()
    try:
        if args.command == "cli" and args.cli_command == "prepare":
            return prepare(args, bundle)
        if args.command == "play":
            return play(args, bundle)
        raise ValueError("unknown headless command")
    except Exception as error:
        print(f"ERROR: {error}", file=sys.stderr, flush=True)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
