// Temporal AA and upscaling in the game's frame (vk_temporal.h): AMD FSR (vk_ffx.c) or NVIDIA DLSS (vk_dlss.c), as
// native anti-aliasing on the lighting target or in place of the game's own upscale. "FSR" below stands for either.
// The structure follows totk-pc-recomp's vk_dlaa.c (pass classification, camera capture, viewport jitter, camera-only
// motion vectors, recording into the replay's command buffer); the classification is HWAoI's own, from the frame
// reconnaissance in docs/hwaoi/temporal-recon.md:
//
// - G-buffer: a pass with t0 RGBA8 sRGB, t1 RGBA16 UNORM, t2 R11G11B10F and a D32S8 depth, all of one size (the
//   scene's). After it, a pass with exactly one R11G11B10F target of that size and that depth is the lighting target
//   (scene colour: opaque, sky and every transparent go into it, linear HDR, before the tonemap): it arms the frame.
// - Camera: the lighting draw's fragment shader (SPIR-V hash ad60a723207be8b6, or 1f117d51945d8d8e on frames with fewer
//   lights) has c3 with vec4 [11] = camera position and [12..15] = the view-projection (row vectors: clip = (x, y, z,
//   1) * M); the draw is recognised by that content (camera * M = (0, 0, near, 0)), not by the hash. The block moves every frame, so its bytes
//   are copied when the draw is recorded; last frame's copy is kept here.
// - Scene end: the bloom chain's first draw (fragment shader hash ea72..., one R11G11B10F target half the scene's width,
//   no depth). FSR runs right before it, in place on the lighting target, so bloom, grade, tonemap and UI see the
//   anti-aliased picture. Two earlier half-width passes (a copy after the lighting, a DoF-like effect) also read the
//   scene colour, so the width alone does not end the scene.
// - Jitter: a Halton(2,3) sub-pixel offset on the viewport of the draws that render with the scene depth (prepass,
//   G-buffer, forward effects, transparents) until the scene ends. Full-screen draws (6 vertices or fewer: lighting, sky,
//   velocity, shadow mask, composites) are not moved: they shade by window position and would sample the G-buffer off
//   by the offset.
// - Motion: the game has no velocity buffer (its rg16f pass is a scaled sky-only motion-blur term), so the vectors are
//   the camera's only (shaders/temporal_mv.comp). Characters and crowds that move on their own get no vectors of their
//   own and can trail.
// The game's RCAS (the scene sharpness override, vk_exec_draw.c) is off while FSR runs; FSR's own sharpening
// (HWAOI_SHARPNESS, 0..100) takes its place.
#include "vk_temporal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "platform/platform.h"
#include "temporal_mv_spv.h"
#include "vk.h"
#include "vk_dlss.h"
#include "vk_ffx.h"

#define BLOOM_FS_PREFIX 0xea72u          // the first bloom pass (top 16 bits of its SPIR-V hash, as passes.txt shows)
#define CAM_OFF (11 * 16)                // vec4 [11] camera position, [12..15] view-projection
#define CAM_BYTES (5 * 16)
#define CAM_BLOCK 0x600u                 // the lighting fragment c3's size
#define FMT_T0 VK_FORMAT_R8G8B8A8_SRGB
#define FMT_T1 VK_FORMAT_R16G16B16A16_UNORM
#define FMT_LIGHT VK_FORMAT_B10G11R11_UFLOAT_PACK32
#define FMT_DEPTH VK_FORMAT_D32_SFLOAT_S8_UINT
#define CAM_NEAR 64.0f                   // reversed Z, finite far (recon section 2)
#define CAM_FAR 210000.0f
#define LOG_EVERY 3600

typedef struct {
    VkImage img;
    VmaAllocation mem;
    VkImageView view;
} Img;

static struct {
    int tried, failed;
    int backend;                      // the BACKEND_* this run settled on, once tried
    // this frame
    int gbuf, armed, done, cam_ok, jit_used, ended;
    uint32_t gw, gh;                  // the G-buffer's size
    VkExecTarget color, depth;        // the armed pair
    double cam[16];                   // this frame's view-projection (row-major, rows = vec4 12..15)
    float cam_pos[4];
    // across frames
    double prev[16];
    int prev_ok, reset;
    double dlss_cam[16], dlss_prev[16];   // the cameras of the frame whose scene ended last, for DLSS
    int dlss_cam_ok, dlss_prev_ok, dlss_moving;
    VkImage last_color;
    uint32_t sw, sh;                  // the scene size FSR last ran at: draws with a D32S8 depth of that size jitter
    float jx, jy;                     // the offset this frame's scene draws take
    float ux, uy;                     // the offset this frame's scene was drawn with (FSR and the motion vectors)
    uint32_t jframe;
    int jit_ok, hold;
    uint64_t last_ns;
    // work images and FSR context for one size
    uint32_t w, h, ow, oh;
    Img mv, zcopy, out, cin;          // cin: the LDR input as RGBA16F (fsr4-vk takes only float colour)
    // The history mask (DLSS) and the characters' motion (mv_pass): this frame's depth with the characters marked
    // (hist, negative where the stencil says character), last frame's (zprev, copied from hist after the pass) and
    // its jitter, the mask made from them (bias), and a 1x1 stencil for a depth target without one (nost).
    Img zprev, bias, hist, nost;
    Img mvsr;   // the upscalers' motion vectors: mv, or far outside the picture where the history is dropped
    int zprev_ok, bias_ok;
    float pjx, pjy;
    void *ctx;
    // motion pass
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe;
    VkSampler samp;
    // counters
    uint64_t runs, frames, missed, nocam, holds;
    int presents;                     // presents since the last frame_begin
} T;

