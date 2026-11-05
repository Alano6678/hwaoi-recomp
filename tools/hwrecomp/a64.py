"""Fail-closed bounded AArch64-to-C99 translator (not a whole-game runtime).

Supported: MOVN/Z/K; ADD/SUB immediate and shifted-register incl NZCV;
AND/ORR/EOR/ANDS and inverted-operand forms; ADR/ADRP; B/BL/B.cond;
CBZ/CBNZ/TBZ/TBNZ; BR/BLR/RET; scalar unsigned LDR/STR (8/16/32/64),
unscaled/pre/post-index variants, W/X LDP/STP offset/pre/post; NOP.
Rejects reserved encodings, unpredictable writeback overlap and every unhandled
instruction BEFORE generating C. No FP/SIMD, exclusives, atomics, SVC, exceptions,
TLS, MMU, synchronization, OS, graphics, relocation or ABI recovery.

translate(bytes, base_address, name) returns C that includes hwr_runtime.h.
int name(HwrContext*) executes from the caller-supplied pc/budget; it yields
HWR_EXTERNAL at an address outside the translated region, HWR_RETURN for an
external RET, and explicit faults/budget exhaustion. An internal BL/RET works
through guest X30. The caller must resolve external transfers; none are faked.
Memory accesses are bounds checked against ONE caller-provided linear mapping.
Instruction words are decoded only in Python; generated C contains specialized
arithmetic and control flow, not an ARM instruction decoder/interpreter.
"""
from __future__ import annotations

from dataclasses import dataclass
import re
import struct


class UnsupportedInstruction(ValueError):
    def __init__(self, address: int, word: int):
        self.address, self.word = address, word
        super().__init__(f"unsupported AArch64 instruction 0x{word:08x} at 0x{address:x}")


@dataclass(frozen=True)
class Instruction:
    kind: str
    fields: dict[str, int]


def _signed(value: int, bits: int) -> int:
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def _decode(w: int) -> Instruction | None:
    sf, rd, rn, rm = w >> 31, w & 31, (w >> 5) & 31, (w >> 16) & 31
    bits = 64 if sf else 32
    def ins(kind: str, **fields: int) -> Instruction:
        return Instruction(kind, fields)
    if w == 0xd503201f:
        return ins("nop")
    if w & 0x1f800000 == 0x12800000:
        op, half = (w >> 29) & 3, (w >> 21) & 3
        if op == 1 or (not sf and half > 1):
            return None
        return ins("move_wide", rd=rd, bits=bits, op=op,
                   shift=half * 16, value=(w >> 5) & 0xffff)
    if w & 0x1f800000 == 0x11000000:
        return ins("add_sub_immediate", rd=rd, rn=rn, bits=bits,
                   subtract=(w >> 30) & 1, flags=(w >> 29) & 1,
                   value=((w >> 10) & 0xfff) << (12 if w & (1 << 22) else 0))
    if w & 0x1f200000 == 0x0b000000:
        shift, count = (w >> 22) & 3, (w >> 10) & 63
        if shift == 3 or count >= bits:
            return None
        return ins("add_sub_shifted", rd=rd, rn=rn, rm=rm, bits=bits,
                   subtract=(w >> 30) & 1, flags=(w >> 29) & 1,
                   shift=shift, count=count)
    if w & 0x1f000000 == 0x0a000000:
        count = (w >> 10) & 63
        if count >= bits:
            return None
        return ins("logical_shifted", rd=rd, rn=rn, rm=rm, bits=bits,
                   op=(w >> 29) & 3, invert=(w >> 21) & 1,
                   shift=(w >> 22) & 3, count=count)
    if w & 0x1f000000 == 0x10000000:
        value = _signed(((w >> 5) & 0x7ffff) << 2 | ((w >> 29) & 3), 21)
        return ins("adrp" if sf else "adr", rd=rd, value=value)
    if w & 0x7c000000 == 0x14000000:
        return ins("bl" if sf else "b", offset=_signed(w & 0x3ffffff, 26) * 4)
    if w & 0xff000010 == 0x54000000:
        return ins("b_cond", cond=w & 15, offset=_signed((w >> 5) & 0x7ffff, 19) * 4)
    if w & 0x7e000000 == 0x34000000:
        return ins("cb", rt=rd, bits=bits, nonzero=(w >> 24) & 1,
                   offset=_signed((w >> 5) & 0x7ffff, 19) * 4)
    if w & 0x7e000000 == 0x36000000:
        return ins("tb", rt=rd, bit=(sf << 5) | ((w >> 19) & 31),
                   nonzero=(w >> 24) & 1, offset=_signed((w >> 5) & 0x3fff, 14) * 4)
    branches = {0xd61f0000: "br", 0xd63f0000: "blr", 0xd65f0000: "ret"}
    if w & 0xfffffc1f in branches:
        return ins(branches[w & 0xfffffc1f], rn=rn)
    if w & 0x3f000000 == 0x39000000:
        op, size = (w >> 22) & 3, (w >> 30) & 3
        if op > 1:
            return None
        return ins("memory", rt=rd, rn=rn, load=op, size=1 << size,
                   offset=((w >> 10) & 0xfff) << size, mode=0)
    if w & 0x3f200000 == 0x38000000:
        op, size, mode = (w >> 22) & 3, (w >> 30) & 3, (w >> 10) & 3
        if op > 1 or mode == 2 or (mode in (1, 3) and rn != 31 and rn == rd):
            return None
        return ins("memory", rt=rd, rn=rn, load=op, size=1 << size,
                   offset=_signed((w >> 12) & 511, 9), mode=mode)
    if w & 0x3e000000 == 0x28000000:
        op, mode, load, rt2 = (w >> 30) & 3, (w >> 23) & 3, (w >> 22) & 1, (w >> 10) & 31
        if op not in (0, 2) or mode == 0:
            return None
        if load and rd == rt2:
            return None
        if mode in (1, 3) and rn != 31 and rn in (rd, rt2):
            return None
        size = 8 if op == 2 else 4
        return ins("pair", rt=rd, rt2=rt2, rn=rn, load=load, size=size,
                   offset=_signed((w >> 15) & 127, 7) * size, mode=mode)
    return None


