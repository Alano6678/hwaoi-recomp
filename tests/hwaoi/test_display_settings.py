"""Independent decoding of native-size patches and strict preset/refusal checks."""
import os
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "recomp"))
import hwaoi
import patches


def compiler():
    """A C compiler for the two tests that build a small program: AOD_TEST_CC, the project's toolchain, or clang."""
    named = os.environ.get("AOD_TEST_CC")
    if named:
        return named
    for own in (ROOT.parent.parent / ".cache/toolchain/windows/llvm/bin/clang.exe",        # the project's (Windows)
                Path(os.environ.get("HWAOI_TOOLS", "F:") + "/llvm-mingw/bin/clang.exe")):  # the upstream layout
        if os.name == "nt" and own.is_file():
            return str(own)
    return shutil.which("clang")


class DisplaySettingsTests(unittest.TestCase):
    def table(self):
        return patches.load(ROOT / "recomp/patches-hwaoi196608-display.txt",
                            [("main", 0x7000000000, 0x29a0000), ("sdk", 0x7002c00000, 0xff0000)])

    def test_hash_and_every_displaced_instruction_are_required(self):
        table = self.table()
        words = {off: row[2] for off, row in table.sites["main"].items()}
        class Module:
            sha256 = json.loads(hwaoi.PROFILE.read_text())["modules"]["main"]
            def code(self, address, length):
                return struct.pack("<I", words[address])
        module = Module()
        with patch("hwrecomp.nso.Nso", return_value=module):
            self.assertTrue(hwaoi.display_profile(Path("unused")).is_file())
            for address in words:
                words[address] ^= 1
                with self.assertRaisesRegex(ValueError, "instruction mismatch"):
                    hwaoi.display_profile(Path("unused"))
                words[address] ^= 1
            module.sha256 = "0" * 64
            with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                hwaoi.display_profile(Path("unused"))

    def test_page_loads_and_region_depth(self):
        from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
        dis = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        dis.detail = True
        table = self.table()
        rows = table.for_module("main", 0x7000000000)
        def instructions(off):
            # Every replacement is lifted at the original PC by the generator.
            return [next(dis.disasm(struct.pack("<I", w), 0x7000000000 + off))
                    for w in rows[0x7000000000 + off][1]]
        expected = {0x1af1cf4: ("x9", 0x80), 0x1af34a4: ("x9", 0x80),
                    0x1af1b20: ("x10", 0x88), 0x1af1cf0: ("x8", 0x88),
                    0x1af3318: ("x8", 0x88), 0x1af332c: ("x8", 0x88),
                    0x1af3338: ("w8", 0x8c), 0x1af4170: ("w3", 0x88),
                    0x1af4174: ("w4", 0x8c), 0x1e62d68: ("w1", 0x88),
                    0x1e62d6c: ("w2", 0x8c), 0x1e62d8c: ("w1", 0x88),
                    0x1e62d90: ("w2", 0x8c), 0x1e63780: ("x8", 0x88), 0x19eb098: ("x12", 0x98),
                    0x1af1af8: ("x9", 0xa0)}
        for off, (register, offset) in expected.items():
            first, load, *_ = instructions(off)
            self.assertEqual(first.mnemonic, "adrp")
            self.assertEqual(first.operands[1].imm, table.page)
            self.assertEqual(load.mnemonic, "ldr")
            self.assertEqual(load.reg_name(load.operands[0].reg), register)
            self.assertEqual(load.operands[1].mem.disp, offset)
            self.assertEqual(load.reg_name(load.operands[1].mem.base), "x" + register[1:])
        shift = instructions(0x1af332c)[2]
        self.assertEqual((shift.mnemonic, shift.operands[2].imm), ("lsl", 32))
        self.assertNotIn(0x1af3348, table.sites["main"])  # retain depth=1, not packed height
        for off in (0x1af1d00, 0x1af34b0, 0x1af1b28, 0x1af1cfc, 0x1af3324, 0x1e6378c, 0x19eb09c,
                    0x1af1afc, 0x1af1b00):
            self.assertEqual(instructions(off)[0].mnemonic, "nop")

    def test_ui_canvas_sites_load_the_canvas_size(self):
        # Field decoding from the A64 encodings: ADRP, LDR (immediate, unsigned offset, 64-bit), LDP (SIMD&FP, 32-bit,
        # signed offset). Each site must take its operand from page +0xa8 in place of the original load.
        table = self.table()
        rows = table.for_module("main", 0x7000000000)
        canvas = {off: row for off, row in table.sites["main"].items() if table.sets[row[0]][0] == "hwaoi_ui_canvas"}
        self.assertEqual(set(canvas), {0x576ff8, 0xadd844, 0xadd874, 0xaf42e4, 0xaf42e8, 0xaf42ec, 0xaf42f0,
                                       0xaf43d8, 0xaf43e0, 0xaf43e8, 0xaf43f0, 0xaf43f8, 0xaf4400, 0xaf4408, 0xaf4410,
                                       0xaf3b74})
        def adrp_target(pc, word):
            self.assertEqual(word & 0x9f000000, 0x90000000)
            imm = ((word >> 5) & 0x7ffff) << 2 | (word >> 29) & 3
            imm -= (imm >> 20 & 1) << 21
            return (pc & ~0xfff) + (imm << 12), word & 31
        expected = {0x576ff8: ("ldr", 25, (25,), 0xf9400119), 0xadd844: ("ldp", 9, (0, 1), 0x2d400680),
                    0xadd874: ("ldp", 9, (1, 2), 0x2d400a81)}
        for off, (kind, base, dest, original) in expected.items():
            self.assertEqual(canvas[off][2], original)
            adrp, load = rows[0x7000000000 + off][1]
            target, reg = adrp_target(0x7000000000 + off, adrp)
            self.assertEqual((target, reg), (table.page, base))
            self.assertEqual((load >> 5) & 31, base)
            if kind == "ldr":
                self.assertEqual(load & 0xffc00000, 0xf9400000)
                self.assertEqual(((load >> 10) & 0xfff) * 8, 0xa8)
                self.assertEqual((load & 31,), dest)
            else:
                self.assertEqual(load & 0xffc00000, 0x2d400000)
                self.assertEqual(((load >> 15) & 0x7f) * 4, 0xa8)
                self.assertEqual((load & 31, (load >> 10) & 31), dest)

    def test_ui_canvas_primitive_corners_scale_by_page_factor(self):
        # main+af42dc copies a 2D primitive's four corners (ldp/stp of x8..x11). The patch loads them as two q
        # registers, multiplies by the f32 at page +0xb8 and stores them; the displaced copies become nops.
        table = self.table()
        rows = table.for_module("main", 0x7000000000)
        words = rows[0x7000000000 + 0xaf42e4][1]
        self.assertEqual(table.sites["main"][0xaf42e4][2], 0xa9402448)
        ldp, adrp, ldr, mul0, mul1, stp = words
        self.assertEqual(ldp & 0xffc00000, 0xad400000)                       # LDP q, q, [x2]
        self.assertEqual(((ldp >> 5) & 31, ldp & 31, (ldp >> 10) & 31, (ldp >> 15) & 0x7f), (2, 0, 1, 0))
        self.assertEqual(adrp & 0x9f00001f, 0x90000009)
        imm = ((adrp >> 5) & 0x7ffff) << 2 | (adrp >> 29) & 3
        imm -= (imm >> 20 & 1) << 21
        self.assertEqual(((0x7000000000 + 0xaf42e4) & ~0xfff) + (imm << 12), table.page)
        self.assertEqual(ldr & 0xffc00000, 0xbd400000)                       # LDR s, [x9, #imm]
        self.assertEqual(((ldr >> 5) & 31, ldr & 31, ((ldr >> 10) & 0xfff) * 4), (9, 2, 0xb8))
        for word, reg in ((mul0, 0), (mul1, 1)):                             # FMUL v.4s, v.4s, v2.s[0]
            self.assertEqual(word & 0xffe0fc00, 0x4f809000)
            self.assertEqual((word & 31, (word >> 5) & 31, (word >> 16) & 15, (word >> 11) & 1, (word >> 21) & 1),
                             (reg, reg, 2, 0, 0))
        self.assertEqual(stp & 0xffc00000, 0xad000000)                       # STP q, q, [x1]
        self.assertEqual(((stp >> 5) & 31, stp & 31, (stp >> 10) & 31, (stp >> 15) & 0x7f), (1, 0, 1, 0))
        for off, original in ((0xaf42e8, 0xa9412c4a), (0xaf42ec, 0xa9012c2a), (0xaf42f0, 0xa9002428)):
            self.assertEqual(table.sites["main"][off][2], original)
            self.assertEqual(rows[0x7000000000 + off][1], [0xd503201f])

    def test_ui_canvas_rectangles_and_lines_scale_by_page_factor(self):
        # main+af43d0 turns a rectangle (x0 y0 x1 y1 at x2) into four corners at x1 with eight ldr s0 / str s0 pairs:
        # each load is followed by FMUL s0, s0, s2, with s2 the f32 at page +0xb8 loaded before the first one. The
        # stores stay the original's. main+af3b24 (a line) multiplies its five kept arguments s12..s8 where the batch
        # has a target, before the instruction it displaces.
        table = self.table()
        rows = table.for_module("main", 0x7000000000)

        def page_load(pc, adrp, ldr, reg):
            self.assertEqual(adrp & 0x9f00001f, 0x90000009)                  # ADRP x9
            imm = ((adrp >> 5) & 0x7ffff) << 2 | (adrp >> 29) & 3
            imm -= (imm >> 20 & 1) << 21
            self.assertEqual((pc & ~0xfff) + (imm << 12), table.page)
            self.assertEqual(ldr & 0xffc00000, 0xbd400000)                   # LDR s<reg>, [x9, #0xb8]
            self.assertEqual(((ldr >> 5) & 31, ldr & 31, ((ldr >> 10) & 0xfff) * 4), (9, reg, 0xb8))

        def fmul(word):                                                      # FMUL s<d>, s<n>, s<m> -> (d, n, m)
            self.assertEqual(word & 0xffe0fc00, 0x1e200800)
            return word & 31, (word >> 5) & 31, (word >> 16) & 31

        loads = {0xaf43d8: 0, 0xaf43e0: 4, 0xaf43e8: 8, 0xaf43f0: 4, 0xaf43f8: 0, 0xaf4400: 12, 0xaf4408: 8, 0xaf4410: 12}
        for off, disp in loads.items():
            original = table.sites["main"][off][2]
            self.assertEqual(original & 0xffc003ff, 0xbd400040)              # LDR s0, [x2, #disp]
            self.assertEqual(((original >> 10) & 0xfff) * 4, disp)
            words = rows[0x7000000000 + off][1]
            if off == 0xaf43d8:
                page_load(0x7000000000 + off, words[0], words[1], 2)
                words = words[2:]
            self.assertEqual(len(words), 2)
            self.assertEqual(words[0], original)
            self.assertEqual(fmul(words[1]), (0, 0, 2))
        words = rows[0x7000000000 + 0xaf3b74][1]
        self.assertEqual(table.sites["main"][0xaf3b74][2], 0xf94202a8)       # LDR x8, [x21, #0x400]
        self.assertEqual(len(words), 8)
        page_load(0x7000000000 + 0xaf3b74, words[0], words[1], 16)
        self.assertEqual([fmul(w) for w in words[2:7]], [(r, r, 16) for r in (12, 11, 10, 9, 8)])
        self.assertEqual(words[7], 0xf94202a8)

    def test_confirm_swap_asks_for_the_other_action(self):
        """hwaoi_confirm: the game's button check (main+141debc) reads the row of action 10 (back) when the game asks
        for 9 (confirm) and the row of 9 for 10; every other action keeps its own row."""
        from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
        table = self.table()
        index = [name for name, _, _ in table.sets].index("hwaoi_confirm")
        self.assertEqual(table.sets[index], ("hwaoi_confirm", "TOTK_CONFIRM_SWAP", 0))
        k, words, original = table.sites["main"][0x141ded4]
        self.assertEqual((k, original), (index, 0x51000428))   # sub w8, w1, #1 after the prologue
        self.assertEqual(words[-1], original)                  # the original instruction runs last
        dis = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        text = [f"{i.mnemonic} {i.op_str}" for w in words for i in dis.disasm(struct.pack("<I", w), 0x700141ded4)]
        self.assertEqual(text, ["and w8, w1, #0xff", "sub w8, w8, #9", "lsr w8, w8, #1", "clz w8, w8",
                                "lsr w8, w8, #5", "orr w8, w8, w8, lsl #1", "eor w1, w1, w8", "sub w8, w1, #1"])

        def asked(action):   # those instructions on the action in w1
            t = (((action & 0xff) - 9) & 0xffffffff) >> 1
            one = (32 if t == 0 else 32 - t.bit_length()) >> 5
            return action ^ (one | one << 1)
        self.assertEqual([asked(a) for a in (9, 10)], [10, 9])
        self.assertEqual(asked(0x109) & 0xff, 10)   # the check reads the action as a byte (uxtb)
        self.assertEqual([asked(a) for a in (0, 1, 8, 11, 17, 18, 19, 26, 99)], [0, 1, 8, 11, 17, 18, 19, 26, 99])

    def test_confirm_swap_turns_the_guide_icons(self):
        """hwaoi_confirm in the button guide (main+15d52c0): the icon code read at main+15d641c becomes ^01 for ^00
        and ^00 for ^01, except under the battle labels 16 (dodge) and 62 (special attack); other icons stay."""
        from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
        table = self.table()
        index = [name for name, _, _ in table.sets].index("hwaoi_confirm")
        k, words, original = table.sites["main"][0x15d641c]
        self.assertEqual((k, original), (index, 0x39400108))   # ldrb w8, [x8]: the icon row's code
        self.assertEqual(words[0], original)                   # the load runs first
        dis = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        text = [f"{i.mnemonic} {i.op_str}" for w in words for i in dis.disasm(struct.pack("<I", w), 0x70015d641c)]
        self.assertEqual(text, ["ldrb w8, [x8]", "lsr w9, w8, #1", "clz w9, w9", "lsr w9, w9, #5",
                                "eor w10, w28, #0x10", "clz w10, w10", "lsr w10, w10, #5", "eor w11, w28, #0x3e",
                                "clz w11, w11", "lsr w11, w11, #5", "orr w10, w10, w11", "bic w9, w9, w10",
                                "eor w8, w8, w9"])

        def zero(v):   # clz(v) >> 5
            return (32 if v == 0 else 32 - v.bit_length()) >> 5

        def shown(code, label):   # those instructions on the code (w8) under the label (w28)
            return code ^ (zero(code >> 1) & ~(zero(label ^ 16) | zero(label ^ 62)) & 1)
        self.assertEqual([shown(0, 18), shown(1, 2), shown(0, 4), shown(1, 3)], [1, 0, 1, 0])   # Choose, Back, Next, Cancel
        self.assertEqual([shown(1, 16), shown(0, 62)], [1, 0])                                   # Dodge, Special attack
        self.assertEqual([shown(c, 18) for c in (2, 3, 0x0a, 0x3a, 0xff)], [2, 3, 0x0a, 0x3a, 0xff])

    def test_native_presets_and_invalid_forms(self):
        cc = compiler()
        if not cc:
            self.skipTest("no C compiler: set AOD_TEST_CC to clang")
        source = r'''
#include <assert.h>
#include "hwaoi_display.h"
int main(void) {
 const char *names[]={"1280x720","1920x1080","2560x1440","3840x2160"};
 unsigned widths[]={1280,1920,2560,3840}, heights[]={720,1080,1440,2160};
 unsigned size[2];
 for(unsigned i=0;i<4;i++) { assert(hwaoi_display_size(names[i],size));
  assert(size[0]==widths[i] && size[1]==heights[i]); }
 const char *bad[]={0,"","1920X1080","1920x1080extra","3840x2161","5120x1440","-1920x1080"," 1920x1080"};
 for(unsigned i=0;i<sizeof bad/sizeof *bad;i++) {size[0]=111;size[1]=222;
  assert(!hwaoi_display_size(bad[i],size));assert(size[0]==111&&size[1]==222);}
 unsigned output[2],scene[2];uint64_t child,parent,extra;
 assert(hwaoi_display_dimensions("3840x2160",NULL,output,scene));
 assert(hwaoi_display_pool_plan(scene,NULL,&child,&parent,&extra));
 assert(child==0x1c0100000ull&&parent==0x1d8c05008ull&&extra==0x9d200000ull);
 assert(hwaoi_display_dimensions("3840x2160","1920x1080",output,scene));
 assert(hwaoi_display_pool_plan(scene,NULL,&child,&parent,&extra));
 assert(child==0x122f00000ull&&parent==0x13ba05008ull&&!extra);
 assert(!hwaoi_display_dimensions("1920x1080","3840x2160",output,scene));
 for(unsigned i=0;i<2;i++) {
  assert(hwaoi_display_dimensions(names[i],NULL,output,scene));
  assert(hwaoi_display_pool_plan(scene,NULL,&child,&parent,&extra));assert(!extra&&child==0x122f00000ull);
 }
 assert(hwaoi_display_dimensions(names[2],NULL,output,scene));   // 1440p: 838 MiB per extra 1080p of pixels
 assert(hwaoi_display_pool_plan(scene,NULL,&child,&parent,&extra));
 assert(child==0x14ba00000ull&&parent==0x164505008ull&&extra==0x28c00000ull);
 assert(hwaoi_display_pool_plan(scene,"1",&child,&parent,&extra)&&extra==0x5d200000ull);
 scene[0]=3840;scene[1]=2160;
 assert(hwaoi_display_pool_plan(scene,"0",&child,&parent,&extra)&&!extra);
 assert(hwaoi_display_pool_plan(scene,"5120",&child,&parent,&extra));
 assert(child==0x140000000ull&&parent==0x158b05008ull&&extra==0x1d200000ull);
 const char *invalid[]={"","2","yes","+1"};
 for(unsigned i=0;i<4;i++){child=1;parent=2;extra=3;assert(!hwaoi_display_pool_plan(scene,invalid[i],&child,&parent,&extra));
  assert(child==1&&parent==2&&extra==3);}
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "display.c"; path.write_text(source)
            exe = path.with_suffix(".exe")
            subprocess.run([str(cc), "-O2", "-I", str(ROOT / "runtime"), str(path), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_application_growth_is_profile_scoped(self):
        cc = compiler()
        if not cc:
            self.skipTest("no C compiler: set AOD_TEST_CC to clang")
        sys.path.insert(0, str(ROOT / "tests"))
        from nvn2_compute_c0_test import function
        text = (ROOT / "runtime/resolution.c").read_text(encoding="utf-8")
        policy = (function(text, "static const char *output_setting(") + "\n" +
                  function(text, "uint64_t res_heap_extra_bytes("))
        source = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "hwaoi_display.h"
#define RT_TARGET_HWAOI 1
static const char *resolution="1920x1080",*native,*experiment,*scene_override,*render_scale="100";
static const char *auto_pick="1920x1080";
static const char *auto_output(void){return auto_pick;}          /* what OUTPUT_RES=auto picked for the screen */
static int profile=1,foreign_heap_reads;
static int rt_patch_set(const char *name){assert(!strcmp(name,"hwaoi_display"));return profile?0:-1;}
static const char *cfg_value(const char *name){
 if(!strcmp(name,"TOTK_RENDER_SCALE"))return render_scale;
 assert(!strcmp(name,"TOTK_OUTPUT_RES"));return resolution;
}
static const char *cfg_get(const char *name){
 if(!strcmp(name,"TOTK_NATIVE_1080"))return native;
 if(!strcmp(name,"TOTK_SCENE_RES"))return scene_override;
 assert(!strcmp(name,"TOTK_DISPLAY_POOL_TEST"));return experiment;
}
static void rt_fatal(const char *fmt,...){(void)fmt;exit(73);}
static uint64_t res_heap_extra_kib(void){foreign_heap_reads++;return 42;}
''' + policy + r'''
int main(int argc,char **argv){
 assert(argc==2);if(!strcmp(argv[1],"bad")){resolution="3840x2161";res_heap_extra_bytes();return 1;}
 assert(!res_heap_extra_bytes());resolution="2560x1440";assert(res_heap_extra_bytes()==0x28c00000ull);
 resolution="3840x2160";assert(res_heap_extra_bytes()==0x9d200000ull);
 resolution="auto";assert(!res_heap_extra_bytes());                /* auto: the growth of the size it picked */
 auto_pick="2560x1440";assert(res_heap_extra_bytes()==0x28c00000ull);resolution="3840x2160";
 scene_override="1920x1080";assert(!res_heap_extra_bytes());scene_override=NULL;
 native="0";assert(!res_heap_extra_bytes());native=NULL;
 render_scale="game";assert(!res_heap_extra_bytes());              /* the game's own 3D resolution: no native growth */
 native="1";assert(res_heap_extra_bytes()==0x9d200000ull);native=NULL;render_scale="100";   /* forced on wins */
 profile=0;assert(!res_heap_extra_bytes());
 assert(!foreign_heap_reads);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "capacity.c"; path.write_text(source)
            exe = path.with_suffix(".exe")
            built = subprocess.run([cc, "-O2", "-I", str(ROOT / "runtime"), str(path), "-o", str(exe)],
                                   capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            subprocess.run([str(exe), "good"], check=True)
            self.assertEqual(subprocess.run([str(exe), "bad"], capture_output=True).returncode, 73)

    def test_original_child_and_parent_capacities_match_observed_pools(self):
        from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
        dis = Cs(CS_ARCH_ARM64, CS_MODE_ARM); dis.detail = True
        table = self.table()
        def wide_constant(offsets):
            value = 0
            for off in offsets:
                word = table.sites["main"][off][2]
                ins = next(dis.disasm(struct.pack("<I", word), off))
                if ins.mnemonic == "mov":
                    value = ins.operands[1].imm
                else:
                    self.assertEqual(ins.mnemonic, "movk")
                    shift = ins.operands[1].shift.value
                    value = value & ~(0xffff << shift) | ins.operands[1].imm << shift
            return value
        child = wide_constant((0x19eb098, 0x19eb09c))
        parent = wide_constant((0x1af1af8, 0x1af1afc, 0x1af1b00))
        self.assertEqual(child, 4655 * 1024 * 1024)
        self.assertEqual(parent, (4655 + 45 + 350) * 1024 * 1024 + 0x5008)
        self.assertEqual(parent - child, 395 * 1024 * 1024 + 0x5008)


if __name__ == "__main__":
    unittest.main()
