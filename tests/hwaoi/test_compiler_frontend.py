"""Portable compiler integrity and output refusal paths; no game content needed."""
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from compiler import backend


class CompilerFrontendTests(unittest.TestCase):
    def bundle(self, root):
        names = ("python/python.exe", "llvm/bin/clang.exe", "bin/ninja.exe", "bin/zlib1.dll",
                 "translator/totk_xlate.exe", "translator/totk_xlate.id", "deps/include/vma/vk_mem_alloc.h",
                 "source/tools/hwaoi.py")
        files = {}
        for name in names:
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"owned synthetic tool fixture\n")
            files[name] = hashlib.sha256(path.read_bytes()).hexdigest()
        manifest = {"format": "hwaoi-compiler-v1", "source_revision": "synthetic", "files": files}
        (root / "bundle.json").write_text(json.dumps(manifest))
        return manifest

    def test_valid_bundle_tamper_missing_tool_and_path_escape(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "compiler"
            root.mkdir()
            manifest = self.bundle(root)
            self.assertEqual(backend.bundle_check(root)["source_revision"], "synthetic")
            (root / "bin/ninja.exe").write_bytes(b"different")
            with self.assertRaisesRegex(ValueError, "changed"):
                backend.bundle_check(root)
            backend.bundle_check(root, defer_materialized=True)  # unused compiler input is verified by synchronization
            (root / "source/tools/hwaoi.py").write_bytes(b"changed validator")
            with self.assertRaisesRegex(ValueError, "changed"):
                backend.bundle_check(root, defer_materialized=True)
            manifest = self.bundle(root)
            del manifest["files"]["bin/ninja.exe"]
            (root / "bundle.json").write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError, "Incomplete"):
                backend.bundle_check(root)
            manifest["files"]["../outside"] = "0" * 64
            (root / "bundle.json").write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError, "Missing"):
                backend.bundle_check(root)

    def test_output_never_overwrites_user_files_bundle_or_source(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            bundle = root / "compiler"
            bundle.mkdir()
            nup = root / "my-source.nup"
            nup.write_bytes(b"synthetic invalid input")
            occupied = root / "existing"
            occupied.mkdir()
            keep = occupied / "keep.txt"
            keep.write_text("keep me")
            for output in (occupied, bundle / "build", root):
                with self.subTest(output=output), self.assertRaises(ValueError):
                    backend.fresh_output(output, bundle, nup)
            self.assertEqual(keep.read_text(), "keep me")
            fresh = root / "fresh"
            with patch.object(backend.shutil, "disk_usage", return_value=type("Disk", (), {"free": 40 << 30})()):
                self.assertEqual(backend.fresh_output(fresh, bundle, nup), fresh.resolve())
            self.assertFalse(fresh.exists())
            with patch.object(backend.shutil, "disk_usage", return_value=type("Disk", (), {"free": 1 << 30})()):
                with self.assertRaisesRegex(ValueError, "20 GiB"):
                    backend.fresh_output(fresh, bundle, nup)

    def test_malformed_nup_is_read_only(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "bad.nup"
            data = b"not a supported game package" * 8
            path.write_bytes(data)
            with self.assertRaises(ValueError):
                backend.inspect_nup(path)
            self.assertEqual(path.read_bytes(), data)

    def test_host_environment_overrides_do_not_leak_into_user_build(self):
        with patch.dict(os.environ, {"HWAOI_FPS_LIMIT": "120", "TOTK_EXEFS": "wrong",
                                     "DKB_XLATE": "wrong", "PYTHONPATH": "wrong"}):
            env = backend.clean_environment(Path("X:/portable"))
            for key in ("HWAOI_FPS_LIMIT", "TOTK_EXEFS", "DKB_XLATE", "PYTHONPATH"):
                self.assertNotIn(key, env)
            self.assertNotIn("F:/Python", env["PATH"])


if __name__ == "__main__":
    unittest.main()
