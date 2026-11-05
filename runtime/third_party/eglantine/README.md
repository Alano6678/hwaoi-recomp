# Eglantine Ampere lowering (GPL-3.0-or-later)

`shader_recompiler/frontend/ampere/lower_to_maxwell.h` and `program_capture.h` originated from
[Franseven/PokeDinner-a-Eglantine-Fork](https://git.eden-emu.dev/Franseven/PokeDinner-a-Eglantine-Fork), a fork of
Eglantine (an Eden-derived Switch 2 emulator), at the commit in `UPSTREAM_COMMIT`. Copyright 2026 Eden Emulator
Project, GPL-3.0-or-later; their encoding reference is Mesa NAK's sm70_encode.rs (MIT, Copyright 2022 Collabora,
Ltd.).

The HWAoI fork retains its Maxwell backend adaptations and independently tested
instruction fixes. Scalar 2D `TLD4.F16` pair packing was cross-checked with
[Eglantine commit 8b8d0fc85f1267dc93d069c5a331c4f24ac3a835](https://github.com/Djihads80/Eglantine/commit/8b8d0fc85f1267dc93d069c5a331c4f24ac3a835)
(GPL-3.0-or-later). Components are packed in enabled-mask order, with the first
component in the low half. Original synthetic tests in
`tests/hwaoi_ampere_lowering.cpp` cover this conversion and the general TEX path;
unsupported half-result gather combinations still fail explicitly.

They rewrite Switch 2 (Ampere, SM86) shader code as Maxwell instructions, which the shader translator this project
already uses can take. `common/common_types.h` is ours: the fixed-width type names the headers expect.

Because these files are GPL-3.0-or-later, DKB Recomp as a whole is distributed under GPL-3.0-or-later (see the
repository's LICENSE).
