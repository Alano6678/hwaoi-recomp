"""Synthetic NUP/cache/index tests, with a native storage-routing check."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests/prototype"))
from test_formats import make_nup, refresh_toc_hash, TITLE_ID
from hwaoi_romfs import prepare
from hwrecomp.nup import Nup
import romfs_index


class RomfsTests(unittest.TestCase):
    def fixture(self, directory):
        payloads = {"raw.bin": b"raw fixture bytes", "z.bin": b"compressed fixture" * 1024}
        raw, offsets = make_nup([{"files": [(n, d, int(n == "z.bin")) for n, d in payloads.items()]}])
        struct.pack_into("<I", raw, 0x80 + 0x30 + 0x10, 1)  # synthetic tree also serves as RomFS
        refresh_toc_hash(raw)
        path = directory / "fixture.nup"
        path.write_bytes(raw)
        expected = {"title_id": f"{TITLE_ID:016X}", "ncm_version": 0,
                    "exefs_sha256": {n: hashlib.sha256(d).hexdigest() for n, d in payloads.items()}}
        return path, expected, payloads

    def test_virtual_files_match_and_original_unchanged(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            path, expected, payloads = self.fixture(folder)
            original = path.read_bytes()
            out = folder / "romfs"
            result = prepare(path, out, expected, reserve_bytes=0)
            self.assertEqual(result["files"], 2)
            self.assertEqual(path.read_bytes(), original)
            manifest = json.loads((out / "index.json").read_text())
            cache = (out / "cache.bin").read_bytes()
            self.assertEqual(hashlib.sha256(cache).hexdigest(), manifest["cache_sha256"])
            for row in manifest["files"]:
                source, start = (original, row["offset"]) if row["storage"] == "nup" else (cache, row["offset"] - len(original))
                self.assertEqual(source[start:start + row["size"]], payloads[row["path"]])
            self.assertFalse((out / "romfs.bin").exists())
            built = folder / "build"
            count, _, _ = romfs_index.write(out, built)
            self.assertEqual(count, 2)
            self.assertEqual((built / "romfs-hashes.bin").stat().st_size, 64)
            self.assertIn("ROMFS_NUP_MODE 1", (built / "romfs_config.h").read_text())
            with self.assertRaisesRegex(ValueError, "exists"):
                prepare(path, out, expected, reserve_bytes=0)

    def test_wrong_exefs_prevents_cache_creation(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            path, expected, _ = self.fixture(folder)
            expected["exefs_sha256"]["raw.bin"] = "0" * 64
            with self.assertRaisesRegex(ValueError, "match"):
                prepare(path, folder / "bad", expected, reserve_bytes=0)
            self.assertFalse((folder / "bad").exists())

    def test_cache_size_mismatch_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            path, expected, _ = self.fixture(folder)
            out = folder / "romfs"
            prepare(path, out, expected, reserve_bytes=0)
            (out / "cache.bin").write_bytes(b"truncated")
            with self.assertRaises(SystemExit):
                romfs_index.write(out, folder / "build")

    def test_path_traversal_rejected(self):
        with self.assertRaises(SystemExit):
            romfs_index.build_index({"romfs_size": 10, "files": [{"path": "a/../b", "offset": 0, "size": 1}]})

    def test_native_storage_boundary_and_overflow(self):
        compiler = Path("F:/llvm-mingw/bin/clang.exe")
        if not compiler.is_file():
            self.skipTest("test Clang not installed")
        source = '''#include "romfs_storage.h"
#include <assert.h>
int main(void) {
 int c; uint64_t o; uint32_t n;
 assert(romfs_storage_route(100,20,90,30,&c,&o,&n) && !c && o==90 && n==10);
 assert(romfs_storage_route(100,20,105,30,&c,&o,&n) && c && o==5 && n==15);
 assert(romfs_storage_route(100,20,120,1,&c,&o,&n) && n==0);
 assert(!romfs_storage_route(100,20,121,1,&c,&o,&n));
 assert(!romfs_storage_route(UINT64_MAX,1,0,1,&c,&o,&n));
 assert(romfs_storage_route(UINT64_C(50000000000),20,UINT64_C(50000000001),1,&c,&o,&n) && c && o==1);
 return 0;
}'''
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "check.c"
            path.write_text(source)
            exe = Path(temp) / "check.exe"
            subprocess.run([str(compiler), "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "runtime"), str(path), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
