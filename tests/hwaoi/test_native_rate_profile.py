"""Exact-build refusal and execution of the production scheduler-rate selector."""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "tools"), str(ROOT / "recomp")]
import hwaoi
import patches
import gen
from lifter import Lifter


class NativeRateProfileTests(unittest.TestCase):
    def table(self):
        return patches.load(ROOT / "recomp/patches-hwaoi196608-rate.txt",
                            [("main", 0x7000000000, 0x29a0000)])

    def test_exact_main_and_instruction_required(self):
        class Module:
            sha256 = json.loads(hwaoi.PROFILE.read_text())["modules"]["main"]
            word = 0xb9403368
            def code(self, address, size):
                assert address == 0x6c281c and size == 4
                return struct.pack("<I", self.word)
        module = Module()
        with patch("hwrecomp.nso.Nso", return_value=module):
            self.assertTrue(hwaoi.rate_profile(Path("unused")).is_file())
            module.word ^= 1
            with self.assertRaisesRegex(ValueError, "instruction mismatch"):
                hwaoi.rate_profile(Path("unused"))
            module.sha256 = "0" * 64
            with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                hwaoi.rate_profile(Path("unused"))

    def test_decoded_selector_uses_no_flags_and_stays_opt_in(self):
        from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
        table = self.table()
        self.assertEqual(table.sets, [("hwaoi_rate", "TOTK_RATE_UNLOCK", 0)])
        pc = 0x70006c281c
        _, words = table.for_module("main", 0x7000000000)[pc]
        dis = Cs(CS_ARCH_ARM64, CS_MODE_ARM); dis.detail = True
        ins = [next(dis.disasm(struct.pack("<I", word), pc)) for word in words]
        self.assertEqual([i.mnemonic for i in ins], ["ldr", "adrp", "ldr", "eor", "clz", "lsr", "neg", "eor", "and", "eor"])
        self.assertTrue(all(not i.update_flags for i in ins))
        self.assertEqual(ins[0].reg_name(ins[0].operands[1].mem.base), "x27")
        self.assertEqual(ins[0].operands[1].mem.disp, 0x30)
        self.assertEqual(ins[1].operands[1].imm, table.page)
        self.assertEqual(ins[2].operands[1].mem.disp, 0xb0)
        self.assertEqual(ins[3].operands[2].imm, 60)
        self.assertEqual(ins[5].operands[2].imm, 5)

    @unittest.skipUnless(os.name == "nt", "Windows native semantic fixture")
    def test_lifted_selector_exhaustive_inputs_and_preserved_state(self):
        table = self.table(); pc = 0x70006c281c
        class Module:
            lifter = Lifter(0x7000000000, "c")
            def lift(self, address):
                return self.lifter.lift(0xb9403368 if address == pc else 0xd65f03c0, address)
        emitter = gen.Emitter(Module(), {pc}, patched=table.for_module("main", 0x7000000000))
        lines, refs = emitter.function(pc, pc + 8)
        self.assertFalse(refs)
        source = r'''
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include "cpu.h"
#include "simd.h"
volatile int g_fnprof=0;uint8_t rt_patch_on[1];
void fnprof_hit(uint64_t a,uint64_t b,uint64_t d){(void)a;(void)b;(void)d;}
''' + "\n".join(lines) + r'''
static unsigned checks;
static uint8_t node[64] __attribute__((aligned(16)));
static void check(uint32_t original,uint32_t target,int enabled){
 Cpu c,before;memset(&c,0xa5,sizeof c);c.x[27]=(uintptr_t)node;c.nzcv=0x90000000;
 memcpy(node+0x30,&original,4);*(uint32_t*)(uintptr_t)0x70029a00b0ull=target;
 rt_patch_on[0]=(uint8_t)enabled;before=c;f_70006c281c(&c);
 assert(c.x[8]==(enabled&&original==60?target:original));
 for(unsigned i=0;i<32;i++)if(i!=8&&(!enabled||(i!=9&&i!=10)))assert(c.x[i]==before.x[i]);
 assert(c.sp==before.sp&&c.pc==before.pc&&c.nzcv==before.nzcv);
 assert(c.fpcr==before.fpcr&&c.fpsr==before.fpsr&&!memcmp(c.v,before.v,sizeof c.v));checks++;
}
int main(void){
 void *page=VirtualAlloc((void*)(uintptr_t)0x70029a0000ull,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(page);
 const uint32_t rates[]={30,60,90,120},edges[]={0x7fffffff,0x80000000,0xffffffc3,0xffffffff};
 for(unsigned r=0;r<4;r++)for(int enabled=0;enabled<2;enabled++){
  for(uint32_t original=0;original<65536;original++)check(original,rates[r],enabled);
  for(unsigned i=0;i<4;i++)check(edges[i],rates[r],enabled);
 }
 assert(checks==524320);VirtualFree(page,0,MEM_RELEASE);puts("524320 native selector cases passed");
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "rate.c"; path.write_text(source)
            for opt in ("-O0", "-O2"):
                exe = path.with_suffix(".exe")
                built = subprocess.run(["F:/llvm-mingw/bin/clang.exe", opt, "-march=x86-64-v3", "-I", str(ROOT / "runtime"),
                                        str(path), "-o", str(exe)], capture_output=True, text=True)
                self.assertEqual(built.returncode, 0, built.stderr)
                run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertIn("524320 native selector cases passed", run.stdout)

    def test_incomplete_or_stale_generation_cannot_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp); inputs = out / "hwaoi-input.json"; inputs.write_text('{"profile":"fixture"}')
            with self.assertRaisesRegex(ValueError, "missing"):
                hwaoi.require_successful_generation(out)
            record = {"status": "generating", "input_sha256": hwaoi.sha256(inputs)}
            (out / "hwaoi-generation.json").write_text(json.dumps(record))
            with self.assertRaisesRegex(ValueError, "incomplete"):
                hwaoi.require_successful_generation(out)
            record["status"] = "pass"; (out / "hwaoi-generation.json").write_text(json.dumps(record))
            hwaoi.require_successful_generation(out)
            inputs.write_text('{"profile":"different"}')
            with self.assertRaisesRegex(ValueError, "different inputs"):
                hwaoi.require_successful_generation(out)


if __name__ == "__main__":
    unittest.main()
