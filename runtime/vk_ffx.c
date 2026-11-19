// FSR for the temporal path (vk_ffx.h). Ported from totk-pc-recomp (branch linux-fsr4-vk, runtime/vk_ffx.c): the
// version query and pinning, contexts, dispatch and the descriptor layouts of ffx_api_min.h (AMD, MIT); frame generation
// and the Windows-only model hook on AMD's DLL are left out.
//
// Windows: the FSR SDK 2.4 ships its Vulkan effects as signed DLLs behind a
// loader: amd_fidelityfx_loader_vk.dll exports the five ffx-api entry points and finds the effect DLL
// (amd_fidelityfx_upscaler_vk.dll) next to itself. Both are loaded from the executable's folder at run time; no SDK
// code is compiled in, only the descriptor layouts of ffx_api_min.h. Linux: AMD ships no library, so
// amd_fidelityfx_upscaler_vk.so next to the executable is the independent open-source FSR 4 build of fsr4-vk
// (tools/build_fsr4_vk.sh makes it on the player's machine); it exports the same five entry points itself, so there
// is no loader, and it is a separate file loaded at run time, never linked or copied (its licence is GPL-3.0).
//
// The upscaler library offers several versions (FSR 4.x, FSR 3.1.x on AMD's; "4.1.1 Vulkan Auto/FP8/INT8" on fsr4-vk)
// and decides by itself which ones a device can run: vk_ffx_init lists them and pins the newest 4.x when there is one,
// else leaves the library's default (3.1 on GPUs without FSR 4). Every context is created with that version
// (ffxOverrideVersion).
//
// What FSR gets (vk_temporal.c): the R11G11B10F lighting target as linear HDR colour with automatic exposure, the scene
// depth (copied to R32F by the motion pass), camera motion vectors at the render size in render pixels pointing from this frame's
// position to last frame's without the jitter (motionVectorScale 1), and the jitter the scene was drawn with. That is
// FSR's own convention as well: its guide derives the projection offset as (2 jx / w, -2 jy / h), which moves the
// picture by (jx, jy) window pixels with y down, the way vk_temporal_viewport moves the viewport. HWAOI_FSR_JITTER_SIGN=-1
// flips it to test the other convention.
#include "vk_ffx.h"

#include "vk_ffx_fetch.h"
#include "cfg.h"
#include "host.h"

#ifdef _WIN32
#include <windows.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/platform.h"
#ifndef _WIN32
#include <dlfcn.h>
#endif

#include "ffx_api_min.h"

#define FFX_MAX_VERSIONS 16
// The state the input images are declared in. AMD's DLL takes the broader generic-read state; fsr4-vk checks for exactly
// COMPUTE_READ (its output is UNORDERED_ACCESS on both).
#ifdef _WIN32
#define FFX_IN_STATE FFX_API_RESOURCE_STATE_GENERIC_READ
#else
#define FFX_IN_STATE FFX_API_RESOURCE_STATE_COMPUTE_READ
#endif
#ifdef _WIN32
#define FFX_LOADER L"amd_fidelityfx_loader_vk.dll"
typedef HMODULE LibHandle;
#else
typedef void *LibHandle;
#endif

static struct {
    LibHandle dll;
    PfnFfxCreateContext create;
    PfnFfxDestroyContext destroy;
    PfnFfxQuery query;
    PfnFfxDispatch dispatch;
    int tried, loaded, available;
    VkDeviceContext dev;
    uint64_t version_id;   // 0: the DLL's default
    char version[48];
    float jitter_sign;
} F = {.jitter_sign = 1.0f};

int vk_ffx_wanted(void) {
    static int wanted = -1;
    if (wanted < 0) wanted = !strcmp(cfg_value("TOTK_TEMPORAL"), "fsr");
    return wanted;
}

// The library's file(s) next to the executable (checked before the device exists).
int vk_ffx_lib_present(void) {
    char exe[PLAT_PATH_MAX], path[PLAT_PATH_MAX + 48];
    if (!plat_exe_path(exe, sizeof exe)) return 0;
    char *last = plat_last_sep(exe);
    if (!last) return 0;
#ifdef _WIN32
    snprintf(path, sizeof path, "%.*s/amd_fidelityfx_loader_vk.dll", (int)(last - exe), exe);
    if (!plat_file_exists(path)) return 0;
    snprintf(path, sizeof path, "%.*s/amd_fidelityfx_upscaler_vk.dll", (int)(last - exe), exe);
#else
    snprintf(path, sizeof path, "%.*s/" VK_FFX_LINUX_LIB, (int)(last - exe), exe);
#endif
    return plat_file_exists(path);
}

