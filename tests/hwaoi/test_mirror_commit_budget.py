"""Real mirror admission policy: low Windows commit, a page file that may still grow, explicit overrides and unknown
readings."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
CC = Path(os.environ.get("HWAOI_TOOLS", "F:") + "/llvm-mingw/bin/clang.exe")
sys.path.insert(0, str(ROOT / "tests"))
from nvn2_compute_c0_test import function


@unittest.skipUnless(os.name == "nt" and CC.is_file(), "Windows Clang required")
class MirrorCommitBudgetTests(unittest.TestCase):
    def test_low_commit_refusal_preserves_explicit_modes_and_other_targets(self):
        policy = function((ROOT / "runtime/vk_memory.c").read_text(encoding="utf-8"), "static int mirror_commit_room(")
        head = r'''
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <stdio.h>
static const char *mode="auto";
static double used,limit,growth;
static unsigned queries;
static const char *cfg_value(const char *key){assert(!strcmp(key,"TOTK_MEMORY"));return mode;}
static void plat_commit_info(double *p,double *u,double *l){*p=15555;*u=used;*l=limit;queries++;}
static double plat_commit_growth_mb(void){return growth;}
'''
        main = r'''
int main(void){
 double available;
 const uint64_t pool=4655ull<<20;
 used=10000;limit=17728;assert(mirror_commit_room(pool,&available));assert(available==7728);
 limit=17727;assert(mirror_commit_room(pool,&available)); // exact 4655+3072 MiB boundary
 limit=17726;assert(!mirror_commit_room(pool,&available));
 limit=48804;used=26349;assert(mirror_commit_room(pool,&available)); // 5070 Ti log has ample commit
 limit=36636;used=36332;assert(!mirror_commit_room(pool,&available));assert(available==304);
 used=limit+1;assert(!mirror_commit_room(0,&available) && available==0);
 limit=0;assert(mirror_commit_room(pool,&available) && available==-1);
 limit=NAN;assert(mirror_commit_room(pool,&available) && available==-1);
 limit=INFINITY;assert(mirror_commit_room(pool,&available) && available==-1);
 used=-1;limit=30000;assert(mirror_commit_room(pool,&available) && available==-1);
 used=10000;limit=20000;assert(!mirror_commit_room(UINT64_MAX,&available));
 // GitHub #15: 16 GB of RAM, 2482 MiB of commit left; a page file Windows manages may still grow, so it counts
 limit=19300;used=16818;growth=0;assert(!mirror_commit_room(pool,&available));assert(available==2482);
 growth=5244;assert(!mirror_commit_room(pool,&available));assert(available==7726);  // one MiB short
 growth=5245;assert(mirror_commit_room(pool,&available));assert(available==7727);
 growth=44000;assert(mirror_commit_room(pool,&available));
 growth=-1;assert(!mirror_commit_room(pool,&available));assert(available==2482);  // unknown: not counted
 growth=NAN;assert(!mirror_commit_room(pool,&available));assert(available==2482);
 growth=INFINITY;assert(!mirror_commit_room(pool,&available));assert(available==2482);
 used=limit+1;growth=8000;assert(mirror_commit_room(pool,&available));assert(available==8000);  // over the limit now
 growth=0;
 const char *overrides[]={"import","mirror","verify"};
 used=30000;limit=30001;
 for(unsigned i=0;i<3;i++){mode=overrides[i];unsigned before=queries;assert(mirror_commit_room(pool,&available));
  assert(queries==before && available==-1);}
 puts("mirror commit policy passed");return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="hwaoi-mirror-commit-") as tmp:
            folder = Path(tmp)
            source = folder / "fixture.c"
            source.write_text(head + policy + main, encoding="utf-8")
            for opt in ("-O0", "-O2"):
                exe = folder / "fixture.exe"
                result = subprocess.run([str(CC), opt, "-DRT_TARGET_HWAOI=1", "-Wall", "-Wextra", "-Werror",
                                         str(source), "-o", str(exe)], capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            source.write_text(head + policy + r'''
int main(void){double available;used=999;limit=1000;assert(mirror_commit_room(1ull<<40,&available));
 assert(!queries && available==-1);(void)cfg_value;(void)plat_commit_info;(void)plat_commit_growth_mb;(void)growth;
 return 0;}
''', encoding="utf-8")
            result = subprocess.run([str(CC), "-O2", "-DRT_TARGET_HWAOI=0", "-Wall", "-Wextra", "-Werror",
                                     str(source), "-o", str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(subprocess.run([str(exe)], timeout=10).returncode, 0)


if __name__ == "__main__":
    unittest.main()
