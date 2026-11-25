// NVIDIA DLSS through Streamline, loaded at run time: DLAA (DLSS at the scene's own size), DLSS Super Resolution (a
// smaller scene scaled to the output) and DLSS Frame Generation (frames between the rendered ones, with Reflex). C
// interface of aod_dlss.cpp; vk_device.c calls the set-up part, gfx_present.c the swapchain and the present,
// vk_dlss.c the evaluation and the scene's inputs for the temporal path (vk_temporal.c), vk_backend.c the frame marks.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

// What a run asks Streamline for (aod_dlss_init).
enum {
    AOD_SL_DLSS = 1,   // DLAA and DLSS Super Resolution: sl.dlss.dll and the player's nvngx_dlss.dll
    AOD_SL_FG = 2,     // frame generation: sl.dlss_g.dll, sl.reflex.dll, sl.pcl.dll and the player's nvngx_dlssg.dll
};

// An image DLSS reads or writes. It is in VK_IMAGE_LAYOUT_GENERAL whenever an evaluation runs.
typedef struct {
    VkImage image;
    VkImageView view;            // depth: a view of the depth aspect only
    VkFormat format;
    uint32_t width, height;
    VkImageUsageFlags usage;
    VkImageAspectFlags aspect;
} AodDlssImage;

// One frame's inputs. Matrices are row major for row vectors (Streamline's convention: p' = p * M) and carry no jitter.
typedef struct {
    AodDlssImage color_in, color_out, depth, mvec;   // color_in, depth and mvec at the render size, color_out the output's
    AodDlssImage bias;           // optional (image 0: none): 1 where DLSS is to take this frame's colour over its history
    double view_to_clip[16], clip_to_view[16], clip_to_prev[16], prev_to_clip[16];
    double cam_pos[3], cam_up[3], cam_right[3], cam_fwd[3];
    double cam_near, cam_far, cam_fov, cam_aspect;
    float jitter[2];             // sub-pixel offset of this frame's scene draws, render pixels (x right, y down)
    int reset;                   // the previous frame has no connection to this one
    int depth_inverted;          // reversed Z: near is 1
    int hdr;                     // color_in is linear HDR (before the tone map), else display-referred
    // The motion vectors: last frame's window position minus this frame's, in render pixels, the camera's motion
    // included and no jitter in them. 0: the image is all zeros and Streamline derives the camera's motion itself.
    int mvec_camera;
    uint32_t frame;              // increases by one per evaluated frame
} AodDlssFrame;

// Before vkCreateInstance. Loads sl.interposer.dll from dir after checking NVIDIA's signature on it and on the
// plugins, and starts Streamline for the features in want (AOD_SL_*; manual hooking, Vulkan, no over-the-air
// downloads). The player's nvngx_dlss.dll and nvngx_dlssg.dll are in dir or in one of the folders of ngx (separated
// by '|'; NULL: none), which Streamline searches after dir. fg_factor: the frame multiplier asked for, 2..6. Returns
// the features that loaded; one that was
// asked for and did not is off for this run, with one log line saying why ("dlss: off: ...", "frame generation: off:
// ..."). 0: Streamline does not run.
int aod_dlss_init(const char *dir, const char *ngx, int want, int fg_factor);
// After aod_dlss_init: the extensions the loaded features need (names stay valid until aod_dlss_shutdown).
int aod_dlss_instance_extensions(const char **names, int max);
int aod_dlss_device_extensions(const char **names, int max);
// The queues the loaded features take for themselves: graphics and compute ones, which the caller creates in its
// graphics family after its own (graphics first), and optical flow ones (1 or 0): the first queue of the first family
// that has VK_QUEUE_OPTICAL_FLOW_BIT_NV, when that is a family the caller does not use.
void aod_dlss_queues(uint32_t *graphics, uint32_t *compute, uint32_t *optical_flow);
// After vkCreateDevice with those extensions and queues: hands the device to Streamline and checks each feature on
// this GPU. queues: the caller's own queues in family (Streamline's follow them); flow_family: the family of the
// optical flow queue created for Streamline, -1 for none (frame generation then finds the motion another way).
// Returns the features that are ready; one that is not is logged as off, and 0 means Streamline was shut down.
int aod_dlss_device(VkInstance instance, VkPhysicalDevice phys, VkDevice device, uint32_t family, uint32_t queues,
                    int flow_family);
