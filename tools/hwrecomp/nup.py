"""Read-only NUP v1 reader, implemented from a55uka/nup's public SPEC.md.

No key handling, package rewriting or ROM execution. Directory/extent bounds
and the TOC hash are checked before content is used. Only explicitly requested
payloads are decompressed and hashed; opening does NOT verify all ROM contents.
"""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
from pathlib import Path
import re
import struct
from typing import BinaryIO


class FormatError(ValueError):
    """Invalid, corrupt, or unsupported input (fail closed)."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise FormatError(message)


def region(offset: int, size: int, limit: int, label: str) -> None:
    require(0 <= offset <= limit and 0 <= size <= limit - offset,
            f"{label}: range outside containing region")


@dataclass(frozen=True)
class Member:
    index: int
    name: str
    size: int
    offset: int
    stored_size: int
    sha256: str
    codec: int


@dataclass(frozen=True)
class Package:
    index: int
    title_id: int
    root_dir: int
    exefs_dir: int
    romfs_dir: int
    version: int
    kind: int
    flags: int
    meta_offset: int
    meta_size: int


KINDS = {0: "base", 1: "update", 2: "addon", 3: "addon_update", 4: "system"}
ABSENT = 0xFFFFFFFF


class Nup:
    MAX_TOC = 64 * 1024 * 1024
    MAX_MEMBER = 512 * 1024 * 1024
    MAX_META = 16 * 1024 * 1024

    def __init__(self, path: str | Path):
        self.path = Path(path).resolve(strict=True)
        self._stream: BinaryIO = self.path.open("rb")
        try:
            self._parse()
        except BaseException:
            self._stream.close()
            raise

    def close(self):
        self._stream.close()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

    def _read(self, offset: int, size: int) -> bytes:
        region(offset, size, self.file_size, "file read")
        self._stream.seek(offset)
        data = self._stream.read(size)
        require(len(data) == size, "file changed or read was truncated")
        return data

    def _parse(self):
        self._stream.seek(0, 2)
        self.file_size = self._stream.tell()
        require(self.file_size >= 0x80, "truncated NUP superblock")
        header = self._read(0, 0x80)
        self.header_sha256 = hashlib.sha256(header).hexdigest()
        require(header[:4] == b"NUP\x01", "expected NUP v1 magic")
        header_size, header_version = struct.unpack_from("<HH", header, 4)
        require(header_size == 0x80 and header_version == 1,
                "only NUP header version 1 / size 0x80 is supported")
        toc_off, toc_size = struct.unpack_from("<QQ", header, 8)
        self.blob_offset, declared_size, package_count, flags = struct.unpack_from("<QQII", header, 0x38)
        require(declared_size == self.file_size, "NUP file size mismatch")
        require(toc_off == header_size and 0x30 <= toc_size <= self.MAX_TOC, "invalid TOC offset/size")
        require(flags == 0 and header[0x60:] == bytes(32), "unknown NUP flags/reserved fields")
        region(toc_off, toc_size, self.file_size, "TOC")
        require(self.blob_offset % 16 == 0 and toc_off + toc_size <= self.blob_offset <= self.file_size,
                "invalid blob offset")
        toc = self._read(toc_off, toc_size)
        self.toc_sha256 = hashlib.sha256(toc).hexdigest()
        require(bytes.fromhex(self.toc_sha256) == header[0x18:0x38], "TOC SHA-256 mismatch")
        require(toc[:4] == b"TOC\x01", "invalid TOC magic")
        toc_packages, nd, nf, pool_off, pool_size, reserved = struct.unpack_from("<IQQQQQ", toc, 4)
        require(1 <= package_count == toc_packages and reserved == 0, "invalid TOC package count/reserved")
        dirs_off = 0x30 + 0x40 * package_count
        files_off = dirs_off + 0x18 * nd
        children_off = files_off + 0x50 * nf
        require(children_off <= pool_off, "TOC tables overlap string pool")
        region(pool_off, pool_size, len(toc), "string pool")
        pool = toc[pool_off:pool_off + pool_size]

        def name(off, size):
            region(off, size, len(pool), "name")
            try:
                result = pool[off:off + size].decode("utf-8", errors="strict")
            except UnicodeError as e:
                raise FormatError("invalid UTF-8 name") from e
            require("\0" not in result, "NUL in node name")
            return result

        self.packages = []
        uuid_seed = bytearray()
        for i in range(package_count):
            off = 0x30 + i * 0x40
            title, root, exefs, romfs, version, kind, pflags, r1, r2, meta, msize = struct.unpack_from("<QIIIIBBHIQI", toc, off)
            require(kind in KINDS and pflags & ~1 == 0, "unknown package kind/flags")
            require(r1 == r2 == 0 and toc[off + 0x2C:off + 0x40] == bytes(20), "package reserved fields")
            require(root < nd and all(v == ABSENT or v < nd for v in (exefs, romfs)), "invalid package directory index")
            require((meta == 0) == (msize == 0), "inconsistent package metadata range")
            if msize:
                require(msize <= self.MAX_META and meta >= self.blob_offset, "invalid metadata size/offset")
                region(meta, msize, self.file_size, "metadata")
            self.packages.append(Package(i, title, root, exefs, romfs, version, kind, pflags, meta, msize))
            uuid_seed.extend(struct.pack("<QIB", title, version, kind))
        self.uuid = header[0x50:0x60].hex()
        require(hashlib.sha256(uuid_seed).digest()[:16].hex() == self.uuid, "package UUID mismatch")
        bases = [p for p in self.packages if p.kind == 0]
        require(len(bases) <= 1, "multiple base packages")
        for p in self.packages:
            if p.kind == 1:
                require(bool(p.flags & 1) == (not bases), "standalone_update flag inconsistent with base presence")
            else:
                require(p.flags == 0, "standalone_update flag on a non-update package")

        self.dirs = []
        for i in range(nd):
            no, nl, first, count, r = struct.unpack_from("<IIIIQ", toc, dirs_off + i * 0x18)
            require(r == 0, "directory reserved field")
            self.dirs.append((name(no, nl), first, count))
        child_count = sum(d[2] for d in self.dirs)
        require(children_off + child_count * 4 <= pool_off, "children array overlaps string pool")
        self.children = struct.unpack_from(f"<{child_count}I", toc, children_off)
        require(all(c < nd + nf for c in self.children), "invalid child node index")
        used_slots = set()
        for _, first, count in self.dirs:
            region(first, count, child_count, "directory children")
            slots = set(range(first, first + count))
            require(not slots & used_slots, "overlapping directory child slots")
            used_slots.update(slots)
        require(len(used_slots) == child_count, "unclaimed child slots")
        self.files = []
        for i in range(nf):
            off = files_off + i * 0x50
            no, nl, size, extent, stored = struct.unpack_from("<IIQQQ", toc, off)
            digest = toc[off + 0x20:off + 0x40].hex()
            codec = toc[off + 0x40]
            require(codec in (0, 1), "unsupported NUP extent codec")
            require(toc[off + 0x41:off + 0x50] == bytes(15), "unknown codec flags/reserved fields")
            if size == 0:
                require(extent == stored == codec == 0, "invalid empty file extent")
            else:
                require(stored > 0 and extent >= self.blob_offset and extent % 16 == 0, "invalid file extent")
                region(extent, stored, self.file_size, "file extent")
            require(codec != 0 or size == stored, "raw extent size mismatch")
            self.files.append(Member(i, name(no, nl), size, extent, stored, digest, codec))
        # Deduplicated files may share an EXACT extent and content identity.
        spans = {}
        for f in self.files:
            if f.stored_size:
                key = (f.offset, f.stored_size)
                identity = (f.size, f.sha256, f.codec)
                require(key not in spans or spans[key] == identity, "inconsistent deduplicated extent")
                spans[key] = identity
        ranges = [(a, a + b) for a, b in spans]
        ranges += [(p.meta_offset, p.meta_offset + p.meta_size) for p in self.packages if p.meta_size]
        previous_end = self.blob_offset
        for start, end in sorted(ranges):
            require(start >= previous_end, "overlapping file/metadata regions")
            previous_end = end
        # Verify the complete tree topology without reading file payloads.
        for p in self.packages:
            list(self.walk(p.root_dir))

    def walk(self, directory: int):
        """Yield (relative slash path, Member); rejects cycles/duplicate nodes."""
        require(0 <= directory < len(self.dirs), "invalid directory")
        stack = [(directory, "")]
        seen = set()
        while stack:
            node, prefix = stack.pop()
            require(node not in seen, "cycle or multiply referenced node in package tree")
            seen.add(node)
            if node >= len(self.dirs):
                yield prefix, self.files[node - len(self.dirs)]
                continue
            _, first, count = self.dirs[node]
            children = self.children[first:first + count]
            names = [self.dirs[c][0] if c < len(self.dirs) else self.files[c - len(self.dirs)].name for c in children]
            require(len(names) == len(set(names)), "duplicate sibling names")
            require(names == sorted(names, key=lambda s: s.encode("utf-8")), "unsorted NUP children")
            for child, child_name in reversed(list(zip(children, names))):
                require(child_name not in ("", ".", "..") and not any(x in child_name for x in ("/", "\\")),
                        "invalid child name")
                stack.append((child, f"{prefix}/{child_name}" if prefix else child_name))

    def read_member(self, member: Member, max_size: int | None = None) -> bytes:
        limit = self.MAX_MEMBER if max_size is None else min(max_size, self.MAX_MEMBER)
        require(member.size <= limit and member.stored_size <= limit, "member exceeds bounded extraction limit")
        data = self._read(member.offset, member.stored_size)
        if member.codec == 1:
            import zstandard
            try:
                require(zstandard.frame_content_size(data) == member.size, "zstd frame size mismatch")
                data = zstandard.ZstdDecompressor().decompress(data, max_output_size=member.size, allow_extra_data=False)
            except zstandard.ZstdError as e:
                raise FormatError(f"invalid zstd member: {member.name}") from e
        require(len(data) == member.size, "decompressed member size mismatch")
        require(hashlib.sha256(data).hexdigest() == member.sha256, f"member SHA-256 mismatch: {member.name}")
        return data

    def metadata(self, package: Package):
        raw = self._read(package.meta_offset, package.meta_size) if package.meta_size else b""
        result = {}
        offset = 0
        names = {4: "display_version", 5: "title", 6: "publisher", 7: "build_id", 8: "source_dump"}
        while offset < len(raw):
            region(offset, 8, len(raw), "metadata TLV header")
            kind, reserved, size = struct.unpack_from("<HHI", raw, offset)
            require(reserved == 0, "metadata reserved field")
            offset += 8
            region(offset, size, len(raw), "metadata TLV payload")
            payload = raw[offset:offset + size]
            if kind in names:
                if kind == 7:
                    require(size == 32, "invalid metadata build ID length")
                    result[names[kind]] = payload.hex()
                else:
                    try:
                        result[names[kind]] = payload.decode("utf-8")
                    except UnicodeError as e:
                        raise FormatError("invalid metadata UTF-8") from e
            offset += size
        return result

    def exefs(self, package_index: int | None = None):
        """Latest matching update shadows base by path; no asset merge/execution."""
        if package_index is not None:
            require(0 <= package_index < len(self.packages), "package index outside range")
            chosen = [self.packages[package_index]]
        else:
            bases = [p for p in self.packages if p.kind == 0]
            require(len(bases) == 1, "no base package; choose an explicit package for analysis")
            base = bases[0]
            updates = [p for p in self.packages if p.kind == 1 and p.title_id in (base.title_id, base.title_id | 0x800)]
            chosen = bases
            if updates:
                maximum = max(p.version for p in updates)
                latest = [p for p in updates if p.version == maximum]
                require(len(latest) == 1, "ambiguous update version; choose a package")
                chosen += latest
        members = {}
        for p in chosen:
            if p.exefs_dir != ABSENT:
                for path, member in self.walk(p.exefs_dir):
                    members[path] = (p, member)
        require(bool(members), "selected package(s) have no ExeFS members")
        return chosen, members

    def summary(self):
        return {
            "path": str(self.path), "file_size": self.file_size, "format": "NUP v1",
            "header_sha256": self.header_sha256, "toc_sha256": self.toc_sha256,
            "uuid": self.uuid, "directory_count": len(self.dirs), "file_count": len(self.files),
            "verification": "TOC SHA-256, UUID, table/tree/extent bounds; payloads not all hashed",
            "packages": [{"index": p.index, "title_id": f"{p.title_id:016X}", "kind": KINDS[p.kind],
                          "version": p.version, "metadata": self.metadata(p),
                          "exefs": [{"path": path, "size": f.size, "stored_size": f.stored_size,
                                     "codec": f.codec, "sha256": f.sha256}
                                    for path, f in self.walk(p.exefs_dir)] if p.exefs_dir != ABSENT else []}
                         for p in self.packages],
        }


def safe_relative(path: str) -> Path:
    parts = path.split("/")
    for part in parts:
        require(part not in ("", ".", "..") and not re.search(r'[<>:"\\|?*\x00-\x1f]', part)
                and not part.endswith((".", " ")), "unsafe Windows extraction name")
        require(part.split(".")[0].upper() not in {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$",
                *(f"COM{i}" for i in range(10)), *(f"LPT{i}" for i in range(10))}, "reserved Windows extraction name")
    return Path(*parts)


def extract_exefs(archive: Nup, output: Path, package_index: int | None = None):
    chosen, members = archive.exefs(package_index)
    names = [safe_relative(path) for path in members]
    require(len({str(p).casefold() for p in names}) == len(names), "case-insensitive extraction name collision")
    output = output.resolve()
    require(not output.exists(), "output directory exists; choose a fresh directory")
    output.mkdir(parents=True)
    extracted = []
    for (path, (package, member)), relative in zip(members.items(), names):
        data = archive.read_member(member)
        destination = output / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        require(destination.resolve().is_relative_to(output), "extraction path escaped output directory")
        with destination.open("xb") as stream:
            stream.write(data)
        extracted.append({"path": path, "package": package.index, "size": len(data), "sha256": member.sha256})
    return {"selected_packages": [p.index for p in chosen], "output": str(output), "files": extracted}