#ifdef _WIN32
static int loader_load(void) {
    if (F.tried) return F.loaded;
    F.tried = 1;
    if (!vk_ffx_lib_present()) vk_ffx_fetch_blocking();   // AMD's signed DLLs from the FidelityFX SDK repository
    wchar_t path[MAX_PATH + 64];
    DWORD len = GetModuleFileNameW(NULL, path, MAX_PATH);
    while (len && path[len - 1] != L'\\') len--;
    path[len] = 0;
    wcscat(path, FFX_LOADER);
    // LOAD_WITH_ALTERED_SEARCH_PATH: the loader's own imports and the effect DLL resolve from the executable's folder
    F.dll = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!F.dll) {
        rt_log("fsr: cannot load amd_fidelityfx_loader_vk.dll next to the executable (error %lu): FSR off", GetLastError());
        return 0;
    }
    F.create = (PfnFfxCreateContext)(void *)GetProcAddress(F.dll, "ffxCreateContext");
    F.destroy = (PfnFfxDestroyContext)(void *)GetProcAddress(F.dll, "ffxDestroyContext");
    F.query = (PfnFfxQuery)(void *)GetProcAddress(F.dll, "ffxQuery");
    F.dispatch = (PfnFfxDispatch)(void *)GetProcAddress(F.dll, "ffxDispatch");
    if (!F.create || !F.destroy || !F.query || !F.dispatch) {
        rt_log("fsr: amd_fidelityfx_loader_vk.dll lacks the ffx-api entry points: FSR off");
        return 0;
    }
    return F.loaded = 1;
}
#else
// fsr4-vk exports ffxCreateContext, ffxDestroyContext, ffxConfigure, ffxQuery and ffxDispatch itself: no loader.
static int loader_load(void) {
    if (F.tried) return F.loaded;
    F.tried = 1;
    char exe[PLAT_PATH_MAX], path[PLAT_PATH_MAX + 48];
    if (!plat_exe_path(exe, sizeof exe)) return 0;
    char *last = plat_last_sep(exe);
    if (!last) return 0;
    snprintf(path, sizeof path, "%.*s/" VK_FFX_LINUX_LIB, (int)(last - exe), exe);
    if (!plat_file_exists(path)) {
        rt_log("fsr: " VK_FFX_LINUX_LIB " is not next to the executable (tools/build_fsr4_vk.sh makes it): FSR off");
        return 0;
    }
    F.dll = plat_lib_open(path);
    if (!F.dll) {
        const char *why = dlerror();
        rt_log("fsr: cannot load %s (%s): FSR off", path, why ? why : "?");
        return 0;
    }
    F.create = (PfnFfxCreateContext)plat_lib_sym(F.dll, "ffxCreateContext");
    F.destroy = (PfnFfxDestroyContext)plat_lib_sym(F.dll, "ffxDestroyContext");
    F.query = (PfnFfxQuery)plat_lib_sym(F.dll, "ffxQuery");
    F.dispatch = (PfnFfxDispatch)plat_lib_sym(F.dll, "ffxDispatch");
    if (!F.create || !F.destroy || !F.query || !F.dispatch) {
        rt_log("fsr: " VK_FFX_LINUX_LIB " lacks the ffx-api entry points: FSR off");
        return 0;
    }
    return F.loaded = 1;
}
#endif


static int has(const VkExtensionProperties *have, uint32_t nhave, const char *name) {
    for (uint32_t i = 0; i < nhave; i++)
        if (!strcmp(have[i].extensionName, name)) return 1;
    return 0;
}

static void add(const VkExtensionProperties *have, uint32_t nhave, const char **ext, uint32_t *n, uint32_t max,
                const char *name) {
    for (uint32_t i = 0; i < *n; i++)
        if (!strcmp(ext[i], name)) return;
    if (has(have, nhave, name) && *n < max) ext[(*n)++] = name;
}