static int knob(const char *name, int def) {
    static const char *names[8];
    static int vals[8], n;
    for (int i = 0; i < n; i++)
        if (names[i] == name) return vals[i];
    int v = rt_knob(name, def ? "1" : "0");
    if (n < 8) names[n] = name, vals[n++] = v;
    return v;
}

// The upscaler of this run, fixed at start: NVIDIA DLSS through Streamline (vk_dlss.c) or AMD FSR (vk_ffx.c). Both take
// the same inputs here and run at the same two places in the frame. BACKEND_FG is no upscaler: frame generation
// (FRAME_GEN, vk_dlss.c) alone needs the scene's depth and motion vectors, so the scene is found and the motion pass
// runs, with no jitter and nothing changed in the picture; with an upscaler it takes the same two images as it does.
enum { BACKEND_NONE, BACKEND_FSR, BACKEND_DLSS, BACKEND_FG };
static int backend(void) {
    if (T.tried) return T.backend;
    return vk_dlss_wanted() ? BACKEND_DLSS : vk_ffx_wanted() ? BACKEND_FSR : vk_fg_wanted() ? BACKEND_FG : BACKEND_NONE;
}
static const char *backend_name(void) {
    int b = backend();
    return b == BACKEND_DLSS ? "DLSS" : b == BACKEND_FG ? "frame generation's motion pass" : "FSR";
}
static int backend_usable(void) {
    int b = backend();
    return b == BACKEND_DLSS ? vk_dlss_available() : b == BACKEND_FG ? vk_fg_available() : vk_ffx_available();
}

int vk_temporal_wanted(void) { return backend() != BACKEND_NONE; }

int vk_temporal_active(void) {
    if (!backend() || T.failed) return 0;
    if (!T.tried) {
        int b = backend(), usable = backend_usable();
        T.backend = b, T.tried = 1;
        if (!usable && b != BACKEND_FG)
            rt_log("temporal: TEMPORAL=%s but %s is not usable on this PC (see the %s lines above): rendering unchanged, "
                   "no jitter", cfg_value("TOTK_TEMPORAL"), backend_name(), b == BACKEND_DLSS ? "dlss:" : "fsr:");
        // without its upscaler the scene is still read when frame generation runs
        if (!usable && vk_fg_available()) T.backend = BACKEND_FG, usable = 1;
        if (!usable) {   // frame generation alone that does not run said why at start ("frame generation: off: ...")
            T.failed = 1;
            return 0;
        }
        T.jit_ok = 0, T.reset = 1;
        if (T.backend == BACKEND_FG)
            rt_log("temporal: the scene's depth and camera motion for frame generation (no upscaler, no jitter)");
        else
            rt_log("temporal: %s on the lighting target (%s); jitter %s, camera motion %s, sharpness %d%s", backend_name(),
                   backend() == BACKEND_DLSS ? vk_dlss_quality() : vk_ffx_version(),
                   knob("TEMPORAL_JITTER", 1) ? "on" : "off", knob("TEMPORAL_MV", 1) ? "on" : "off",
                   cfg_int("TOTK_SHARPNESS"), vk_fg_available() ? "; its depth and motion also go to frame generation" : "");
    }
    return 1;
}

int vk_temporal_running(void) { return backend() && backend_usable() && !T.failed; }

// 1 while the upscaler sharpens its own result (FSR's RCAS), so the game's sharpening pass stays off; DLSS has none.
int vk_temporal_sharpens(void) { return backend() == BACKEND_FSR && vk_temporal_running(); }

static void fail(const char *what) {
    rt_log("temporal: %s failed: %s off for this run (rendering unchanged from here on)", what, backend_name());
    T.failed = 1, T.jit_ok = 0;
}

// FNV-1a of the SPIR-V, as vk_dump.c names shaders in passes.txt
static uint64_t spirv_hash(const GfxShader *s) {
    static struct { const GfxShader *s; uint64_t h; } tab[4096];
    if (!s) return 0;
    uint32_t j = (uint32_t)(((uintptr_t)s >> 4) * 0x9E3779B97F4A7C15ull >> 52);
    for (uint32_t i = 0; i < 4096; i++, j = (j + 1) & 4095) {
        if (tab[j].s == s) return tab[j].h;
        if (tab[j].s) continue;
        uint64_t words = 0, h = 0xcbf29ce484222325ull;
        const uint8_t *b = (const uint8_t *)gfx_shader_spirv(s, &words);
        for (uint64_t n = 0; b && n < words * 4; n++) h = (h ^ b[n]) * 0x100000001b3ull;
        tab[j].s = s, tab[j].h = h;
        return h;
    }
    return 0;
}

// ---------------------------------------------------------------- per frame
static float halton(uint32_t i, uint32_t b) {
    float f = 1, r = 0;
    for (; i; i /= b) f /= (float)b, r += f * (float)(i % b);
    return r;
}

