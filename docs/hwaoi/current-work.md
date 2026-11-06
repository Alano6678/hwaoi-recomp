# Live bring-up ledger — 2026-11-06

## November 6: rendering, movie and bandwidth investigation

- Corrected sampled UNORM/sRGB reinterpretation at explicit registered views.
  Scene-23's pass 0475/0476 had requested UNORM reads of sRGB storage, but the
  old get_view ignored that format. Material views also requested sRGB over BC1,
  BC3, BC7 and RGBA8 linear storage. Native descriptor/failure tests and original
  GPU pixel fixtures cover the supported pairs; other reinterpretations fail.
- Fixed the present path's extra sRGB decode into the UNORM swapchain by copying
  encoded RGBA/BGRA bytes to a matching UNORM intermediate before scaling.
  The 72-frame original GPU fixture passes with active Vulkan validation,
  including crop, resize, double-slot reuse, FSR and optional colour adjustment.
  Scene-28 repeats the outdoor scene with visible shadow detail. This is not a
  hardware-reference proof of all lighting/material semantics.
- The interactive trial later reached a missing DrawArraysIndirect entry
  (main+0x7293e4, QUADS). The new path consumes real GPU arguments; QUADS uses an
  ordered 16-byte readback then indexed expansion, with visible limits and sync
  cost. Original GPU-produced argument/pixel fixtures pass eight cases and 8,192
  exact pixels with validation. An initial fixture retained a producer geometry
  stage; binding all graphics stages for its plain program corrected the fixture.
- Black opening video: RomFS opens succeeded, but MediaExtractor::Initialize
  repeatedly returned 0x3ed00 with a null work buffer. Default-visible weak C++
  singleton data/guard definitions were locally bound in subsdk0 and sdk, splitting
  their allocator TLS state. Default-visible weak definitions now share the first
  eligible load-order definition; local/hidden/protected own references retain
  local binding. Synthetic multi-module tests and the old-policy mutation pass;
  the actual initializer then returns zero and configures the VP9 decoder.
- The newly reached SetCopyImageStride entry records a separate slice pitch and
  applies it to buffer/image copy regions with checked bounds and overflow.
  Original O0/O2 tests cover tight/padded/block layouts and rejected cases.
- Bandwidth: the mirror flush tracker previously joined disjoint flushes into one
  min/max envelope. Scene-33 uploaded about 2.8 GiB per displayed video frame
  despite about 10 MiB of declared flushes and ran at about 4 FPS. The tracker now
  retains sorted disjoint page intervals. Independent page-coverage/GPU-gap
  canaries and old-envelope mutations pass at O0/O2. The later movie run transfers
  about 9 MiB per present and presents around 59.9 FPS; the movie itself is 29.97
  FPS. Earlier 60-FPS settings-only runs are not counted as video/3D performance.
- Scene-35 plays the first opening WebM through EOS: 3,561 decoded frames,
  zero decoder errors, 5 dropped to retain wall-clock pacing. It then displays a
  second 1080p VP9 story clip. The 300-second supervisor requests close and gets
  exit 0; timed_out remains true, so this is a bounded playback observation.
- Current suite: 93 HWAoI tests pass with the verified translator and both opt-in
  GPU probes enabled. Reports: scene-35-full-tests.log, scene-31-indirect-tests.log,
  scene-36-loader. These checks do not establish combat or save round trips.
- Scene-36-story-walk completes its 600-second owned run with no fatal error and
  exit 0 after the supervisor closes the window (timed_out=true). Both movies
  reach EOS: 3,561 and 7,068 frames, zero decode errors. The scene then responds to
  movement and camera inputs at frames 33,000/33,900/34,200; images show a changed
  character position and view. Final interval: 3,470 frames / 57.9s, 59.9 FPS.
  The later attack events were beyond the presents reached before the deadline,
  so this run does not validate attacks. Executable SHA-256:
  e51b76614e947e156b3cff56701367b0ddd200e77306c84e45345acf9b82c703.
