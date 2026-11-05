# Resource job pool investigation

The render-15 crash is an unchecked null allocation at main+0x289bc, after
main+0x2894c calls the virtual allocator at main+0x1adce64 for 0x50 bytes.
These locations apply only to main SHA-256
`4c33cfd4c818d9ed62822af792782b4ea443417ed24acea6f1cee739cde604c3`.
The original module is unchanged. The observations below come from local NSO
disassembly and retained runtime logs, not from an REA native session.

The allocator uses 0x80-byte slots in a 0x1f4000-byte block (16000 slots).
Its helper at main+0x1add040 increments the count before selecting an unused
bitmap bit. At full capacity, it can clear a flag and return null. The captured
x9=0 and x10=0x80000000 match that path. This is evidence of exhaustion, but
does not establish why the worker pool has not reclaimed enough slots.

The corresponding free interface is main+0x1adcfa8. It forwards to
main+0x1add1c0, which clears the bitmap bit, marks a retry flag, then decrements
the count. If that reaches zero it retires the slot size after waiting for
in-flight allocators.

The job vtable installed by main+0x2894c points at main+0x28708. That function
calls main+0x28760 to decompress exactly 16 KiB and asserts the output size.
The decompressor constructs a 0x70-byte z_stream, initializes with windowBits
15, calls inflate with flush 4, expects result 1, and finalizes the stream.
The target version string identifies a Nintendo variant of zlib 1.3.1. There
is an optional native experiment described below. Decompression throughput
relative to job production was an initial hypothesis, alongside scheduling and
lifted instruction behavior; acceleration alone did not resolve exhaustion.

`HWAOI_POOL_DIAG=1` enables an optional dispatcher observer, gated by the
compiled main hash. It calls the original allocator, free and job functions
without changing guest state or return values. Per-pool logs report observed
successful/null allocations, completed frees, and started/completed inflate
jobs. A null allocation also samples the bitmap. Guest counters and bitmap
are concurrent observations, not one consistent snapshot. Direct calls bypass
the observer, and logging overhead can affect timing. No direct calls to the
allocation/free interfaces were found in the generated main module at this
checkpoint.

Use a bounded render probe with this switch and the already documented local
GSV policy. The native fixture checks disabled behavior, hash/module rejection,
original-call result/register preservation, unchanged guest pool memory and
counter classification at O0 and O2. It is not a test of game scheduling or
pool allocation semantics.

## Native decompression experiment

`HWAOI_NATIVE_INFLATE=verify` intercepts only the resource job vfunc at
main+0x28708. It decodes to private scratch with a native zlib, then runs the
original lifted function and compares all 16384 output bytes and the original
input snapshot. A mismatch is fatal. `HWAOI_NATIVE_INFLATE=1` enables the
experimental fast path; unset or `0` retains the original implementation.
Unsupported descriptors, checksum failures, overlaps and decoding failures
fall through to the original function. The game still owns job allocation,
completion, frees and scheduling.

The packet's custom 8-byte checksum is checked before zlib. Its key table is
read at runtime from the validated module, not copied into repository source.
Native zlib never sees a guest z_stream. The original wrapper's persistent
successful output is the 16 KiB destination; main+0x28708 returns zero without
writing the job descriptor. Its intermediate stream and allocator arena are
on the guest stack. The adapter preserves callee-saved CPU state and uses a
normal write to the destination, retaining host write-watch behavior.

`HWAOI_ZLIB_DLL` must explicitly name an existing absolute library path.
Development uses `F:/GCC/ucrt64/bin/zlib1.dll` (reports zlib 1.3.2), already
installed under the user's tool directory. No library is downloaded, copied or
redistributed by this experiment. zlib is by Jean-loup Gailly and Mark Adler;
its original license notice remains in the installed library's headers,
including `F:/GCC/ucrt64/include/zlib.h`. The adapter contains original calling
and packet-validation code, not copied zlib implementation source.

Host-only validation, native calls and diagnostics have the same host-depth
tracking convention as hostfn_tracked. This tracking is released before
executing the original guest function. `HWAOI_NATIVE_INFLATE_TRACE=1` adds
bounded per-job stage observations and initial memory-query timings.

Synthetic tests cover compressed/stored blocks, trailing input, corrupted
checksums, length/address/permission/overlap fallback, mode/profile/library
failures, CPU-state preservation and composition with the pool observer at
O0 and O2. These fixtures establish adapter plumbing, not equivalence with the
Nintendo zlib variant. Real `verify` probes are required before interpreting
any accelerated run. The first verify probes matched real packets but were
too slow to finish loading. The inflate-timing-01 probe isolated Windows
VirtualQuery: input queries cost 48.9–56.8 ms and several output queries cost
54.76 ms, while the corresponding logical memory-map queries cost at most
0.001 ms. This overhead was introduced by validation, not decompression.