// The jitter moves on right after FSR ran (the replay may record the next frame's scene before this frame's present
// is handled). The sequence has 8 x (output / scene)^2 phases, 16 at least.
static void next_jitter(uint32_t w, uint32_t ow) {
    double r = w ? (double)ow / w : 1.0;
    uint32_t phases = (uint32_t)ceil(8.0 * r * r - 1e-6);
    if (phases < 16) phases = 16;
    if (phases > 72) phases = 72;
    uint32_t i = T.jframe++ % phases + 1;
    T.jx = halton(i, 2) - 0.5f, T.jy = halton(i, 3) - 0.5f;
    T.jit_ok = !T.hold && knob("TEMPORAL_JITTER", 1);
}

// A new scene: the depth prepass or a G-buffer pass after the last scene was resolved, or after two presents without
// FSR (a frame that never reached the FSR point). The present itself is no frame boundary: the replay can record the
// next frame's scene before this frame's present is handled; and the game draws depth-only and G-buffer-shaped passes
// between its lighting and its bloom, so an armed frame alone does not start a new one.
static void frame_begin(void) {
    T.frames++;
    __atomic_store_n(&T.presents, 0, __ATOMIC_RELAXED);
    if (!T.done) {
        T.missed++;
        // a jittered frame that FSR did not resolve shakes on screen: no jitter for a second
        if (T.jit_used) T.hold = 60, T.holds++;
        T.jit_ok = 0, T.reset = 1;
    } else if (T.hold) {
        T.hold--;
    }
    T.gbuf = T.armed = T.done = T.cam_ok = T.jit_used = T.ended = 0;
}

void vk_temporal_present(void) { __atomic_fetch_add(&T.presents, 1, __ATOMIC_RELAXED); }

static int same_size(const VkExecTarget *a, const VkExecTarget *b) {
    return a->image && b->image && a->width == b->width && a->height == b->height;
}

void vk_temporal_pass(VkExec *ex) {
    if (!vk_temporal_active()) return;
    const VkExecTarget *c = ex->color, *z = &ex->depth;
    int scene_depth = z->image && z->format == FMT_DEPTH && !z->level && z->width >= 320;
    if (scene_depth && (T.done || __atomic_load_n(&T.presents, __ATOMIC_RELAXED) >= 2) &&
        (ex->pass_ncolor == 0 || (ex->pass_ncolor == 3 && c[0].format == FMT_T0)))
        frame_begin();
    if (T.done) return;
    if (ex->pass_ncolor == 3 && z->image && z->format == FMT_DEPTH && c[0].format == FMT_T0 && c[1].format == FMT_T1 &&
        c[2].format == FMT_LIGHT && same_size(&c[0], z) && same_size(&c[1], z) && same_size(&c[2], z) &&
        !c[0].level && z->width >= 320) {
        T.gbuf = 1, T.gw = z->width, T.gh = z->height;
        return;
    }
    if (T.gbuf && ex->pass_ncolor == 1 && c[0].image && c[0].format == FMT_LIGHT && !c[0].level && !c[0].is3d &&
        c[0].samples <= 1 && z->image && z->format == FMT_DEPTH && c[0].width == T.gw && c[0].height == T.gh &&
        same_size(&c[0], z)) {
        if (!T.armed || T.color.image != c[0].image) T.cam_ok = 0;
        T.color = c[0], T.depth = *z, T.armed = 1;
    }
}

// ---------------------------------------------------------------- work images, motion pass
static int image2d(uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags usage, Img *im) {
    const VkCtx *c = vk_ctx();
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ai, &im->img, &im->mem, NULL) != VK_SUCCESS) return 0;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    return vkCreateImageView(c->device, &vi, NULL, &im->view) == VK_SUCCESS;
}

static void free_img(Img *im) {
    const VkCtx *c = vk_ctx();
    if (im->view) vkDestroyImageView(c->device, im->view, NULL);
    if (im->img) vmaDestroyImage(c->vma, im->img, im->mem);
    memset(im, 0, sizeof *im);
}

static void full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

