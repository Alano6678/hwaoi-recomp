# HWAoI Recomp

[![License: GPL-3.0-or-later](https://img.shields.io/badge/license-GPL--3.0--or--later-blue)](LICENSE)
[![Content guard](https://github.com/Alano6678/hwaoi-recomp/actions/workflows/content-guard.yml/badge.svg?branch=main)](https://github.com/Alano6678/hwaoi-recomp/actions/workflows/content-guard.yml)
[![Stars](https://img.shields.io/github/stars/Alano6678/hwaoi-recomp?style=flat)](https://github.com/Alano6678/hwaoi-recomp/stargazers)

**HWAoI Recomp** is an experimental static recompilation of *Hyrule Warriors: Age of Imprisonment* for Nintendo Switch 2 into a native Windows x86-64 application with a Vulkan renderer.

The game's ARM64 code is translated to C ahead of time and compiled with a C/C++ host runtime. Graphics calls are handled by an NVN 2 implementation, with Ampere shaders translated to SPIR-V. The project is based on [DKB Recomp](https://github.com/Lucas-blip-png/dkb-pc-recomp).

> No game files, keys or game-derived builds are included. You need your own matching game dump.

## Status

**Work in progress.** The game boots, plays both opening cutscenes and enters the outdoor 3D scene. Movement and camera controls work, and the first outdoor area has run at approximately **60 FPS** in measured tests on an RTX 4060. Adaptation is now progressing into combat scenes.

| Phase | | Progress |
| --- | --- | --- |
| F0 CPU | ✅ | All three guest modules are generated, compiled and linked into a native Windows executable. |
| F1 OS | ✅ | Boot, resource loading, system services, audio and controller integration are running. |
| F2 NVN 2 | ✅ | Texture views, depth/stencil copies and indirect drawing have been adapted. |
| F3 Shaders | ✅ | Ampere-to-Maxwell lowering and SPIR-V translation support the opening scenes. |
| F4 Vulkan | ✅ | Settings, cutscenes and the first outdoor area render, with colour-space and memory-transfer improvements. |
| F5 Progression | 🚧 | Movement, camera controls and opening movie playback are verified; combat rendering and performance are being adapted. |

Native semantic tests and Vulkan pixel-readback probes cover the implemented paths. Further frame-rate and resolution profiles are being investigated.

## Credits

Based on [DKB Recomp](https://github.com/Lucas-blip-png/dkb-pc-recomp), with the upstream revision recorded in [config/upstream.json](config/upstream.json).

GPL-3.0-or-later overall; inherited MIT and other third-party terms remain intact. See [LICENSE](LICENSE), [LICENSE-MIT](LICENSE-MIT) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
