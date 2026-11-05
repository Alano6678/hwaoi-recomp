"""Prepare a read-only NUP-backed RomFS: raw extents stay in the original NUP.

Only compressed selected files are expanded into cache.bin. Runtime addresses
describe a virtual concatenation [original NUP][cache.bin], not a physical image.
Compressed payloads are verified here; all payloads are verified on first open by
the runtime. The original NUP is never changed and no loose resource tree is made.
"""
from __future__ import annotations
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import time
from hwrecomp.nup import Nup, ABSENT, require

STORAGE = "nup-cache-v1"


def select_romfs(archive, packages):
    selected = {}
    for package in packages:
        if package.romfs_dir != ABSENT:
            selected.update(dict(archive.walk(package.romfs_dir)))
    require(bool(selected), "selected packages contain no RomFS")
    # Prevent ambiguous file/directory shadows across packages.
    for name in selected:
        parts = name.split("/")
        require(all(p and p not in (".", "..") for p in parts), "invalid virtual RomFS path")
        require(not any("/".join(parts[:i]) in selected for i in range(1, len(parts))), "RomFS file/directory conflict")
    return sorted(selected.items(), key=lambda item: item[0].encode("utf-8"))


def prepare(nup_path, output, expected, reserve_bytes=8 << 30):
    output = Path(output).resolve()
    require(not output.exists(), "RomFS output exists; choose a new directory")
    output.parent.mkdir(parents=True, exist_ok=True)
    with Nup(nup_path) as archive:
        before = archive.path.stat()
        require(not any(p.kind in (2, 3) for p in archive.packages),
                "DLC/addon packages are not supported by the current empty-AOC runtime")
        packages, exefs = archive.exefs()
        actual = {name: member.sha256 for name, (_, member) in exefs.items()}
        require(actual == expected["exefs_sha256"], "NUP ExeFS does not match the verified local HWAoI profile")
        require(packages[0].title_id == int(expected["title_id"], 16), "NUP title ID mismatch")
        require(packages[-1].version == expected["ncm_version"], "NUP update version mismatch")
        files = select_romfs(archive, packages)
        unique = {(m.offset, m.stored_size, m.sha256): m for _, m in files if m.codec == 1}
        estimated = sum((m.size + 15) & ~15 for m in unique.values())
        require(shutil.disk_usage(output.parent).free >= estimated + reserve_bytes,
                f"RomFS cache needs {estimated} bytes plus {reserve_bytes} bytes free headroom")
        output.mkdir()
        marker = output / "preparation.json"
        marker.write_text(json.dumps({"status": "incomplete", "source": str(archive.path)}), encoding="utf-8")
        entries, locations, offset, verified = [], {}, 0, 0
        cache_hash = hashlib.sha256()
        t0 = time.monotonic()
        # A failed preparation leaves only an explicitly incomplete, fresh output.
        with (output / "cache.bin.part").open("xb") as cache:
            for i, (name, member) in enumerate(files):
                if member.codec == 0:
                    virtual_offset = member.offset
                else:
                    key = member.offset, member.stored_size, member.sha256
                    if key not in locations:
                        data = archive.read_member(member)
                        padding = (-offset) % 16
                        if padding:
                            cache.write(bytes(padding))
                            cache_hash.update(bytes(padding))
                            offset += padding
                        locations[key] = archive.file_size + offset
                        cache.write(data)
                        cache_hash.update(data)
                        offset += len(data)
                        verified += 1
                    virtual_offset = locations[key]
                entries.append({"path": name, "offset": virtual_offset, "size": member.size,
                                "sha256": member.sha256, "storage": "nup" if member.codec == 0 else "cache"})
                if i % 200 == 0 or i + 1 == len(files):
                    print(f"RomFS {i + 1}/{len(files)}, cache {offset / 2**30:.2f} GiB, {time.monotonic()-t0:.1f}s", flush=True)
            cache.flush()
            os.fsync(cache.fileno())
        after = archive.path.stat()
        require((before.st_size, before.st_mtime_ns) == (after.st_size, after.st_mtime_ns), "source NUP changed during preparation")
        with archive.path.open("rb") as stream:
            header = stream.read(0x80)
        toc_offset, toc_size = struct.unpack_from("<QQ", header, 8)
        manifest = {"status": "pass", "storage": STORAGE,
                    "source": "validated NUP tables; raw payload hashes checked lazily on first runtime open",
                    "title_id": expected["title_id"], "ncm_version": expected["ncm_version"],
                    "exefs_sha256": actual, "nup_path": str(archive.path), "nup_size": archive.file_size,
                    "nup_header_sha256": archive.header_sha256, "nup_toc_sha256": archive.toc_sha256,
                    "nup_toc_offset": toc_offset, "nup_toc_size": toc_size,
                    "cache_size": offset, "cache_sha256": cache_hash.hexdigest(),
                    "romfs_size": archive.file_size + offset, "romfs_sha256": "",
                    "files": entries, "selected_packages": [p.index for p in packages],
                    "compressed_extents_verified": verified, "raw_payload_verification": "runtime-first-open"}
        (output / "cache.bin.part").rename(output / "cache.bin")
        pending = output / "index.json.part"
        pending.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        pending.rename(output / "index.json")
        marker.write_text(json.dumps({"status": "complete", "files": len(files), "cache_bytes": offset}), encoding="utf-8")
        return {"directory": str(output), "files": len(files), "cache_bytes": offset,
                "raw_bytes_not_copied": sum(m.size for _, m in files if m.codec == 0),
                "verification": manifest["source"], "storage": STORAGE}
