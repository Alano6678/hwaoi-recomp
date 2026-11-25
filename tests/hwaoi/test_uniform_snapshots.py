"""Real collector and immutable readout: packed/dense wire and owner bounds."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
CC=Path(os.environ.get('HWAOI_TOOLS','F:')+'/llvm-mingw/bin/clang.exe')
sys.path.insert(0,str(ROOT/'tests'))
from nvn2_compute_c0_test import function


def source(omit_normalization=False):
    module=(ROOT/'runtime/vk_usnap.c').read_text(encoding='utf-8')
    collector=function(module,'static void collect(')
    if omit_normalization:
        first=collector.index('        NvnOpScratch normalized;')
        last=collector.index('        if (h->op == OP_END)',first)
        collector=collector[:first]+'        const void *a=nvn_op_payload(h);\n'+collector[last:]
    functions='\n'.join([collector,*[function(module,name) for name in ('static int first_above(',
        'static int snapshot_span(', 'int vk_usnap_find(', 'int vk_usnap_read(')]])
    return r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "nvn_api.h"
typedef uint64_t VkBuffer;
typedef struct {VkBuffer buffer;uint64_t offset,range;} VkDescriptorBufferInfo;
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
#define ALIGN 256u
#define MAX_RANGE (64u<<10)
#define CALL_DEPTH 16
typedef struct {uint64_t lo,hi;} Rng;
typedef struct {uint64_t lo,hi,off;int dropped;} Reg;
typedef struct {VkBuffer buf;uint8_t *map;uint64_t size;} Arena;
static struct {int on;Arena *arena;Rng *col;int ncol,ccol;Reg *reg;int nreg;uint64_t hits;} U;
static void *grow(void *p,int *cap,size_t item){*cap=*cap?*cap*2:8;void *q=realloc(p,(size_t)*cap*item);CHECK(q);return q;}
void rt_fatal(const char *format,...){va_list ap;va_start(ap,format);vfprintf(stderr,format,ap);va_end(ap);exit(73);}
''' +functions+r'''
typedef union {uint64_t align;uint8_t bytes[128];} Packet;
static void short_binding(Packet *p,uint64_t addr,uint32_t size){
 memset(p,0,sizeof *p);op_hdr *h=(op_hdr*)p->bytes;h->op=OP_BIND_UBO;h->size=24;
 // Authored independent wire: packed VS/index0 tag, u32 extent, u64 address.
 uint32_t tag=0x80000000u;memcpy(p->bytes+8,&tag,4);memcpy(p->bytes+12,&size,4);memcpy(p->bytes+16,&addr,8);
 op_hdr *end=(op_hdr*)(p->bytes+24);end->op=OP_END;end->size=8;
}
static void collection(void){
 Packet short_packet;short_binding(&short_packet,0x10080,256);collect((op_hdr*)short_packet.bytes,0);
 CHECK(U.ncol==1&&U.col[0].lo==0x10000&&U.col[0].hi==0x10180);
 Packet dense={0};op_hdr *h=(op_hdr*)dense.bytes;h->op=OP_BIND_UBO;h->size=32;
 p_bind_buf b={0,0,0x10080,256};memcpy(h+1,&b,sizeof b);op_hdr *end=(op_hdr*)(dense.bytes+32);*end=(op_hdr){OP_END,8};
 collect(h,0);CHECK(U.ncol==2&&!memcmp(&U.col[0],&U.col[1],sizeof(Rng)));
 // The child is collected before the parent continuation; CALL/JUMP stay dense.
 Packet nested={0},jumped={0};h=(op_hdr*)nested.bytes;*h=(op_hdr){OP_CALL,16};((p_jump*)(h+1))->target=(uintptr_t)short_packet.bytes;
 end=(op_hdr*)(nested.bytes+16);*end=(op_hdr){OP_END,8};collect(h,0);CHECK(U.ncol==3&&U.col[2].hi==0x10180);
 h=(op_hdr*)jumped.bytes;*h=(op_hdr){OP_JUMP,16};((p_jump*)(h+1))->target=(uintptr_t)short_packet.bytes;
 collect(h,0);CHECK(U.ncol==4&&U.col[3].hi==0x10180);
 short_binding(&short_packet,0x20000,0);collect((op_hdr*)short_packet.bytes,0);CHECK(U.ncol==4);
 short_binding(&short_packet,0x20000,65537);collect((op_hdr*)short_packet.bytes,0);CHECK(U.ncol==5&&U.col[4].hi==0x30000);
 free(U.col);U.col=NULL;
}
static void immutable(void){
 uint8_t submitted[512],live[256],got[256];for(unsigned i=0;i<512;i++)submitted[i]=(uint8_t)(i*37);
 memcpy(live,submitted+128,256);Arena arena={.buf=42,.map=submitted,.size=sizeof submitted};Reg reg={0x10000,0x10100,128,0};
 U.on=1;U.arena=&arena;U.reg=&reg;U.nreg=1;memset(live,0xee,sizeof live);
 CHECK(vk_usnap_read(0x10000,got,sizeof got)&&!memcmp(got,submitted+128,sizeof got)&&memcmp(got,live,sizeof got));
 CHECK(vk_usnap_read(0x10007,got,17)&&!memcmp(got,submitted+135,17));
 VkDescriptorBufferInfo descriptor;CHECK(vk_usnap_find(0x10000,64,64,&descriptor));
 CHECK(descriptor.buffer==42&&descriptor.offset==128&&descriptor.range==64);
 CHECK(!vk_usnap_find(0x10000,64,0,&descriptor));CHECK(!vk_usnap_find(0x10001,16,64,&descriptor));
 const uint64_t bad[]={0xffff,0x10100,UINT64_MAX-7};
 for(unsigned i=0;i<3;i++){memset(got,0xa6,sizeof got);CHECK(!vk_usnap_read(bad[i],got,16)&&got[0]==0xa6);CHECK(!vk_usnap_find(bad[i],16,1,&descriptor));}
 CHECK(!vk_usnap_read(0x100f0,got,17)&&!vk_usnap_read(0x10000,NULL,16)&&!vk_usnap_read(0x10000,got,0));
 reg.dropped=1;CHECK(!vk_usnap_read(0x10000,got,16)&&!vk_usnap_find(0x10000,16,1,&descriptor));reg.dropped=0;
 reg.off=UINT64_MAX-10;CHECK(!vk_usnap_read(0x10020,got,16));reg.off=500;CHECK(!vk_usnap_read(0x10000,got,16));reg.off=128;
 U.on=0;U.arena=(Arena*)(uintptr_t)1;CHECK(!vk_usnap_read(0x10000,got,16)&&!vk_usnap_find(0x10000,16,1,&descriptor));
 U.on=1;U.arena=NULL;CHECK(!vk_usnap_read(0x10000,got,16));
}
int main(int argc,char **argv){
 if(argc>1){Packet p;short_binding(&p,UINT64_MAX-7,16);
  if(!strcmp(argv[1],"malformed")){uint32_t tag=0;memcpy(p.bytes+8,&tag,4);}
  collect((op_hdr*)p.bytes,0);return 2;}
 collection();immutable();puts("packed/dense uniform collection and immutable snapshot lifetime/bounds passed");return 0;
}
'''


@unittest.skipUnless(os.name=='nt' and CC.is_file(),'Windows Clang required')
class UniformSnapshotTests(unittest.TestCase):
    def test_real_collector_and_immutable_readout(self):
        with tempfile.TemporaryDirectory(prefix='hwaoi-usnap-') as directory:
            path=Path(directory)/'fixture.c';exe=path.with_suffix('.exe')
            for opt in ('-O0','-O2'):
                for omission in (False,True):
                    path.write_text(source(omission),encoding='utf-8')
                    built=subprocess.run([str(CC),opt,'-Wall','-Wextra','-Werror','-Wno-unused-function',
                        '-Wno-missing-field-initializers','-I',str(ROOT/'runtime'),str(path),'-o',str(exe)],
                        capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=60)
                    self.assertEqual(built.returncode,0,built.stdout+built.stderr)
                    checked=subprocess.run([str(exe)],capture_output=True,text=True,timeout=20)
                    self.assertEqual(checked.returncode,1 if omission else 0,checked.stdout+checked.stderr)
                    if omission:self.assertIn('U.ncol==1',checked.stderr);continue
                    for mode in ('overflow','malformed'):
                        checked=subprocess.run([str(exe),mode],capture_output=True,text=True,timeout=20)
                        self.assertEqual(checked.returncode,73,checked.stdout+checked.stderr)


if __name__=='__main__':unittest.main()