def instruction_support(word: int) -> str | None:
    """Return supported encoding family, or None (not execution coverage)."""
    if not 0 <= word <= 0xffffffff:
        return None
    instruction = _decode(word)
    return instruction.kind if instruction else None


def _u64(value: int) -> str:
    return f"UINT64_C(0x{value & 0xffffffffffffffff:x})"


def _get(reg: int, sp: int = 0) -> str:
    return f"hwr_reg(c, {reg}, {sp})"


def _set(reg: int, value: str, bits: int = 64, sp: int = 0) -> str:
    return f"hwr_write(c, {reg}, {sp}, {bits}, {value});"


def _emit(i: Instruction, address: int, start: int, end: int) -> list[str]:
    k, f = i.kind, i.fields
    following = f"c->pc = {_u64(address + 4)};"
    lines: list[str] = []
    if k == "nop":
        pass
    elif k == "move_wide":
        value = f["value"] << f["shift"]
        expression = _u64(~value) if f["op"] == 0 else _u64(value)
        if f["op"] == 3:
            expression = f"({_get(f['rd'])} & {_u64(~(0xffff << f['shift']))}) | {_u64(value)}"
        lines.append(_set(f["rd"], expression, f["bits"]))
    elif k in ("add_sub_immediate", "add_sub_shifted"):
        immediate = k == "add_sub_immediate"
        rhs = _u64(f["value"]) if immediate else f"hwr_shift({_get(f['rm'])}, {f['shift']}, {f['count']}, {f['bits']})"
        lines += [f"uint64_t a = {_get(f['rn'], int(immediate))} & hwr_mask({f['bits']});",
                  f"uint64_t b = {rhs};",
                  f"uint64_t r = (a {'-' if f['subtract'] else '+'} b) & hwr_mask({f['bits']});"]
        if f["flags"]:
            lines.append(f"hwr_arithmetic_flags(c, a, b, r, {f['bits']}, {f['subtract']});")
        lines.append(_set(f["rd"], "r", f["bits"], int(immediate and not f["flags"])))
    elif k == "logical_shifted":
        lines += [f"uint64_t a = {_get(f['rn'])};",
                  f"uint64_t b = {'~' if f['invert'] else ''}hwr_shift({_get(f['rm'])}, {f['shift']}, {f['count']}, {f['bits']});",
                  f"uint64_t r = a {['&', '|', '^', '&'][f['op']]} b;"]
        if f["op"] == 3:
            lines.append(f"hwr_logical_flags(c, r, {f['bits']});")
        lines.append(_set(f["rd"], "r", f["bits"]))
    elif k in ("adr", "adrp"):
        result = address + f["value"] if k == "adr" else (address & ~0xfff) + (f["value"] << 12)
        lines.append(_set(f["rd"], _u64(result)))
    elif k in ("b", "bl", "b_cond", "cb", "tb"):
        target = _u64(address + f["offset"])
        if k == "bl":
            lines.append(_set(30, _u64(address + 4)))
        if k in ("b", "bl"):
            following = f"c->pc = {target};"
        else:
            if k == "b_cond":
                condition = f"hwr_condition(c, {f['cond']})"
            elif k == "cb":
                condition = f"({_get(f['rt'])} & hwr_mask({f['bits']})) {'!=' if f['nonzero'] else '=='} 0"
            else:
                condition = f"({_get(f['rt'])} & {_u64(1 << f['bit'])}) {'!=' if f['nonzero'] else '=='} 0"
            following = f"c->pc = ({condition}) ? {target} : {_u64(address + 4)};"
    elif k in ("br", "blr", "ret"):
        lines.append(f"uint64_t target = {_get(f['rn'])};")
        if k == "blr":
            lines.append(_set(30, _u64(address + 4)))
        following = "c->pc = target;"
        if k == "ret":
            following += f"\nif (target < {_u64(start)} || target >= {_u64(end)} || (target & 3)) return c->status = HWR_RETURN;"
    elif k in ("memory", "pair"):
        post = f["mode"] == 1
        pair = k == "pair"
        writeback = f["mode"] in (1, 3)
        lines += [f"uint64_t origin = {_get(f['rn'], 1)};",
                  f"uint64_t adjusted = origin + {_u64(f['offset'])};",
                  f"uint64_t address = {'origin' if post else 'adjusted'};"]
        if pair:
            lines.append(f"if (!hwr_memory_range(c, address, {f['size'] * 2})) return c->status;")
        if f["load"]:
            lines += ["uint64_t value;", f"if (!hwr_load(c, address, {f['size']}, &value)) return c->status;"]
            if pair:
                lines += ["uint64_t value2;", f"if (!hwr_load(c, address + {f['size']}, {f['size']}, &value2)) return c->status;"]
            lines.append(_set(f["rt"], "value", 64 if f["size"] == 8 else 32))
            if pair:
                lines.append(_set(f["rt2"], "value2", 64 if f["size"] == 8 else 32))
        else:
            lines.append(f"if (!hwr_store(c, address, {f['size']}, {_get(f['rt'])})) return c->status;")
            if pair:
                lines.append(f"if (!hwr_store(c, address + {f['size']}, {f['size']}, {_get(f['rt2'])})) return c->status;")
        if writeback:
            lines.append(_set(f["rn"], "adjusted", sp=1))
    else:
        raise AssertionError(k)
    return lines + [following, "break;"]


