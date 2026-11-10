"""NVN2-only GPU QUADS admission and restoration of guest graphics binding state."""
import os
import re
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests"))
from nvn2_compute_c0_test import function
CC = Path("F:/llvm-mingw/bin/clang.exe")
DEPS = Path("F:/TotKDependencies/runtime/deps/include")


def source():
    text = (ROOT / "runtime/vk_exec_draw.c").read_text()
    stages = "\n".join(re.findall(r"^#define NVN_STAGE_\w+ \d+$", text, re.MULTILINE)) + "\n"
    prefix = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game_target.h"
#include "vk_quad.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static unsigned present=(1u<<NVN_STAGE_VS)|(1u<<NVN_STAGE_FS),nvn2=~0u;
static unsigned reset,prepares,indices,draws,notes,counts;
static int backed=1,converted=1,draw_ok=1;
static const char *setting="1";
static int ab;
int rt_ab(void){return ab;}
static uint64_t bind_gen=9;static uint32_t c0_binst=99;
VkExecFrame vk_exec_frame;int vk_mirror_on;uint32_t rt_trace;
static const prog_t *program_stage_source(const VkExec *ex,int stage){(void)stage;return ex->prog;}
static int program_stage_lookup(const prog_t *p,unsigned stage,NVNshaderData *out){
 (void)p;if(!(present&(1u<<stage)))return 0;out->control=(const uint8_t *)(uintptr_t)(stage+1);out->data=1;return 1;}
int gfx_shader_is_nvn2(const uint8_t *control){return !!(nvn2&(1u<<((uintptr_t)control-1)));}
const char *cfg_get(const char *key){CHECK(!strcmp(key,"TOTK_GPU_QUADS"));return setting;}
int vk_mem_resolve(uint64_t addr,uint64_t size,VkMemSpan *out){CHECK(addr==0x10000 && size==16);
 *out=(VkMemSpan){.buffer=(VkBuffer)(uintptr_t)22,.offset=0,.avail=16};return backed;}
void vk_mem_use(VkMemSpan *span){(void)span;CHECK(0);}
void vk_mem_bound_note(void *pool){(void)pool;CHECK(0);}
void vk_mem_count(int cls,int use,uint64_t bytes){(void)cls;CHECK(use==VK_MUSE_IND && bytes==16);counts++;}
int vk_quad_indirect_prepare(VkExec *ex,const VkMemSpan *source,uint64_t address,VkQuadIndirect *out){
 CHECK(source->buffer==(VkBuffer)(uintptr_t)22 && address==0x10000);if(!converted)return 0;
 *out=(VkQuadIndirect){(VkBuffer)(uintptr_t)33,(VkBuffer)(uintptr_t)44,512};ex->recorded+=8;return 1;
}
void vk_exec_draw_new_cb(void){reset++;}
static int prepare_draw(VkExec *ex,int primitive,int32_t count,int restart){(void)ex;
 CHECK(primitive==NVN_DRAW_PRIMITIVE_QUADS && count==INT32_MAX && restart==0);prepares++;return draw_ok;}
void rc_vkCmdBindIndexBuffer(VkCommandBuffer cb,VkBuffer b,VkDeviceSize offset,VkIndexType type){
 (void)cb;CHECK(b==(VkBuffer)(uintptr_t)33 && !offset && type==VK_INDEX_TYPE_UINT32);indices++;}
void rc_vkCmdDrawIndexedIndirect(VkCommandBuffer cb,VkBuffer b,VkDeviceSize offset,uint32_t count,uint32_t stride){
 (void)cb;CHECK(b==(VkBuffer)(uintptr_t)44 && offset==512 && count==1 && stride==20);draws++;}
static void note_work(const VkExec *ex,int kind,uint64_t work){(void)ex;CHECK(kind==1 && !work);notes++;}
'''
    main = r'''
int main(int argc,char **argv){
 CHECK(argc==2);VkExec ex={.prog=(const prog_t *)(uintptr_t)1,.fence=1};p_draw_ind draw={NVN_DRAW_PRIMITIVE_QUADS,0,0x10000};
 _Static_assert(NVN_STAGE_FS==1 && GFX_STAGE_FS==4,"different NVN and translator stage namespaces");
 CHECK(gpu_quads_graphics(&ex));nvn2&=~(1u<<NVN_STAGE_FS);CHECK(!gpu_quads_graphics(&ex));nvn2=~0u;
 for(unsigned stage=NVN_STAGE_GS;stage<NVN_STAGE_CS;stage++){
  present|=1u<<stage;CHECK(!gpu_quads_graphics(&ex));present&=~(1u<<stage);
 }
 present=1u<<NVN_STAGE_FS;CHECK(!gpu_quads_graphics(&ex));present=(1u<<NVN_STAGE_VS)|(1u<<NVN_STAGE_FS);
 if(!strcmp(argv[1],"off"))setting="0";
 else if(!strcmp(argv[1],"legacy"))nvn2=0;
 else if(!strcmp(argv[1],"unbacked"))backed=0;
 else if(!strcmp(argv[1],"allocation"))converted=0;
 else if(!strcmp(argv[1],"skip"))draw_ok=0;
 else if(!strcmp(argv[1],"ab-off"))setting="ab";
 else if(!strcmp(argv[1],"ab-on")){setting="ab";ab=1;}
 else CHECK(!strcmp(argv[1],"good"));
 int result=gpu_quads_draw(&ex,&draw),accepted=!strcmp(argv[1],"good")||!strcmp(argv[1],"skip")||!strcmp(argv[1],"ab-on");
 CHECK(result==accepted);
 if(!accepted){CHECK(!reset && !prepares && !draws && bind_gen==9 && c0_binst==99 && !vk_exec_frame.quads_gpu);}
 else{CHECK(reset==1 && prepares==1 && bind_gen==10 && c0_binst==0 && vk_exec_frame.quads_gpu==1);
  CHECK(counts==1 && (ex.dirty&VK_EXEC_DIRTY_ALL)==VK_EXEC_DIRTY_ALL);
  CHECK(draws==(unsigned)draw_ok && indices==(unsigned)draw_ok && notes==(unsigned)draw_ok);
  CHECK(ex.recorded==(draw_ok?10:8));}
 puts("GPU QUADS scope checks passed");return 0;
}
'''
    return stages + prefix + function(text, "static int gpu_quads_graphics(") + function(text, "static int gpu_quads_draw(") + main


@unittest.skipUnless(os.name == "nt" and CC.is_file(), "Windows Clang required")
class GpuQuadScopeTests(unittest.TestCase):
    def test_abi_fallback_and_cache_restore(self):
        with tempfile.TemporaryDirectory(prefix="hwaoi-quad-scope-") as tmp:
            p = Path(tmp) / "scope.c"; p.write_text(source(), encoding="utf-8"); exe = p.with_suffix(".exe")
            for opt in ("-O0", "-O2"):
                result = subprocess.run([str(CC), opt, "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
                                         "-I", str(ROOT / "runtime"), "-I", str(DEPS), "-I", str(DEPS / "vma"),
                                         str(p), "-o", str(exe)], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                for case in ("good", "off", "legacy", "unbacked", "allocation", "skip", "ab-off", "ab-on"):
                    result = subprocess.run([str(exe), case], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=10)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
