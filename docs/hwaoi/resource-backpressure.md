# Bounded resource allocation retry

`HWAOI_POOL_BACKPRESSURE=1` enables an experimental host timing adaptation.
It is off by default and requires the pinned HWAoI main SHA-256. It applies
only to the indirect allocator at main+0x1adce64 and exact audited request pairs:
main+0x289b0/0x50 (decompression child), main+0x26454/0x70 (decompression
parent), and eleven 0x50-byte submission sites at main+0x1ec13c, 0x1ec29c,
0x1ec454, 0x1ec5c8, 0x1ec75c, 0x1ec8dc, 0x1eca5c, 0x1ecbdc, 0x1ecd7c,
0x1ecf44 and 0x1ed0f8, plus main+0x1668b4/0x58 and main+0x1669e8/0x58
(one or two resource children of the running executor job), and the observed
main+0x806dc/0x38 wrapper call with the saved caller chain described below. Other calls retain the
original allocator result. No generated game C, ROM, pool capacity or queue
contents are patched.

The evidence is `work/reports/pool-jobs-01`: three concurrent resource parent
jobs (main+0x27eac, priority 3) and three executing decompression children
(main+0x28708, priority 4), while the 16,000-slot pool fills. Two parents are
already assisting their children; the third is submitting a batch of 8,531.
Allocation/free counts and the bitmap agree with actual occupancy. This does
not prove a hardware scheduler bug or instruction-lifter bug. It motivates
an explicit timing adaptation for this local port.

Disassembly of main+0x1adce64 shows that its active-operation counter at
pool+0x24 is decremented before returning, including when allocation fails.
The adapter calls that original function first. Only after a null result does
it validate committed heap ranges, the 128-byte slot layout, backing storage,
pool state and occupancy. It then gives up the virtual core, waits 250 us,
reacquires the core and retries the original allocator with the entire incoming
CPU context restored. The successful original result and register state are
returned unchanged. The retry does not invent a pointer or mark any task done.

No allocatable slot within five seconds (or 20,000 retries) causes a visible
fatal error. It cannot solve a case where every worker is a blocked producer;
that remains a bounded failure. Reacquiring the core uses the normal scheduler.
This is not asserted to reproduce console allocation timing or semantics.

The additional sites follow observed full-pool null-pointer crashes in hang-02
(main+0x1eca5c, then a write to null+0x18) and hang-05 (main+0x26454, then a
write to null+0x24). Static disassembly verifies that the new task is initialized
and published only after allocation, prior sibling jobs are already published,
and no queue mutex is held by those submission sequences. The variable-size
buffer allocator at main+0x261a8 has its own null fallback and is left alone.
The parent job's in-flight count is retained across retries for transfer into
the original job. Full transitive caller lock/progress safety is not proven;
the five-second failure limit remains essential. Local evidence is retained in
work/analysis/pool-submit-1ec0c0-audit.md.

The hang-10 0x58 pair is a separate audited submitter inside main+0x162a60.
Its two free-list locks (owner offsets 0x5d0/0x5e8) are released before allocation.
The parent task's unfinished count is already set to children+1; retry must
preserve it, detached payloads and all incoming registers. Each child is
initialized and published only after its allocation, and the first is fully
published before the second allocation. Once publication finishes, the original
code calls main+0x7f500 to help execute queued work while awaiting the children.
Sleep/retry delays that helping loop: if every executor is a producer blocked
on a child allocation, this adaptation cannot free a slot and must time out.
The captured run had two independent inflate children and continued frees;
this supports a bounded experiment, not a proof of deadlock freedom or all
transitive callback locks. Other 0x58 allocations, including main+0x1672ec,
remain unchanged. Exact evidence: work/analysis/pool-submit-1668b4-audit.md.

The hang-17 allocation is through generic wrapper main+0x8069c. Its return LR
0x806dc and size0x38 do not identify a unique producer. Retry is enabled only
with the original wrapper's SP/FP/argument layout and four saved return PCs:
main+0x14c3e24, +0x14dfdac, +0x1af5e64, +0x6c3980. Fixed frame offsets come
from the hash-locked prologues; fault-safe reads under host tracking reject
unreadable or mismatched frames. Other callers receive the original allocation
result. The immediate publisher acquires no lock before allocation, and its
parent-count exchange/publication occurs afterward. Visible enclosing mutex
scopes release before this call, but arbitrary indirect callbacks and global
progress are not proved. The unchanged five-second bound remains essential.
Audit: work/analysis/pool-submit-806dc-audit.md.

Validation:

- `tests/hwaoi/test_pool_backpressure.py`: 144 native subprocess cases across
  O0/O2, covering default-off, hash/extent/function checks, call-site/size bypass,
  full CPU restoration across failed calls, preserved successful CPU result,
  core release/reacquisition and timeout/layout/state failure paths. The new
  pair tests also preserve the parent's unfinished count, bypass neighboring
  and unrelated 0x58 calls, and retain timeout/closing-pool failure behavior.
  The wrapper cases also check every saved caller/frame link, unreadable frames,
  invalid SP/FP/arguments, preserved stack bytes and parent count, and bypass
  for unknown callers of the same generic wrapper. Extent tests require the
  complete instruction at the farthest saved return, including boundary cases.
- `render-25`, executable SHA-256
  `484e85a0d24b975bb986e431dad5f004df3f337b7a47def5b0672c7132da2c75`:
  logged allocations recovering after approximately 0.25 ms; decompression
  advanced beyond 45,056 jobs, past the previous approximately 26,000-job
  failure. The run later failed in the previously unadapted GetSaveDataSize
  SDK entry, after 206 presents. No meaningful game image was established.
- The optional pool observer wraps the retry through rt_dispatch_lookup;
  observer allocation counters describe the aggregate adapter call, not its
  internal failed attempts. Retry telemetry is logged separately.

Use with the existing bounded render probe; enabling this option does not
remove the ordinary boot guard. Native decompression remains a separate,
independently tested opt-in.