// The extensions the FSR SDK's own sample enables for its effects (Cauldron2 device_vk.cpp): cooperative matrix and
// 8-bit floats for FSR 4's network where the device has them. The rest it uses (16-bit floats and storage, integer
// dot products, subgroup size control) is core Vulkan 1.3, which vk_device.c enables as far as the driver reports it.
// fsr4-vk (Linux) also keeps its descriptors in descriptor buffers and refuses a device without
// VK_EXT_descriptor_buffer (vk_device.c chains its feature with the extension).
void vk_ffx_device_exts(const VkExtensionProperties *have, uint32_t nhave, const char **ext, uint32_t *n, uint32_t max) {
    if (!vk_ffx_wanted()) return;
    add(have, nhave, ext, n, max, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
    add(have, nhave, ext, n, max, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
#ifdef VK_EXT_SHADER_FLOAT8_EXTENSION_NAME
    add(have, nhave, ext, n, max, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME);
#endif
}

void vk_ffx_init(void) {
    if (!vk_ffx_wanted() || !loader_load()) return;
    const VkCtx *c = vk_ctx();
    F.dev = (VkDeviceContext){c->device, c->phys, vkGetDeviceProcAddr};
    const char *js = cfg_get("TOTK_FSR_JITTER_SIGN");
    if (js && !strcmp(js, "-1")) F.jitter_sign = -1.0f;
    uint64_t count = FFX_MAX_VERSIONS, ids[FFX_MAX_VERSIONS] = {0};
    const char *names[FFX_MAX_VERSIONS] = {0};
    struct ffxQueryDescGetVersions q = {.header = {FFX_API_QUERY_DESC_TYPE_GET_VERSIONS, NULL},
                                        .createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, .device = &F.dev,
                                        .outputCount = &count, .versionIds = ids, .versionNames = names};
    ffxReturnCode_t r = F.query(NULL, &q.header);
    if (r != FFX_API_RETURN_OK || !count) {
        rt_log("fsr: no upscaler for this device (query %u, %llu versions; is amd_fidelityfx_upscaler_vk next to "
               "the game?): FSR off", (unsigned)r, (unsigned long long)count);
        return;
    }
    if (count > FFX_MAX_VERSIONS) count = FFX_MAX_VERSIONS;
    char list[512] = "";
    int best = -1, best_major = 0, best_minor = 0, best_patch = 0, want = -1;
    // HWAOI_FSR_VERSION: a version's name from the list in the log ("4.1.2", or the start of one, "3.1"), or its id in
    // hex ("0x..."), instead of the newest FSR 4; anything the library does not offer leaves the choice automatic
    const char *pin = cfg_get("TOTK_FSR_VERSION");
    if (pin && (!*pin || !strcmp(pin, "auto"))) pin = NULL;
#ifndef _WIN32
    // fsr_model on Linux picks one of the library's own version names ("4.1.1 Vulkan FP8", "... INT8"): there is no
    // AMD provider class to patch (the Windows hook, above, works on AMD's DLL), and fsr4-vk offers its models as versions
    const char *mkey = NULL, *mval = cfg_get("TOTK_FSR_MODEL");
    if (mval && !strcmp(mval, "fp8")) mkey = "FP8";
    else if (mval && !strcmp(mval, "int8")) mkey = "INT8";
    else if (mval && *mval && strcmp(mval, "auto"))
        rt_log("fsr: fsr_model = %s is not available with this library (auto, fp8 and int8 are): automatic", mval);
    if (pin) mkey = NULL;   // a pinned version wins
#endif
    for (uint64_t i = 0; i < count; i++) {
        const char *nm = names[i] ? names[i] : "?";
        size_t used = strlen(list);
        snprintf(list + used, sizeof list - used, "%s%s (id %#llx)", i ? ", " : "", nm, (unsigned long long)ids[i]);
        if (pin && want < 0 && (!strncmp(nm, pin, strlen(pin)) || (!strncmp(pin, "0x", 2) && strtoull(pin, NULL, 16) == ids[i])))
            want = (int)i;
#ifndef _WIN32
        if (mkey && want < 0 && strstr(nm, mkey)) want = (int)i;
#endif
        int ma = 0, mi = 0, pa = 0;
        if (sscanf(nm, "%d.%d.%d", &ma, &mi, &pa) < 1 || ma < 4) continue;
        if (best < 0 || ma > best_major || (ma == best_major && (mi > best_minor || (mi == best_minor && pa > best_patch))))
            best = (int)i, best_major = ma, best_minor = mi, best_patch = pa;
    }
    if (pin && want < 0) rt_log("fsr: fsr_version = %s is not among the versions offered: automatic", pin);
    if (want >= 0) best = want;   // a pinned version, FSR 4 or not
    if (best >= 0) {
        F.version_id = ids[best];
        snprintf(F.version, sizeof F.version, "FSR %s", names[best]);
    } else {
        snprintf(F.version, sizeof F.version, "FSR %s", names[0] ? names[0] : "?");   // the DLL's default
    }
    F.available = 1;
    rt_log("fsr: upscaler versions for this device: %s; using %s%s", list, F.version,
           best >= 0 ? "" : " (FSR 4 is not offered for this GPU)");
}

int vk_ffx_available(void) { return vk_ffx_wanted() && F.available; }

const char *vk_ffx_version(void) { return F.version; }


// Messages from the upscaler (errors and warnings), in UTF-16; into the log as UTF-8.
static void on_message(uint32_t type, const wchar_t *msg) {
    char s[512];
#ifdef _WIN32
    if (!msg || !WideCharToMultiByte(CP_UTF8, 0, msg, -1, s, (int)sizeof s, NULL, NULL)) return;
#else
    // wchar_t is a 32-bit code point on Linux: to UTF-8 by hand
    if (!msg) return;
    size_t n = 0;
    for (; *msg && n + 5 < sizeof s; msg++) {
        uint32_t c = (uint32_t)*msg;
        if (c < 0x80) s[n++] = (char)c;
        else if (c < 0x800) s[n++] = (char)(0xC0 | c >> 6), s[n++] = (char)(0x80 | (c & 0x3F));
        else if (c < 0x10000)
            s[n++] = (char)(0xE0 | c >> 12), s[n++] = (char)(0x80 | (c >> 6 & 0x3F)), s[n++] = (char)(0x80 | (c & 0x3F));
        else
            s[n++] = (char)(0xF0 | c >> 18), s[n++] = (char)(0x80 | (c >> 12 & 0x3F)),
            s[n++] = (char)(0x80 | (c >> 6 & 0x3F)), s[n++] = (char)(0x80 | (c & 0x3F));
    }
    s[n] = 0;
#endif
    rt_log("fsr: %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", s);
}

void *vk_ffx_create(uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, int hdr, int depth_inverted, int depth_infinite) {
    if (!F.available) return NULL;
    struct ffxOverrideVersion ov = {.header = {FFX_API_DESC_TYPE_OVERRIDE_VERSION, NULL}, .versionId = F.version_id};
    struct ffxCreateContextDescUpscaleVersion ver = {
        .header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION, F.version_id ? &ov.header : NULL},
        .version = FFX_UPSCALER_VERSION};
#ifdef _WIN32
    struct ffxCreateBackendVKDesc be = {.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK, &ver.header},
                                        .deviceContext = F.dev};
#else
    // fsr4-vk was written against the SDK's older ffx-api numbering, where the Vulkan backend description's type is the
    // plain 3 (the SDK 2.4 headers, which AMD's DLL follows, put the backend id in the high byte). The library finds the
    // description by type along the chain, so both numberings go in, with the same device: it takes whichever it knows.
    struct ffxCreateBackendVKDesc be_old = {.header = {3u, &ver.header}, .deviceContext = F.dev};
    struct ffxCreateBackendVKDesc be = {.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK, &be_old.header},
                                        .deviceContext = F.dev};
