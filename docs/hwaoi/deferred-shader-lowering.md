# Deferred NVN2 program lowering

`ProgramSetShaders` now retains compact NVN2 inputs without translating each
loaded stage immediately. It copies the control block, instruction bytes and
inline constants into one program-owned allocation. Registration succeeding
means the input was retained; it does not mean the shader was compiled.

The shared `gfx_shader_nvn2_sizes` helper bounds control/code extents. The
registration path uses exact guarded memory reads, rejects incomplete compute
controls (fields extend through offset 0xb6), and requires at least four code
bytes for stage classification. Failed reads, invalid extents and allocation
failure leave the previous program identity and allocation intact. Replacement
prepares all inputs before draining backend references and releasing old data,
including when the replacement references the old program's own snapshot.

Raw inputs are classified without touching legacy control offsets. All replay
consumers needing legacy controls go through `find_stage`, which lowers the
requested stage and caches the resulting pointers by program serial and stage.
This includes uniform/constant binding, shader construction and the optional
geometry path. The existing process-wide native lowering cache still owns the
lowered allocations. Failure to lower a required stage is fatal and identifies
its stage, program and origin; an unused unsupported stage is never presented as
compiled. Native geometry is still unsupported by the experimental ordinary-GS
path. The lowering algorithm and shader semantics are unchanged.

Legacy programs retain their previous ownership contract; portable geometry
control metadata is copied as before. Raw NVN2 snapshots are per program, so
loading duplicate programs can retain duplicate input bytes. Lowered content
continues to share the existing content cache. No memory-use improvement is
claimed.

## Verification

- `tests/geometry_program_control_test.py --controls`: 280 checks pass at O0
  and O2. Deliberate borrowed-control and early-release mutations fail. Cases
  cover caller mutation/free, mixed raw/legacy inputs, self-alias replacement,
  guarded unreadable input, size boundaries and allocation-failure rollback.
- `tests/hwaoi/test_deferred_program.py`: actual production registration and
  stage lookup functions pass at O0/O2. A lowerer spy verifies classification
  performs no lowering, only the requested stage resolves, memoization follows
  program serials, legacy inputs bypass lowering, and a required failure exits
  through the fatal path. This tests dispatch/lifetime contracts, not shader
  instruction correctness or the existing native content-cache implementation.
- `tests/geometry_replay_test.py`: O0/O2 pass, 63 checks default-off and 118
  enabled. A required raw GS is resolved and then rejected by its native origin.
- `tests/storage_candidates_runtime_test.py`: O0/O2, 38 checks pass, preserving
  guarded descriptor resolution and legacy uniform offsets.
- All 2,342 controls from the local render-17 capture satisfy the new shared
  extent rules. This is compatibility evidence for those inputs only.

## Runtime result

The local `work/reports/render-24` probe ran the deferred path with native
inflate and the IR-bank translator. Executable SHA-256:
`6f39aa667dd7076b94a66652f580550541869a5870f73ba3448c16dfe9ffc513`.
It observed 173 presents in 11.766 seconds, with zero logged lowering failures
for requested shaders. The frame-150 capture was black. The run ended with the
same resource-job pool failure path, a null-derived write to address 0x18 in
main+0x2894c, exit 71. It did not reach the requested 600 presents.

Deferred lowering therefore executes successfully for this bounded probe, but
does not resolve the loading failure or establish visible gameplay. The lower
failure count excludes unused stages by design and must not be compared with
the previous eager count as a shader-coverage improvement.
