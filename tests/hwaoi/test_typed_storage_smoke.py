"""Builder failure-path checks; no .NET build, translator or GPU is executed."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
SPEC = importlib.util.spec_from_file_location("typed_storage_builder", ROOT / "tools/build_xlate_hwaoi.py")
builder = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(builder)


class TypedStorageSmoke(unittest.TestCase):
    def run_probe(self, stdout, code=0):
        output = ROOT / "build/test/typed-storage-smoke"
        output.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=output) as directory:
            directory = Path(directory).resolve()
            self.assertTrue(directory.is_relative_to(output.resolve()))
            result = subprocess.CompletedProcess(["inert-translator"], code, stdout, "diagnostic\n")
            with patch.object(builder.subprocess, "run", return_value=result) as run:
                report = builder.smoke_typed_storage_address(Path("inert-translator"), directory, {})
            self.assertEqual(run.call_args.args[0], ["inert-translator", "--probe-hwaoi-typed-storage-address"])
            self.assertEqual((directory / "typed-storage-address-probe.log").read_text(), stdout + "diagnostic\n")
            return report

    def test_accepts_completed_independent_probe(self):
        report = self.run_probe("typed storage address probe: 8212 checks\n")
        self.assertEqual(report["checks"], 8212)
        self.assertFalse(report["gpu_execution"])

    def test_nonzero_exit_cannot_publish_success(self):
        with self.assertRaisesRegex(ValueError, "semantic/provenance probe failed"):
            self.run_probe("typed storage address probe: 8212 checks\n", 1)

    def test_missing_arithmetic_checks_cannot_publish_success(self):
        with self.assertRaisesRegex(ValueError, "semantic/provenance probe failed"):
            self.run_probe("typed storage address probe: 8191 checks\n")

    def test_unknown_or_malformed_probe_cannot_publish_success(self):
        for output in ("", "ok\n", "typed storage address probe: all checks\n",
                       "typed storage address probe: 9000 checks\nunexpected output\n"):
            with self.subTest(output=output), self.assertRaisesRegex(ValueError, "semantic/provenance probe failed"):
                self.run_probe(output)


if __name__ == "__main__":
    unittest.main()