- Scene-37/38 attempt early movie skips before attack input. The first movie
  still plays to EOS; inputs sent while a movie is active are not gameplay
  evidence. Desktop automation can capture the window but cannot activate it.
  An explicit diagnostic input feed now permits expiring buttons/sticks without
  desktop focus. It is off by default, bounds holds to 10 seconds, rejects stale
  or malformed commands, and leaves the game to process normal HID input.
  See diagnostic-input.md. Scene-39 uses this for controlled progression.

The user's overnight priorities are FPS, rendering, crashes, controlled progression
and movie playback. Multi-rate FPS patches and 720p–4K scene-resolution changes
are conditional on qualifying those earlier milestones. Current window scaling
and an FPS limit are not evidence of game-speed-correct high-FPS patches.

## Earlier bring-up record

The user requested continued implementation without stopping at intermediate
milestones. Current code is still an experimental local port, not playable.
The initial meaningful-frame milestone is now observed: render-28 shows the
Chinese first-run settings UI, and hang-20 shows an actual 3D scene with characters,
grass and ruins. Firmware inspection remains paused. Scripted
Continue input advances loading; live controller gameplay remains unverified.

## Latest executed milestone

- hang-21 repeats the actual 3D scene after the capability fix. Capture
  work/reports/hang-21/presents/f01230.png shows the characters and outdoor scene.
  Vulkan validation is enabled; complete-log fatal, lowering-failure and
  validation-error counts are all zero. Runtime SHA-256:
  0307d0dd07ccdcf9fd208286e9b50afde2243c05c6111be775aa55c823235c6e.
  The report confirms the new translator source identity and managed DLL hashes.
  The supervisor closes the owned window at its 180-second deadline, then records
  timeout/124 after 185.703 seconds; this is not a normal-exit or stability pass.
  Performance remains poor: the final measured interval is 39 frames / 54.2s,
  with long CPU replay times. This first run under a new translator/cache identity
  is not a steady-state benchmark. Lighting correctness, responsive gameplay,
  performance and exit teardown remain outstanding. No game/helper process remains.
- The hang-20 capability error is reproduced byte-for-byte from its retained
  cache input. Fork translator patch 0002 declares StorageImageReadWithoutFormat
  only for emitted Unknown-format image reads; typed and write-only images do
  not gain it. The regenerated real module differs only by OpCapability 55 and
  passes Vulkan 1.3 validation. Eight synthetic image-use cases, removed-capability
  negative controls and O0/O2 device-feature rejection tests pass. The runtime
  now requires the matching supported/enabled read feature.
  Activated translator identity:
  22fe81c97e7c074efa43842cb71ad2b9c79425e9e9f3680183745f4dece44cba.
  The previous deployment remains in xlate-managed-irbanks-before-readformat;
  translator-activation-readformat.json records the rename. All 252 build-manifest
  payload hashes were checked. The current 79-test suite passes with this helper;
  hang-21 provides the subsequent live verification above. Probe reports retain managed DLL
  hashes and source identity, since the apphost EXE alone can remain unchanged.
- hang-20 is the first observed 3D-scene milestone. Actual capture
  work/reports/hang-20/presents/f01260.png shows characters, grass, trees and ruins.
  GPU completion timestamps for submissions including 8608 and 8650 name both
  formerly failing compute stages (5159ea53b0d4c432 and 2771af0c1ea82a4c).
  This establishes execution through those stages, not visual correctness or
  controllable gameplay. Lighting/contrast still needs investigation.
  Executable SHA-256:
  94b9903825635ff801c96d6abf020488d7e3a3d289e359922fc386527beb406e.
  The 180-second probe requests window close, then records timeout/124 after
  185.782 seconds. Fatal/lowering failures are zero; active Vulkan validation
  reports one missing StorageImageReadWithoutFormat SPIR-V capability at
  OpImageRead result 4400. The last heartbeat at present 1200 predates this
  validation error and later scene capture. This is not a clean-exit pass.
- The hang-19 shader's 20 scalar 2D LOD-zero F16 texel fetches use packed
  destinations that may be odd-numbered. A dedicated path now fetches F32 into
  scratch then rounds/packs each half pair; the old F32 alignment gate no longer
  rejects these supported forms. Independent Maxwell decoding/ALU fixtures pass
  2,617 cases at each O0/O2, and restored-alignment and reversed-packing mutations
  both fail. The complete original shader lowers and translates, and its SPIR-V
  passes vulkan1.2 validation. These checks do not prove GPU pixel correctness.
  The combined 77-test suite passes again; hang-20 tests the linked integration.
