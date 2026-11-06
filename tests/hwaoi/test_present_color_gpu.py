"""Opt-in swapchain readback regression using original, non-game colour fixtures.

Set HWAOI_PRESENT_TEST_EXE to the freshly built work/build executable. The owned
Windows runner bounds execution to 45 seconds and retains its validation log.
This test does not build, load a game, or run concurrently with a game capture.
"""
import os
from pathlib import Path
import re
import sys
import unittest
from unittest.mock import patch
from uuid import uuid4

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from hwaoi_probe import capture


@unittest.skipUnless(os.name == "nt" and os.environ.get("HWAOI_PRESENT_TEST_EXE"),
                     "set HWAOI_PRESENT_TEST_EXE for the local Vulkan presentation test")
class PresentColorGpuTests(unittest.TestCase):
    def test_encoded_swapchain_values(self):
        executable = Path(os.environ["HWAOI_PRESENT_TEST_EXE"])
        report = ROOT / "work/reports" / ("present-color-test-" + uuid4().hex[:12])
        with patch.dict(os.environ, {"HWAOI_VK_VALIDATION": "1"}):
            result = capture(executable, "--probe-present-color", report, 45)
        self.assertFalse(result["timed_out"], result)
        self.assertEqual(result["exit_code"], 0, result)
        output = Path(result["stdout"]).read_text(encoding="utf-8", errors="replace")
        errors = Path(result["stderr"]).read_text(encoding="utf-8", errors="replace")
        matched = re.search(r"probe-present-color: passed \(72 frames, (\d+) checked pixels, 0 validation errors", output)
        self.assertIsNotNone(matched, output + errors)
        self.assertGreater(int(matched[1]), 1_000_000)
        self.assertIn("FSR upscales", errors + output, "FSR coverage must not silently fall back to a blit")
        self.assertIn("validation layer and error reporting enabled", errors + output)
        self.assertNotIn("Validation Error", errors + output)


if __name__ == "__main__":
    unittest.main()
