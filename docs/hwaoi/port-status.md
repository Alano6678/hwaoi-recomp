# HWAoI port status — 2026-11-06

Full native compilation and linking are complete for the current exact input
profile. The runtime plays the two opening videos and renders the first outdoor
3D scene around 60 FPS in measured intervals. Movement/camera input has changed
the character position and view. **Combat and overall playability remain under
investigation.** Later scenes, A/V synchronization, visual accuracy and save
round trips still need verification.

This is an adaptation of DKB Recomp at
`42fcd9d78b4b44c2831b68ba21e5807d74a96f45`. Upstream Bananza/TotK results do not
establish HWAoI compatibility. Detailed historical failures and fixes remain in
[current-work.md](current-work.md); that ledger does not mean every earlier
blocker is still present.

## Verified milestones

| Stage | Evidence | Boundary |
| --- | --- | --- |
| Input/profile | Title ID, NPDM, four module SHA-256 values and NSO segment hashes checked | Only the pinned input profile; NCM version 196608 is not a display version |
| Generation | 142,393 candidate functions across main, subsdk0 and sdk; 362 chunks | Explicit traps and reachable-code coverage remain unresolved |
| Native build | Complete Windows x64 executable compiled and linked | Generated C and binaries stay in ignored local directories |
| Loader/resources | Selftest, RomFS and loader probes exit 0; zero strong unresolved imports | Startup does not read every resource or prove every ABI path |
| Initialization/UI | All three DT_INIT callbacks, nnMain, Chinese settings and autosave UI reached | Earlier chat:u startup stop was superseded by later bring-up |
| Rendering | hang-20 first shows a 3D scene; hang-21 repeats it after the capability fix | Lighting, shadows and output correctness need further checking |
| Movies | Two opening clips reach EOS, 3,561 and 7,068 frames, zero decoder errors | Later clips and A/V synchronization remain unverified |
| Control/performance | scene-36: movement/camera observed; final interval 59.9 FPS | Attacks were not reached before that run's deadline |
| Regression | Latest HWAoI suite: 93 tests pass, including opt-in GPU probes | Not a whole-game playability test |

## Latest completed progression check

`scene-36-story-walk` runs for 600 seconds, completes both opening clips and
enters the outdoor scene. Scripted left/right-stick input changes the character
position and camera. The final interval is 3,470 frames in 57.9 seconds, 59.9 FPS.
The supervisor requests close and receives exit 0, with no fatal errors;
`timed_out=true` remains recorded. Vulkan validation is off for this measurement.
Original colour/presentation and indirect-draw GPU probes separately pass with
validation active. The test is not a combat or save-round-trip pass.

Runtime SHA-256:
`e51b76614e947e156b3cff56701367b0ddd200e77306c84e45345acf9b82c703`.
Translator identity remains
`22fe81c97e7c074efa43842cb71ad2b9c79425e9e9f3680183745f4dece44cba`.

The main measured performance fix keeps disjoint memory flushes separate, rather
than uploading a huge min/max envelope between distant ranges. The movie stage
previously copied about 2.8 GiB per frame and presented around 4 FPS; later runs
copy about 9 MiB per present and present near 60 FPS. This does not prove a
game-speed-correct 90/120-FPS patch or a custom internal-resolution profile.

## Earlier validation-enabled scene check

Local report: `work/reports/hang-21/result.json`; scene capture:
`work/reports/hang-21/presents/f01230.png`. These files are deliberately not part
of the source repository. The test used Vulkan validation on an RTX 4060.

- Runtime SHA-256: `0307d0dd07ccdcf9fd208286e9b50afde2243c05c6111be775aa55c823235c6e`.
- Translator source identity: `22fe81c97e7c074efa43842cb71ad2b9c79425e9e9f3680183745f4dece44cba`.
- Complete-log fatal errors, lowering failures and Vulkan validation errors: all **0**.
- At the 180-second deadline the supervisor requested window close; final result
  was timeout/exit **124** after **185.703 seconds**, not a clean exit.
- The final measured interval was **39 frames in 54.2 seconds**. This first run
  with a new translator/cache identity is not a steady-state benchmark.
- Scripted Continue input advanced loading. Keyboard/controller availability and
  Xbox detection do not establish responsive in-scene control or combat.

The subsequent interactive launch uses the same runtime and has no probe timeout.
Opening a responsive window is not an additional gameplay milestone.

## Recent integration work

- Scoped Ampere-to-Maxwell F16 gather/texel-fetch lowering and corrected half
  packing. Independent instruction/semantic fixtures pass 2,617 cases at each
  O0/O2, with mutation controls that detect the former bugs.
- D32F/S8 texture-to-texture copying preserves both aspects and rejects
  unsupported aliases, view formats, sample counts and bounds.
- CPU-authoritative, equal-channel 1x1 RGBA8 comparison textures have a narrow
  D16 conversion, including the observed BGR mapping under explicit sampler
  constraints. This does not prove arbitrary color-comparison semantics.
- The translator declares StorageImageReadWithoutFormat for untyped reads; the
  runtime checks the corresponding Vulkan feature. Synthetic cases and the
  retained failing shader replay pass validation after the fix.
- Resource-pool backpressure is gated by exact module identity, caller/size and,
  for the generic wrapper, verified frame shape. It retries the original
  allocator with a bounded deadline. The native fixture covers 144 cases.
- Virtual-core scheduling handles an observed equal-priority starvation path;
  shader failure capture and probe dependency hashes improve diagnosis.

## Remaining work

1. Measure and reduce long CPU replay/resource-loading intervals; repeat with
   warmed caches before drawing steady-state performance conclusions.
2. Check lighting, shadows, comparison sampling and scene output correctness.
3. Verify responsive movement, camera, combat, later scenes and save round trips.
4. Fix shutdown/validation-layer teardown and test sustained runs with clean exit.
5. Continue instruction, system-service, audio/movie and synchronization coverage
   as execution reaches new paths. Firmware investigation remains deferred.

## Input and resource model

The validated base/update NUP overlay exposes 2,153 resources. Compressed members
are expanded into an 8,530,745,320-byte local cache; raw members remain in the
immutable original NUP. The runtime authenticates the index and verifies each
resource hash before its first open. Startup probes do not verify that all
resources have been exercised by the game.

HWAoI has its own title identity, INI/environment prefix, save path and window
name. Bananza-specific GPU skips/reset/readback patches are disabled by the
target gate. The inherited rt_game_version discriminator is not the HWAoI NCM
version. Unsupported paths fail visibly.

## Earlier generator evidence

| Module | Candidate functions | Generated C bytes | Explicit traps | Unsupported exceptions |
| --- | ---: | ---: | ---: | ---: |
| main | 91,768 | 514,046,918 | 63,406 | 0 |
| subsdk0 | 2,716 | 12,521,103 | 2,286 | 0 |
| sdk | 47,909 | 138,392,820 | 39,862 | 0 |

The 105,554 trap nodes include unclassified exceptions/padding; zero unsupported
exceptions does not mean complete semantics. An inherited differential harness
sampled 3,000 encodings and compared 8,997 randomized states with Unicorn, with
zero differences and three skipped states. Its exclusions include system
instructions, exceptions and atomics. Earlier 44-test prototype results are
historical and do not replace the current runtime regression suite.