// The work images and FSR's context for a render and output size (a new size frees the old ones).
static int size_for(VkCommandBuffer cb, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, int hdr) {
    if (T.ctx && T.w == w && T.h == h && T.ow == ow && T.oh == oh) return 1;   // the mode (hdr) follows the sizes
    if (T.ctx) {
        vk_device_idle(vk_ctx()->device);
        if (backend() == BACKEND_FSR) vk_ffx_release(T.ctx);
        T.ctx = NULL;
        free_img(&T.mv), free_img(&T.zcopy), free_img(&T.out), free_img(&T.cin);
        free_img(&T.zprev), free_img(&T.bias), free_img(&T.hist), free_img(&T.nost), free_img(&T.mvsr);
    }
    VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    int upscaler = backend() != BACKEND_FG;   // frame generation alone: the motion vectors and the depth copy only
    if (!image2d(w, h, VK_FORMAT_R16G16_SFLOAT, u, &T.mv) || !image2d(w, h, VK_FORMAT_R32_SFLOAT, u, &T.zcopy) ||
        (upscaler && !image2d(ow, oh, VK_FORMAT_R16G16B16A16_SFLOAT, u, &T.out)) ||
        (upscaler && !hdr && !image2d(w, h, VK_FORMAT_R16G16B16A16_SFLOAT, u, &T.cin)) ||
        !image2d(w, h, VK_FORMAT_R32_SFLOAT, u, &T.zprev) || !image2d(w, h, VK_FORMAT_R8_UNORM, u, &T.bias) ||
        !image2d(w, h, VK_FORMAT_R32_SFLOAT, u, &T.hist) || !image2d(w, h, VK_FORMAT_R16G16_SFLOAT, u, &T.mvsr) ||
        !image2d(1, 1, VK_FORMAT_R8_UINT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &T.nost))
        return 0;
    T.zprev_ok = T.bias_ok = 0;
    VkImageMemoryBarrier2 ib[9];
    VkImage im[9] = {T.mv.img, T.zcopy.img, T.zprev.img, T.bias.img, T.hist.img, T.nost.img, T.mvsr.img, T.out.img,
                     T.cin.img};
    uint32_t nim = !upscaler ? 7 : T.cin.img ? 9 : 8;
    for (uint32_t i = 0; i < nim; i++)
        ib[i] = (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im[i],
                                        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = nim,
                           .pImageMemoryBarriers = ib};
    vkCmdPipelineBarrier2(cb, &di);
    // DLSS keeps its state inside Streamline, per size (aod_dlss.cpp): only the work images are ours
    T.ctx = backend() != BACKEND_FSR ? (void *)&T : vk_ffx_create(w, h, ow, oh, hdr, 1, 0);
    if (!T.ctx) return 0;
    T.w = w, T.h = h, T.ow = ow, T.oh = oh, T.reset = 1;
    return 1;
}

static int mv_init(void) {
    if (T.pipe) return 1;
    const VkCtx *c = vk_ctx();
    VkDescriptorSetLayoutBinding b[8] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 8, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 112};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                              .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                   .codeSize = sizeof temporal_mv_spv, .pCode = temporal_mv_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(c->device, &dci, NULL, &T.dsl) != VK_SUCCESS) return 0;
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &T.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (vkCreatePipelineLayout(c->device, &li, NULL, &T.layout) != VK_SUCCESS ||
        vkCreateSampler(c->device, &si, NULL, &T.samp) != VK_SUCCESS ||
        vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS)
        return 0;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = T.layout};
    VkResult r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &T.pipe);
    vkDestroyShaderModule(c->device, m, NULL);
    return r == VK_SUCCESS;
}

// Gauss-Jordan inverse in double precision (totk vk_dlaa.c inv4)
static int inv4(const double m[16], double o[16]) {
    double a[4][8];
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 8; k++) a[r][k] = k < 4 ? m[r * 4 + k] : (k - 4 == r);
    for (int col = 0; col < 4; col++) {
        int piv = col;
        for (int r = col + 1; r < 4; r++)
            if (fabs(a[r][col]) > fabs(a[piv][col])) piv = r;
        if (fabs(a[piv][col]) < 1e-300) return 0;
        for (int k = 0; k < 8; k++) { double t = a[col][k]; a[col][k] = a[piv][k]; a[piv][k] = t; }
        double d = a[col][col];
        for (int k = 0; k < 8; k++) a[col][k] /= d;
        for (int r = 0; r < 4; r++)
            if (r != col) {
                double f = a[r][col];
                for (int k = 0; k < 8; k++) a[r][k] -= f * a[col][k];
            }
    }
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) o[r * 4 + k] = a[r][k + 4];
    return 1;
}

// The motion pass: reprojection R = inverse(this VP) * last VP (row vectors), the depth copied to R32F. 0: no motion
// (no camera now or last frame; the vectors are zero and the history restarts).
// The relative difference of distance from which last frame's surface counts as another one (temporal_mv.comp).
// 0.03: a hand in front of a body differs by more, a slope seen at a low angle by less inside its 3x3 pixels.
#define BIAS_TOLERANCE 0.03f