// Logs why and shuts Streamline down (before the device exists: an extension or a queue it needs is missing).
void aod_dlss_stop(const char *why);
int aod_dlss_ready(void);        // DLAA / DLSS Super Resolution
// Records DLSS into cb outside any render pass: DLAA when color_in and color_out have one size, else the DLSS mode
// nearest to their ratio. Streamline binds pipelines and descriptor sets of its own: the caller forgets the state it
// had bound. 1 = recorded; 0 = nothing was recorded (logged once per reason).
int aod_dlss_evaluate(VkCommandBuffer cb, const AodDlssFrame *f);
// Before vkDestroyDevice (the device is idle, the swapchain destroyed).
void aod_dlss_shutdown(void);

// ---- frame generation. With it ready, Streamline owns the real swapchain: the caller creates, asks, acquires and
// destroys through the calls below, gets images of Streamline's own to draw into, and Streamline shows them with the
// generated frames in between from a thread of its own. Without it, each call below is the loader's.
int aod_fg_ready(void);
int aod_fg_factor(void);         // the multiplier in use, 2..6 (what was asked for, as far as the GPU goes); 0 when not ready
int aod_fg_max(void);            // the largest multiplier this GPU reaches, 2..6; 0 when not ready
VkResult aod_fg_create_swapchain(VkDevice device, VkSwapchainCreateInfoKHR *info, VkSwapchainKHR *swapchain);
void aod_fg_destroy_swapchain(VkDevice device, VkSwapchainKHR swapchain);
VkResult aod_fg_swapchain_images(VkDevice device, VkSwapchainKHR swapchain, uint32_t *count, VkImage *images);
VkResult aod_fg_acquire(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence,
                        uint32_t *index);
// vkDeviceWaitIdle, any thread: Streamline's waits for the presents its own thread still has to make as well.
VkResult aod_fg_wait_idle(VkDevice device);
void aod_fg_pause(void);         // the present thread, before the swapchain changes: no frames are generated meanwhile
// The window's surface (Windows; hinstance and hwnd as Win32 gives them): Streamline keeps which window a surface is.
VkResult aod_fg_win32_surface(VkInstance instance, void *hinstance, void *hwnd, VkSurfaceKHR *surface);
void aod_fg_destroy_surface(VkInstance instance, VkSurfaceKHR surface);

// The game's frames, for Reflex and for matching a present with the scene it shows. All three are no-ops without
// frame generation. Called by the thread that replays the game's submissions and presents, in the game's order.
uint32_t aod_fg_frame_end(void);   // the game presents: the number of the frame that ends, for AodFgPresent
void aod_fg_frame_begin(void);     // ... once that present was handed on: Reflex's wait, then the next frame starts
void aod_fg_render_begin(void);    // a submission of the game: the frame's first one ends its simulation
// The frame's scene is drawn: its camera (the matrices, jitter and reset of f) and its depth and motion vectors
// (f->depth, f->mvec, both in VK_IMAGE_LAYOUT_GENERAL), which Streamline copies inside cb, so both images may change
// before the frame is shown. 1 = taken; the frame's present may then carry generated frames.
int aod_fg_scene(VkCommandBuffer cb, const AodDlssFrame *f);

typedef struct {
    uint32_t frame;              // aod_fg_frame_end's number of the frame this present shows; 0: none of the game's
    int generate;                // nothing but the game's picture is on the image (no menu over it)
    uint32_t width, height;      // the swapchain image
    VkFormat format;
    uint32_t images;             // how many the swapchain has
    int rect[4];                 // where the game's picture is on it: {x, y, w, h}
} AodFgPresent;
// vkQueuePresentKHR. With frame generation ready (p is needed then): frames are generated up to this one when its
// frame has a scene (aod_fg_scene) and p->generate is set, else it is shown alone; Reflex's present marks go around
// it. With only DLSS ready: through Streamline for its once-per-frame bookkeeping. Else the loader's.
VkResult aod_dlss_present(VkQueue queue, const VkPresentInfoKHR *info, const AodFgPresent *p);
// Frames shown per rendered frame over the last half second of presents: 1 when nothing is generated.
double aod_fg_rate(void);

#ifdef __cplusplus
}
#endif
