"""NSO0 segment inspection. This is not an ELF linker or Horizon loader."""
from __future__ import annotations
from dataclasses import dataclass
import hashlib
from pathlib import Path
import struct
from .nup import FormatError, require, region


@dataclass
class Segment:
    name: str
    memory_offset: int
    data: bytes
    compressed: bool
    hash_checked: bool


class Nso:
    MAX_SEGMENT = 512 * 1024 * 1024

    def __init__(self, path: str | Path):
        self.path = Path(path)
        require(self.path.stat().st_size <= self.MAX_SEGMENT, "NSO input too large")
        raw = self.path.read_bytes()
        require(len(raw) >= 0x100 and raw[:4] == b"NSO0", "expected NSO0 header")
        self.sha256 = hashlib.sha256(raw).hexdigest()
        self.version, reserved, self.flags = struct.unpack_from("<III", raw, 4)
        require(self.version == 0 and reserved == 0, "unsupported NSO version/reserved")
        require(self.flags & ~0xFF == 0, "unknown NSO flags")
        require(not self.flags & 0x80, "NSO ZBIC compression is not implemented")
        self.build_id = raw[0x40:0x60].hex()
        self.bss_size = struct.unpack_from("<I", raw, 0x3C)[0]
        self.segments = []
        file_ranges = []
        memory_ranges = []
        for i, name in enumerate(("text", "rodata", "data")):
            offset, memory_offset, size = struct.unpack_from("<III", raw, 0x10 + i * 0x10)
            compressed = bool(self.flags & (1 << i))
            stored_size = struct.unpack_from("<I", raw, 0x60 + i * 4)[0] if compressed else size
            require(size <= self.MAX_SEGMENT and stored_size <= self.MAX_SEGMENT, "NSO segment too large")
            require(offset >= 0x100 or size == 0, "NSO segment overlaps header")
            region(offset, stored_size, len(raw), name)
            data = raw[offset:offset + stored_size]
            if compressed and size:
                import lz4.block
                try:
                    data = lz4.block.decompress(data, uncompressed_size=size)
                except lz4.block.LZ4BlockError as e:
                    raise FormatError(f"invalid LZ4 {name} segment") from e
            require(len(data) == size, "NSO decompressed size mismatch")
            check = bool(self.flags & (1 << (i + 3)))
            if check:
                require(hashlib.sha256(data).digest() == raw[0xA0 + i * 32:0xC0 + i * 32], f"NSO {name} hash mismatch")
            if size:
                file_ranges.append((offset, offset + stored_size))
                memory_ranges.append((memory_offset, memory_offset + size))
            self.segments.append(Segment(name, memory_offset, data, compressed, check))
        for label, ranges in (("file", file_ranges), ("memory", memory_ranges)):
            last_end = 0
            for start, end in sorted(ranges):
                require(start >= last_end, f"overlapping NSO {label} segments")
                last_end = end
        self.text = self.segments[0]
        require(self.text.memory_offset % 4 == 0 and len(self.text.data) % 4 == 0, "unaligned NSO text")
        self.image_end = max(s.memory_offset + len(s.data) for s in self.segments)
        self.bss_offset = self.segments[2].memory_offset + len(self.segments[2].data)
        require(not self.bss_size or self.bss_offset >= self.image_end, "NSO BSS overlaps another segment")
        self.image_end = max(self.image_end, self.bss_offset + self.bss_size)
        self.dynstr = struct.unpack_from("<II", raw, 0x90)
        self.dynsym = struct.unpack_from("<II", raw, 0x98)

    def code(self, address: int, size: int) -> bytes:
        require(address % 4 == size % 4 == 0 and size > 0, "instruction range must be nonempty and aligned")
        offset = address - self.text.memory_offset
        region(offset, size, len(self.text.data), "text selection")
        return self.text.data[offset:offset + size]

    def symbols(self):
        """Declared ELF64 dynamic symbols, not discovered function boundaries."""
        ro = self.segments[1].data
        so, ss = self.dynstr
        yo, ys = self.dynsym
        region(so, ss, len(ro), "dynstr")
        region(yo, ys, len(ro), "dynsym")
        require(ys % 24 == 0, "invalid ELF64 symbol table size")
        strings = ro[so:so + ss]
        result = []
        for i in range(ys // 24):
            no, info, other, section, value, size = struct.unpack_from("<IBBHQQ", ro, yo + i * 24)
            require(no < len(strings) or no == 0, "symbol name outside dynstr")
            end = strings.find(b"\0", no)
            require(end >= no or not strings, "unterminated dynamic symbol name")
            name = strings[no:end].decode("utf-8", errors="replace") if strings else ""
            result.append({"name": name, "type": info & 15, "binding": info >> 4,
                           "section": section, "value": value, "size": size})
        return result

    def summary(self):
        return {"path": str(self.path), "sha256": self.sha256, "build_id": self.build_id,
                "flags": self.flags, "image_size": self.image_end, "bss_size": self.bss_size,
                "segments": [{"name": s.name, "memory_offset": s.memory_offset, "size": len(s.data),
                              "compressed": s.compressed, "hash_checked": s.hash_checked,
                              "sha256": hashlib.sha256(s.data).hexdigest()} for s in self.segments],
                "dynamic_symbol_count": len(self.symbols()),
                "limitations": ["Module-relative addresses; no relocations, imports or system services resolved",
                                "Text contains data/padding too; linear decode does not prove reachable code"]}
