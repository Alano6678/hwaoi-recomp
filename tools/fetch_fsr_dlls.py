#!/usr/bin/env python3
"""Download AMD's signed FSR Vulkan DLLs (FSR 4 and FSR 3.1 upscalers) from the FidelityFX SDK repository into DEST
(default: the build folder of the version), for Windows. The game also does this by itself when FSR 4 is selected and the
DLLs are missing; this tool is for a release folder or an offline-first setup. Pinned to SDK tag v2.4.0 with SHA-256 sums.

    python tools/fetch_fsr_dlls.py [DEST]
"""
import hashlib
import sys
import urllib.request
from pathlib import Path

RAW = 'https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/Kits/FidelityFX/signedbin/'
FILES = {
    'amd_fidelityfx_loader_vk.dll': '38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9',
    'amd_fidelityfx_upscaler_vk.dll': '4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7',
}


def main():
    dest = Path(sys.argv[1]) if len(sys.argv) > 1 else Path.cwd()
    dest.mkdir(parents=True, exist_ok=True)
    for name, want in FILES.items():
        out = dest / name
        if out.is_file() and hashlib.sha256(out.read_bytes()).hexdigest() == want:
            print(f'{name}: present')
            continue
        print(f'{name}: downloading')
        data = urllib.request.urlopen(RAW + name, timeout=120).read()
        got = hashlib.sha256(data).hexdigest()
        if got != want:
            sys.exit(f'{name}: SHA-256 {got} differs from the pinned {want}')
        out.write_bytes(data)
    print(f'FSR DLLs in {dest}')


if __name__ == '__main__':
    main()