The adapter now accepts only job/input/output ranges wholly inside the runtime's
guest heap reservation, and checks every covered committed/permission interval
with mem_query. Moved-stack inaccessible ranges still reject. Non-heap ranges
fall through to the original guest code. This restriction matters because
mem_query's generic address-gap fallback alone is not proof of mapped memory.
The key table is bounded by the known module extent and checked for host
readability once before making an immutable host snapshot. Tests additionally
cover non-heap and uncommitted input, inaccessible keys, malformed query results,
multi-interval ranges and module extent overflow.

After the validation change, inflate-verify-03 reached 236 presents in 13.45 s.
It explicitly logged 24576 successful native/lifted comparisons; the pool
observer had recorded 26087 completed inflate jobs with no fallback or mismatch
before the original allocation exhaustion recurred. This supports equivalence
for those observed packets. It does not establish all Nintendo zlib modes or
arbitrary packets. The baseline exhaustion remains expected in verify mode,
which still executes every original job.

The first accelerated run, render-22, also exhausted the same pool: 163
presents in 15.688 s, 26394 inflate jobs completed before the null allocation,
no native fallback. Native decompression alone therefore does **not** fix the
resource backlog. ProgramSetShaders calls occur during the growth toward full
capacity; host shader work and worker scheduling remain under investigation.

Static job-consumer evidence: main+0x7fa00 searches the highest available
priority, pops the tagged relative-pointer list, invokes vtable+0x10 at
main+0x7fc88, and frees through the allocator's vtable+0x60 at main+0x7fcfc.
Decompression children receive parent priority plus one (queue index clamped
to 128). The chunk producer still queues its whole batch before assisting
other jobs. No queue-order or missing-free correction has been applied.

The pool observer now also recognizes the executor return site main+0x7fc8c
for any dispatched target. It verifies a tracked-heap job descriptor and a
known-module vtable whose execution slot matches that target, so tail helpers
that retain LR are not interpreted as new jobs. Per-type counters record
starts, completions and inclusive elapsed time. Active records retain job,
vfunc, vtable, priority, guest parent pointer, pool, host thread and Cpu identity.
Nesting follows the Cpu context; a different guest fiber on the same host
thread is not treated as a child of a suspended context.

On null allocation, at most 128 active jobs and 128 job-type summaries are
logged. Storage is bounded at 512 simultaneous records and 512 types; excess
observations are counted as untracked, without changing guest behavior.
Known inflate counters remain separate. Synthetic O0/O2 cases cover nested
jobs, fiber context separation, failure snapshots, non-job tail dispatch,
bounded storage, original CPU results and balanced host-depth tracking.

## Observed concurrent producers

The `pool-jobs-01` probe used native decompression, deferred shader lowering
and the all-job observer. Its executable SHA-256 was
`576453b8c6a47e03f3c53a13a2cb3f226f7ea8d518a65427ea6c4db4e2cae6e9`.
At the null allocation, both the used count and sampled bitmap showed all
16000 slots occupied. Observed counters were 63360 successful allocations,
47361 frees and 26633 completed inflate jobs out of 26636 starts.

The bounded executor snapshot contained seven active records, eight job types
and no untracked observations:

| Executor target, relative to main | Active | Priority | Interpretation |
| --- | ---: | ---: | --- |
| `0x27eac` | 3 | 3 | Resource parent/deserializer jobs |
| `0x28708` | 3 | 4 | Inflate children; two were nested under assisting parents |
| `0x7e600` | 1 | 20 | Generic callback wrapper |

The newest parent was constructing child 3943 of an 8531-child batch when
allocation failed. Two older parents were already assisting their own child
jobs. Since main+0x281e0 queues an entire batch before assisting, concurrent
parents can request more than the pool's 16000 slots. This snapshot supports
a real queue backlog with continuing consumer progress; it does not establish
a lost-free defect or a stuck inflater. Across the run, the observer counted
277 parent starts and 274 completions. Inflate durations had a maximum of
0.257 ms in this probe.

The generic main+0x7e600 job calls another virtual callback through
job+0x48, then object+0x28, at main+0x7e670. Its outer executor label does not
identify that callback, so the snapshot cannot exclude shader work inside it.
The repeatedly requeued main+0x29c1c job completed 519925 observed executions.
It calls main+0x24ca0 and requeues until a flag in job+0x40 is set; the count
alone does not prove those executions were empty polling. These observations
do not justify changing pool capacity or guest queue ordering.
