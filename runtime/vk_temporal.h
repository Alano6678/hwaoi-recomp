// Temporal anti-aliasing / upscaling in HWAoI's frame (vk_temporal.c, HWAOI_TEMPORAL=fsr): AMD FSR through vk_ffx.c on
// the lighting target, with a sub-pixel jitter on the scene's geometry draws and camera motion vectors from depth.
// The replay calls these hooks from vk_exec.c and vk_exec_draw.c; all run on the replaying thread.
#pragma once
#include "vk_exec.h"

int vk_temporal_wanted(void);   // TEMPORAL = fsr, dlss or dlaa (fixed for the run)
int vk_temporal_active(void);   // wanted and its upscaler is usable on this device, not failed
int vk_temporal_running(void);  // the same without its first-call log
int vk_temporal_sharpens(void); // running with an upscaler that sharpens its own result (FSR; DLSS does not)
// vk_exec_pass_begin, after the targets are resolved: the G-buffer and the lighting target arm the frame
void vk_temporal_pass(VkExec *ex);
// prepare_draw, once the draw's shaders are known (the pass is open): the camera from the lighting draw, and at the
// bloom chain's first draw FSR runs on the lighting target (the pass is ended and begun again around it). Returns 1
// when this draw takes the jitter (record_fixed then calls vk_temporal_viewport).
int vk_temporal_draw(VkExec *ex, const GfxShader *fs, int32_t count);
void vk_temporal_viewport(float *x, float *y);   // adds this frame's jitter to a jittered draw's viewport origin
// op_dispatch, before the game's compute dispatch is bound: 1 when vk_temporal.c recorded a replacement for it (the
// game's FSR 1 EASU at a scene smaller than the output) and the dispatch is to be skipped
int vk_temporal_dispatch(VkExec *ex, const GfxShader *cs, const VkPipeInfo *info, const int32_t groups[3]);
void vk_temporal_present(void);   // vk_exec_frame_log: a frame ended
// vk_exec_draw.c: bytes [off, off + n) of the uniform block bound at slot (c3 = 0) of stage as the replay sees it now;
// 0 when unbound or too short
int vk_exec_ubo_read(const VkExec *ex, int stage, int slot, uint64_t off, void *out, size_t n);
