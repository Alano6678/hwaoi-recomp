// NVIDIA DLSS as a backend of the temporal path (vk_temporal.c): DLAA at the scene's own size (TEMPORAL=dlss with
// DLSS_QUALITY=native, or the older TEMPORAL=dlaa) and DLSS Super Resolution below it. Streamline itself is in
// aod_dlss.cpp; this file holds the settings, where the files are and the per-frame inputs.
// Frame generation (FRAME_GEN = 2..6, the frame multiplier; DLSS-G through the same Streamline, with the player's
// nvngx_dlssg.dll) rides on the same path: the scene's depth and motion vectors go to Streamline at the scene's end,
// whether an upscaler runs or not, and the present shows the generated frames (gfx_present.c).
#pragma once
#include "vk.h"

int vk_dlss_wanted(void);            // TEMPORAL = dlss or dlaa at start (the device may still lack DLSS)
// DLSS_QUALITY for the scene size: "native" (DLAA; always with TEMPORAL=dlaa), "quality", "balanced", "performance"
const char *vk_dlss_quality(void);
void vk_dlss_started(int asked);     // vk_device.c: whether DLSS was asked for when the device was made
int vk_dlss_available(void);         // Streamline runs DLSS on this device
int vk_dlss_failed(void);            // asked for at start but not running (no RTX card, a DLL missing, Streamline failed)
int vk_dlss_files_present(void);     // Streamline's DLLs and the player's nvngx_dlss.dll are in place
int vk_dlss_offered(void);           // the menu offers DLSS: an NVIDIA GPU and the files in place
// Where Streamline's DLLs are: TOTK_DLSS_DIR, else the streamline folder beside the executable. 0 when no path fits.
int vk_dlss_dir(char *dir, size_t cap);
// Where the player's nvngx_dlss.dll and nvngx_dlssg.dll may be, when not with Streamline's: the folders of
// TOTK_DLSS_NGX and TOTK_FRAME_GEN_NGX (each a file or its folder; the launcher sets them), and the ngx folder beside
// the executable if it has one of the two (RecompLauncher's guide names <game folder>/ngx). The folders, each once,
// separated by '|' (no path holds one on Windows, the only system with Streamline). 0: none.
int vk_dlss_ngx_dir(char *dirs, size_t cap);

int vk_fg_wanted(void);              // FRAME_GEN at start: the multiplier 2..6, 0 when off
void vk_fg_started(int asked);       // vk_device.c: whether frame generation was asked for when the device was made
int vk_fg_available(void);           // DLSS-G runs on this device: the swapchain and the presents are Streamline's
int vk_fg_failed(void);              // asked for at start but not running (no RTX 40/50 card, a file missing, ...)
int vk_fg_files_present(void);       // Streamline's frame generation DLLs and the player's nvngx_dlssg.dll are in place
int vk_fg_offered(void);             // the menu offers it: an NVIDIA GPU and the files in place
int vk_fg_factor(void);              // the multiplier in use, 2..6; 0 while it does not run
int vk_fg_max(void);                 // the largest one this GPU reaches; 0 while it does not run
double vk_fg_rate(void);             // frames shown per frame the game drew, over the last half second (1: none added)

typedef struct {   // an image in VK_IMAGE_LAYOUT_GENERAL, level 0 and layer 0 of view
    VkImage image;
    VkImageView view;
    VkFormat format;
    uint32_t width, height;
    VkImageUsageFlags usage;
} VkDlssImage;

typedef struct {
    VkDlssImage color, depth, mv, out;   // depth: the R32F copy of the scene depth (reversed Z); mv: R16G16F
    VkDlssImage bias;                    // R8, 1 where the history is another surface's; no image: no mask
    float jx, jy;                        // this frame's jitter, render pixels (window x right, y down)
    const double *vp, *prev_vp;          // view-projections (row major, row vectors); NULL when unknown
    int reset;                           // a new history
    int hdr;                             // color is linear HDR, else the tone-mapped picture
    int moving;                          // mv holds the camera's motion (else zeros)
} VkDlssDispatch;

// Records DLSS into cb, outside any render pass. 0 on failure (logged). Streamline binds pipelines and descriptors of
// its own.
int vk_dlss_dispatch(VkCommandBuffer cb, const VkDlssDispatch *d);
// The scene of the game's current frame for frame generation: the cameras, the jitter and reset of d, and d->depth
// and d->mv, which Streamline copies inside cb (outside any render pass); d->color and d->out are not read. Before
// vk_dlss_dispatch of the same frame, which then shares the camera and the two images. 1 = taken.
int vk_fg_scene(VkCommandBuffer cb, const VkDlssDispatch *d);