def translate(code: bytes, base_address: int, name: str = "hwr_function") -> str:
    """Translate an explicitly selected code range; no function discovery implied."""
    if not code or len(code) % 4:
        raise ValueError("code must be a nonempty multiple of 4 bytes")
    if base_address < 0 or base_address % 4 or base_address + len(code) >= 1 << 64:
        raise ValueError("aligned address range must fit uint64")
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
        raise ValueError("name must be a C identifier")
    decoded = []
    for index, (word,) in enumerate(struct.iter_unpack("<I", code)):
        instruction = _decode(word)
        if instruction is None:
            raise UnsupportedInstruction(base_address + index * 4, word)
        decoded.append((base_address + index * 4, word, instruction))
    result = ["/* Generated bounded AArch64 static translation. Private game-derived output. */",
              '#include "hwr_runtime.h"', f"int {name}(HwrContext *c) {{",
              "    c->status = HWR_RUNNING;", "    for (;;) {", "        switch (c->pc) {"]
    for address, word, instruction in decoded:
        result += [f"        case {_u64(address)}: {{ /* {word:08x}: {instruction.kind} */",
                   "            if (!c->budget) return c->status = HWR_BUDGET_EXHAUSTED;",
                   "            --c->budget;"]
        for line in _emit(instruction, address, base_address, base_address + len(code)):
            result.extend("            " + part for part in line.splitlines())
        result.append("        }")
    result += ["        default: return c->status = HWR_EXTERNAL;", "        }", "    }", "}", ""]
    return "\n".join(result)
