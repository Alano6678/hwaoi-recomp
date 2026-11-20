# Temporal AA / upscaling reconnaissance

Reconnaissance only: no runtime code changed. Measured with the frame dump (runtime/vk_dump.c) on the
battle replay (work/replays/battle-slow.txt, mid-battle), HWAOI_OUTPUT_RES=1920x1080:

- run A: HWAOI_DUMP=3602, HWAOI_DUMP_LOG=3597, UBO=1, SKIP_RUNS=1 (six consecutive frames from one run,
  images of 3602), plus a temporary raw-float depth write (reverted);
- run B: HWAOI_SCENE_RES=1280x720, metadata only (pass structure at a smaller scene).

Pass numbers vary per run and frame (run A frame 3602 started at pass 1664; an earlier dump had the lighting
at 123), so the criteria below never use them. Shader hashes are SPIR-V hashes as printed in passes.txt.

## 1. Frame structure (in order)

| Step | Recognise by | Purpose |
|---|---|---|
| probes | 2x1 rgba32f, 1x1 rg32f, 1-2 draws | readbacks; the 1x1 pass's vs c3 [0..3] already holds this frame's VP |
| shadows | depth-only d32f 2048x2048 x2, 1024x1024 x2 | cascades / local shadows |
| depth prepass | depth-only d32fs8 at scene size, ~100 draws, vs 13b6/fs 4f00 | |
| G-buffer | t0 rgba8srgb (vk 43), t1 rgba16 unorm (91, normals), t2 r11g11b10f (122, emissive), depth d32fs8 (130), all scene size; ~12 passes; the draws write stencil | |
| shadow mask | rgba8 (37) array layer, 1 draw vs 545b/fs 32a6, samples shadow maps + depth + t1 | |
| compute | many 1x1x1 dispatches (cs a28c... etc.), then cs f356 at (w/64)x(h/64)x16 | culling/light lists; froxel volume |
| **lighting** | single r11g11b10f target at scene size + depth, cleared to 0, 2-3 fullscreen draws vs 545be95c29f4f09d fs ad60a723207be8b6 | deferred lighting -> **scene colour** |
| half-res copy | 960x540 r11, fs 207c (plain copy) | |
| post-lighting G-buffer | 5 targets (G-buffer + rgba8 + t2 again), 2-3 draws | decal-like touch-up |
| sky/fog | scene colour + depth, 2 draws (vs 545b fs 5107, vs 6979 fs 0286), 32x128x256 rgba16f volume | |
| velocity | rg16f (83) scene size + depth, 1 draw vs 0e09 fs 675fd1beeaa4390d, stencil test | see 4 |
| refraction/water | fs dfff/bef2; the scene colour is copied first and sampled with depth | |
| forward effects | ~10 one-draw passes (vs e442) into scene colour with depth | |
| half-res effect | w/2 r11 + r16f from scene colour + depth, r16f blur, rgba16f with a 37x37 kernel, composite back into scene colour without depth (vs a111) | DoF-like, before the transparents |
| **transparents/particles** | dozens of passes into the same scene colour with depth (vs 5bc7/7516/87ef/90a1, fs 4f9c...) | all **pre-tonemap, HDR** |
| bloom | w/2 r11 fs **ea72** (reads scene colour + 4x1 r32f exposure), mip chain down to 7x4 (fs 2163/2aa5) and up (fs c2fa) | starts the post chain |
| bloom composite | scene colour, no depth, vs d67d fs 3b04 | |
| exposure | cs b972 (w/16)x(h/16) histogram, 1x1 dispatches -> 4x1 r32f | read by next frame's bloom (only full-frame history: 4x1 exposure) |
| grade | new r11 target, vs d67d fs 9db7, 32x32x32 rgba16f LUT | |
| **tonemap** | rgba8srgb, vs d67d fs 047b6309d5f91a1c; at native scene it writes the output image (3840x2160 allocation, render area = output) | HDR -> LDR |
| world UI | 6914/e585/c06e draws into the tonemapped target; 1800x1024 rgba8 offscreen UI | markers, numbers |
| game upscale | scene < output only: cs 7f87acdabd353565 (FSR1 EASU signature: 64 threads, 16x16 tiles, (ow/16)x(oh/16)) scene -> output rgba8srgb | |
| sharpen | cs 503c621cb3352326 (FSR1 RCAS: 64 threads, 4 px each, limit 0.1875), output size, then fs 207c copies it back | always runs |
| HUD | 2D passes into the output image; 910x170 / 480x270 small panels | |

