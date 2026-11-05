# HWAoI-only translator patches

These patches are applied to the managed builder's private source copy after the
unchanged upstream patch selection. `tools/vendor/ryujinx` is never edited.
`tools/build_xlate_hwaoi.py` includes each extra patch's name/content digest in
the translator identity and refuses a skipped application.

## 0001: private register banks, protocol version 1

The Ampere lowerer may emit the explicit private MOV format used by Eglantine's
`NativeRegisterCopy` (reference commit
`30604547544e6f2c766262850b88a31a2404c538`). It carries source/destination bank
numbers 0..2 in otherwise reserved bits and a strict marker. This is an internal
lowerer-to-translator format, not an invented NVIDIA GPU opcode. The adapter and
mask/SSA changes here are original; underlying source licenses remain intact.

Only a translation job containing integer `"hwaoi_ir_banks": 1` enables it.
Absent metadata, a different version, malformed marker/predicate/quad fields or
bank 3 are rejected. Ordinary Maxwell MOV behavior remains unchanged.

Architectural RZ stays index **255**. IR GPR capacity is **768**, so bank 1's
register 255 is real IR register **511**, and bank 2's is **767**. Predicate/flag
keys move after all 768 IR GPRs; register-use dataflow uses twelve 64-bit GPR masks
plus separate predicate and flag masks. The mask storage is a value type, not a
shared mutable array.

The library's original synthetic probe checks mapping, feature refusal, RZ,
live-mask boundaries and three independent GPR/predicate/flag SSA phis. The
builder additionally sends a hand-assembled private-bank shader through the
actual translation pipeline. In the initial validated build its SPIR-V is
byte-identical to the original constant-color shader after optimization.
This is not a real-GPU semantic test or proof that all native shaders are correct.

The older `xlate-managed` deployment is preserved. Protocol-capable deployment:
`work/build/hwaoi196608/xlate-managed-irbanks` (see its build-report.json and .id).
