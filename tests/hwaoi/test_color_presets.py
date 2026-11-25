"""The picture's colour adjustment follows a named profile even when only its name is in hwaoi.ini."""
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
class ColorPresetTests(unittest.TestCase):
    def test_named_profiles_set_the_four_values(self):
        adjusting = function((ROOT / "runtime/vk_color.c").read_text(encoding="utf-8"), "static int adjusting(")
        source = r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "color_presets.h"
static struct { unsigned seen; float adj[4]; } C;
static unsigned version = 1;
static const char *preset, *values[4];
static unsigned cfg_version(void) { return version; }
static const char *cfg_value(const char *key) { assert(!strcmp(key, "TOTK_COLOR_PRESET")); return preset ? preset : "default"; }
static double cfg_double(const char *key) {
    static const char *const keys[] = {"TOTK_COLOR_BRIGHTNESS", "TOTK_COLOR_CONTRAST", "TOTK_COLOR_SATURATION", "TOTK_COLOR_GAMMA"};
    for (int i = 0; i < 4; i++) if (!strcmp(key, keys[i])) return strtod(values[i], NULL);
    assert(0); return 0;
}
''' + adjusting + r'''
static void expect(const char *p, float b, float c, float s, float g, int on) {
    preset = p; version++;
    assert(adjusting() == on);
    assert(fabsf(C.adj[0] - b) < 1e-6f && fabsf(C.adj[1] - c) < 1e-6f && fabsf(C.adj[2] - s) < 1e-6f && fabsf(C.adj[3] - g) < 1e-6f);
}
int main(void) {
    values[0] = "10"; values[1] = "120"; values[2] = "80"; values[3] = "1.1";   // the player's own (custom) values
    expect("vivid", 0.0f, 1.15f, 1.35f, 0.95f, 1);    // only the name in the file: the profile decides
    expect("clarity", 0.02f, 1.10f, 1.15f, 1.0f, 1);
    expect("default", 0.0f, 1.0f, 1.0f, 1.0f, 0);     // default leaves the picture unchanged
    expect(NULL, 0.0f, 1.0f, 1.0f, 1.0f, 0);          // unset is the default profile
    expect("custom", 0.10f, 1.20f, 0.80f, 1.1f, 1);   // custom: the four values as set
    expect("unknown", 0.10f, 1.20f, 0.80f, 1.1f, 1);  // another name: treated as custom
    assert(color_preset_find("vivid") && !color_preset_find("custom") && !color_preset_find(NULL));
    puts("colour profiles passed");
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="hwaoi-color-") as temp:
            p = Path(temp) / "fixture.c"
            p.write_text(source, encoding="utf-8")
            for opt in ("-O0", "-O2"):
                exe = Path(temp) / "fixture.exe"
                build = subprocess.run([str(CC), opt, "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "runtime"),
                                        str(p), "-o", str(exe)], capture_output=True, text=True)
                self.assertEqual(build.returncode, 0, build.stderr)
                run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=5)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
