"""Build the RomFS index the runtime binary-searches (runtime/romfs.c) and its romfs_config.h.

Adapted from MacNativeGames games/totk/scripts/prepare_romfs_backend.py @ b75be8cf (MIT, Copyright (c) 2026
MacNativeGames contributors).

Input: <romfs>/index.json ({status, romfs_size, romfs_sha256, files: [{path, offset, size}]}) as tools/extract.py
writes it, and <romfs>/romfs.bin, whose size must match. Output, both in the build directory (never in the repo):

    romfs-index.bin   TOTKRFS1 layout: header <8sQQ (magic, count, strings offset), count 24-byte rows <QQII (offset,
                      size, name offset, name length including the NUL), then the NUL-terminated names; rows sorted by
                      the UTF-8 bytes of the path, which is the order strcmp sees
    romfs_config.h    ROMFS_IMAGE_SIZE, ROMFS_IMAGE_SHA256, ROMFS_INDEX_SHA256, ROMFS_FILE_COUNT

Both are rewritten only when their bytes change, so Ninja does not rebuild romfs.c for nothing.

    .venv\\Scripts\\python tools\\romfs_index.py --romfs build\\extracted\\v121\\romfs --out build\\v121

Standard library only.
"""
import argparse
import hashlib
import json
import struct
import sys
import time
from pathlib import Path

from check_content import GENERATED_MARKER

MAGIC = b'TOTKRFS1'
HEADER = struct.Struct('<8sQQ')
ROW = struct.Struct('<QQII')


def build_index(manifest):
    """The index bytes for an index.json manifest; exits on a malformed one."""
    files = sorted(manifest['files'], key=lambda e: e['path'].encode())
    size = manifest['romfs_size']
    strings = bytearray()
    rows = bytearray()
    previous = None
    for item in files:
        path = item['path']
        name = path.encode()
        if not name or name.startswith(b'/') or b'\\' in name or b'\0' in name:
            sys.exit(f'romfs_index: malformed path in index.json (entry {len(rows) // ROW.size})')
        if any(part in ('', '.', '..') for part in path.split('/')):
            sys.exit('romfs_index: path traversal in index.json')
        if name == previous:
            sys.exit('romfs_index: duplicate path in index.json')
        previous = name
        if item['offset'] < 0 or item['size'] < 0 or item['offset'] + item['size'] > size:
            sys.exit(f'romfs_index: entry {len(rows) // ROW.size} lies outside the image')
        rows += ROW.pack(item['offset'], item['size'], len(strings), len(name) + 1)
        strings += name + b'\0'
    return HEADER.pack(MAGIC, len(files), HEADER.size + len(rows)) + bytes(rows) + bytes(strings)