// sv: the scene depth's stencil (VK_NULL_HANDLE: the target has none, and characters get the camera's motion).
// TEMPORAL_CHARS: the stencil bits of a character (HWAoI's G-buffer writes 1 for characters, 2 for the rest of the
// scene; seen in a frame dump on 2026-11-23); 0 gives every pixel the camera's motion, as before.
static int mv_pass(VkExec *ex, VkImageView zv, VkImageView sv, uint32_t w, uint32_t h, float jx, float jy) {
    VkCommandBuffer cb = ex->cb;
    double r[16], ci[16];
    int moving = T.cam_ok && T.prev_ok && knob("TEMPORAL_MV", 1) && inv4(T.cam, ci);
    for (int i = 0; i < 16; i++) r[i] = (i % 5 == 0);
    if (moving)
        for (int i = 0; i < 4; i++)
            for (int k = 0; k < 4; k++) {
                double v = 0;
                for (int j = 0; j < 4; j++) v += ci[i * 4 + j] * T.prev[j * 4 + k];
                r[i * 4 + k] = v;
            }
    // The characters' motion (every backend: DLSS, FSR, frame generation) and the history mask (DLSS only), with a
    // camera for both frames and last frame's depth at this size. DLSS_BIAS=0 leaves the mask out. Two tests: ab, a
    // second with the characters' motion and the mask and a second without, by the present's number (for HWAOI_SHOTS);
    // all, every pixel marked (DLSS then keeps no history).
    static int mode = -1, chars = -1, drop = 1;
    if (mode < 0) {
        const char *e = cfg_get("TOTK_DLSS_BIAS");
        mode = !e || !*e ? 1 : !strcmp(e, "0") ? 0 : !strcmp(e, "ab") ? 2 : !strcmp(e, "all") ? 3 : 1;
        if (mode != 1) rt_log("temporal: DLSS history mask: %s", mode == 0 ? "off" : mode == 2 ? "a second on, a second off" : "every pixel");
        e = cfg_get("TOTK_TEMPORAL_CHARS");
        chars = e && *e ? (int)(strtol(e, NULL, 0) & 0xff) : 1;
        drop = knob("TEMPORAL_DROP", 1);
        rt_log("temporal: characters %s; a history that is another surface's is %s", chars ? "get their own motion (stencil bit 1)" : "get the camera's motion only",
               drop ? "dropped (motion out of the picture)" : "kept");
    }
    int on = mode == 2 ? (int)((__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) / 60) & 1) : 1;
    int want = backend() == BACKEND_DLSS && mode != 0 && on, all = mode == 3;
    int history = all ? 2 : moving && T.zprev_ok && !T.reset;
    T.bias_ok = want || all;
    struct { float r[16], size[2], jitter[2], pjitter[2], history, tolerance; uint32_t chars, drop; } pc;
    for (int i = 0; i < 16; i++) pc.r[i] = (float)r[i];
    pc.size[0] = (float)w, pc.size[1] = (float)h, pc.jitter[0] = jx, pc.jitter[1] = jy;
    pc.pjitter[0] = T.pjx, pc.pjitter[1] = T.pjy, pc.history = (float)history, pc.tolerance = BIAS_TOLERANCE;
    pc.chars = sv && on ? (uint32_t)chars : 0;
    pc.drop = (uint32_t)drop;
    VkDescriptorImageInfo zi = {T.samp, zv, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo mi = {VK_NULL_HANDLE, T.mv.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo di = {VK_NULL_HANDLE, T.zcopy.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo pi = {T.samp, T.zprev.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo bi = {VK_NULL_HANDLE, T.bias.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo si = {T.samp, sv ? sv : T.nost.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo hi = {VK_NULL_HANDLE, T.hist.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo ri = {VK_NULL_HANDLE, T.mvsr.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr[8] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &zi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &mi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &di},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 3, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &pi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 4, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &bi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 5, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &si},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 6, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &hi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 7, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &ri}};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, T.pipe);
    rec_push(vk_pipeline_push_fn(), cb, VK_PIPELINE_BIND_POINT_COMPUTE, T.layout, 0, 8, wr);
    vkCmdPushConstants(cb, T.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    full_barrier(cb);
    // This frame's depth with its characters is next frame's "last frame" (FSR and Streamline read T.zcopy).
    VkImageCopy whole = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                         .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {w, h, 1}};
    vkCmdCopyImage(cb, T.hist.img, VK_IMAGE_LAYOUT_GENERAL, T.zprev.img, VK_IMAGE_LAYOUT_GENERAL, 1, &whole);
    full_barrier(cb);
    T.zprev_ok = 1, T.pjx = jx, T.pjy = jy;
    ex->recorded += 7;
    return moving;
}

// ---------------------------------------------------------------- the scene's end and FSR
// The scene is drawn (the bloom's first draw): the motion vectors and the R32F depth are made from the scene depth
// now, whether FSR runs right away (native) or at the game's EASU (upscaling), where that depth may be reused.
static int scene_end(VkExec *ex, uint32_t ow, uint32_t oh, int hdr) {
    const VkExecTarget *t = &T.color, *z = &T.depth;
    uint32_t w = t->width, h = t->height;
    vk_exec_barrier(ex);   // ends the pass: what drew the scene is done
    VkCommandBuffer cb = ex->cb;
    T.ended = 1;
    if (!mv_init()) return fail("motion pass setup"), 0;
    if (!size_for(cb, w, h, ow, oh, hdr)) return fail("work image or FSR context creation"), 0;
    if (T.last_color != t->image) T.reset = 1, T.last_color = t->image;
    T.ux = T.jit_used ? T.jx : 0.0f, T.uy = T.jit_used ? T.jy : 0.0f;
    VkImageView zv = vk_exec_target_view(z->img, z->format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT, z->level,
                                         z->base_layer, 1, 0);
    // DLSS also takes the two cameras themselves (vk_dlss.c); T.prev becomes this frame's below
    T.dlss_cam_ok = T.cam_ok, T.dlss_prev_ok = T.cam_ok && T.prev_ok;
    if (T.cam_ok) memcpy(T.dlss_cam, T.cam, sizeof T.dlss_cam);
    if (T.dlss_prev_ok) memcpy(T.dlss_prev, T.prev, sizeof T.dlss_prev);
    // the stencil, where the depth target has one: it marks the characters (mv_pass)
    VkImageView sv = z->format == VK_FORMAT_D32_SFLOAT_S8_UINT || z->format == VK_FORMAT_D24_UNORM_S8_UINT ||
                             z->format == VK_FORMAT_D16_UNORM_S8_UINT
                         ? vk_exec_target_view(z->img, z->format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_STENCIL_BIT,
                                               z->level, z->base_layer, 1, 0)
                         : VK_NULL_HANDLE;
    T.dlss_moving = mv_pass(ex, zv, sv, w, h, T.ux, T.uy);
    if (!T.dlss_moving) {
        if (!T.cam_ok) T.nocam++;
        if (!T.cam_ok || !T.prev_ok) T.reset = 1;   // TEMPORAL_MV=0 (a test) keeps the history
    }
    if (knob("TEMPORAL_LOG", 0)) {   // stretches of a still camera, by present number (for HWAOI_SHOTS)
        static uint64_t still, from;
        uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
        int same = T.cam_ok && T.prev_ok && !memcmp(T.prev, T.cam, sizeof T.cam);
        if (same && !still++) from = f;
        if (!same && still >= 30) rt_log("temporal: still camera at presents %llu-%llu", (unsigned long long)from,
                                         (unsigned long long)f);
        if (!same) still = 0;
    }
    if (T.cam_ok) memcpy(T.prev, T.cam, sizeof T.prev);
    T.prev_ok = T.cam_ok;
    T.sw = w, T.sh = h;
    // Frame generation takes this frame's cameras, depth and motion vectors now: Streamline copies the two images
    // inside the command buffer, since the next frame's scene may overwrite them before this frame is shown.
    if (vk_fg_available()) {
        const VkImageUsageFlags work = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkDlssDispatch fd = {.depth = {T.zcopy.img, T.zcopy.view, VK_FORMAT_R32_SFLOAT, w, h, work},
                             .mv = {T.mv.img, T.mv.view, VK_FORMAT_R16G16_SFLOAT, w, h, work},
                             .jx = T.ux, .jy = T.uy, .vp = T.dlss_cam_ok ? T.dlss_cam : NULL,
                             .prev_vp = T.dlss_prev_ok ? T.dlss_prev : NULL, .reset = T.reset, .moving = T.dlss_moving};
        rec_drain();   // Streamline records into the command buffer itself
        vk_fg_scene(cb, &fd);
        full_barrier(cb);
        ex->recorded += 3;
    }
    vk_exec_draw_new_cb();   // the motion pass bound a pipeline of its own
    ex->dirty |= VK_EXEC_DIRTY_ALL;
    return 1;
}

// Frame generation without an upscaler: nothing in the picture changes, the frame is done once its depth and motion
// vectors are handed over.
static void fg_only_done(void) {
    T.reset = 0, T.done = 1;
    if (++T.runs == 1 || T.runs == 300 || T.runs % LOG_EVERY == 0)
        rt_log("temporal: %llu scenes for frame generation at %ux%u (%llu frames without the scene's end, %llu without a "
               "camera; camera %.0f %.0f %.0f)", (unsigned long long)T.runs, T.w, T.h, (unsigned long long)T.missed,
               (unsigned long long)T.nocam, (double)T.cam_pos[0], (double)T.cam_pos[1], (double)T.cam_pos[2]);
}

// FSR from color (the render size) into T.out (RGBA16F, the output size), then T.out blitted into dst: the lighting
// target (R11G11B10F), or the game's upscaled 8-bit image. fsr4-vk takes only float colour, so an 8-bit input
// (upscaling) is blitted into T.cin first; sRGB images decode there and encode again into dst.
static void fsr(VkExec *ex, VkImage color, VkFormat cf, VkImage dst, uint32_t level, uint32_t layer, int copy) {
    VkCommandBuffer cb = ex->cb;
    uint32_t w = T.w, h = T.h, ow = T.ow, oh = T.oh;
    uint64_t now = plat_now_ns();
    double dt = T.last_ns ? (double)(now - T.last_ns) / 1e6 : 0;
    T.last_ns = now;
    int sharp = cfg_int("TOTK_SHARPNESS");
    VkFfxDispatch d = {.color = {color, cf, w, h}, .depth = {T.zcopy.img, VK_FORMAT_R32_SFLOAT, w, h},
                       .mv = {T.mvsr.img, VK_FORMAT_R16G16_SFLOAT, w, h},
                       .out = {T.out.img, VK_FORMAT_R16G16B16A16_SFLOAT, ow, oh},
                       .jx = T.ux, .jy = T.uy, .dt_ms = dt > 0 && dt < 100 ? (float)dt : 16.67f, .cam_near = CAM_NEAR,
                       .cam_far = CAM_FAR, .fov_y = 41.0f * 3.14159265f / 180.0f, .reset = T.reset,
                       .sharpness = sharp <= 0 ? 0.0f : sharp >= 100 ? 1.0f : (float)sharp / 100.0f};
    vk_exec_barrier(ex);
    if (copy) {
        VkImageBlit b = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                         .srcOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}},
                         .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                         .dstOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}}};
        vkCmdBlitImage(cb, color, VK_IMAGE_LAYOUT_GENERAL, T.cin.img, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_NEAREST);
        full_barrier(cb);
        ex->recorded += 2;
        d.color = (VkFfxImage){T.cin.img, VK_FORMAT_R16G16B16A16_SFLOAT, w, h};
    }
    rec_drain();   // the upscaler records into the command buffer itself: the recorder thread must have made every call so far
    if (backend() == BACKEND_DLSS) {
        const VkImageUsageFlags work = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        // copy: the picture was blitted into T.cin; else the lighting target itself, through a view of its level 0
        VkImageView cv = copy ? T.cin.view
                              : vk_exec_target_view(T.color.img, T.color.format, VK_IMAGE_VIEW_TYPE_2D,
                                                    VK_IMAGE_ASPECT_COLOR_BIT, T.color.level, T.color.base_layer, 1, 0);
        VkDlssDispatch dd = {
            .color = {d.color.image, cv, d.color.format, w, h,
                      copy ? work : T.color.img->usage},
            .depth = {T.zcopy.img, T.zcopy.view, VK_FORMAT_R32_SFLOAT, w, h, work},
            .mv = {T.mvsr.img, T.mvsr.view, VK_FORMAT_R16G16_SFLOAT, w, h, work},
            .out = {T.out.img, T.out.view, VK_FORMAT_R16G16B16A16_SFLOAT, ow, oh, work},
            .bias = {T.bias_ok ? T.bias.img : VK_NULL_HANDLE, T.bias.view, VK_FORMAT_R8_UNORM, w, h, work},
            .jx = T.ux, .jy = T.uy, .vp = T.dlss_cam_ok ? T.dlss_cam : NULL, .prev_vp = T.dlss_prev_ok ? T.dlss_prev : NULL,
            .reset = T.reset, .hdr = !copy, .moving = T.dlss_moving};
        if (!cv || !vk_dlss_dispatch(cb, &dd)) return fail("DLSS evaluation");
    } else if (!vk_ffx_dispatch(T.ctx, cb, &d)) {
        return fail("FSR dispatch");
    }
    full_barrier(cb);
    // TEMPORAL_DEBUG=1 (native): the motion vectors (x red, y green, render pixels) in place of the picture
    // ... and TEMPORAL_DEBUG=mask: the history mask (red where DLSS is told to drop its history)
    int dbg = !copy && knob("TEMPORAL_DEBUG", 0);
    static int dbg_mask = -1;
    if (dbg_mask < 0) {
        const char *e = cfg_get("TOTK_TEMPORAL_DEBUG");
        dbg_mask = e && !strcmp(e, "mask");
    }
    {
        VkImageBlit b = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                         .srcOffsets = {{0, 0, 0}, {(int32_t)ow, (int32_t)oh, 1}},
                         .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1},
                         .dstOffsets = {{0, 0, 0}, {(int32_t)ow, (int32_t)oh, 1}}};
        VkImage shown = dbg && dbg_mask && T.bias_ok ? T.bias.img : dbg ? T.mv.img : T.out.img;
        vkCmdBlitImage(cb, shown, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                       VK_FILTER_NEAREST);
    }
    full_barrier(cb);
    ex->recorded += 4;
    vk_exec_draw_new_cb();   // FSR bound pipelines and descriptors of its own: the next draw binds everything again
    ex->dirty |= VK_EXEC_DIRTY_ALL;
    T.reset = 0, T.done = 1;
    next_jitter(w, ow);
    if (++T.runs == 1 || T.runs == 300 || T.runs % LOG_EVERY == 0)
        rt_log("temporal: %llu frames through %s at %ux%u -> %ux%u (%llu frames without the upscaling point, %llu "
               "without a camera, %llu jitter holds; last jitter %.3f %.3f, camera %.0f %.0f %.0f)",
               (unsigned long long)T.runs, backend_name(), w, h, ow, oh, (unsigned long long)T.missed,
               (unsigned long long)T.nocam,
               (unsigned long long)T.holds, (double)T.ux, (double)T.uy, (double)T.cam_pos[0], (double)T.cam_pos[1],
               (double)T.cam_pos[2]);
}

