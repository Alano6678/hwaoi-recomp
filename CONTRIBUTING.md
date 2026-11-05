# Contributing to HWAoI Recomp

Start with the [README](README.md), [current status](docs/hwaoi/port-status.md)
and [bring-up ledger](docs/hwaoi/current-work.md). This fork has rendered an actual
3D scene but is not ready to play. Performance, rendering correctness, gameplay
coverage and clean shutdown are open work. Use this repository's issues for
HWAoI reports; upstream DKB/TotK documentation is historical reference.

## Source-only contributions

Never commit ROMs, keys, extracted game resources, generated game C, game-derived
binaries, shader payloads/caches, fonts, audio or movies. Do not attach these,
firmware, or game screenshots to issues or pull requests. Information such as
addresses, hashes, diagnostics and independently written patch/test code is useful.
Keep private diagnostic inputs and outputs under ignored `work/`, `build/` or
`generated/`. Do not modify the original NUP.

Run the content guard before handing off and before committing:

```powershell
python tools/check_content.py
python tools/check_content.py --staged
git config core.hooksPath .githooks
```

CI repeats the content guard and its regression tests. Do not bypass a failing
guard with `--no-verify`.

## Code and tests

- Use `tools/hwaoi.py` as the fork entry point and retain exact profile/hash checks.
- Change `recomp/` rather than editing generated game C.
- Do not import Bananza/TotK address patches or shader workarounds without HWAoI
  evidence. Unsupported paths must fail visibly.
- Run relevant tests and `python -m unittest discover -s tests/hwaoi -v` when the
  required toolchain is available. Report dependency-related skips.
- Lifter changes need independent instruction/semantic checks and failure-path
  tests; successful generation or compilation alone is not correctness proof.
- Keep generation, compilation, linking, boot, rendering and playability claims
  separate. A probe timeout is not a normal exit.
- Newly installed compilers/SDKs for this project's local Windows environment
  belong under `F:\ToolName`; reuse existing tools first.

## Reporting a problem

Include the source commit, input profile, GPU/driver/Windows version, exact
command and reproduction steps. Interactive logs are under
`work/reports/interactive-<timestamp>/`; probes use their requested `--out`
directory. Paste a relevant text excerpt after checking for personal paths or
credentials. Do not upload an entire report folder: it can contain game-derived
shader payloads, memory captures and screenshots. Describe the visual symptom.

## Pull requests and attribution

Keep each change focused. Explain the observed problem, resulting behavior and
validation limits. Preserve third-party copyright/license headers and record
new copied code with its source, exact commit and license in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

The project inherits GPL-3.0-or-later overall; inherited MIT and other compatible
third-party components keep their original terms. The full DKB attribution and
notices remain intact. Upstream installer/release workflows are not HWAoI-ready;
do not use them to publish builds.