#endif
    // Linear HDR scene light with the upscaler's own exposure (native AA on the lighting target), or the game's
    // tonemapped LDR picture (upscaling in place of the game's EASU)
    // (fsr4-vk wants an exposure texture without auto exposure, so both take FSR's own)
    uint32_t flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | (hdr ? FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE : 0);
    if (depth_inverted) flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
    if (depth_infinite) flags |= FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
    if (rt_knob("FSR_DEBUG", "0")) flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;   // HWAOI_FSR_DEBUG=1: API checks
    struct ffxCreateContextDescUpscale cd = {.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, &be.header},
                                             .flags = flags, .maxRenderSize = {w, h}, .maxUpscaleSize = {ow, oh},
                                             .fpMessage = on_message};
    ffxContext ctx = NULL;
    ffxReturnCode_t r = F.create(&ctx, &cd.header, NULL);
    if (r != FFX_API_RETURN_OK || !ctx) {
        rt_log("fsr: context creation failed (%u) at %ux%u -> %ux%u", (unsigned)r, w, h, ow, oh);
        return NULL;
    }
    struct ffxQueryGetProviderVersion pv = {.header = {FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION, NULL}};
    F.query(&ctx, &pv.header);
    rt_log("fsr: context created at %ux%u -> %ux%u with %s (flags %#x)", w, h, ow, oh,
           pv.versionName ? pv.versionName : F.version, flags);
    return ctx;
}

