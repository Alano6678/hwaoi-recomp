"""Original synthetic fixtures for the bounded package/module readers.

The fixtures contain no game data. These tests do not execute guest code.
"""
from __future__ import annotations

import hashlib
from pathlib import Path
import struct
import tempfile
import unittest

import lz4.block
import zstandard

from hwrecomp.nso import Nso
from hwrecomp.nup import ABSENT, FormatError, Nup, extract_exefs, safe_relative


TITLE_ID = 0x0400000000001000


def align16(value):
    return (value + 15) & ~15


def make_nup(packages=None):
    """Build simple two-level package trees; return bytes plus mutation offsets."""
    if packages is None:
        packages = [{"files": [("main", b"synthetic main bytes", 0)]}]
    directory_count = len(packages) * 2
    file_specs = [f for package in packages for f in package["files"]]
    file_count = len(file_specs)
    child_count = len(packages) + file_count
    dirs_off = 0x30 + 0x40 * len(packages)
    files_off = dirs_off + 0x18 * directory_count
    children_off = files_off + 0x50 * file_count
    pool_off = children_off + 4 * child_count
    pool = bytearray()

    def add_name(value):
        encoded = value.encode("utf-8")
        result = (len(pool), len(encoded))
        pool.extend(encoded)
        return result

    dir_names = []
    for i in range(len(packages)):
        dir_names.extend([add_name(f"pkg{i}"), add_name("exefs")])
    file_names = [add_name(name) for name, _, _ in file_specs]
    toc = bytearray(pool_off + len(pool))
    struct.pack_into("<4sIQQQQQ", toc, 0, b"TOC\x01", len(packages),
                     directory_count, file_count, pool_off, len(pool), 0)
    toc[pool_off:] = pool
    blob_offset = align16(0x80 + len(toc))
    blob = bytearray()
    file_offsets = []
    file_index = 0
    child_index = 0
    uuid_seed = bytearray()
    for i, package in enumerate(packages):
        title = package.get("title_id", TITLE_ID)
        kind = package.get("kind", 0)
        version = package.get("version", 0)
        flags = package.get("flags", 0)
        struct.pack_into("<QIIIIBBHIQI", toc, 0x30 + i * 0x40,
                         title, i * 2, i * 2 + 1, ABSENT, version,
                         kind, flags, 0, 0, 0, 0)
        uuid_seed.extend(struct.pack("<QIB", title, version, kind))
        struct.pack_into("<IIIIQ", toc, dirs_off + i * 2 * 0x18,
                         *dir_names[i * 2], child_index, 1, 0)
        struct.pack_into("<I", toc, children_off + child_index * 4, i * 2 + 1)
        child_index += 1
        struct.pack_into("<IIIIQ", toc, dirs_off + (i * 2 + 1) * 0x18,
                         *dir_names[i * 2 + 1], child_index, len(package["files"]), 0)
        for name, payload, codec in package["files"]:
            stored = (zstandard.ZstdCompressor(write_content_size=True).compress(payload)
                      if codec == 1 else payload)
            if not payload:
                codec, stored, extent = 0, b"", 0
            else:
                padding = align16(len(blob)) - len(blob)
                blob.extend(bytes(padding))
                extent = blob_offset + len(blob)
                blob.extend(stored)
            struct.pack_into("<IIQQQ", toc, files_off + file_index * 0x50,
                             *file_names[file_index], len(payload), extent, len(stored))
            toc[files_off + file_index * 0x50 + 0x20:
                files_off + file_index * 0x50 + 0x40] = hashlib.sha256(payload).digest()
            toc[files_off + file_index * 0x50 + 0x40] = codec
            struct.pack_into("<I", toc, children_off + child_index * 4,
                             directory_count + file_index)
            file_offsets.append(extent)
            file_index += 1
            child_index += 1
    header = bytearray(0x80)
    struct.pack_into("<4sHHQQ", header, 0, b"NUP\x01", 0x80, 1, 0x80, len(toc))
    header[0x18:0x38] = hashlib.sha256(toc).digest()
    struct.pack_into("<QQII", header, 0x38, blob_offset, blob_offset + len(blob), len(packages), 0)
    header[0x50:0x60] = hashlib.sha256(uuid_seed).digest()[:16]
    raw = header + toc + bytes(blob_offset - 0x80 - len(toc)) + blob
    return raw, {"toc": 0x80, "dirs": 0x80 + dirs_off, "files": 0x80 + files_off,
                 "children": 0x80 + children_off, "pool": 0x80 + pool_off,
                 "extents": file_offsets}


