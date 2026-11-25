"""Compile the production driver classifier and recorder entry point against synthetic GPU properties.

No Vulkan device, game input or recorder worker is created. The real functions decide whether the recorder thread
is started; the fixture counts that and rejects accidental policy changes: off on AMD's own driver for every card
generation, on everywhere else, and vk_recorder = 0 or 1 wins on every driver.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(text, signature):
    start = text.index(signature)
    opening = text.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def source():
    caps = (ROOT / "runtime/vk_caps.c").read_text(encoding="utf-8")
    rec = (ROOT / "runtime/vk_rec.c").read_text(encoding="utf-8")
    header = (ROOT / "runtime/vk.h").read_text(encoding="utf-8")
    end = header.index("} VkCaps;") + len("} VkCaps;")
    definition = header[header.rfind("typedef struct {", 0, end):end]
    host = (ROOT / "runtime/host.h").read_text(encoding="utf-8")
    knob_macro = next(line for line in host.splitlines() if line.startswith("#define RT_KNOB_ON("))
    return r'''
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
''' + definition + r'''
VkCaps vk_caps;
static uint32_t vendor, device;
static VkDriverId driver;
static const char *driver_info, *override;
static int selected, thread_calls;
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice p, VkPhysicalDeviceProperties2 *out) {
    (void)p;
    out->properties.vendorID = vendor;
    out->properties.deviceID = device;
    VkPhysicalDeviceDriverProperties *props = out->pNext;
    props->driverID = driver;
    snprintf(props->driverInfo, sizeof(props->driverInfo), "%s", driver_info);
}
#define RT_GAME_INI "hwaoi.ini"
static int rt_knob(const char *key, const char *fallback) {
    (void)key;
    selected = atoi(override && *override ? override : fallback);
    return selected;
}
// The settings table's default for TOTK_VK_RECORDER is "auto" (cfg.c).
static const char *cfg_value(const char *key) { (void)key; return override && *override ? override : "auto"; }
// rec_begin also reads how long the recorder polls before it sleeps (TOTK_REC_SPIN_US): 0 here, the choice of the
// recorder itself is what is under test.
static int cfg_int(const char *key) { (void)key; return 0; }
static uint32_t plat_cpu_usable(void) { return 8; }
static uint64_t rec_spin_ns;
static void rt_log(const char *format, ...) { (void)format; }
static int rt_ab(void) { return 1; }
typedef struct { int unused; } PlatEvent;
typedef struct { int unused; } PlatThread;
static PlatEvent wake_ev;
static PlatThread *rec_thread;
static int rec_prio, rec_live;
static VkCommandBuffer rec_cb;
static struct { int parts, parts_on; } RS_;
static void plat_event_init(PlatEvent *e, int manual, int signalled) { (void)e; (void)manual; (void)signalled; }
static int plat_thread_priority_get(void) { return 0; }
static void *rec_main(void *arg) { return arg; }
static PlatThread *plat_thread_new(void *(*fn)(void *), void *arg, const char *name) {
    (void)fn; (void)arg; (void)name;
    thread_calls++;
    return NULL;  // no worker is needed to observe the production default and explicit override
}
''' + knob_macro + "\n" + function(caps, "void vk_caps_driver(") + "\n" + function(rec, "void rec_begin(") + r'''
int main(int argc, char **argv) {
    if (argc != 6) return 2;
    vendor = (uint32_t)strtoul(argv[1], NULL, 0);
    device = (uint32_t)strtoul(argv[2], NULL, 0);
    driver = !strcmp(argv[3], "amd") ? VK_DRIVER_ID_AMD_PROPRIETARY : VK_DRIVER_ID_MESA_RADV;
    driver_info = argv[4];
    override = argv[5];
    vk_caps_driver(VK_NULL_HANDLE);
    rec_begin(VK_NULL_HANDLE);
    if (rec_prio || rec_live || rec_cb) return 3;
    printf("%d %d %d\n", vk_caps.amd_own, selected, thread_calls);
    return 0;
}
'''


class AmdRecorderDefaults(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="aod-amd-recorder-")
        folder = Path(cls.tmp.name)
        c = folder / "policy.c"
        c.write_text(source(), encoding="utf-8")
        cls.exe = folder / ("policy.exe" if os.name == "nt" else "policy")
        tools = Path(os.environ.get("HWAOI_TOOLS", "F:"))
        cc = os.environ.get("AOD_TEST_CC") or (str(tools / "llvm-mingw/bin/clang.exe") if os.name == "nt" else shutil.which("clang"))
        if not cc:
            raise RuntimeError("Set AOD_TEST_CC to clang and provide Vulkan headers")
        command = [cc, "-O2", "-Wall", "-Werror", str(c), "-o", str(cls.exe)]
        if os.environ.get("AOD_TEST_INCLUDE"):
            command += ["-I", os.environ["AOD_TEST_INCLUDE"]]
        subprocess.run(command, check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def check_gpu(self, device, own, vendor="0x1002", driver="amd", info="26.11.2 LLPC"):
        """own: the card is on AMD's own driver, where the recorder thread stays off unless it is asked for."""
        for override in ("", "auto", "0", "1"):
            with self.subTest(device=device, vendor=vendor, driver=driver, info=info, override=override):
                output = subprocess.check_output([str(self.exe), vendor, device, driver, info, override], text=True)
                classified, selected, threads = map(int, output.split())
                asked = override in ("0", "1")
                self.assertEqual(classified, own)
                self.assertEqual(threads, int(override) if asked else 1 - own)
                if asked:   # only a forced value goes through rt_knob
                    self.assertEqual(selected, int(override))

    def test_amd_own_driver_has_no_recorder_on_any_generation(self):
        # Discrete RDNA 3 (Navi 31, 32, 33), RDNA 4 (RX 9070 XT, RX 9060 XT), an RDNA 3 handheld, an RDNA 2 laptop chip.
        for device in ("0x7440", "0x744c", "0x747e", "0x7480", "0x749f", "0x7550", "0x7590", "0x15bf", "0x1681"):
            self.check_gpu(device, 1)
        # RDNA 1 and 2 cards are on the driver's other branch: the same rule.
        self.check_gpu("0x73df", 1, info="26.11.2 (AMD proprietary shader compiler)")

    def test_other_vendors_and_drivers_keep_the_recorder(self):
        self.check_gpu("0x2c05", 0, vendor="0x10de")
        self.check_gpu("0x747e", 0, vendor="0x10de")
        self.check_gpu("0x747e", 0, vendor="0x8086")
        for device in ("0x747e", "0x7550", "0x15bf"):
            self.check_gpu(device, 0, driver="radv")


if __name__ == "__main__":
    unittest.main()
