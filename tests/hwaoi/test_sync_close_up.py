"""The Sync Strike close-up's size patches (recomp/patches-hwaoi196608-display.txt, set hwaoi_display): decoded and
evaluated without the game. Age of Decomp."""
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "recomp"))
import patches

MAIN = 0x7000000000
PAGE = MAIN + 0x29a0000
SITES = {
    0x1c9718c: (0x297e23aa, ["ldp w10, w8, [x29, #-0x10]", "ldp w9, w11, [x29, #-8]", "sub w12, w9, #0x640",
                             "sub w16, w11, #0x384", "orr w12, w12, w16", "clz w12, w12", "lsr w12, w12, #5"]),
    0x1c97190: (0x52841942, ["mov w2, #0x20ca", "adrp x16, #PAGE", "ldp w16, w17, [x16, #0x80]", "sub w16, w16, w9",
                             "madd w9, w16, w12, w9", "sub w17, w17, w11", "madd w11, w17, w12, w11",
                             "stp w9, w11, [x29, #-8]"]),
    0x19d4234: (0xf9407b57, ["ldur w9, [x29, #-0xc]", "ldur w10, [x29, #-0x10]", "sub w11, w9, #0x640",
                             "sub w12, w10, #0x384", "orr w11, w11, w12", "clz w11, w11", "lsr w11, w11, #5",
                             "msub w9, w9, w11, w9", "msub w10, w10, w11, w10", "stur w9, [x29, #-0xc]",
                             "stur w10, [x29, #-0x10]", "ldr x23, [x26, #0xf0]"]),
}
# The generator lifts a patched site only when its words leave the flags as the original does (recomp/gen.py
# patched_effect); none of these originals touches them.
FLAG_MNEMONICS = {"cmp", "cmn", "tst", "ccmp", "ccmn", "csel", "csinc", "csinv", "csneg", "cset", "csetm", "cinc",
                  "adds", "subs", "ands", "bics", "adcs", "sbcs", "fcmp", "fccmp", "fcsel", "b.eq", "b.ne"}
M32 = 0xffffffff


def clz32(v):
    v &= M32
    return 32 if v == 0 else 32 - v.bit_length()


def view_size(w, h, scene):
    """main+1c9718c and main+1c97190 on the view's locals (w, h) with the scene's size on the page."""
    m = clz32(((w - 0x640) | (h - 0x384)) & M32) >> 5
    return (w + (scene[0] - w) * m) & M32, (h + (scene[1] - h) * m) & M32


def target_params(w, h):
    """main+19d4234 on a target definition's parameters."""
    m = clz32(((w - 0x640) | (h - 0x384)) & M32) >> 5
    return (w - w * m) & M32, (h - h * m) & M32


def target_size(w, h, scene):
    """The game's own reading of the parameters right after main+19d4234 (main+19d4238..19d428c)."""
    def one(v, s):
        if v == 0:
            return s
        if v & 0x80000000:
            return s >> ((-v) & 31)
        return v
    return one(w, scene[0]), one(h, scene[1])


class SyncCloseUpTests(unittest.TestCase):
    def table(self):
        return patches.load(ROOT / "recomp/patches-hwaoi196608-display.txt",
                            [("main", MAIN, 0x29a0000), ("sdk", 0x7002c00000, 0xff0000)])

    def test_sites_decode(self):
        from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
        dis = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        table = self.table()
        display = [name for name, _, _ in table.sets].index("hwaoi_display")
        resolved = table.for_module("main", MAIN)   # @page as the ADRP the generator lifts
        self.assertEqual(table.page, PAGE)
        for offset, (original, expected) in SITES.items():
            k, _, orig = table.sites["main"][offset]
            self.assertEqual((k, orig), (display, original), hex(offset))
            words = resolved[MAIN + offset][1]
            text = []
            for w in words:
                ins = next(dis.disasm(struct.pack("<I", w), MAIN + offset))
                text.append(f"{ins.mnemonic} {ins.op_str}".replace(f"#{PAGE:#x}", "#PAGE"))
            self.assertEqual(text, expected, hex(offset))
            self.assertIn(original, words, "the original instruction still runs")
            self.assertFalse({t.split()[0] for t in text} & FLAG_MNEMONICS, hex(offset))

    def test_view_takes_the_scene_size_only_for_1600x900(self):
        for scene in ((2560, 1440), (1920, 1080), (1280, 720), (3840, 2160), (2128, 1200)):
            self.assertEqual(view_size(1600, 900, scene), scene)
            for other in ((1600, 901), (1599, 900), (3200, 900), (900, 1600), (0, 0), (16, 16), (M32, M32),
                          (1600 + (1 << 31), 900)):
                self.assertEqual(view_size(*other, scene), other)

    def test_targets_of_1600x900_read_as_the_scene(self):
        self.assertEqual(target_params(1600, 900), (0, 0))
        for scene in ((2560, 1440), (1920, 1080), (3840, 2160)):
            self.assertEqual(target_size(*target_params(1600, 900), scene), scene)
            # what the data asks for otherwise keeps its meaning: the scene, a fraction of it, an absolute size
            self.assertEqual(target_size(*target_params(0, 0), scene), scene)
            self.assertEqual(target_size(*target_params(-1 & M32, -2 & M32), scene), (scene[0] >> 1, scene[1] >> 2))
            for absolute in ((4096, 4096), (256, 256), (2, 1), (1, 1), (1600, 1600), (800, 450)):
                self.assertEqual(target_size(*target_params(*absolute), scene), absolute)


if __name__ == "__main__":
    unittest.main()