def refresh_toc_hash(raw):
    toc_offset, toc_size = struct.unpack_from("<QQ", raw, 8)
    raw[0x18:0x38] = hashlib.sha256(raw[toc_offset:toc_offset + toc_size]).digest()


def make_nso(*, compressed=False, checked=True):
    """Header + invented aligned code words + plain synthetic rodata/data."""
    payloads = [struct.pack("<4I", 1, 2, 3, 4), b"fixture rodata", b"fixture data"]
    memory_offsets = [0, 0x1000, 0x2000]
    header = bytearray(0x100)
    header[:4] = b"NSO0"
    flags = (0x07 if compressed else 0) | (0x38 if checked else 0)
    struct.pack_into("<III", header, 4, 0, 0, flags)
    header[0x40:0x60] = bytes(range(32))
    struct.pack_into("<I", header, 0x3C, 32)
    contents = bytearray()
    for i, payload in enumerate(payloads):
        stored = lz4.block.compress(payload, store_size=False) if compressed else payload
        struct.pack_into("<III", header, 0x10 + i * 16,
                         0x100 + len(contents), memory_offsets[i], len(payload))
        struct.pack_into("<I", header, 0x60 + i * 4, len(stored))
        header[0xA0 + i * 32:0xC0 + i * 32] = hashlib.sha256(payload).digest()
        contents.extend(stored)
    return header + contents, payloads


class FixtureTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def write(self, raw, name="input.bin"):
        path = self.root / name
        path.write_bytes(raw)
        return path