No temporal AA: no scene-size image is sampled before the frame writes it (only static LUTs and the 4x1
exposure carry over), and the projection has no jitter (section 3). The game's AA is "none + RCAS".

Upscaler input: **the lighting target** (r11g11b10f, linear HDR, contains opaque + sky + transparents) as it is
when the first bloom pass (w/2, fs ea72, samples it) begins; depth = the scene d32fs8 bound with it.
Robust arming rule: after a G-buffer pass (3 colour targets 43/91/122 + 130 depth), the next pass with exactly
one r11g11b10f colour target at the G-buffer size plus that depth is the lighting target; the scene ends at
the first pass whose fragment shader is ea72 (or structurally: a w/2 single-target pass that samples the
armed image and is followed by a halving chain). Do not use "first pass narrower than the scene": two w/2
passes (the copy after lighting, the DoF-like effect) come earlier and also read the scene colour.

## 2. Camera

The lighting pass (fs ad60) fragment c3, size 0x600 (same in all three draws):

- vec4 [11] = camera world position (x, y, z, 1);
- vec4 [12..15] = view-projection, row-vector convention: clip = (x, y, z, 1) * M, M rows = vec4 12..15
  (equivalently column j of the usual column-vector matrix is vec4 12+j). Column 3 = (forward, -forward.cam).

Verified on frame 3602: unprojecting 576 depth samples of the raw d32 depth with inv(M) gives view depths
matching B/(z-A) to 1.4e-4 relative; the lower screen lands on flat ground (world y 8720-8766 at rows
540-1020, camera y 9011); cam * M = (~0, ~0, 64.02, ~0). P00 = 1.50448, P11 = 2.67462 (aspect 1.77777,
vertical FOV 41.000 deg). Same layout at a 1280x720 scene (P00 unchanged).

Depth: **reversed Z, finite far**: z = 64.0195 / d - 0.000304855 -> near = 64.000, far = 210000 (world units;
likely cm). Depth cleared to 0, compare GEQUAL (func 7), range 0..1. Clip y is up: ndc.y = 1 - 2 py / h
(the velocity vs c3 [0] = (2/w, -2/h, -1, 1)).

The block address is NOT stable: a per-frame ring (0x2321d1000, 0x232750300, 0x231ccdb00, ... in six
frames), so copy the 64 bytes when the lighting draw is recorded (as totk vk_dlaa_draw does with cam_addr).
Many object blocks embed the same VP at shader-specific offsets (vs c3 [0], [1] or [2]); only the lighting
block is a fixed, single place. No previous-frame VP was found in any listed block (first 16 vec4); the
runtime must keep last frame's copy itself (totk vk_dlaa_prev_camera pattern).

## 3. Jitter

None. Over frames 3597-3602 the projection's x and y rows dotted with forward (the jitter terms) are
<= 3.1e-6 and <= 6e-7 NDC (< 0.003 px); the translation row matches -cam * rows exactly. The runtime must
inject it (totk vk_dlaa_viewport: viewport offset on passes drawing with the scene depth).

## 4. Motion vectors

No object velocity. The only velocity output is the rg16f full-scene pass (fs 675f, cleared before the
G-buffer): a fullscreen triangle with no texture inputs and a stencil test; it evaluates
p' = (ndc.x, ndc.y, c3[0].x = 0, 1) * c3[1..4] (z = 0 = far plane), so it gives only camera rotation motion
for background (sky) pixels. In frame 3602 (no sky visible) it is all zero, and no pass in the frame samples
it. Its matrix c3[1..4] is a reprojection to an earlier camera but measures only 0.47-0.62 of the real
frame-to-frame motion (variable, likely shutter/frame-time scaled for motion blur), so it is not usable.

Plan: camera-only reprojection from depth + current/previous VP (totk dlaa_mv.comp). The camera-only motion
of a static point between 3601 and 3602 was a median 0.33 px; the battle crowds and the player move on
their own and will ghost/smear. Mitigations: a reactive/disocclusion mask for characters (the G-buffer
draws write stencil; whether a stencil bit or t1.a separates characters is unverified), or later per-object
motion from cached instance matrices (object blocks hold per-instance 3x4 world matrices from vec4 [6]).

