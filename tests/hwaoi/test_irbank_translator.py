"""Original synthetic IR-bank tests for the optional locally built translator."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import build_xlate_hwaoi as builder

DEPLOYMENT = ROOT / "work/build/hwaoi196608/xlate-managed-irbanks"
EXECUTABLE = DEPLOYMENT / "totk_xlate.exe"


class IrBankPatchTests(unittest.TestCase):
    def test_extra_patch_separate_from_upstream_selection(self):
        self.assertEqual(len(list((ROOT / "tools/xlate/patches").glob("*.patch"))), 16)
        self.assertEqual(len(builder.upstream.selected_patches()), 15)
        self.assertEqual(len(builder.upstream.selected_patches(defined_float_minmax=True)), 16)
        self.assertTrue(any(p.name == "0001-private-register-banks.patch" for p in builder.hwaoi_patches()))


@unittest.skipUnless(os.name == "nt" and EXECUTABLE.is_file(), "private managed translator not built locally")
class IrBankNativeTranslatorTests(unittest.TestCase):
    def test_deployment_identifies_the_exact_extra_patch(self):
        report = json.loads((DEPLOYMENT / "build-report.json").read_text(encoding="utf-8"))
        self.assertEqual(EXECUTABLE.with_suffix(".id").read_text().strip(), report["identity"])
        recorded = {entry["name"]: entry["sha256"] for entry in report["inputs"]["hwaoi_patches"]}
        for patch in builder.hwaoi_patches():
            self.assertEqual(recorded[patch.name], hashlib.sha256(patch.read_bytes()).hexdigest())

    def test_ir_capacity_zero_register_live_masks_and_phi_separation(self):
        result = subprocess.run([str(EXECUTABLE), "--probe-hwaoi-ir-banks"], capture_output=True,
                                text=True, encoding="utf-8", errors="replace", timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for expected in ("passed", "RZ255", "GPR511/767", "12 live masks", "SSA phis"):
            self.assertIn(expected, result.stdout)

    def test_private_move_gating_and_constant_shader_equivalence(self):
        base = ROOT / "work/build/hwaoi196608"
        with tempfile.TemporaryDirectory(prefix="irbank-test-", dir=base) as temporary:
            proof = builder.smoke_ir_banks(EXECUTABLE, Path(temporary), dict(os.environ), None)
        report = json.loads((DEPLOYMENT / "build-report.json").read_text(encoding="utf-8"))
        self.assertEqual(proof["spv_sha256"], report["smoke"]["artifacts"]["synthetic_constant"]["spv_sha256"])
        self.assertTrue(proof["replies"][0].startswith("fail denied "))
        self.assertTrue(proof["replies"][2].startswith("fail wrong_version "))


if __name__ == "__main__":
    unittest.main()
