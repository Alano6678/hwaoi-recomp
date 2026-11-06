"""Original rate-period cases; gameplay speed remains a separate runtime check."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
CC=Path("F:/llvm-mingw/bin/clang.exe")
sys.path.insert(0,str(ROOT/"tests"))
from nvn2_compute_c0_test import function


@unittest.skipUnless(os.name=="nt" and CC.is_file(),"Windows Clang required")
class HwaoiRateTests(unittest.TestCase):
    def test_actual_present_policy_and_default_behavior(self):
        policy=function((ROOT/"runtime/nvn_api_obj.c").read_text(encoding="utf-8"),"static uint64_t present_period(")
        source=r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "hwaoi_rate.h"
#define RT_TARGET_HWAOI 1
#define VSYNC_HZ 60ull
typedef struct {int interval;} win_t;
static int experiment,realtime;static const char *value;
static const char *cfg_get(const char *key){
 if(!strcmp(key,"TOTK_EXPERIMENTAL_RATE"))return experiment ? "1" : NULL;
 assert(!strcmp(key,"TOTK_FPS_LIMIT"));return value;
}
static int frame_step_realtime(void){return realtime;}
static uint64_t frame_step_cap_ns(void){return 1234567;}
static int64_t plat_xchg64(volatile int64_t *p,int64_t v){int64_t old=*p;*p=v;return old;}
static void rt_fatal(const char *fmt,...){(void)fmt;exit(73);}
static void rt_log(const char *fmt,...){(void)fmt;}
''' + policy + r'''
int main(int argc,char **argv){
 assert(argc==2);win_t window={2};
 if(!strcmp(argv[1],"bad")){experiment=1;value="144";(void)present_period(&window);return 1;}
 for(int i=-1;i<=4;i++){window.interval=i;assert(present_period(&window)==(uint64_t)(i>0?i:1)*1000000000/60);}
 realtime=1;assert(present_period(&window)==1234567); // inherited target behavior is untouched by default
 experiment=1;
 const char *rates[]={"30","60","90","120"};uint64_t periods[]={33333333,16666667,11111111,8333333};
 for(int i=0;i<4;i++){value=rates[i];assert(present_period(&window)==periods[i]);}
 value=NULL;assert(present_period(&window)==16666667);return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="hwaoi-present-policy-") as temp:
            p=Path(temp)/"fixture.c";p.write_text(source,encoding="utf-8")
            for opt in ("-O0","-O2"):
                exe=Path(temp)/"fixture.exe"
                build=subprocess.run([str(CC),opt,"-Wall","-Wextra","-Werror","-I",str(ROOT/"runtime"),
                    str(p),"-o",str(exe)],capture_output=True,text=True)
                self.assertEqual(build.returncode,0,build.stderr)
                for mode,expected in (("good",0),("bad",73)):
                    run=subprocess.run([str(exe),mode],capture_output=True,text=True,timeout=5)
                    self.assertEqual(run.returncode,expected,run.stdout+run.stderr)

    def test_periods_and_unsupported_modes(self):
        source=r'''
#include <assert.h>
#include <stdio.h>
#include "hwaoi_rate.h"
int main(void){
 uint64_t out=0;
 const char *rates[]={"30","60","90","120"};
 const uint64_t periods[]={33333333,16666667,11111111,8333333};
 for(int i=0;i<4;i++){assert(hwaoi_rate_period(rates[i],&out));assert(out==periods[i]);}
 assert(hwaoi_rate_period(NULL,&out) && out==periods[1]);
 const char *bad[]={"0","-60","75","90fps","120.0","144","display","999999999999999999999999999999"};
 for(unsigned i=0;i<sizeof bad/sizeof *bad;i++){out=42;assert(!hwaoi_rate_period(bad[i],&out));assert(out==42);}
 puts("rate period checks passed");return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="hwaoi-rate-") as temp:
            p=Path(temp)/"fixture.c";p.write_text(source,encoding="utf-8")
            for opt in ("-O0","-O2"):
                exe=Path(temp)/"fixture.exe"
                build=subprocess.run([str(CC),opt,"-Wall","-Wextra","-Werror","-I",str(ROOT/"runtime"),
                    str(p),"-o",str(exe)],capture_output=True,text=True)
                self.assertEqual(build.returncode,0,build.stderr)
                run=subprocess.run([str(exe)],capture_output=True,text=True,timeout=5)
                self.assertEqual(run.returncode,0,run.stdout+run.stderr)


if __name__=="__main__":unittest.main()