void vk_ffx_release(void *ctx) {
    if (!ctx) return;
    ffxContext c = ctx;
    F.destroy(&c, NULL);
}

static uint32_t surface_format(VkFormat f) {
    switch (f) {
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    case VK_FORMAT_R16G16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
    case VK_FORMAT_D32_SFLOAT: return FFX_API_SURFACE_FORMAT_R32_FLOAT;
    case VK_FORMAT_R32_SFLOAT: return FFX_API_SURFACE_FORMAT_R32_FLOAT;
    case VK_FORMAT_R8_UNORM: return FFX_API_SURFACE_FORMAT_R8_UNORM;
    case VK_FORMAT_R8G8B8A8_UNORM: return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB: return FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB;
    default: return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}

// Every image of the temporal path stays in VK_IMAGE_LAYOUT_GENERAL: GENERIC_READ and UNORDERED_ACCESS are the states the
// Vulkan backend maps to that layout (ffx_vk.cpp getVKImageLayoutFromResourceState), and it puts each resource back
// into the state it was given at the end of the dispatch.
static struct FfxApiResource resource(const VkFfxImage *im, uint32_t usage, uint32_t state) {
    struct FfxApiResource r;
    memset(&r, 0, sizeof r);
    if (!im->image) return r;
    r.resource = (void *)im->image;
    r.description = (struct FfxApiResourceDescription){.type = FFX_API_RESOURCE_TYPE_TEXTURE2D,
                                                       .format = surface_format(im->format), .width = im->width,
                                                       .height = im->height, .depth = 1, .mipCount = 1, .usage = usage};
    r.state = state;
    return r;
}

int vk_ffx_dispatch(void *ctx, VkCommandBuffer cb, const VkFfxDispatch *d) {
    if (!ctx) return 0;
    ffxContext c = ctx;
    struct ffxDispatchDescUpscale u;
    memset(&u, 0, sizeof u);
    u.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    u.commandList = (void *)cb;
    u.color = resource(&d->color, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_IN_STATE);
    u.depth = resource(&d->depth, FFX_API_RESOURCE_USAGE_DEPTHTARGET, FFX_IN_STATE);
    u.motionVectors = resource(&d->mv, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_IN_STATE);
    u.reactive = resource(&d->reactive, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_IN_STATE);
    u.output = resource(&d->out, FFX_API_RESOURCE_USAGE_UAV, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    u.jitterOffset = (struct FfxApiFloatCoords2D){d->jx * F.jitter_sign, d->jy * F.jitter_sign};
    u.motionVectorScale = (struct FfxApiFloatCoords2D){1.0f, 1.0f};
    u.renderSize = (struct FfxApiDimensions2D){d->color.width, d->color.height};
    u.upscaleSize = (struct FfxApiDimensions2D){d->out.width, d->out.height};
    u.frameTimeDelta = d->dt_ms;
    u.preExposure = 1.0f;
    u.reset = d->reset != 0;
    // FSR's sharpening pass (RCAS) in place of the game's own, which vk_temporal.c turns off while FSR runs
    u.enableSharpening = d->sharpness > 0.0f;
    u.sharpness = d->sharpness;
    static int sharp_logged;
    if (!sharp_logged) sharp_logged = 1, rt_log("fsr: sharpening %s (sharpness %.2f)", u.enableSharpening ? "on" : "off", (double)d->sharpness);
    u.cameraNear = d->cam_near;
    u.cameraFar = d->cam_far;
    u.cameraFovAngleVertical = d->fov_y;
    u.viewSpaceToMetersFactor = 0.01f;   // HWAoI's world units look like centimetres (near plane 64, far 210000)
    ffxReturnCode_t r = F.dispatch(&c, &u.header);
    if (r != FFX_API_RETURN_OK) {
        rt_log("fsr: dispatch failed (%u)", (unsigned)r);
        return 0;
    }
    return 1;
}
