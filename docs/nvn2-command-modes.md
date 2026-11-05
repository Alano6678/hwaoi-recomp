# NVN 2 compute overlap

`runtime/nvn_api_nvn2.c`, `nvnCommandBufferSetComputeOverlapMode` preserves the replay's existing serialized
execution. This call controls hardware compute/graphics scheduling; it does not change shader bindings, resource
contents, or which dispatches execute. Allowing concurrent execution does not require a host to overlap the work.

The Bananza base SDK establishes the contract without relying on the function's name:

- Proc-table entry 185, SDK offset `0x5e9780`, takes the command buffer in `x0` and a 32-bit mode in `w1`, returns
  void, and appends an eight-byte queue control record with opcode `0x27` and the mode.
- The queue control reader at SDK offset `0x5c5784` sends that record to the scheduling helper at `0x5e96c0`.
  When the queue supports this control, changing the mode writes compute method byte offset `0x1e8` inside an
  atomic sequence. Mode zero sets bit zero; a nonzero mode clears it.
- NVIDIA's public Ampere compute definition names that method `SET_SM_SCG_CONTROL` and bit zero
  `COMPUTE_IN_GRAPHICS`. The call selects where compute is scheduled; the SDK reader changes no shader or resource
  state. See [NVIDIA, Ampere compute class](https://github.com/NVIDIA/open-gpu-doc/blob/master/classes/compute/clc7c0.h).

The replay provides a conservative implementation of either choice. `runtime/vk_exec_draw.c`, `op_dispatch` and
`op_dispatch_ind`, call `vk_exec_barrier` before compute work. `runtime/vk_exec.c`, `vk_exec_pass_begin`, calls
`barrier_to` before graphics work. Those full barriers order graphics and compute in both directions on the single
host queue. No command-stream state needs to be added for this scheduling preference.

A 60-second tutorial run observed mode `1`, called from guest LR `0x70002d27f8`. That selects the SDK's separate
compute scheduling path; the host retains its existing barriers and serialized execution. The implementation gives
up potential overlap performance. The observation covers this call in the tutorial, with full build and live
validation of the explicit handler still required; it does not establish behavior for every game scene or SDK.

This evidence applies only to compute overlap. `SetGeometryProcessingMode` changes mesh-pipeline state, and
nonzero `SetRecordingMode` values select engine-specific synchronization paths in the SDK. They are not justified
by the compute-overlap evidence. The HWAoI default recording mode has its own evidence and boundary below.

## HWAoI recording modes 0 and restricted copy mode 2

The host accepts mode 0 (ordinary) and mode 2 (a restricted copy-engine capability) on an initialized command buffer.
It retains the mode trace, does not increment the unimplemented counter, and emits no extra immediate barrier.
Modes 1/3/4/unknown, null buffers, uninitialized buffers and finalized buffers fail explicitly. A host-owned 16-bit
mode field shares the old 32-bit recording-boolean storage, preserving the 160-byte `cmd_t` and its existing pointer
offsets. The host layout is not the SDK layout: the setter must not write the SDK's offset `0x18` into it.

### Target-bound static evidence

These observations are from the supplied HWAoI NCM-196608 `sdk`, SHA-256
`4617761033cab45951a19fa7797fa79ad691c46f324048f832484877d3df034c`, inspected read-only using the NSO loader and
Capstone. Addresses below are module-relative offsets in the decompressed image, **not** compressed-file offsets.
No firmware, original SDK execution, or hardware runtime observation was used.

- `nvnBootstrapLoader` at `0x633060` resolves through the bootstrap table to `0x5d5680`, whose default dispatch
  target is the getter `0x5dab00`. The getter strips the `nvn` prefix, searches the string pool at `0xb9a62a`
  using the u32 offset table at `0xb9f98c`, and selects the corresponding procedure pointer.
- `CommandBufferSetRecordingMode` is index 219, name at `0xb9c132`. The default procedure table at `0xe056e0`
  has slot `0xe05db8` pointing to `0x5c4930` (including its relative relocation). That function only stores the
  32-bit argument at command-buffer offset `0x18` and returns; it emits no command and performs no synchronization.
- `CommandBufferInitialize`, index 145 at `0x5c4500`, explicitly writes zero to that same field at `0x5c4538`.
  Thus zero is the initialized default, not an enum name guessed from the API spelling.
- `FenceSync` at `0x5c5f20` emits control opcode 5. Its record's byte 2 is true only for modes 1/4, and byte 3
  only for mode 2 (`0x5c5f6c` through `0x5c5f94`). Both engine-selection flags are false for mode 0.
- `WaitSync` at `0x5c61a0` selects control opcode `0x5f` for modes 1/4 and ordinary opcode 6 for mode 0
  (`0x5c61c8` through `0x5c61e8`). `EndRecording` at `0x5c4a00` takes a special command-handle path only for
  modes 3/4 (`0x5c4ba8` through `0x5c4bd4`); mode 0 bypasses it.
- `Barrier` at `0x61b970` dispatches modes 1/4 to `0x60ebb0`, mode 2 to `0x61bd6c`, and mode 0 to its ordinary
  path at `0x61b9d4`. `CopyBufferToBuffer` at `0x5d3720` similarly records special control opcode `0x5a` for
  modes 1/4 and takes its ordinary GPU-copy path at `0x5d378c` for mode 0.

This establishes a narrow implementation decision: selecting the already-default mode does not add work at the
setter call. Actual ordering remains the responsibility of the subsequent commands. The host records `OP_BARRIER`
and replays it through `vk_exec_barrier`, with all-command write-to-read/write memory barriers. Its FenceSync and
WaitSync paths use a conservative whole-submission completion frontier. Mode 0 therefore uses those existing
ordinary paths rather than adding an invented standalone barrier at the mode call.

### Why mode 2 is a copy-engine mode, not a compute hint

The real `render-10` attempt stopped on mode 2 after the earlier permissive handler had only logged the first
observed value (0). The following additional static data flow establishes a narrower implementation than merely
allowing that value:

- In HWAoI `main` (SHA-256 `4c33cfd4c818d9ed62822af792782b4ea443417ed24acea6f1cee739cde604c3`), the wrapper
  at `0x4800` initializes an NVN command buffer and calls the mode setter at `0x48b0`. Configuration u16 at offset
  2 selects mode 0 when bits 0/3 are set, mode 1 for bit 1, and mode 2 when the field equals 4
  (`0x4870` through `0x489c`). These are observed numeric choices, not recovered enum names.
- The SDK FenceSync record's byte 3 (`mode == 2`) becomes argument `w5` in the queue reader at `0x5e98c4`.
  `0x5ec3c0` carries it to `0x5ec270`; it selects the helper `0x61f810` instead of the ordinary graphics helper.
  That helper emits words `0x20038090` and `0x200180c0`: subchannel 4 methods at byte offsets `0x240/0x244/0x248`
  and `0x300`. The addresses, payload and launch value 8 match the copy-engine semaphore registers and one-word
  release operation documented in [NVIDIA's Ampere DMA copy class](https://github.com/NVIDIA/open-gpu-doc/blob/master/classes/dma-copy/clc7b5.h).
- Mode 2's Barrier branch at `0x61b9a0` jumps directly to the return epilogue `0x61bd6c`. The host's ordinary full
  barrier is therefore a conservative extra ordering point, not an attempt to reproduce an absent SDK barrier.
  Mode 2 retains ordinary WaitSync opcode 6 and does not enter the mode-3/4 special EndRecording path.
- Copy-enable is not a harmless scheduling hint. `SetCopyEnable` at `0x5d4fb0` writes the copy-engine enable method;
  modes 0/3 also write another engine's enable. Its conditional form at `0x5d5040` writes an address and comparison
  mode. The host does not implement those commands and refuses them when mode 2 is active.

The implementation records and replays buffer/texture copies and copy stride using the existing typed host
operations. Barrier, WaitSync and FenceSync use the existing full-queue ordering, a stronger completion scope than
copy-engine-only completion. The mode does not suppress or invent a copy, change its arguments, or make a fence
complete earlier. Switching back to 0 changes the capability for later calls only; already-recorded copy operations
keep their same host execution and ordering.

`rec()` rejects other recorded operations while mode 2 is active. CallCommands/CopyCommands validate every inlined
operation too, preventing an ordinary graphics recording from bypassing that boundary. Mode-2 WaitEvent, filtered
Downsample, the inherited shader/tessellation no-ops in this recorder, and unimplemented NVN2 calls (including
conditional copy enable and geometry mode) fail explicitly. Ordinary diagnostic labels and other inherited hints
are not new copy-engine implementations. Full general mode-2 support is not claimed.

### Limits and checks

This does **not** establish complete synchronization equivalence or implement the other modes. Not every SDK field
consumer or optional debugger procedure-table override was analyzed. Mode 3 shares some table entries with mode 0
but has different EndRecording behavior, so it remains rejected. The existing `WaitEvent` no-op and unsupported
same-submission FenceSync-then-WaitSync split remain separate limitations; mode 2 refuses WaitEvent explicitly.

`tests/nvn_mode_trace_test.py` compiles the actual wrapper/recorder at O0 and O2 with synthetic CPU data. It checks
that selecting the existing mode 0 leaves recorder storage, CPU registers, command contents and the unimplemented
count unchanged while retaining trace events. The mode-2 fixture checks the exact typed copy/barrier/wait/fence
sequence, copied arguments, unchanged data while recording, mode 2-to-0-to-2 transitions, and safe inlining.
Separate child processes reject modes 1/3/4/UINT32_MAX, invalid buffer lifetimes, graphics/compute operations,
graphics recording inlining, WaitEvent, conditional copy enable and geometry mode while in copy mode. Geometry
tracing in ordinary mode and arbitrary scalar trace-helper inputs remain separate from the stricter public API.
These tests do not execute game code or prove a rendered frame or real GPU copy completion.