## 5. Blockers and hazards

- Scene size: HWAOI_SCENE_RES (diagnostic override, resolution.c res_init / hwaoi_display.h) renders the
  whole scene chain (G-buffer through tonemap and world UI) at that size; run B confirmed every scene
  target is allocated at 1280x720 (not a 1080p allocation with a smaller viewport) and the game's EASU then
  scales to the output. Only the presets 1280x720, 1920x1080, 2560x1440, 3840x2160 are accepted, scene <=
  output, fixed at start (restart required, pool plan). Upscaler ratios (1.5x, 1.7x, 2x) need arbitrary
  scene sizes added there and tested for the texture pool. Dynamic resolution must stay off
  (native1080/display patches pin the scale to 1.0).
- The game scales after tonemap, in LDR, with the world UI already drawn into the scene-size image. To
  upscale in HDR the bloom/grade/tonemap/world-UI passes would have to run at output size; replacing EASU
  instead keeps the post chain but feeds the upscaler tonemapped LDR containing world UI.
- The output image is a 3840x2160 allocation drawn with a 1920x1080 render area: use render area, not
  image extent.
- Fullscreen passes that bind the scene depth (lighting, sky, velocity, refraction, shadow mask) must not be
  jittered by a viewport offset; jitter only geometry passes (prepass, G-buffer, transparents) or the
  lighting unprojection will be misaligned by the offset.
- RCAS (cs 503c) always sharpens the output; a temporal pass with its own sharpening should skip it.
- The present has FSR1 scaling (gfx_present.c present_image); a present-side AA pass, if any, is redundant with it.
- UpdateUniformBuffer can rewrite blocks within a submission (vk_exec_draw.c upd_note); the lighting block
  values matched depth, but read it at record time.

## Integration plan

1. vk_exec.c vk_exec_pass_begin: classify passes (G-buffer signature, lighting target arm, bloom start by
   fs ea72 sampling the armed image, scene-size vs output-size); reset per frame at present.
2. vk_exec_draw.c prepare_draw: when the bound fs is ad60 in the armed pass, resolve ex->ubo[fs][0] (c3)
   with vk_mem_resolve and copy vec4 [11..15]; keep last frame's copy (prev VP). Missing camera or a target
   change resets history.
3. vk_exec_draw.c record_fixed (viewport): add the sub-pixel jitter (Halton) to viewports of non-fullscreen
   passes that use the scene depth from the prepass to the last transparent pass; record ux/uy for the
   upscaler and the MV pass.
4. At the bloom-start pass begin (vk_exec_pass_begin, before recording it): run an MV compute (depth +
   inv(VP) + prev VP, camera only, as totk dlaa_mv.comp) and the temporal AA (DLAA/FSR native AA) in place
   on the lighting target at scene == output. Bloom, grade, tonemap and UI then see the AA'd picture.
5. Upscaling (scene < output): in vk_exec_draw.c op_dispatch, replace cs 7f87 (EASU) by the upscaler reading
   its input (LDR, scene size) with the scene depth/MVs and writing its output image; optionally skip cs
   503c. Alternative HDR path needs the post chain at output size (see 5) and is a larger change.
6. resolution.c / hwaoi_display.h: accept arbitrary 16:9 scene sizes below the output for DLSS/FSR ratios,
   with the pool plan re-measured.
7. gfx_present.c present_image: keep the FSR1 present scaling.

## Implementation (runtime/vk_temporal.c, runtime/vk_ffx.c)

`HWAOI_TEMPORAL=fsr` (restart: device extensions) runs AMD FSR through the ffx-api of
`amd_fidelityfx_upscaler_vk.so` next to the executable (fsr4-vk, GPL-3.0, never linked or copied: build it with
`tools/build_fsr4_vk.sh --work work/fsr4-vk-build --dest work/build/hwaoi196608`). Without the library the log says
`fsr: ... FSR off` and `temporal: ... rendering unchanged, no jitter`, and the frame is the game's own.

- Classification as above (G-buffer with formats 43/91/122 + 130, lighting target armed after it, bloom start by
  fs `ea72...` on a half-width R11G11B10F target without depth). Frames start at the next depth prepass or
  G-buffer pass after FSR ran (the present is not a frame boundary in the replay).