- hang-19, executable
  1e0577076fb7962d7f57c840827db1dea4fd11dad451fd84d571704c820a68d2,
  records both actual BGR conversions (cube six layers and 2D array one layer)
  as D16 value 65535, with original compare sampler and clamp-to-edge on all
  axes. Vulkan validation is active and logs zero errors. The preceding D32F/S8
  copy path is accepted. GPU timing reports completed later draw/dispatch parts,
  but do not individually prove every fixed shader's output correct.
  The last image at present 1230 is black loading. A new required compute shader
  fails at native +0x210, "native texel fetch mode or descriptor binding",
  origin 2771af0c1ea82a4c; raw 6400-byte code and 224-byte control are retained.
  The validation-layer exit issue leaves supervisor timeout/124 at 180.766s;
  this is not a completed run. The next missing form is scalar 2D F16 texel fetch.
- The observed BGR-only mapping now has a scoped conversion: actual equal RGBA
  values remain mandatory, the original comparison sampler/Dref remain intact,
  and unsupported mappings or border-sampling modes fail explicitly. This is
  an inference from component-source mappings and RGB-replicated native shadow
  outputs, not a hardware capture proving arbitrary color comparison behavior.
  The sampler snapshot uses a lock-held helper to avoid recursive texture locks.
- The generic-wrapper pool failure now has a separate gate for size 0x38,
  LR+0x806dc, exact wrapper register/frame shape and four validated saved
  returns (+14c3e24, +14dfdac, +1af5e64, +6c3980). Original allocation and
  five-second retry limits are retained. All 138 native O0/O2 cases pass;
  transitive deadlock freedom is not established. A later module-extent guard
  additionally covers the farthest saved return; its 144 native cases pass and
  it postdates hang-19. The combined HWAoI suite passes 77 tests.
- hang-18 (validation disabled) passes the new depth/stencil T2T recording path
  and captures the actual comparison mapping: B,G,R,A over six all-255 RGBA
  texels. It rejects this mapping explicitly after 1200 presents, exiting 70
  in 83.453 seconds. This is not a game frame or a successful comparison pass.
  Source texture selectors are identity; the registered view requests BGR.
  The native comparison's channel-selection semantics remain under audit.
- The same executable, SHA-256
  2502ee37a6e7ced80a2bd230efd09126f67535826deb9c16e07a981b89e7338c,
  hit an earlier pool-full null dereference in validation run hang-17:
  main+0x806dc, size 0x38, through generic wrapper main+0x8069c. This new
  caller requires a separate lock/call-chain audit before any retry is enabled.
  It does not test the later copy or comparison path.
- The dedicated D32F/S8 image-copy path preserves both aspects and rejects
  unsupported flags, aliases, view reinterpretation, bounds, sample counts and
  unequal extents. Its 64 native cases pass at O0/O2; 76 HWAoI tests pass in the
  combined suite. Buffer/depth copies outside that scope remain explicit
  failures. A recorded copy alone does not prove completed GPU pixel results.
- hang-16 diagnoses an earlier inherited skip: a T2T copy between distinct
  1600x900 DEPTH32F_STENCIL8 images, one mip/layer/sample, full matching regions,
  zero copy flags and only a mip-level view selection. The previous backend
  skipped combined depth/stencil transfers. The diagnostic version fails
  explicitly instead: exit 70 in 88.359 seconds, no timeout, with validation
  disabled. Its executable SHA-256 is
  207674dd6300891da757648d95814a7c7157e6e4be8d326fbad174731d23fca6.
  This failure occurs before comparison-view mapping, so that run does not
  establish its actual swizzle or execute the fixed F16 shader.
- The user's compute-stage crash is reproduced from its retained native input.
  Scalar 2D TLD4.F16 now gathers into float scratch registers and packs each
  pair with the first component in the low half. The same audit corrected the
  existing TEX.F16 packing operand order. The independent native fixture passes
  1,856 cases at each of O0/O2, and a mutation restoring the old operand order
  fails. All 48 gathers in the captured shader lower, its complete shader
  translates, and the resulting SPIR-V passes vulkan1.2 validation. This is
  offline evidence; successful GPU execution of that stage remains unverified.