class NupTests(FixtureTest):
    def test_minimal_raw_inventory_and_read(self):
        raw, _ = make_nup()
        with Nup(self.write(raw)) as archive:
            self.assertEqual(archive.file_size, len(raw))
            self.assertEqual(archive.packages[0].title_id, TITLE_ID)
            self.assertEqual([path for path, _ in archive.walk(0)], ["exefs/main"])
            selected, members = archive.exefs()
            self.assertEqual([p.index for p in selected], [0])
            self.assertEqual(archive.read_member(members["main"][1]), b"synthetic main bytes")
            self.assertEqual(archive.summary()["file_count"], 1)

    def test_zstd_extent(self):
        payload = b"compressible synthetic payload\n" * 200
        raw, _ = make_nup([{"files": [("main", payload, 1)]}])
        with Nup(self.write(raw)) as archive:
            member = archive.files[0]
            self.assertLess(member.stored_size, member.size)
            self.assertEqual(archive.read_member(member), payload)

    def test_empty_extent(self):
        raw, _ = make_nup([{"files": [("empty", b"", 0)]}])
        with Nup(self.write(raw)) as archive:
            self.assertEqual(archive.read_member(archive.files[0]), b"")

    def test_toc_hash_mismatch(self):
        raw, offsets = make_nup()
        raw[offsets["pool"]] ^= 1
        with self.assertRaisesRegex(FormatError, "TOC SHA-256"):
            Nup(self.write(raw))

    def test_declared_file_size_mismatch(self):
        raw, _ = make_nup()
        struct.pack_into("<Q", raw, 0x40, len(raw) + 1)
        with self.assertRaisesRegex(FormatError, "file size mismatch"):
            Nup(self.write(raw))

    def test_extent_out_of_bounds(self):
        raw, offsets = make_nup()
        struct.pack_into("<Q", raw, offsets["files"] + 0x10, align16(len(raw)) + 16)
        refresh_toc_hash(raw)
        with self.assertRaisesRegex(FormatError, "file extent.*outside"):
            Nup(self.write(raw))

    def test_toc_table_out_of_bounds(self):
        raw, offsets = make_nup()
        struct.pack_into("<Q", raw, offsets["toc"] + 0x10, 10**9)
        refresh_toc_hash(raw)
        with self.assertRaisesRegex(FormatError, "tables overlap"):
            Nup(self.write(raw))

    def test_directory_cycle(self):
        raw, offsets = make_nup()
        struct.pack_into("<I", raw, offsets["children"], 0)
        refresh_toc_hash(raw)
        with self.assertRaisesRegex(FormatError, "cycle"):
            Nup(self.write(raw))

    def test_traversal_names_rejected(self):
        for name in ("../escape", "..", "a/b", "a\\b", "."):
            with self.subTest(name=name):
                raw, _ = make_nup([{"files": [(name, b"fixture", 0)]}])
                with self.assertRaisesRegex(FormatError, "invalid child name"):
                    Nup(self.write(raw))

    def test_duplicate_child_nodes_rejected(self):
        raw, offsets = make_nup([{"files": [("main", b"a", 0), ("sdk", b"b", 0)]}])
        struct.pack_into("<I", raw, offsets["children"] + 8, 2)
        refresh_toc_hash(raw)
        with self.assertRaisesRegex(FormatError, "duplicate sibling"):
            Nup(self.write(raw))

    def test_unsorted_children_rejected(self):
        raw, _ = make_nup([{"files": [("sdk", b"a", 0), ("main", b"b", 0)]}])
        with self.assertRaisesRegex(FormatError, "unsorted"):
            Nup(self.write(raw))

    def test_payload_hash_checked_when_read_not_on_open(self):
        raw, offsets = make_nup()
        raw[offsets["extents"][0]] ^= 1
        with Nup(self.write(raw)) as archive:
            with self.assertRaisesRegex(FormatError, "member SHA-256"):
                archive.read_member(archive.files[0])

    def test_extract_fresh_directory_and_no_overwrite(self):
        raw, _ = make_nup()
        with Nup(self.write(raw)) as archive:
            output = self.root / "extracted"
            report = extract_exefs(archive, output)
            self.assertEqual(report["selected_packages"], [0])
            self.assertEqual((output / "main").read_bytes(), b"synthetic main bytes")
            (output / "main").write_bytes(b"keep existing file")
            with self.assertRaisesRegex(FormatError, "directory exists"):
                extract_exefs(archive, output)
            self.assertEqual((output / "main").read_bytes(), b"keep existing file")

    def test_extract_windows_unsafe_names(self):
        for name in ("C:escape", "NUL", "COM1.txt", "CONIN$", "bad.", "bad ", "a?b"):
            with self.subTest(name=name):
                raw, _ = make_nup([{"files": [(name, b"fixture", 0)]}])
                with Nup(self.write(raw)) as archive:
                    output = self.root / "unsafe-output"
                    with self.assertRaises(FormatError):
                        extract_exefs(archive, output)
                    self.assertFalse(output.exists())

    def test_relative_path_rejects_absolute_and_traversal(self):
        for path in ("../outside", "/absolute", "C:/absolute", "a/../../outside", "a//b", "a/./b"):
            with self.subTest(path=path), self.assertRaises(FormatError):
                safe_relative(path)
        self.assertEqual(safe_relative("a/b"), Path("a") / "b")

    def test_extract_rejects_case_collisions(self):
        raw, _ = make_nup([{"files": [("Main", b"one", 0), ("main", b"two", 0)]}])
        with Nup(self.write(raw)) as archive:
            with self.assertRaisesRegex(FormatError, "case-insensitive"):
                extract_exefs(archive, self.root / "collision")
            self.assertFalse((self.root / "collision").exists())

    def test_latest_update_overlays_base_only(self):
        raw, _ = make_nup([
            {"files": [("main", b"base", 0), ("sdk", b"base sdk", 0)]},
            {"kind": 1, "version": 65536, "files": [("main", b"old update", 0)]},
            {"kind": 1, "title_id": TITLE_ID | 0x800, "version": 131072,
             "files": [("main", b"latest update", 0)]},
            {"kind": 1, "title_id": TITLE_ID + 0x10000, "version": 999999,
             "files": [("main", b"unrelated update", 0)]},
        ])
        with Nup(self.write(raw)) as archive:
            selected, members = archive.exefs()
            self.assertEqual([p.index for p in selected], [0, 2])
            self.assertEqual(archive.read_member(members["main"][1]), b"latest update")
            self.assertEqual(archive.read_member(members["sdk"][1]), b"base sdk")
            selected, members = archive.exefs(1)
            self.assertEqual([p.index for p in selected], [1])
            self.assertEqual(archive.read_member(members["main"][1]), b"old update")

    def test_ambiguous_latest_update_rejected(self):
        raw, _ = make_nup([
            {"files": [("main", b"base", 0)]},
            {"kind": 1, "version": 10, "files": [("main", b"a", 0)]},
            {"kind": 1, "version": 10, "files": [("main", b"b", 0)]},
        ])
        with Nup(self.write(raw)) as archive:
            with self.assertRaisesRegex(FormatError, "ambiguous update"):
                archive.exefs()

    def test_standalone_update_requires_explicit_selection(self):
        raw, _ = make_nup([{"kind": 1, "flags": 1, "version": 10,
                            "files": [("main", b"standalone", 0)]}])
        with Nup(self.write(raw)) as archive:
            with self.assertRaisesRegex(FormatError, "no base package"):
                archive.exefs()
            self.assertEqual(archive.exefs(0)[0][0].version, 10)

    def test_bounded_read(self):
        raw, _ = make_nup()
        with Nup(self.write(raw)) as archive:
            with self.assertRaisesRegex(FormatError, "bounded extraction limit"):
                archive.read_member(archive.files[0], max_size=2)