- Camera: the lighting draw has two variants, fs `ad60a723207be8b6` (c3 0x600) and `1f117d51945d8d8e` (c3 0x500,
  frames with fewer lights); both hold the camera at vec4 [11..15]. The block is accepted by content
  (camera * VP = (0, 0, 64, 0)), not by hash.
- Jitter: Halton(2,3) viewport offset on draws with more than 6 vertices that use the scene-size D32S8 depth, until
  the scene ends; full-screen draws are never moved. Sign convention checked on a still-camera cutscene
  (presents 1386-1505 of battle-slow): `HWAOI_FSR_JITTER_SIGN=-1` and `HWAOI_TEMPORAL_JITTER=0` both give
  visibly worse silhouette edges than the default.
- Motion: camera-only, `shaders/temporal_mv.comp`; the scene depth is copied to R32F for FSR (D32S8 is not an
  FSR depth format). Characters and crowds moving on their own get no vectors and can trail.
- While FSR runs the game's RCAS gets strength 0 (`HWAOI_SCENE_SHARPNESS` set
  explicitly still wins); FSR's own sharpening is `HWAOI_SHARPNESS` (0..100, the menu's Sharpness). Strength 0 was
  verified to remove the game's sharpening (edge energy of a still frame 3.64 -> 1.75).
- Debug keys: `HWAOI_TEMPORAL_DEBUG=1` (motion vectors in place of the picture), `HWAOI_TEMPORAL_MV=0`,
  `HWAOI_TEMPORAL_JITTER=0`, `HWAOI_TEMPORAL_LOG=1` (still-camera stretches by present number),
  `HWAOI_FSR_DEBUG=1` (ffx-api checks), `HWAOI_FSR_MODEL=fp8|int8`, `HWAOI_FSR_VERSION`.

### Upscaling (`HWAOI_FSR_QUALITY`, restart)

`quality`, `balanced` and `performance` render the scene at the output divided by 1.5, 1.7 or 2 per side
(1920x1080: 1280x720, 1130x636, 960x540; `hwaoi_display_fsr_scene`); an explicit `HWAOI_SCENE_RES` wins, and
without the library the scene stays at the output size (logged). The motion vectors and R32F depth are made at
the bloom start as in native mode; FSR then runs in place of the game's EASU dispatch (cs `7f87acdabd353565`,
recognised by its (ow/16)x(oh/16) groups, one scene-size RGBA8 input and one output-size RGBA8 store) on the
tonemapped LDR picture, which is blitted to RGBA16F first (fsr4-vk takes only float colour, and without auto
exposure it wants an exposure texture, so both modes use auto exposure). The result is blitted back into the
EASU's output (sRGB decode/encode through the blits), and RCAS (strength 0) and the HUD follow at output size.
Checked on battle-slow at 1920x1080: quality and performance run every frame, colours match the game's EASU
path, silhouettes are clean where the game's EASU+RCAS shows stair-steps and halo noise, the HUD stays sharp.
Known: the world UI (markers, damage numbers) is drawn into the scene picture before the upscale and gets
only camera motion, so moving markers can smear; characters trail as in native mode; no texture LOD bias.

## Windows

Same code path as Linux; the library is AMD's, not fsr4-vk. `amd_fidelityfx_loader_vk.dll` exports the ffx-api and loads
`amd_fidelityfx_upscaler_vk.dll` (FSR 4.1.2 on RDNA 2 and newer, FSR 3.1.6 otherwise) from the executable's folder. When
FSR 4 is selected and they are missing, `runtime/vk_ffx_fetch_win.c` downloads both with Windows' `curl.exe` from the
FidelityFX SDK repository (tag v2.4.0, `Kits/FidelityFX/signedbin/`), checks the pinned SHA-256 sums and only then renames
them into place (menu: picking FSR 4 starts it in the background, restart afterwards; at start it blocks). Offline or for
a release folder: `python tools/fetch_fsr_dlls.py <folder>`. The Settings "FSR version" row (`HWAOI_FSR_VERSION`, auto or
3.1) pins FSR 3.1 instead of the newest FSR 4. Not built or run on Windows yet: the Windows branch (loader, DLL fetch,
menu note) was only syntax-checked against stub headers.