- hang-14 binding diagnostics identify the comparison inputs as actual game
  RGBA8 textures, not fallback descriptors: one 1x1 six-layer cube array and one
  1x1 one-layer array. A narrow conversion to D16 preserves each actual equal-
  component UNORM8 value exactly, retaining the sampler and Dref operation.
  GPU-written sources, unequal components and unproved view mappings fail
  explicitly. Immutable content versions keep earlier submissions valid.
- hang-15, executable
  e939f352e3b724043118147ff5300732a19568be84f914bddf6072cb37839bd9,
  reaches 1200 presents at 117.137 seconds, then rejects a comparison texture's
  component mapping before the former shader failure. No Vulkan validation
  errors were logged before that rejection; no successful live conversion is
  established. The supervisor records timeout/124 during exit. The combined
  74-test suite passes, including conversion layout, content-version and failure
  cases. Actual mapping values are the next diagnostic requirement.
- hang-13 captured the actual failing compute input as
  shader-failures/nvn2-native/5159ea53b0d4c432.ctl (224 bytes) and .bin (22,784
  bytes). Native gather lowering at +0x1c0 is now reproducible offline. With
  Vulkan validation active, later fragment draws also report depth-comparison
  sampling from RGBA8_UNORM views (fp_t_tcb_18 and fp_t_tcb_1A). These occur after
  the last 1200-present heartbeat; its zero counters do not describe the later
  errors. The later fixes and their current verification limits are recorded
  above.
- interactive-20261105-211239-808455: at the user's request, executable
  1911b5cc68dbd7fce6209b72f22ea291a7bc7b3ccdb61c1a9e32dddaa845bd06 opened a
  responsive window with live input; its 1920x1080 capture shows the Chinese
  autosave explanation. The user then reported a crash and requested continued
  adaptation. The log confirms an explicit required-compute-stage lowering
  failure at native offset +0x1c0, "native texture gather mode", origin
  5159ea53b0d4c432. No game process remains from that trial. Gameplay is unverified.
- hang-11 reached the same later shader failure after at least 1320 presents,
  beyond the former RegisterImage rejection. The last capture remains black
  loading. Native lowering failures now retain original .ctl/.bin inputs under
  the report's shader-failures/nvn2-native folder; the previous failed-request
  capture covered only the subsequent external translation stage.
- RGBA8 sRGB -> UNORM storage-view support is implemented with device capability
  and complete image-create parameter checks. Storage, sampled and attachment
  views restrict their usages and have distinct cache keys. Other format
  reinterpretations, target changes and multisampled reinterpretation fail
  explicitly. Synthetic native tests cover registration, capabilities, view
  creation and cache separation; actual GPU storage execution is not yet proved.
- hang-10 (validation enabled) hit another full-pool null allocation at
  main+0x1668b4 / 0x58. The audited pair and its sibling +0x1669e8 now use bounded
  original-allocator retry. All 90 O0/O2 cases pass, including unchanged parent
  counters and timeout failure. This retains the five-second limit; it cannot
  guarantee progress if every executor is waiting to allocate child jobs.
  hang-12 ended with a recorded supervisor timeout while work switched to the
  user's interactive trial; its ending is not a completed-loading pass.
- Follow-up to the user's interactive stall: the window was closed at 19:18:54
  local time; frame-summary timing implies about 39 seconds without a new frame
  before close. No fatal error was recorded in that particular run. hang-03
  also stopped around present 208. Later hang-07/08 evidence identifies a
  virtual-core starvation path; this does not explain every possible stall.