int vk_temporal_draw(VkExec *ex, const GfxShader *fs, int32_t count) {
    if (!vk_temporal_active()) return 0;
    if (T.done) return 0;
    uint64_t fh = fs ? spirv_hash(fs) : 0;
    // The camera: the lighting draws (fragment shaders ad60a723207be8b6 and, with fewer lights, 1f117d51945d8d8e,
    // whose c3 blocks share the layout) are full-screen draws into the armed target. Any such draw's c3 is taken when
    // its vec4 [11] is a point and [12..15] a view-projection that maps it to clip (0, 0, near, 0), as the camera does.
    if (T.armed && !T.cam_ok && count <= 6 && ex->color[0].image == T.color.image && ex->depth.image &&
        ex->ubo[1][0].size >= 0x100) {
        float v[CAM_BYTES / 4];
        if (vk_exec_ubo_read(ex, 1, 0, CAM_OFF, v, sizeof v) && v[3] == 1.0f) {
            double p[4];
            for (int k = 0; k < 4; k++) p[k] = v[0] * v[4 + k] + v[1] * v[8 + k] + v[2] * v[12 + k] + v[16 + k];
            if (fabs(p[0]) < 0.5 && fabs(p[1]) < 0.5 && fabs(p[3]) < 0.5 && fabs(p[2] - CAM_NEAR) < 1.0) {
                for (int i = 0; i < 16; i++) T.cam[i] = v[4 + i];
                memcpy(T.cam_pos, v, sizeof T.cam_pos);
                T.cam_ok = 1;
                static uint64_t told[4];
                for (int i = 0; i < 4 && told[i] != fh; i++)
                    if (!told[i]) {
                        told[i] = fh;
                        rt_log("temporal: camera from the lighting draw of fragment shader %016llx (c3 %#llx bytes)",
                               (unsigned long long)fh, (unsigned long long)ex->ubo[1][0].size);
                        break;
                    }
            }
        }
    }
    if (T.armed && fh >> 48 == BLOOM_FS_PREFIX && !ex->depth.image && ex->pass_ncolor == 1 &&
        ex->color[0].format == FMT_LIGHT && ex->color[0].width * 2 + 2 >= T.color.width &&
        ex->color[0].width * 2 <= T.color.width + 2) {
        if (T.ended) return 0;
        unsigned o[2], s[2];
        res_sizes(o, s);
        int open = ex->in_pass, native = T.color.width >= o[0];
        if (T.color.level || T.color.base_layer) fail("a lighting target that is not level 0 / layer 0");
        // native: FSR in place now; upscaling: FSR runs in place of the game's EASU (vk_temporal_dispatch)
        else if (scene_end(ex, native ? T.color.width : o[0], native ? T.color.height : o[1], native)) {
            if (backend() == BACKEND_FG) fg_only_done();
            else if (native) fsr(ex, T.color.image, T.color.format, T.color.image, 0, 0, 0);
        }
        if (open) vk_exec_pass_begin(ex);
        return 0;
    }
    int jit = T.jit_ok && !T.ended && count > 6 && ex->depth.image && ex->depth.format == FMT_DEPTH &&
              ex->depth.width == T.sw && ex->depth.height == T.sh;
    if (jit) T.jit_used = 1;
    return jit;
}

