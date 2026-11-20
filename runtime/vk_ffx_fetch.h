// Windows: fetches AMD's signed FSR Vulkan DLLs (amd_fidelityfx_loader_vk.dll, amd_fidelityfx_upscaler_vk.dll: FSR 4 and
// FSR 3.1 upscalers) from the FidelityFX SDK repository into the executable's folder, so FSR works without a manual
// download. Linux: AMD ships no Vulkan library, so these do nothing (tools/build_fsr4_vk.sh).
#pragma once

#define VK_FFX_FETCH_IDLE 0
#define VK_FFX_FETCH_RUNNING 1
#define VK_FFX_FETCH_DONE 2
#define VK_FFX_FETCH_FAILED 3

// Downloads whatever is missing and waits for it (startup, before the device is created). 1 when both DLLs are in place.
int vk_ffx_fetch_blocking(void);
// The same on a background thread (the menu): the DLLs are used after a restart.
void vk_ffx_fetch_async(void);
int vk_ffx_fetch_state(void);   // VK_FFX_FETCH_*