- hang-07's two external snapshots put the main thread in core_take after its
  NVN fence wait had returned. hang-08's optional ownership diagnostic shows
  the pinned main thread (priority 44, mask 1) waiting for core 0 while an equal
  priority worker (mask 0x3f) owns it and cores 1 through 5 are idle. The old
  scheduler only requested displacement for strictly better priority.
  The scheduler now permits an equal-priority owner to relocate at the existing
  safe point if another allowed core is idle. Capacity is rechecked under the
  scheduler lock; guest affinity, preemption-disable handling and asynchronous
  host-call guards remain in force. Independent native tests cover all 24,576
  mask/occupancy/origin combinations at O0/O2 plus real handoff, stale requests,
  priority ordering, disable counts and asynchronous host guards. Releasing a
  core with no eligible waiter now refreshes relocation requests; a new or
  renewed target wakes the pinned waiter so its asynchronous retry can start.
  Tests cover initially full cores, renewed same-target requests and release
  outside the owner's mask. The linked
  runtime's probe-kernel also passes (wait ordering, virtual cores, fibers and
  lifted ticks); the full HWAoI suite has 71 passing tests at this checkpoint.
- hang-09, executable ca4d658bc8b22df04073bbf19f28364ff9d5058318dd41dbdea0fc960f1e5a51:
  the new scheduler reaches 1200 presents at 79.515 seconds with no core-stall
  diagnostic or 8-second missing-image trigger. It then exits 70 explicitly on
  RegisterImage format reinterpretation 56 -> 37 (RGBA8 sRGB -> UNORM). This
  reaches the next unsupported backend operation; the last image is black
  loading, not gameplay. Reported shader/lowering/validation/unimplemented/
  skipped counters were all zero before this failure (Vulkan validation was
  disabled in these two runs). hang-09b repeats the same result at 1200 presents
  / 81.989 seconds, again without either stall trigger.
- hang-02/05 reproduce separate full-pool null dereferences at main+0x1eca5c
  (0x50-byte submission) and main+0x26454 (0x70-byte decompression parent).
  The exact audited pairs plus the eleven unrolled 0x50 siblings now use the
  bounded original-allocator retry. Variable buffer allocations with their own
  failure handling remain unchanged. All 76 O0/O2 native fixture cases pass.
- hang-06, executable 56cfc7f9595cbeb74f830f94535be5391645c0448299887ce6e28805d2f73ab6:
  scripted Continue input advanced past the autosave explanation through later
  resource loading and at least 1440 presents. At 117.469 seconds it exited 70
  on the explicit missing API nvnTexturePoolRegisterImage (caller main+0x71c854).
  This run did not stall or hit either previous null-pointer allocation crash;
  it does not prove the intermittent stall fixed. Its last captured image is
  a black loading screen, not gameplay.
- nvnTexturePoolRegisterImage now validates and snapshots descriptor, texture
  and optional view inputs, then uses the existing texture descriptor backend
  consumed by storage-image bindings. Unsupported format/target reinterpretation
  fails explicitly except for the tested sRGB/UNORM implementation above.
  Registration now has 66 O0/O2 native fixture runs, with 16 backend-view runs
  and per-run capability cases. This alone does not verify GPU execution.
- External per-thread capture is available through tools/hwaoi_threads.py;
  hang-04 and hang-06 snapshots succeeded. Snapshot pause durations and partial
  unwind/errors are retained, and threads resume before unwind/symbolization.
  Existing in-process chain sampling caused an immediate native crash in
  hang-01, so the runner's optional sampler uses instruction pointers only.
- Bounded play-experimental probes accept up to 180 seconds, isolate scripted
  input from live controls, retain the exact script bytes and hash, and request
  an owned-window close before Job Object termination. A timeout remains marked
  as such even after exit 0. Interactive progress logging now timestamps every
  120th present with error counters. No firmware or original NUP changes.
- validation-baseline-02 confirms the existing F:/VulkanValidationLayers layer
  and error-reporting messenger are active, with zero validation errors through
  the 30th rendered frame. This run times out during process exit, not playback:
  its external snapshot contains one remaining thread in plat_exit/ExitProcess,
  the validation DLL's teardown and a winpthread wait. The supervisor records
  timeout/exit 124; it is not a clean-exit pass. No exit bypass was introduced.

- render-28: 600 presents, exit 0 in 34.219 seconds, with zero reported shader,
  lowering, validation, unimplemented and skipped-draw failures. The capture
  shows difficulty, voice/text language and camera settings. Native inflate,
  resource-pool backpressure and pool diagnostics were enabled. This is an
  actual game UI, not proof of gameplay. render-27/29 timed out at 185/192
  presents with the same executable; the stall was unresolved at that checkpoint.
