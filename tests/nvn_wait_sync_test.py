"""Condition-latched NVN recorder/submit regression, with no game or GPU.

Usage: python tests/nvn_wait_sync_test.py --cc /path/to/clang [--source-root BASELINE_CHECKOUT]
The subprocess timeout detects a deadlocked fixture; assertions use condition handshakes, never elapsed time.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=os.environ.get('CC') or shutil.which('clang'))
    parser.add_argument('--source-root', type=Path, default=ROOT)
    args = parser.parse_args()
    if not args.cc:
        parser.error('pass --cc with the project Clang executable')
    source = args.source_root.resolve()
    out = ROOT/'work'/'build'/'tests'/'nvn-wait-sync'
    out.mkdir(parents=True, exist_ok=True)
    exe = out/('nvn_wait_sync.exe' if os.name == 'nt' else 'nvn_wait_sync')
    # gfx_null.c is included by the fixture; compile from the selected source root for an exact baseline comparison.
    fixture = out/'nvn_wait_sync.c'
    text = (ROOT/'tests/nvn_wait_sync.c').read_text(encoding='utf-8').replace(
        '#include "../runtime/gfx_null.c"', '#include "' + (source/'runtime/gfx_null.c').as_posix() + '"')
    if 'uint64_t submitted_ticks' not in (source/'runtime/gfx.h').read_text(encoding='utf-8'):
        # Older source-root comparisons use the two-argument backend API; ordering assertions stay the same.
        text = text.replace(', uint64_t submitted_ticks)', ')').replace('    (void)submitted_ticks;\n', '')
        text = text.replace('gfx_null_backend.submit(queued, 2, 0)', 'gfx_null_backend.submit(queued, 2)')
    fixture.write_text(text, encoding='utf-8')
    subprocess.run([args.cc, '-std=gnu11', '-O1', '-Wall', '-Wextra', '-fno-strict-aliasing',
                    '-ffunction-sections', '-fdata-sections', '-I', str(source/'runtime'), str(fixture),
                    str(source/'runtime/nvn_api_cmd.c'), str(source/'runtime/nvn_api_core.c'),
                    '-Wl,--gc-sections', *([] if os.name == 'nt' else ['-pthread']), '-o', str(exe)], check=True)
    result = subprocess.run([str(exe)], timeout=20)
    if result.returncode:
        return result.returncode
    for argument, message in (
        ('--null-queue', 'invalid queue'), ('--bad-queue', 'invalid queue'),
        ('--null-sync', 'null sync'), ('--bad-sync', 'invalid sync state'),
        ('--bad-state', 'invalid sync state'), ('--bad-acquire', 'invalid sync state'),
    ):
        result = subprocess.run([str(exe), argument], capture_output=True, text=True,
                                encoding='utf-8', errors='replace', timeout=10)
        if result.returncode != 70 or message not in result.stderr:
            raise AssertionError(f'{argument}: expected named fatal, got {result.returncode}: {result.stderr}')
    print('QueueWaitSync: six malformed-object/state negative cases passed')
    for argument, message in (
        ('--fence-flags', 'unsupported condition 0 flags 0x1'),
        ('--fence-condition', 'unsupported condition 2 flags 0'),
        ('--fence-null', 'nonnull sync'),
        ('--fence-cycle', 'same-submission WaitSync after FenceSync'),
        ('--fence-cross-cycle', 'same-submission WaitSync after FenceSync'),
        ('--fence-invalid-sync', 'invalid output sync'),
        ('--fence-zero-submit', 'no completion fence'),
    ):
        result = subprocess.run([str(exe), argument], capture_output=True, text=True,
                                encoding='utf-8', errors='replace', timeout=10)
        if result.returncode != 70 or message not in result.stderr:
            raise AssertionError(f'{argument}: expected named fatal, got {result.returncode}: {result.stderr}')
    print('CommandBufferFenceSync: seven invalid/cyclic/unsupported cases rejected')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