def write_if_changed(path, data):
    if path.exists() and path.read_bytes() == data:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def write(romfs, out):
    """Write romfs-index.bin and romfs_config.h into out. Returns (file count, index bytes, seconds)."""
    t0 = time.monotonic()
    manifest = json.loads((romfs / 'index.json').read_text(encoding='utf-8'))
    if manifest.get('status') != 'pass':
        sys.exit(f'romfs_index: {romfs / "index.json"} does not record a verified extraction')
    nup_mode = manifest.get('storage') == 'nup-cache-v1'
    extra_config = ''
    if nup_mode:
        source = Path(manifest['nup_path'])
        cache = romfs / 'cache.bin'
        if (not source.is_file() or source.stat().st_size != manifest['nup_size'] or
                not cache.is_file() or cache.stat().st_size != manifest['cache_size'] or
                manifest['romfs_size'] != manifest['nup_size'] + manifest['cache_size']):
            sys.exit('romfs_index: NUP/cache identity or size mismatch')
        with source.open('rb') as stream:
            header = stream.read(0x80)
            if hashlib.sha256(header).hexdigest() != manifest['nup_header_sha256']:
                sys.exit('romfs_index: source NUP header changed')
            toc_off, toc_size = struct.unpack_from('<QQ', header, 8)
            if (toc_off != manifest['nup_toc_offset'] or toc_size != manifest['nup_toc_size'] or
                    toc_size > (64 << 20) or toc_off > source.stat().st_size - toc_size):
                sys.exit('romfs_index: invalid NUP TOC range')
            stream.seek(toc_off)
            if hashlib.sha256(stream.read(toc_size)).hexdigest() != manifest['nup_toc_sha256']:
                sys.exit('romfs_index: source NUP TOC changed')
        hashes = bytearray()
        for item in sorted(manifest['files'], key=lambda e: e['path'].encode()):
            off, size = item['offset'], item['size']
            if off < manifest['nup_size'] and size > manifest['nup_size'] - off:
                sys.exit('romfs_index: entry crosses NUP/cache boundary')
            digest = bytes.fromhex(item['sha256'])
            if len(digest) != 32:
                sys.exit('romfs_index: invalid entry SHA-256')
            hashes.extend(digest)
        write_if_changed(out / 'romfs-hashes.bin', bytes(hashes))
        extra_config = ('#define ROMFS_NUP_MODE 1\n'
                        f'#define ROMFS_NUP_PATH {json.dumps(str(source), ensure_ascii=False)}\n'
                        f'#define ROMFS_NUP_SIZE {manifest["nup_size"]}ull\n'
                        f'#define ROMFS_CACHE_SIZE {manifest["cache_size"]}ull\n'
                        f'#define ROMFS_NUP_HEADER_SHA256 "{manifest["nup_header_sha256"]}"\n'
                        f'#define ROMFS_NUP_TOC_OFFSET {toc_off}ull\n'
                        f'#define ROMFS_NUP_TOC_SIZE {toc_size}ull\n'
                        f'#define ROMFS_NUP_TOC_SHA256 "{manifest["nup_toc_sha256"]}"\n'
                        f'#define ROMFS_CACHE_SHA256 "{manifest["cache_sha256"]}"\n'
                        f'#define ROMFS_HASHES_SHA256 "{hashlib.sha256(hashes).hexdigest()}"\n')
    else:
        image = romfs / 'romfs.bin'
        if not image.is_file() or image.stat().st_size != manifest['romfs_size']:
            sys.exit(f'romfs_index: {image} is missing or its size differs from index.json')
    index = build_index(manifest)
    count = len(manifest['files'])
    write_if_changed(out / 'romfs-index.bin', index)
    config = (f'// RomFS image and index identity, written by tools/romfs_index.py; do not edit. {GENERATED_MARKER}\n'
              '#pragma once\n'
              f'#define ROMFS_IMAGE_SIZE {manifest["romfs_size"]}ull\n'
              f'#define ROMFS_IMAGE_SHA256 "{manifest["romfs_sha256"].lower()}"\n'
              f'#define ROMFS_INDEX_SHA256 "{hashlib.sha256(index).hexdigest()}"\n'
              f'#define ROMFS_FILE_COUNT {count}ull\n' + extra_config)
    write_if_changed(out / 'romfs_config.h', config.encode())
    return count, len(index), time.monotonic() - t0


def write_placeholder(out):
    """romfs_config.h without a RomFS (no extracted game on this machine): totk.exe builds, and fs_init stops with a
    named fatal."""
    (out / 'romfs-index.bin').unlink(missing_ok=True)
    config = (f'// No RomFS found by tools/romfs_index.py; do not edit. {GENERATED_MARKER}\n'
              '#pragma once\n'
              '#define ROMFS_MISSING 1\n'
              '#define ROMFS_IMAGE_SIZE 0ull\n'
              '#define ROMFS_IMAGE_SHA256 ""\n'
              '#define ROMFS_INDEX_SHA256 ""\n'
              '#define ROMFS_FILE_COUNT 0ull\n')
    write_if_changed(out / 'romfs_config.h', config.encode())


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--romfs', type=Path, required=True, help='directory with romfs.bin and index.json')
    ap.add_argument('--out', type=Path, required=True, help='build directory')
    args = ap.parse_args()
    count, size, dt = write(args.romfs.resolve(), args.out.resolve())
    print(f'romfs index: {count} files, {size:,} bytes, {dt:.1f} s')


if __name__ == '__main__':
    main()
