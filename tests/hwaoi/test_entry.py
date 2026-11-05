"""Synthetic fork-entry tests; no game payloads required."""
import contextlib
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import hwaoi


class EntryTests(unittest.TestCase):
    def npdm(self, path, title=0x04003F40232DE000):
        data = bytearray(0xC0)
        data[:4] = b"META"
        struct.pack_into("<II", data, 0x70, 0x80, 0x40)
        data[0x80:0x84] = b"ACI0"
        struct.pack_into("<Q", data, 0x90, title)
        path.write_bytes(data)
        return data

    def test_npdm_identity(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "main.npdm"
            self.npdm(path)
            self.assertEqual(hwaoi.program_id(path), "04003F40232DE000")

    def test_npdm_bounds(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "main.npdm"
            data = self.npdm(path)
            struct.pack_into("<II", data, 0x70, 0xB0, 0x40)
            path.write_bytes(data)
            with self.assertRaisesRegex(ValueError, "outside"):
                hwaoi.program_id(path)

    def test_npdm_magic(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "main.npdm"
            path.write_bytes(bytes(0xC0))
            with self.assertRaisesRegex(ValueError, "header"):
                hwaoi.program_id(path)

    def test_output_boundary(self):
        self.assertEqual(hwaoi.local_output(ROOT / "work/generated/test", "generated"),
                         ROOT / "work/generated/test")
        for path in (ROOT, ROOT / "work/generated", ROOT / "work/generated/../../runtime"):
            with self.assertRaises(ValueError):
                hwaoi.local_output(path, "generated")

    def test_unmarked_generation_refused(self):
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(ValueError, "unmarked"):
                hwaoi.prepare_generated(Path(temp), {"exefs_sha256": {"main": "a"}})

    def test_changed_input_refused(self):
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp) / "fresh"
            hwaoi.prepare_generated(out, {"exefs_sha256": {"main": "a"}})
            with self.assertRaisesRegex(ValueError, "another input"):
                hwaoi.prepare_generated(out, {"exefs_sha256": {"main": "b"}})

    def test_verify_failure_cannot_start_generator(self):
        with patch.object(hwaoi, "verify", side_effect=ValueError("wrong game")), patch.object(hwaoi, "run") as run:
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(hwaoi.main(["generate"]), 2)
            run.assert_not_called()

    def test_game_identity_and_hacks_are_separate(self):
        target = (ROOT / "runtime/game_target.h").read_text()
        self.assertIn("RT_TARGET_BANANZA 0", target)
        self.assertIn("0x04003f40232de000ull", target)
        self.assertIn("HWAOI_ALLOW_EXPERIMENTAL_BOOT 0", target)
        draw = (ROOT / "runtime/vk_exec_draw.c").read_text()
        self.assertIn('RT_TARGET_BANANZA && cfg_bool("TOTK_FIX_VS_FAULT")', draw)
        self.assertIn('RT_TARGET_BANANZA && cfg_bool("TOTK_FIX_INDIRECT_RESET")', draw)


if __name__ == "__main__":
    unittest.main()
