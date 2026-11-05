# HWAoI Recomp — Hyrule Warriors: Age of Imprisonment

An experimental native Windows recompilation of **Hyrule Warriors: Age of Imprisonment**, adapted from [DKB Recomp](https://github.com/Lucas-blip-png/dkb-pc-recomp). The project combines a static recompilation generator, a C/C++ runtime and a Vulkan backend, with ongoing adaptation of system services, resource loading and NVN2 shaders.

**Full compilation and linking are complete, and an actual 3D scene with characters, grass and ruins has been rendered. Combat and responsive gameplay remain unverified.** Performance is low, and lighting, stability and shutdown still need work. This repository contains research source code, not a ready-to-play release. No game assets or game-derived binaries are included.

## Progress — November 5, 2026

| Stage | Verified result | Limitations |
| --- | --- | --- |
| Input validation | Title ID, module SHA-256 and NSO segment hash checks | Only the exact pinned input profile is accepted |
| Static generation | 3 modules, 142,393 candidate functions and 362 C chunks | Successful generation does not prove all instructions or reachable paths correct |
| Compilation and linking | Complete Windows x64 executable; loader probe reports zero strong unresolved imports | Generated game C, objects and executables remain local |
| Startup and UI | Reached `nnMain`; displayed Chinese settings and autosave information | The complete game flow has not been verified |
| 3D rendering | Displayed characters, vegetation, grass and ruins | Lighting, shadows and frame rate still have problems |
| Input and saves | Interactive launch supports keyboard/controller input, detects an Xbox controller and uses a separate local save path | Combat controls and save round trips remain unverified |
| Regression checks | Latest HWAoI suite: 79 tests passed | Automated checks do not replace gameplay verification |

The latest completed bounded check, `hang-21`, displayed the 3D scene again with Vulkan validation enabled. Its complete log contained **zero fatal errors, lowering failures or Vulkan validation errors**. At 180 seconds the probe requested window close; the final result was timeout/exit code `124`. This is not a clean-exit or long-running stability pass. The last measured interval was 39 frames in 54.2 seconds; this first run with a new shader cache identity is not a steady-state benchmark.

Recent fixes cover F16 texture fetch/gather packing, D32F/S8 depth-stencil copies, narrowly scoped RGBA8 comparison-texture conversion, storage-image read capability declarations, bounded resource-allocation retries at verified call sites, and virtual-core scheduling. Unsupported modes continue to fail explicitly.

The current priority is correct 3D rendering, followed by performance work toward 60 FPS. That frame-rate target has not been achieved. Next investigations cover lighting/shadows, CPU replay and resource-loading costs, interactive control and combat, and shutdown cleanup. See the [status report](docs/hwaoi/port-status.md) for evidence and limits, and the [bring-up ledger](docs/hwaoi/current-work.md) for individual runs. Raw reports, screenshots and shader inputs remain in ignored local directories.

## Inputs and building

The current target is title ID `04003F40232DE000`, NCM version `196608`. NCM version is an internal value and **must not be interpreted as the game's display version**. The entry point validates exact module hashes; arbitrary versions are unsupported. The original NUP is read-only.

These commands describe the existing development workflow, not an automatic installer. Supply your own lawfully obtained, matching ExeFS/NUP and runtime dependencies. ExeFS defaults to `work/game/exefs`; to change it, put `--exefs <directory>` before the subcommand. Python runs generators and build tools; the game executes through the compiled C/C++ runtime.

The development environment uses Windows x64, Python, llvm-mingw/Clang, Ninja, a .NET shader translator and Vulkan. Python dependencies are listed in [requirements-hwaoi.txt](tools/requirements-hwaoi.txt). Reuse existing tools. This project's local environment convention places newly downloaded compilers/SDKs under `F:\ToolName`. Adjust the example dependency paths for your environment.

```powershell
# Validate inputs and inspect linkage candidates
python tools/hwaoi.py verify
python tools/hwaoi.py audit

# Generate all functions; without --limit 0, the default is 256 per module
python tools/hwaoi.py generate --limit 0 --out work/generated/hwaoi196608-full --jobs 2

# Prepare the resource cache once; reuse an existing valid cache
# The NUP path below is an example
python tools/hwaoi.py prepare-romfs --nup 'E:\Games\HyruleWarriors.nup'

# Build the complete executable, reusing unchanged objects
python tools/hwaoi.py build --romfs work/game/romfs --deps F:/TotKDependencies/runtime/deps --jobs 4

# Explicit experimental launch: live input, local saves and separate logs
# This interactive command has no automatic runtime deadline
python tools/hwaoi.py launch
```

The executable is written to `work/build/hwaoi196608/hwaoi.exe`. Direct execution remains guarded; use `tools/hwaoi.py launch` for interactive experiments. The launcher and translator configuration currently contain local development paths. Review the [entry point](tools/hwaoi.py) and [translator builder](tools/build_xlate_hwaoi.py) during initial setup. Upstream `installer/` and release scripts have not been adapted for this game and are not installation instructions for this fork.

## Validation

```powershell
python tools/check_content.py
python -m unittest discover -s tests/hwaoi -v

# Probe output directories must be new; see each subcommand's --help
python tools/hwaoi_probe.py selftest --out work/reports/selftest-new
python tools/hwaoi_probe.py probe-romfs --out work/reports/romfs-new
python tools/hwaoi_probe.py probe-loader --out work/reports/loader-new
```

Some native/shader tests require the local compiler and translator. The latest 79-test pass used the verified `HWAOI_TEST_XLATE` helper; skipped tests caused by missing dependencies do not reproduce that result. Repository CI checks submitted content and the content-guard rules. It does not generate or run the game.

## Layout and provenance

| Path | Purpose |
| --- | --- |
| `recomp/` | Static recompilation generator |
| `runtime/` | C/C++ system services, graphics, audio and input runtime |
| `tools/hwaoi.py` | Fork entry point for validation, generation, building and launching |
| `tools/hwrecomp/` | Earlier NUP/NSO analysis prototype |
| `config/` | Exact input profiles and upstream provenance |
| `tests/hwaoi/` | Fork regression tests |
| `docs/hwaoi/` | HWAoI progress and adaptation records |
| `docs/upstream/` | Historical imported documentation, not HWAoI verification |
| `work/` | Local inputs, generated code, builds and diagnostics; ignored by Git |

The upstream base is DKB Recomp commit `42fcd9d78b4b44c2831b68ba21e5807d74a96f45`, recorded in [config/upstream.json](config/upstream.json). Bananza/TotK address patches and playability claims cannot be applied to HWAoI without evidence.

Do not commit or attach ROMs, keys, extracted resources, generated game code, game shader payloads or game-derived binaries. See [CONTRIBUTING.md](CONTRIBUTING.md) for contribution and issue-reporting guidelines.

The project inherits GPL-3.0-or-later overall. MIT and other third-party components retain their original terms and attributions; see [LICENSE](LICENSE), [LICENSE-MIT](LICENSE-MIT) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). This project is not affiliated with Nintendo or Koei Tecmo. Game names and trademarks belong to their respective owners.
