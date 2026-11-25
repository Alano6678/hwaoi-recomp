"""Compile the production gamepad focus gate against deterministic host focus events."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class FocusGate(unittest.TestCase):
    def test_overlay_focus_and_held_buttons(self):
        source = (ROOT / "runtime/input.c").read_text(encoding="utf-8")
        start = source.index("static int pad_focus_gate(")
        brace = source.index("{", start)
        depth = 1
        end = brace + 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        production = source[start:end]
        fixture = r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#define INPUT_PLAYERS 2
static unsigned epoch;
static int foreground = 1, background;
static unsigned gfx_window_focus_messages(void) { return epoch; }
static int ikb_foreground(void) { return foreground; }
static bool SDL_GetHintBoolean(const char *name, bool fallback) { (void)name; (void)fallback; return background; }
''' + production + r'''
int main(void) {
    uint64_t b = 1; int32_t l[2] = {100, 200}, r[2] = {300, 400};
    assert(!pad_focus_gate(0, &b, l, r) && b == 1);
    foreground = 0; epoch++; b = 3;
    assert(pad_focus_gate(0, &b, l, r) && b == 0 && l[0] == 0 && r[1] == 0);
    foreground = 1; epoch++; b = 1;
    assert(pad_focus_gate(0, &b, l, r) && b == 0);
    b = 0; pad_focus_gate(0, &b, l, r);
    b = 1; pad_focus_gate(0, &b, l, r); assert(b == 1);
    epoch += 2; b = 8; // Focus lost/regained while the guest was paused.
    assert(pad_focus_gate(0, &b, l, r) && b == 0);
    b = 0; pad_focus_gate(0, &b, l, r);
    b = 8; pad_focus_gate(0, &b, l, r); assert(b == 8);
    b = 2; pad_focus_gate(1, &b, l, r); assert(b == 0);
    b = 0; pad_focus_gate(1, &b, l, r);
    b = 2; pad_focus_gate(1, &b, l, r); assert(b == 2);
    b = 8; pad_focus_gate(0, &b, l, r); assert(b == 8); // Players are independent.
    foreground = 0; background = 1; b = 4; l[0] = 900;
    assert(!pad_focus_gate(2, &b, l, r) && b == 4 && l[0] == 900);
    background = 0; b = 4; pad_focus_gate(3, &b, l, r); assert(b == 0);
    return 0;
}
'''
        compiler = os.environ.get("AOD_TEST_CC") or shutil.which("clang") or shutil.which("cc")
        self.assertIsNotNone(compiler, "Set AOD_TEST_CC to a C compiler")
        with tempfile.TemporaryDirectory(prefix="aod-focus-") as tmp:
            code = Path(tmp) / "test.c"
            exe = Path(tmp) / ("test.exe" if os.name == "nt" else "test")
            code.write_text(fixture, encoding="utf-8")
            subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", str(code), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
