// AMD FSR (FSR 3.1 API, FSR 4 where the library offers it) through the ffx-api C interface, for the temporal path
// (vk_temporal.c, HWAOI_TEMPORAL=fsr). The library is a separate file next to the executable, loaded at run time and
// never linked: amd_fidelityfx_upscaler_vk.so (fsr4-vk, built by tools/build_fsr4_vk.sh) on Linux, AMD's
// amd_fidelityfx_loader_vk.dll + amd_fidelityfx_upscaler_vk.dll on Windows. Without it every call does nothing.
// Ported from totk-pc-recomp (branch linux-fsr4-vk, runtime/vk_ffx.h); frame generation and the Windows model hook
// are left out.
#pragma once
#include "vk.h"

int vk_ffx_wanted(void);   // HWAOI_TEMPORAL = fsr at start (the device may still lack FSR)
// vk_device.c, while it builds the device: the extensions FSR's shaders use (cooperative matrix, 8-bit floats,
// descriptor buffers for fsr4-vk) join the list when the device has them; their feature structures are chained there.
void vk_ffx_device_exts(const VkExtensionProperties *have, uint32_t nhave, const char **ext, uint32_t *n, uint32_t max);
void vk_ffx_init(void);      // after the device exists: loads the library and picks the upscaler version
int vk_ffx_available(void);  // vk_ffx_init found an upscaler for this device

typedef struct {   // one image FSR reads or writes, in VK_IMAGE_LAYOUT_GENERAL, level 0 and layer 0
    VkImage image;
    VkFormat format;
    uint32_t width, height;
} VkFfxImage;

typedef struct {
    VkFfxImage color, depth, mv, reactive, out;   // reactive.image VK_NULL_HANDLE: none
    float jx, jy;               // this frame's jitter, render pixels (window x right, y down), as the scene was drawn
    float dt_ms;                // time since the previous dispatch
    float cam_near, cam_far;    // view-space distances of the depth range's ends
    float fov_y;                // vertical field of view, radians
    int reset;                  // a new history
    float sharpness;            // 0..1: FSR's own sharpening pass (RCAS) on the result, 0 = none
} VkFfxDispatch;

// A context for one render and output size. hdr: linear HDR colour (auto exposure), else LDR. depth_inverted: depth 1
// is near; depth_infinite: the far plane is at infinity. NULL on failure (logged).
void *vk_ffx_create(uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, int hdr, int depth_inverted, int depth_infinite);
// Records the upscaling into cb. 0 on failure (logged). FSR binds pipelines and descriptors of its own.
int vk_ffx_dispatch(void *ctx, VkCommandBuffer cb, const VkFfxDispatch *d);
void vk_ffx_release(void *ctx);
const char *vk_ffx_version(void);   // the upscaler version in use, for the log ("" before vk_ffx_init)
int vk_ffx_lib_present(void);       // the library's file is next to the executable
#define VK_FFX_LINUX_LIB "amd_fidelityfx_upscaler_vk.so"