class NsoTests(FixtureTest):
    def test_raw_segments_and_build_identity(self):
        raw, payloads = make_nso()
        nso = Nso(self.write(raw))
        self.assertEqual([segment.data for segment in nso.segments], payloads)
        self.assertTrue(all(segment.hash_checked for segment in nso.segments))
        self.assertFalse(any(segment.compressed for segment in nso.segments))
        self.assertEqual(nso.build_id, bytes(range(32)).hex())
        self.assertEqual(nso.code(4, 8), payloads[0][4:12])
        self.assertEqual(nso.bss_offset, 0x2000 + len(payloads[2]))
        self.assertEqual(nso.image_end, nso.bss_offset + 32)
        self.assertEqual(nso.symbols(), [])

    def test_lz4_segments_and_hashes(self):
        raw, payloads = make_nso(compressed=True)
        nso = Nso(self.write(raw))
        self.assertEqual([segment.data for segment in nso.segments], payloads)
        self.assertTrue(all(s.compressed and s.hash_checked for s in nso.segments))

    def test_segment_hash_mismatch(self):
        raw, _ = make_nso()
        raw[0x100] ^= 1
        with self.assertRaisesRegex(FormatError, "text hash mismatch"):
            Nso(self.write(raw))

    def test_truncated_header(self):
        with self.assertRaisesRegex(FormatError, "expected NSO0"):
            Nso(self.write(b"NSO0"))

    def test_segment_outside_file(self):
        raw, _ = make_nso()
        struct.pack_into("<I", raw, 0x10, len(raw) + 1)
        with self.assertRaisesRegex(FormatError, "text.*outside"):
            Nso(self.write(raw))

    def test_segment_overlaps_header(self):
        raw, _ = make_nso()
        struct.pack_into("<I", raw, 0x10, 0x20)
        with self.assertRaisesRegex(FormatError, "overlaps header"):
            Nso(self.write(raw))

    def test_overlapping_file_segments(self):
        raw, _ = make_nso(checked=False)
        struct.pack_into("<I", raw, 0x20, 0x100)
        with self.assertRaisesRegex(FormatError, "overlapping NSO file"):
            Nso(self.write(raw))

    def test_overlapping_memory_segments(self):
        raw, _ = make_nso()
        struct.pack_into("<I", raw, 0x24, 4)
        with self.assertRaisesRegex(FormatError, "overlapping NSO memory"):
            Nso(self.write(raw))

    def test_code_selection_bounds_and_alignment(self):
        raw, _ = make_nso()
        nso = Nso(self.write(raw))
        for address, size in ((-4, 4), (16, 4), (0, 20), (1, 4), (0, 3), (0, 0)):
            with self.subTest(address=address, size=size), self.assertRaises(FormatError):
                nso.code(address, size)

    def test_malformed_lz4_segment(self):
        raw, _ = make_nso(compressed=True)
        struct.pack_into("<I", raw, 0x60, 1)
        raw[0x100] = 0xFF
        with self.assertRaisesRegex(FormatError, "invalid LZ4 text"):
            Nso(self.write(raw))

    def test_unknown_compression_rejected(self):
        raw, _ = make_nso()
        struct.pack_into("<I", raw, 0x0C, 0x80)
        with self.assertRaisesRegex(FormatError, "ZBIC"):
            Nso(self.write(raw))

    def test_dynamic_symbol_bounds(self):
        raw, _ = make_nso()
        struct.pack_into("<II", raw, 0x90, 0, 1000)
        nso = Nso(self.write(raw))
        with self.assertRaisesRegex(FormatError, "dynstr.*outside"):
            nso.symbols()


if __name__ == "__main__":
    unittest.main()