- The SDK save-capacity query now uses the selected host save volume and the
  CommitSaveData alias uses the existing transactional host commit. storage-01
  passes 103 checks, including crash recovery; no console journal quota is
  invented. See local-save-capacity.md and resource-backpressure.md.
- render-29 records an explicit 1920x1080 window crop of the 3840x2160 swap
  texture. Captures now pass that crop to readback, matching presentation.
  Older captures retain the full texture and black unused right/bottom areas.
- The explicit `python tools/hwaoi.py launch` command validates the ExeFS and
  opens `--play-experimental` with live input, separate normal local saves and
  timestamped logs under work/reports/interactive-*. Ordinary default/--boot
  remains guarded; the interactive command does not auto-exit after 600 frames.
  Its process and window must be observed before claiming a successful boot.
- After the interactive/crop changes: 67 tests/hwaoi tests pass, the Windows
  executable links successfully, and the repository content check passes.
- interactive-20261105-191632-107593: executable SHA-256
  6fdcd32805bc0d1600d89a650cad8fc6b7c79507c22e8ee972def8e25b216f2f
  opened a responsive HWAoI Recomp window and detected an Xbox One Controller.
  At present 840 the actual 1920x1080 cropped capture shows the Chinese autosave
  explanation and Continue/A prompt, with no unused texture margins. The game
  was left running for the user's requested trial; gameplay remains unverified.

Earlier rendering and resource investigations:

- render-13 onward: independent NVN program stage bindings are merged correctly;
  a later compute-stage unbind no longer discards the bound vertex/fragment
  stages. Native binding fixtures pass. Actual Vulkan graphics pipelines execute.
- render-17/18: a 256x256 offscreen RG16F lookup image contains 55,492 distinct
  texels; the 1600x900 postprocess target contains RGB byte 3 and opaque alpha.
  These are intermediate rendering evidence, not a game scene or playability.
  The final presented image remains entirely black. Shader lowering still
  rejects two loaded programs; they are not proven to cause this black frame.
- render-20: recording-order traces confirm that the guest explicitly issues
  ClearColor after the postprocess draw and after the final blit. Image handles
  in the GPU dump confirm those clears target the previously drawn images.
  The cause of the guest's clear requests remains under investigation; no
  clear-skipping workaround has been introduced.
- render-15/crash-diag-01/pool-diag-02: longer runs fail in a 16,000-slot
  resource-job allocator. The optional hash-gated observer sees the allocation
  bitmap fully occupied while decompression jobs continue completing. Near the
  failure, successful allocations minus frees agrees with occupancy (within
  concurrent calls). This supports a real work backlog rather than a proven
  missing-free bug. See resource-pool-diagnostic.md. No firmware was used.

Earlier checkpoints (historical results):

- boot-20: exit 0, first NVN bootstrap nvnDeviceGetProcAddress reached after real
  module constructors, GameChat initialization, GameShare local display creation,
  controller setup and local save mount.
- render-01: Vulkan initializes successfully on NVIDIA GeForce RTX 4060;
  nvnQueueWaitSync was then implemented with real backend frontier waiting.
- render-04: actual CommandBufferFenceSync request has condition=0, flags=0,
  caller main+0x71f67c. Recorded fences now publish a genuine backend completion
  frontier on submit. Unsupported flags and same-submission fence/wait cycles
  are rejected, not silently ignored.
- render-05: first 3840x2160 present captured, but visually black with shader
  lowering failures. Its historical exit 0 did not count lowering failures;
  the probe now counts those and skipped draws as failures.
- render-06: continued beyond first present; blocked in aud:u command 1.
- render-07: AudioOut opens a real SDL output (48000 Hz, PCM16, 6 channels).
  Next failure is missing hwopus service, requested by the real SDK multistream
  decoder wrapper. No final 120th-present capture and no gameplay verification.
- render-08: hwopus work-buffer query is served; AudioOut starts. The original
  IsLanguageSelectionLimited wrapper dereferences an uncreated oe service.
  Added an explicit unrestricted local-host language policy alongside the
  existing host-owned oe initialization, not a console-region assertion.
- render-09: four presents, then singular GetNpadState enters an uninitialized
  SDK HID shared-memory reader. Added all five singular overloads using the same
  host state as the existing plural APIs. The captured first present is black.
