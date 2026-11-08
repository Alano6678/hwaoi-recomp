"""Exact-build patch refusal and independent AArch64 operand checks."""
import json
from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import hwaoi


def sites():
    rows = []
    for line in (ROOT / "recomp/patches-hwaoi196608-native1080.txt").read_text().splitlines():
        fields = line.split("#", 1)[0].split()
        if fields and fields[0] == "site":
            rows.append(tuple(int(v, 16) for v in fields[3:]))
    return rows


class Native1080Tests(unittest.TestCase):
    def test_exact_module_and_original_words_required(self):
        words = {row[0]: row[1] for row in sites()}
        class Module:
            sha256 = json.loads(hwaoi.PROFILE.read_text())["modules"]["main"]
            def code(self, address, length):
                self.assert_length = length
                return struct.pack("<I", words[address])
        module = Module()
        with patch("hwrecomp.nso.Nso", return_value=module):
            self.assertEqual(hwaoi.native_1080_profile(Path("unused")),
                             ROOT / "recomp/patches-hwaoi196608-native1080.txt")
            words[next(iter(words))] ^= 1
            with self.assertRaisesRegex(ValueError, "instruction mismatch"):
                hwaoi.native_1080_profile(Path("unused"))
            module.sha256 = "0" * 64
            with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                hwaoi.native_1080_profile(Path("unused"))

    def test_two_allocation_pairs_and_scale_store(self):
        from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
        dis = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        dis.detail = True
        rows = sites()
        self.assertEqual(len(rows), 12)
        for start in (0, 2):
            # Independent AArch64 decoding: MOV initializes X9, MOVK replaces bits32..47.
            dimensions = []
            for column in (1, 2):
                first = next(dis.disasm(struct.pack("<I", rows[start][column]), rows[start][0]))
                second = next(dis.disasm(struct.pack("<I", rows[start + 1][column]), rows[start + 1][0]))
                self.assertEqual((first.mnemonic, second.mnemonic), ("mov", "movk"))
                self.assertEqual(first.reg_name(first.operands[0].reg), "x9")
                self.assertEqual(second.reg_name(second.operands[0].reg), "x9")
                self.assertEqual(second.operands[1].shift.value, 32)
                dimensions.append((first.operands[1].imm, second.operands[1].imm))
            self.assertEqual(dimensions, [(1600, 900), (1920, 1080)])
        scale = next(dis.disasm(struct.pack("<I", rows[4][2]), rows[4][0]))
        self.assertEqual(scale.mnemonic, "fmov")
        self.assertEqual(scale.reg_name(scale.operands[0].reg), "s0")
        self.assertEqual(scale.operands[1].fp, 1.0)
        for row in rows[5:]:
            # The scale is overridden after both paths merge; the original
            # scalar/integer/stack operation is retained byte-for-byte.
            self.assertEqual(len(row), 4)
            self.assertEqual(row[3], row[1])
            scale = next(dis.disasm(struct.pack("<I", row[2]), row[0]))
            self.assertEqual(scale.mnemonic, "fmov")
            self.assertEqual(scale.operands[1].fp, 1.0)


if __name__ == "__main__":
    unittest.main()