void vk_temporal_viewport(float *x, float *y) { *x += T.jx, *y += T.jy; }

// The game's FSR 1 EASU at a scene smaller than the output (cs 7f87acdabd353565: (ow/16) x (oh/16) groups, the
// tonemapped scene sampled, the output-size picture stored) is replaced by FSR on the same input. The input is the
// game's LDR picture after its tonemap, with the world UI already in it (FSR without HDR flags). RCAS and the HUD
// follow unchanged.
int vk_temporal_dispatch(VkExec *ex, const GfxShader *cs, const VkPipeInfo *info, const int32_t g[3]) {
    if (!vk_temporal_active() || T.done || !T.ended || !T.ctx || !info) return 0;
    unsigned o[2], s[2];
    res_sizes(o, s);
    if (T.sw >= o[0] || T.ow != o[0] || T.oh != o[1] || g[2] != 1 || (uint32_t)g[0] != (o[0] + 15) / 16 ||
        (uint32_t)g[1] != (o[1] + 15) / 16)
        return 0;
    const VkTexImage *in = NULL, *out = NULL;
    int n_in = 0, n_out = 0;
    for (uint32_t i = 0; i < info->n_bindings; i++) {
        const VkPipeBinding *bd = &info->bindings[i];
        if (!bd->res || (bd->kind != GFX_RES_TEX && bd->kind != GFX_RES_IMG)) continue;
        int img = bd->kind == GFX_RES_IMG, slot = bd->res->slot;
        uint64_t hd = img ? (slot >= 0 && slot < 32 ? ex->img[5][slot] : 0) : (slot >= 0 && slot < 128 ? ex->tex[5][slot] : 0);
        view_t v;
        int hv = 0;
        const VkTexImage *t = hd ? vk_texture_by_id(&ex->tex_pool, NVN_HANDLE_TEX(hd), &v, &hv) : NULL;
        if (!t || (hv && ((v.set_levels && v.base_level) || (v.set_layers && v.min_layer)))) continue;
        if (img) out = t, n_out++;
        else in = t, n_in++;
    }
    static int told;
    uint64_t hash = spirv_hash(cs);
    int fmt_ok = in && out && (in->format == VK_FORMAT_R8G8B8A8_SRGB || in->format == VK_FORMAT_R8G8B8A8_UNORM) &&
                 (out->format == VK_FORMAT_R8G8B8A8_SRGB || out->format == VK_FORMAT_R8G8B8A8_UNORM);
    if (n_in != 1 || n_out != 1 || in->width != T.sw || in->height != T.sh || out->width < o[0] || out->height < o[1] ||
        !fmt_ok) {
        if (told++ < 4)
            rt_log("temporal: a dispatch of cs %016llx at %dx%d groups is not the expected EASU (%d inputs %ux%u fmt %d, "
                   "%d outputs %ux%u fmt %d): the game's own runs", (unsigned long long)hash, g[0], g[1], n_in,
                   in ? in->width : 0, in ? in->height : 0, in ? (int)in->format : 0, n_out, out ? out->width : 0,
                   out ? out->height : 0, out ? (int)out->format : 0);
        return 0;
    }
    if (!told) {
        told = 1;
        rt_log("temporal: %s replaces the game's EASU (cs %016llx): %ux%u fmt %d -> %ux%u of %ux%u fmt %d", backend_name(),
               (unsigned long long)hash, T.sw, T.sh, (int)in->format, o[0], o[1], out->width, out->height, (int)out->format);
    }
    fsr(ex, in->image, in->format, out->image, 0, 0, 1);
    return T.done;
}