- render-10: stricter recording-mode validation reveals mode 2 after mode 0;
  the historical logger only reported the first value. Copy-engine semantics
  are being adapted explicitly; accepting mode 0 alone was insufficient.
- A managed self-contained .NET shader translator was built and integrated;
  identity c4d7ec0932d80ffed4f01836fa403856766ecacb0f0e86ed83c43686a3dd74b8.
  Synthetic translation/protocol tests pass. Real shader inputs expose Ampere
  lowering gaps; 60 control/code pairs are retained in render-03/nvn2-dump.
- Selective Ampere lowering additions translate 58/60 of that initial corpus;
  the later render-06 corpus exposes more instructions. The two remaining
  initial shaders need collision-safe native/uniform register separation.
  A separately identified managed helper with gated private IR banks is built
  under xlate-managed-irbanks; synthetic tests pass, GPU integration pending.
- Private-bank-capable lowering now handles the original 60/60 corpus. The next
  pass lowers 353/353 render-06 stages and emits 352 VS/FS/CS SPIR-V modules; one
  GS was not covered by the offline exporter. Real rendering still unverified.
  Request serialization tests prove HIR1 opt-in changes shader identity and is
  preserved through stored-input reconstruction.

## Changes since boot-01

Original offline chat:u service with owned event/scoped interface; CMIF domains
with independent copied-session framing; genuine SDK GSV UserSession bootstrap
through a local raw transport; GSV chat subscriptions; VI2 local object registry;
screen-sharing audio policy; empty-AOC count; disabled Horizon FS access-log query
(host diagnostics remain); stored handheld activation/hold policies.

No networking, microphone, camera or stream capture backend was enabled. Unknown
requests remain explicit failures. The raw ROM was not modified. Code generated
from the game, cache, binaries and captures stay in ignored work/.

## Probe tools

`python tools/hwaoi_probe.py probe-boot --out work/reports/<fresh> --timeout 45`
stops at first NVN API using null rendering.
`probe-render` uses Vulkan, disables live input/rumble, requests a small window
and exits after first present, saving frame.png if its format is supported.
The supervised process tree is bounded by a Windows Job Object and time limit.
Ordinary unbounded game launch remains guarded.

`probe-render --frames N` (1..600) now records progress for every present and
small intermediate snapshots of the first ten presents and every 30th present. The
requested final frame is full resolution. This preserves evidence if another
initialization stage fails before N. Exit 0 alone does not establish a meaningful
image: inspect the capture, counters and logs.

## AudioOut scope

Original service adapter in runtime/audout.c: default-device enumeration/open,
PCM16 mono/stereo/5.1, state/start/stop, validated snapshotted buffers, ordered
release IDs, readable event copies, sample count, stopped flush and volume.
SDL input-byte consumption paces releases; it does not measure DAC latency.
Explicit HWAOI_AUDIO=none uses a fractional real-time sample clock. Device-open
failures otherwise remain visible. Stop retains unconsumed buffers and flush is
only implemented while stopped; exact hardware stop-tail timing is not proven.

Protocol layouts follow [libnx audout](https://github.com/switchbrew/libnx/blob/master/nx/source/services/audout.c)
and its [buffer declarations](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/services/audout.h),
with the target SDK OpenAudioOut wrapper at sdk+0x1cd0a0 and actual render-07
initialization corroborating use. This is a local host adapter, not firmware.
Nine synthetic native test groups cover buffer/clock/event behavior; all 47
tests/hwaoi tests passed at this checkpoint. Mocks are not device-playback proof.

hwopus now uses actual libopus multistream decoding with per-session state,
reset, channel mapping, packet/final-range validation and transactional failure
rollback. The real game has so far exercised the work-size query only; synthetic
tests exercise decode. Unknown modes remain failures.

## Focused service regression

`probe-input` tests host oe/hid ABI through rt_dispatch without graphics boot.
input-02 passed 130 checks, including singular/plural state equivalence and
callee-saved registers. Input is disabled; vibration test amplitudes are zero.
The inherited broad probe-services is **not** HWAoI-ready: services-01 reported
import-inventory mismatches and aborted when it booted GSV without constructors.
Do not describe that broader probe as passed.
