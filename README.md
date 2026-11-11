# HWAoI Recomp

[![License: GPL-3.0-or-later](https://img.shields.io/badge/license-GPL--3.0--or--later-blue)](LICENSE)
[![Content guard](https://github.com/Alano6678/hwaoi-recomp/actions/workflows/content-guard.yml/badge.svg?branch=main)](https://github.com/Alano6678/hwaoi-recomp/actions/workflows/content-guard.yml)
[![Stars](https://img.shields.io/github/stars/Alano6678/hwaoi-recomp?style=flat)](https://github.com/Alano6678/hwaoi-recomp/stargazers)

**HWAoI Recomp** is an experimental static recompilation of *Hyrule Warriors: Age of Imprisonment* for Nintendo Switch 2 into a native Windows x86-64 application with a Vulkan renderer.

The game's ARM64 code is translated to C ahead of time and compiled with a C/C++ runtime. An NVN 2 implementation handles graphics, translating Ampere shaders to SPIR-V for Vulkan. This project builds on [DKB Recomp](https://github.com/Lucas-blip-png/dkb-pc-recomp).

## Project lead — [Alano6678 / AlanoFX](https://github.com/Alano6678)

HWAoI adaptation, runtime and renderer development, Windows compiler distribution, memory optimization, testing, and project maintenance.

## Major contributor — [LibertyPrime27](https://github.com/LibertyPrime27)

Combat performance and QUADS batching, buffer-texture lighting, first-load stutter reduction, update and DLC support, two-player controls, and compiler/settings improvements. Their work is included through [#9](https://github.com/Alano6678/hwaoi-recomp/pull/9), [#10](https://github.com/Alano6678/hwaoi-recomp/pull/10), and [#11](https://github.com/Alano6678/hwaoi-recomp/pull/11).

> [!NOTE]
> Bring your own matching game files. No ROMs, keys, firmware, generated game code, shader payloads or game-derived executables are distributed here. Recompilation happens locally.

## Current status

**Early development build — updated November 12, 2026.** The game boots, plays both opening movies and reaches outdoor exploration and initial combat. Movement, camera controls and the opening 3D rendering work. A complete playthrough has not been verified.

Native **1080p / 60 FPS** is the current testing baseline. The integrated build reaches the opening field, renders correctly and responds to movement on the RTX 4060 / Core i5-14600KF test PC; a stationary opening sample measured **59.9 FPS** at the 60 FPS target. A complete playthrough and the latest crowded-combat performance have not been verified locally.

## Recent progress

| Area | What has changed | Validation and remaining work |
| --- | --- | --- |
| Combat rendering | Indirect QUADS are converted on the GPU, adjacent conversions are batched, and invalidate-only barriers avoid unnecessary GPU stalls. | GPU tests pass. LibertyPrime27 reports approximately 57 FPS in fights that previously ran at 13–18 FPS on their PC; this is their measurement. |
| Local lighting | Buffer textures and graphics texel-buffer fetches now expose the game's light lists and parameters. | The contributor verified the previously dark cave. Broader scene coverage remains open. |
| Loading | Background pipeline builds, prioritized shader translation, early compute translation and remembered RomFS verification reduce repeated work. | The contributor reports a cold-scene stall reduction from about 7.5 seconds to 0.9 seconds. Compute prefetch is integrated with our compressed source cache. |
| Host memory | Compact command records, smaller Vulkan pools, bounded background reclamation and lossless program-source compression. | Integrated opening run retains approximately **163 MiB less shader source**. 16 GiB PCs and long battles still require validation. |
| Updates and DLC | Separate update/DLC NUP inputs, verified extraction and runtime add-on mounts. | Parser, refusal and mount tests pass; installed game content still needs matching dumps. |
| Controls and co-op | Per-player devices and bindings, controller hotplug, rumble/motion routing, bindable host menu and unbound actions. | Integrated in source. Full two-player mission coverage remains unverified. |
| Windows compiler | Reuses verified resources and unchanged native objects; supports compile/update/resume and a locally extracted game icon. | Game data and artwork stay in the user's output folder. |
| Regression coverage | **183 tests passed, 3 skipped** in the latest integration run. | Synchronous and asynchronous GPU geometry each passed 6,416 checks; presentation checked 24,868,359 pixels with zero Vulkan validation errors. |

## Memory progress

Current source uses compact host command records, smaller Vulkan allocation pools and bounded background reclamation of completed CPU command copies. An opening-field census stored the equivalent of **186 MiB** of dense records in **128 MiB** (about 31.3% less). Large copy-retirement events fell from approximately **193 ms** on the recording thread to **3–5 ms**. These changes reduce memory overhead and periodic stalls; a paired opening-field test did not establish a sustained FPS gain.

Unused single-stage NVN2 program source is now retained with lossless compression and a checksum, then expanded on first use. An opening-field test before the recent performance PRs saved approximately **166 MiB** of host allocations; the compressed payload is released after expansion. That same-executable comparison measured **76.9 FPS** with compression both enabled and disabled. Registration-time compute prefetch also uses checked expansion. These figures cover this scene and do not establish combat performance.

An opt-in Windows demand-commit heap experiment also left approximately **1.4 GiB** of guest heap backing uncommitted in the tested opening, while preserving the guest-visible heap capacity. It remains disabled by default. These measurements cover the opening field; 16 GiB RAM systems and extended combat still need validation.

> [!WARNING]
> **Frame-rate modes other than 60 FPS still have gameplay-speed errors.** Current source corrects the native scheduler clock, but a normal-input movement comparison still shows a separate speed difference. Character, animation and combat timing remain under adaptation.
>
> **Higher-resolution modes are unstable.** Their 3D render sizes have been verified, but broader gameplay, UI and movie scaling are still being adapted.

## Current priorities

1. **FPS++ timing:** keep walking, running, animation, gameplay timers and save behavior consistent at 30, 60, 90 and 120 FPS. A higher frame cap alone does not complete this work.
2. **Resolution and UI:** preserve the interface's relative size and layout at 720p, 1080p, 1440p and 4K while rendering the scene at the selected resolution.
3. **Sustained performance:** reduce crowded-combat cost and first-use stalls without dropping required draws or changing game logic.

## Feature status

| | Part | Progress |
| --- | --- | --- |
| ✅ | Static recompiler | All three guest modules generate, compile and link into a native Windows executable. |
| ✅ | Boot and resources | The matching build passes title/module checks and loads its game resources. Automatic GPU mirroring now checks Windows system-commit headroom. |
| ✅ | Opening movies and audio | Both opening movies play through; audio and voices work in the tested opening. |
| ✅ | Movement and camera | Keyboard/mouse movement and camera controls work; SDL controller support is integrated. |
| 🔧 | Rendering | Outdoor scenes and initial combat render. Small CPU-authored RGBA8 comparison textures up to 8×8 now have a tested conversion path; the reported crash scenes await retesting. |
| 🔧 | Combat performance | NVN2 indirect QUADS now have a GPU argument/index conversion path, avoiding their ordered CPU readback. Local GPU/recorder tests pass; crowded-scene FPS gains still need measurement. |
| 🔧 | Memory use | Compact command records, smaller allocation pools and bounded background CPU-copy reclamation are enabled. Demand-committed guest heap backing and the read-only vertex cache remain optional experiments. |
| 🧪 | 30 / 60 / 90 / 120 FPS | The native unlock exceeds 60 FPS. Inherited/fixed scheduler steps now follow each node's elapsed clock and preserve parent time scales. Live clock checks pass at all four targets; movement still has a separate frame-dependent speed error. |
| 🧪 | Native resolution presets | 720p, 1080p, 1440p and 4K 3D dimensions are confirmed in GPU captures. Higher resolutions remain experimental and unstable. |
| 🔧 | Playthrough coverage | Opening exploration and initial combat only; later missions remain unverified. |

✅ working in the tested scope · 🔧 adaptation in progress · 🧪 experimental

## Trying the development build

The [Releases page](https://github.com/Alano6678/hwaoi-recomp/releases) provides an experimental Windows x64 compiler package. Extract the complete ZIP, open **HWAoI-Compiler.exe**, select your own matching **NUP** and an output folder, then click **Compile / update**. Python, the C/C++ toolchain and the shader translator are bundled; no separate tool installation is needed.

After compilation, use **Open compiled game**. Keep the NUP and output folder at their original locations. Allow at least **20 GiB of free output space**; an AVX2 CPU, Vulkan GPU and sufficient memory are required (32 GiB RAM recommended). The compiler accepts only the exact build below; encrypted XCI/NSP files and other versions are unsupported. The resulting local game files are not for redistribution.

Compiler v0.1.0-alpha.5 also accepts a complete game stored as a `standalone_update` without a `base` record. It verifies both the exact executable modules and the complete resource inventory before extraction. Partial update dumps still need the matching base. NUPs that also contain merged DLC/addon packages are accepted: the verified base + update is compiled as before, and each DLC is installed under `work/game/aoc/<index>` for the game's add-on content mounts (see [DLC support](docs/hwaoi/dlc.md)). DLC dumped separately can be added with `tools/hwaoi_aoc.py from-dir`.

The compiler window also has two optional fields. **Update file** takes the game's update as its own NUP when the game file holds only the base game; the base and update are verified together as one game and the update's files are copied into the output, so only the game file must stay in place afterwards. **DLC file** takes a NUP holding the game's DLC. Leave both blank when the game file already includes the update or DLC. The v0.1.0-alpha.1 compiler can report `no base package` for this layout.

Current source builds support updating or resuming an existing compiler-owned output folder. Verified resource caches, unchanged generated C and native objects are reused; saves and settings are retained, and replaced source files are backed up under `work/backups`. **Build jobs = 0** chooses generation and compilation concurrency from available RAM and system commit, with large C files limited to one concurrent compile. Compilation optimization levels are unchanged.

Source builds remain available. The inherited upstream installers and release scripts are not HWAoI-ready; this fork uses its own compiler frontend.

Only the exact build recorded in [the HWAoI game profile](config/games/hwaoi-update-196608.json) is accepted. The tools check the title and module hashes before selecting game-specific patches, and leave the original game package unchanged.

Start with the fork's entry point:

```text
python tools/hwaoi.py --help
python tools/hwaoi.py verify
```

Resource preparation, C generation, compilation/linking and launching are separate steps. The `prepare-romfs`, `generate`, `build` and `launch` subcommands describe their required inputs.

The experimental native frame-rate profile is generated with:

```text
python tools/hwaoi.py generate --rate-unlock --out work/generated/hwaoi196608-full --limit 0 --jobs 2
```

After building that generation, `HWAOI_RATE_UNLOCK=1` enables the candidate. It remains off by default while gameplay timing is being tested. The frame-rate target can then be selected in the host settings. Older generations without the matching step profile must be regenerated. `HWAOI_RATE_DIAG=1` optionally logs scheduler-step totals against wall time without altering the clock.

Press **Esc** for host settings, **F10** to cycle the performance overlay, and **F11** for fullscreen. Resolution changes require a restart. Actual FPS depends on the scene, hardware and selected resolution.

For Windows source-build testing, `HWAOI_LAZY_HEAP=1` enables the demand-commit heap experiment. It changes when host backing is allocated, while keeping the game's logical heap size unchanged; additional pages are committed when needed. It is not yet a validated 16 GiB system preset.

Lossless program-source compression is enabled in current HWAoI source builds. `HWAOI_SOURCE_COMPRESS=0` disables it for comparison or troubleshooting; a restart is required.

Asynchronous replay can be tested with `HWAOI_REPLAY_ASYNC=1`. A same-executable pair in the stationary opening field, with 1080p and a 120 FPS target, measured median rates of **75.5 FPS synchronously and 86.3 FPS asynchronously** on the test PC with power-saving enabled. It remains disabled by default pending broader scene and synchronization checks; the gameplay-speed caveat above still applies.

## Testing and reports

Validation includes module/hash refusal checks, independent instruction/semantic tests and Vulkan pixel-readback probes. Generation and a successful build alone do not establish playthrough coverage.

For a useful [issue report](https://github.com/Alano6678/hwaoi-recomp/issues), include the commit, GPU/CPU, resolution, frame-rate mode, where the problem occurs and the runtime log. Please keep game files and keys out of reports.

## Credits and license

Based on [DKB Recomp](https://github.com/Lucas-blip-png/dkb-pc-recomp), with its revision pinned in [config/upstream.json](config/upstream.json). Menu adaptations also credit [Savage Wind Recomp](https://github.com/Revivedx/SavageWindRecomp); the full inherited attributions remain in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Thanks to [LibertyPrime27](https://github.com/LibertyPrime27/Hyrule-Warriors-AOI-Recomp-Optimized) for the NVN/GFX stage-ID fix, batched GPU QUADS conversion, barrier optimization, buffer-texture lighting, background shader loading, update/DLC support, and co-op/control improvements. Their contributions were merged through [#9](https://github.com/Alano6678/hwaoi-recomp/pull/9), [#10](https://github.com/Alano6678/hwaoi-recomp/pull/10), and [#11](https://github.com/Alano6678/hwaoi-recomp/pull/11).

GPL-3.0-or-later overall, with inherited MIT and other third-party terms preserved. See [LICENSE](LICENSE) and [LICENSE-MIT](LICENSE-MIT).
