# Render clears during loading

The black captures are not evidence that every shader failed. The render-17
GPU dump contains a 256x256 two-channel floating-point lookup target with 55,492
distinct texels. Later frames execute three graphics draws, including a
1600x900 postprocessing target and the final display blit. This is partial GPU
execution evidence, not a meaningful game frame or proof of shader correctness.

## Observed clear sequence

The render-18 dump shows the postprocessing target containing a uniform RGB
value of 3/255 with alpha 1 at the end of its pass. When the next pass samples
that target, it contains zero in every channel. render-19's image handles and
clear records identify an explicit clear of the same image between those
passes. The final display image is also explicitly cleared after the blit.

The frontend trace in render-20 corroborates this sequence before Vulkan replay:

1. Draw to the 1600x900 postprocessing texture.
2. Bind that same texture without a depth attachment, then clear it to zero.
3. Draw the final blit, sampling the cleared texture.
4. Bind the same display target without a depth attachment, then clear it.

The texture object, immutable texture serial and storage address agree across
these operations. Recorded byte offsets increase in that order. The host
recording code appends commands, copies recordings in order, and retains
immutable texture snapshots by serial. No command reorder or stale texture
reference was established in this path.

## Guest provenance

These offsets apply to main SHA-256
`4c33cfd4c818d9ed62822af792782b4ea443417ed24acea6f1cee739cde604c3` only.
The evidence is local NSO disassembly plus supervised runtime logs; it is not
an REA native-session result.

The clear wrapper at main+0x718fe0 selects the renderer's current target stack
entry, binds its color attachments, and calls ClearColor at main+0x7191c0.
The observed ABI is the implemented one: command buffer, color index, pointer
to four float values, and channel mask 15. The wrapper's saved return address
is main+0x72529c in the render thread's command interpreter.

The interpreter at main+0x725288 reads a 16-bit command opcode and dispatches
through the table at main+0x271f740. Opcode 4 selects main+0x725b38, which reads
a 12-byte clear command, extracts clear flags from bits 16..23 of its first
word, reads packed color and depth, then tail-calls the clear wrapper.

In inflate-verify-03, the cursor evidence for all six traced color clears in
frames 20 and 21 identifies that standalone opcode-4 command with color flags
15 and zero color/depth. Thus the clears are explicit serialized game commands,
not an invented Vulkan load operation or a mistaken bind-and-clear decoder.
That run used executable SHA-256
`d0d481690aeabcb43435e62847b09e52524b323c0d6c5358c6140be8113386a1`.

No SetRenderEnable or SetRenderEnableConditional call was observed in the
bounded frontend trace for those frames. This does not establish their absence
in all later execution, but does not support conditional-render mis-evaluation
as the cause of these particular clears.

## Remaining question and diagnostics

Why the higher-level game code emits these commands during loading is still
unknown. Loading has independently encountered resource-job pool exhaustion.
Do not remove, reorder or ignore the clears to manufacture a non-black result.
The next useful evidence is behavior after resource loading advances.

`HWAOI_RT_TRACE=1` enables frontend order diagnostics only while the completed
present count is 20 or 21. It logs target binds, clears, draw entry points,
recording copies and conditional-render calls. Clear diagnostics include a
bounded readable frame chain and raw bytes preceding the interpreter cursor;
those raw bytes are retained only in ignored work reports. The trace changes
no guest values but adds timing overhead.

GPU dumps now include image handles, viewport/scissor and fixed-function state,
and explicit color-clear records. With `TOTK_DUMP_UBO=1`, they also list written
graphics root words alongside ordinary uniform blocks. These diagnostics are
observations of the existing render path, not rendering fixes.
