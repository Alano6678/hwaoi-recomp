# NVIDIA Streamline headers

The `include/` headers of the Streamline SDK v2.14.16 (https://github.com/NVIDIA-RTX/Streamline, release asset
`streamline-sdk-v2.14.16.zip`, SHA-256 32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b), unmodified,
for `runtime/aod_dlss.cpp`: the core ones, `sl_dlss.h` (DLSS), and `sl_dlss_g.h`, `sl_reflex.h` and `sl_pcl.h` (frame
generation with Reflex). MIT licence: `LICENSE.txt` (the SDK's `license.txt`; its Nsight Perf exception covers
`sl_nvperf.h`, which is not here).

Only the headers live in this tree. The Streamline DLLs (`sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll`,
`sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll`; MIT, signed by NVIDIA) are loaded at run time from the player's engine
folder. NVIDIA's own files are never shipped: `nvngx_dlss.dll` and `nvngx_dlssg.dll` (NVIDIA RTX SDK licence) come
from the player's own PC, and `NvLowLatencyVk.dll` (NVIDIA's SDK licence, the SDK's `bin/x64/reflex.license.txt`) is
not used: Reflex runs through the graphics driver's `VK_NV_low_latency2`.
