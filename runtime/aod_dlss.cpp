// DLSS through NVIDIA Streamline v2.14.16 (aod_dlss.h), Age of Decomp's own code.
// Streamline's headers are MIT (third_party/streamline, LICENSE.txt), and so are its DLLs: sl.interposer.dll,
// sl.common.dll, sl.dlss.dll and, for frame generation, sl.dlss_g.dll, sl.reflex.dll and sl.pcl.dll, signed by NVIDIA,
// are loaded at run time (llvm-mingw cannot link the SDK's MSVC import library) after their signature is checked with
// sl_security.h. nvngx_dlss.dll and nvngx_dlssg.dll (NVIDIA RTX SDK licence) are never shipped: Streamline looks for
// the player's copies in the same folder or in the ngx folder.
// Manual hooking (the SDK's docs/ProgrammingGuideManualHooking.md): vk_device.c creates the instance and the device
// with the extensions and the extra queues slGetFeatureRequirements lists and hands them over with slSetVulkanInfo;
// after slEvaluateFeature the caller restores its own command buffer state. Over-the-air updates of Streamline and
// its plugins stay off.
//
// Frame generation (docs/ProgrammingGuideDLSS_G.md): gfx_present.c creates, acquires and presents the swapchain
// through Streamline's own vkCreateSwapchainKHR, vkGetSwapchainImagesKHR, vkAcquireNextImageKHR, vkQueuePresentKHR,
// vkDestroySwapchainKHR and vkDeviceWaitIdle, and the window's surface through its vkCreateWin32SurfaceKHR. Each of
// the game's frames has a number: Streamline's frame token of that number carries the frame's camera, its depth and
// its motion vectors (aod_fg_scene, tagged "only valid now": the replay may record the next frame's scene before this
// one is shown), Reflex's marks and, around the present, the two present marks that tell DLSS-G which frame it shows.
// A token is asked for again at every use: Streamline hands them out of a ring of six. DLSS-G is switched on for a
// present whose frame has a scene and off for the others (menus, movies, loading), keeping its memory.
#include "aod_dlss.h"

#include <stdio.h>
#include <string.h>

extern "C" {
#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
}

#ifdef _WIN32
#include <windows.h>

#include <vulkan/vulkan_win32.h>

#include <atomic>
#include <string>
#include <vector>

#include "third_party/streamline/sl.h"
#include "third_party/streamline/sl_dlss.h"
#include "third_party/streamline/sl_dlss_g.h"
#include "third_party/streamline/sl_helpers_vk.h"
#include "third_party/streamline/sl_pcl.h"
#include "third_party/streamline/sl_reflex.h"
#include "third_party/streamline/sl_security.h"

namespace {

HMODULE g_mod;
PFun_slInit *sl_init;
PFun_slShutdown *sl_shutdown;
PFun_slIsFeatureSupported *sl_supported;
PFun_slGetFeatureRequirements *sl_requirements;
PFun_slSetVulkanInfo *sl_set_vulkan;
PFun_slGetFeatureFunction *sl_feature_fn;
PFun_slSetFeatureLoaded *sl_set_loaded;
PFun_slGetNewFrameToken *sl_frame_token;
PFun_slSetConstants *sl_constants;
PFun_slSetTagForFrame *sl_tag;
PFun_slEvaluateFeature *sl_evaluate;
PFun_slFreeResources *sl_free;
PFun_slDLSSSetOptions *dlss_options;
PFun_slDLSSGSetOptions *fg_options;
PFun_slDLSSGGetState *fg_state;
PFun_slReflexSetOptions *reflex_options;
PFun_slReflexSleep *reflex_sleep;
PFun_slPCLSetMarker *pcl_marker;
// Streamline's own entry points once the device is handed over
PFN_vkQueuePresentKHR g_present;
PFN_vkCreateSwapchainKHR g_sc_create;
PFN_vkDestroySwapchainKHR g_sc_destroy;
PFN_vkGetSwapchainImagesKHR g_sc_images;
PFN_vkAcquireNextImageKHR g_sc_acquire;
PFN_vkDeviceWaitIdle g_wait_idle;
PFN_vkCreateWin32SurfaceKHR g_surface_create;
PFN_vkDestroySurfaceKHR g_surface_destroy;

std::vector<std::string> g_inst_ext, g_dev_ext;
uint32_t g_q_graphics, g_q_compute, g_q_flow;   // the queues the loaded features take
std::wstring g_dir;
std::vector<std::wstring> g_search;   // g_dir, then the folders NVIDIA's own files may be in
const wchar_t *g_paths[4];
int g_loaded;                      // AOD_SL_*: the features slInit loaded and whose requirements were read
int g_ready, g_verbose;            // g_ready: DLSS; g_verbose: TOTK_DLSS_LOG=verbose, Streamline's info messages too
uint32_t g_out_w, g_out_h, g_in_w, g_in_h;   // the sizes the DLSS options were set for
int g_hdr = -1;                              // ... and whether for HDR colour
const sl::ViewportHandle g_view{0u};      // frame generation's (and Reflex's) viewport
const sl::ViewportHandle g_view_sr{1u};   // DLSS super resolution's: its motion vectors are not frame generation's (vk_temporal.c mvsr)
// A fixed project id for Streamline's engine identification (Preferences::projectId): a random UUID, not NVIDIA's.
const char *const k_project = "6b0d5a52-61a4-4b7e-9f5c-6f2d3e8c9a17";

// ---- frame generation
std::atomic<int> g_fg;             // DLSS-G, Reflex and PCL are ready: the swapchain is Streamline's
int g_fg_want;                     // the multiplier asked for, 2..6
uint32_t g_fg_max = 1;             // generated frames per rendered one this GPU reaches (DLSSGState)
uint32_t g_fg_min;                 // the smallest swapchain side DLSS-G takes
std::atomic<uint32_t> g_frame{1};  // the frame the game is making
std::atomic<uint32_t> g_scene[8];  // per frame number modulo 8: the frame whose scene was tagged (aod_fg_scene)
std::atomic<uint32_t> g_const_frame;   // the frame whose camera frame generation's viewport last got
std::atomic<uint32_t> g_sim_frame;                  // ... its simulation start mark
std::atomic<int> g_render_begun;                    // the current frame's first submission was seen
std::atomic<int> g_api_error;                       // the last Vulkan error of Streamline's own presents
std::atomic<uint32_t> g_rate_milli{1000};           // aod_fg_rate, in thousandths
// present thread only
int g_fg_on = -1;                  // the mode last set: 1 generating, 0 off, -1 none yet
uint32_t g_fg_w, g_fg_h, g_fg_n;   // the swapchain size and the frames to generate the options were set for
int g_fg_cool;                     // presents left before DLSS-G is tried again after a bad status
uint32_t g_fg_status = ~0u;        // the status last logged
uint64_t g_stat_presents, g_stat_shown, g_stat_on, g_stat_gen, g_stat_t0, g_stat_logged;
bool g_stat_begun, g_stat_idle;    // a frame was generated; the line about DLSS-G adding none was written
uint64_t g_win_presents, g_win_shown, g_win_t0;

void sl_log(sl::LogType type, const char *msg) {
    static volatile LONG logged;
    if (type == sl::LogType::eInfo && !g_verbose) return;
    if (InterlockedIncrement(&logged) > 300 && type != sl::LogType::eError) return;
    size_t n = strlen(msg);
    while (n && (msg[n - 1] == '\n' || msg[n - 1] == '\r')) n--;
    const char *kind = type == sl::LogType::eError ? "error" : type == sl::LogType::eWarn ? "warning" : "info";
    // The same error every frame must not fill the log (a player's log of 30 minutes once held 220,000 lines of one):
    // each message, told apart by what follows Streamline's time and thread prefix, is written 5 times, then once at
    // every power of ten with its count.
    const char *body = strstr(msg, "us]");
    body = body && body < msg + n ? body + 3 : msg;
    uint64_t key = 0xcbf29ce484222325ull;
    for (const char *c = body; c < msg + n; c++) key = (key ^ (uint8_t)*c) * 0x100000001b3ull;
    static SRWLOCK lock = SRWLOCK_INIT;
    static uint64_t keys[64];
    static uint64_t counts[64];
    uint64_t count = 1;
    AcquireSRWLockExclusive(&lock);
    for (int i = 0; i < 64; i++) {
        if (keys[i] == key || !keys[i]) {
            keys[i] = key;
            count = ++counts[i];
            break;
        }
    }
    ReleaseSRWLockExclusive(&lock);
    if (count <= 5) {
        rt_log("dlss: streamline %s: %.*s", kind, (int)n, msg);
    } else {
        uint64_t p = 10;
        while (p < count) p *= 10;
        if (p == count) rt_log("dlss: streamline %s (%llu times so far): %.*s", kind, (unsigned long long)count, (int)n, msg);
    }
}

const char *result_name(sl::Result r) {
    switch (r) {
    case sl::Result::eOk: return "ok";
    case sl::Result::eErrorIO: return "I/O error";
    case sl::Result::eErrorDriverOutOfDate: return "the NVIDIA driver is too old";
    case sl::Result::eErrorOSOutOfDate: return "Windows is too old";
    case sl::Result::eErrorOSDisabledHWS:
        return "hardware-accelerated GPU scheduling is off (Windows Settings, System, Display, Graphics)";
    case sl::Result::eErrorDeviceNotCreated: return "no device yet";
    case sl::Result::eErrorNoSupportedAdapterFound: return "no NVIDIA GPU that supports it";
    case sl::Result::eErrorAdapterNotSupported: return "this GPU does not support it";
    case sl::Result::eErrorNoPlugins: return "the Streamline plugins were not found";
    case sl::Result::eErrorVulkanAPI: return "Vulkan error";
    case sl::Result::eErrorFeatureMissing: return "the feature was not loaded";
    case sl::Result::eErrorFeatureNotSupported: return "not supported on this PC";
    case sl::Result::eErrorFeatureFailedToLoad: return "the feature failed to load (NVIDIA's nvngx file missing?)";
    case sl::Result::eErrorMissingInputParameter: return "an input is missing";
    case sl::Result::eErrorNotInitialized: return "not initialised";
    default: return "error";
    }
}

std::wstring widen(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    std::wstring w(n > 0 ? (size_t)n : 1, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    w.resize(wcslen(w.c_str()));
    return w;
}

template <class F> bool get(F *&f, const char *name) {
    f = reinterpret_cast<F *>(reinterpret_cast<void *>(GetProcAddress(g_mod, name)));
    return f != nullptr;
}

template <class F> bool feature_fn(sl::Feature feature, F *&f, const char *name) {
    void *fn = nullptr;
    f = sl_feature_fn(feature, name, fn) == sl::Result::eOk ? reinterpret_cast<F *>(fn) : nullptr;
    return f != nullptr;
}

void unload() {
    if (g_mod) FreeLibrary(g_mod);
    g_mod = nullptr;
    g_ready = 0, g_loaded = 0;
    g_fg.store(0);
    g_present = nullptr, g_sc_create = nullptr, g_sc_destroy = nullptr, g_sc_images = nullptr, g_sc_acquire = nullptr;
    g_wait_idle = nullptr, g_surface_create = nullptr, g_surface_destroy = nullptr;
    dlss_options = nullptr, fg_options = nullptr, fg_state = nullptr, reflex_options = nullptr, reflex_sleep = nullptr;
    pcl_marker = nullptr;
    g_inst_ext.clear();
    g_dev_ext.clear();
    g_q_graphics = g_q_compute = g_q_flow = 0;
}

// One line per feature that was loaded and now is off.
void off(int features, const char *why) {
    if (features & AOD_SL_DLSS) rt_log("dlss: off: %s", why);
    if (features & AOD_SL_FG) rt_log("frame generation: off: %s", why);
}

// Every Streamline module must carry NVIDIA's signature; the interposer then checks its plugins itself.
bool signed_modules(std::initializer_list<const wchar_t *> names, const char *dir, char *why, size_t cap) {
    for (const wchar_t *name : names) {
        std::wstring path = g_dir + L"\\" + name;
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            snprintf(why, cap, "%ls is not in %s", name, dir);
            return false;
        }
        if (!sl::security::verifyEmbeddedSignature(path.c_str())) {
            snprintf(why, cap, "%ls in %s is not signed by NVIDIA", name, dir);
            return false;
        }
    }
    return true;
}

// NVIDIA's own file of a feature, which the player brings: in one of its folders or beside Streamline's DLLs.
bool player_file(const wchar_t *name, char *why, size_t cap) {
    std::string where;
    for (auto d = g_search.rbegin(); d != g_search.rend(); ++d) {
        if (GetFileAttributesW((*d + L"\\" + name).c_str()) != INVALID_FILE_ATTRIBUTES) return true;
        char utf8[1024];
        if (WideCharToMultiByte(CP_UTF8, 0, d->c_str(), -1, utf8, sizeof utf8, NULL, NULL) > 0)
            where += (where.empty() ? "" : " or ") + std::string(utf8);
    }
    snprintf(why, cap, "%ls is not in %s", name, where.c_str());
    return false;
}

void add_unique(std::vector<std::string> &list, const char *name) {
    for (const std::string &e : list)
        if (e == name) return;
    list.emplace_back(name);
}

// A feature's Vulkan needs go into the lists. 0 with why when Streamline did not load it or it has no Vulkan path.
bool requirements(sl::Feature feature, char *why, size_t cap) {
    sl::FeatureRequirements req{};
    sl::Result r = sl_requirements(feature, req);
    if (r != sl::Result::eOk || !(req.flags & sl::FeatureRequirementFlags::eVulkanSupported)) {
        snprintf(why, cap, "%s (%d)", r == sl::Result::eOk ? "no Vulkan support" : result_name(r), (int)r);
        return false;
    }
    for (uint32_t i = 0; i < req.vkNumInstanceExtensions; i++) add_unique(g_inst_ext, req.vkInstanceExtensions[i]);
    for (uint32_t i = 0; i < req.vkNumDeviceExtensions; i++) add_unique(g_dev_ext, req.vkDeviceExtensions[i]);
    g_q_graphics += req.vkNumGraphicsQueuesRequired, g_q_compute += req.vkNumComputeQueuesRequired;
    g_q_flow += req.vkNumOpticalFlowQueuesRequired;
    return true;
}

void put(sl::float4x4 &m, const double *a) {
    for (int r = 0; r < 4; r++)
        m.row[r] = sl::float4((float)a[4 * r], (float)a[4 * r + 1], (float)a[4 * r + 2], (float)a[4 * r + 3]);
}

void resource(sl::Resource &r, const AodDlssImage &i) {
    r.width = i.width, r.height = i.height, r.nativeFormat = (uint32_t)i.format;
    r.mipLevels = 1, r.arrayLayers = 1, r.flags = 0, r.usage = i.usage;
}

// The token of a frame number, for use right away (the next request for another number may reuse its storage).
sl::FrameToken *token_of(uint32_t frame) {
    sl::FrameToken *token = nullptr;
    uint32_t index = frame;
    static int fails;
    sl::Result r = sl_frame_token(token, &index);
    if ((r != sl::Result::eOk || !token) && fails++ < 5) rt_log("dlss: no frame token: %s (%d)", result_name(r), (int)r);
    return r == sl::Result::eOk ? token : nullptr;
}

// The camera of f for the frame of token. rw, rh: the size of the depth and the motion vectors.
bool set_constants(const sl::FrameToken &token, const AodDlssFrame *f, uint32_t rw, uint32_t rh,
                   const sl::ViewportHandle &view = g_view) {
    static int fails;
    sl::Constants c{};
    put(c.cameraViewToClip, f->view_to_clip);
    put(c.clipToCameraView, f->clip_to_view);
    static const double identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    put(c.clipToLensClip, identity);
    put(c.clipToPrevClip, f->clip_to_prev);
    put(c.prevClipToClip, f->prev_to_clip);
    c.jitterOffset = sl::float2(f->jitter[0], f->jitter[1]);
    c.mvecScale = sl::float2(1.0f / (float)rw, 1.0f / (float)rh);   // render pixels to the -1..1 range Streamline wants
    c.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
    c.cameraPos = sl::float3((float)f->cam_pos[0], (float)f->cam_pos[1], (float)f->cam_pos[2]);
    c.cameraUp = sl::float3((float)f->cam_up[0], (float)f->cam_up[1], (float)f->cam_up[2]);
    c.cameraRight = sl::float3((float)f->cam_right[0], (float)f->cam_right[1], (float)f->cam_right[2]);
    c.cameraFwd = sl::float3((float)f->cam_fwd[0], (float)f->cam_fwd[1], (float)f->cam_fwd[2]);
    c.cameraNear = (float)f->cam_near, c.cameraFar = (float)f->cam_far;
    c.cameraFOV = (float)f->cam_fov, c.cameraAspectRatio = (float)f->cam_aspect;
    c.depthInverted = f->depth_inverted ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    // mvec_camera: the image holds the camera's motion (vk_temporal.c's motion pass). Otherwise it is zero and
    // Streamline adds the camera motion wherever it finds the invalid value, that is everywhere.
    c.cameraMotionIncluded = f->mvec_camera ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    c.motionVectorsInvalidValue = f->mvec_camera ? -32768.0f : 0.0f;
    c.motionVectors3D = sl::Boolean::eFalse;
    c.reset = f->reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    c.orthographicProjection = sl::Boolean::eFalse;
    c.motionVectorsDilated = sl::Boolean::eFalse;
    c.motionVectorsJittered = sl::Boolean::eFalse;
    sl::Result r = sl_constants(c, token, view);
    if (r != sl::Result::eOk && fails++ < 5) rt_log("dlss: slSetConstants: %s (%d)", result_name(r), (int)r);
    return r == sl::Result::eOk;
}

void mark(sl::PCLMarker m, uint32_t frame) {
    sl::FrameToken *t = token_of(frame);
    if (t) pcl_marker(m, *t);
}

// Vulkan errors of the presents Streamline makes on its own thread (vkQueuePresentKHR itself returns the last known).
void api_error(const sl::APIError &e) { g_api_error.store((int)e.vkRes, std::memory_order_relaxed); }

uint32_t frames_to_generate() {
    uint32_t n = g_fg_want > 1 ? (uint32_t)g_fg_want - 1 : 1;
    return n > g_fg_max ? g_fg_max : n;
}

const char *status_name(sl::DLSSGStatus s) {
    if (s & sl::DLSSGStatus::eFailResolutionTooLow) return "the window is too small";
    if (s & sl::DLSSGStatus::eFailReflexNotDetectedAtRuntime) return "Reflex is not running";
    if (s & sl::DLSSGStatus::eFailHDRFormatNotSupported) return "the HDR format is not supported";
    if (s & sl::DLSSGStatus::eFailCommonConstantsInvalid) return "the camera of a frame was not valid";
    return "DLSS-G reports an error";
}

// ---- the present with frame generation (the present thread, under the caller's queue lock)
VkResult fg_present(VkQueue queue, const VkPresentInfoKHR *info, const AodFgPresent *p) {
    uint32_t frame = p->frame ? p->frame : g_frame.load(std::memory_order_relaxed);
    bool scene = p->frame && g_scene[p->frame & 7].load(std::memory_order_relaxed) == p->frame;
    bool want = scene && p->generate && !g_fg_cool && p->width >= g_fg_min && p->height >= g_fg_min;
    if (g_fg_cool) g_fg_cool--;
    uint32_t n = frames_to_generate();
    static int fails;
    if ((int)want != g_fg_on || (want && (p->width != g_fg_w || p->height != g_fg_h || n != g_fg_n))) {
        sl::DLSSGOptions o{};
        o.mode = want ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
        o.numFramesToGenerate = n;
        o.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;   // menus and movies come and go: no stutter at their ends
        o.numBackBuffers = p->images;
        o.colorWidth = p->width, o.colorHeight = p->height, o.colorBufferFormat = (uint32_t)p->format;
        o.onErrorCallback = api_error;
        sl::Result r = fg_options(g_view, o);
        if (r != sl::Result::eOk) {
            if (fails++ < 5) rt_log("frame generation: slDLSSGSetOptions: %s (%d)", result_name(r), (int)r);
            want = false;
        } else {
            g_fg_on = want, g_fg_w = p->width, g_fg_h = p->height, g_fg_n = n;
        }
    }
    sl::FrameToken *token = token_of(frame);
    // The picture's place on the image: with bars around it, generated frames copy the bars as they are.
    if (token && p->rect[2] > 0 && p->rect[3] > 0) {
        sl::Extent ext{(uint32_t)p->rect[1], (uint32_t)p->rect[0], (uint32_t)p->rect[2], (uint32_t)p->rect[3]};
        sl::ResourceTag tag(nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent, &ext);
        sl::Result r = sl_tag(*token, g_view, &tag, 1, nullptr);
        if (r != sl::Result::eOk && fails++ < 5) rt_log("frame generation: backbuffer tag: %s (%d)", result_name(r), (int)r);
    }
    if (token) pcl_marker(sl::PCLMarker::ePresentStart, *token);
    VkResult res = g_present(queue, info);
    if ((token = token_of(frame))) pcl_marker(sl::PCLMarker::ePresentEnd, *token);

    // What DLSS-G did: the frames it showed since the last present, and whether it still runs
    sl::DLSSGState st{};
    uint32_t shown = 1;
    if (fg_state(g_view, st, nullptr) == sl::Result::eOk) {
        if (st.numFramesActuallyPresented) shown = st.numFramesActuallyPresented;
        if (st.minWidthOrHeight) g_fg_min = st.minWidthOrHeight;
        if (g_fg_on == 1 && st.status != sl::DLSSGStatus::eOk) {
            if ((uint32_t)st.status != g_fg_status)
                rt_log("frame generation: paused: %s (DLSS-G status %#x); trying again in a few seconds",
                       status_name(st.status), (unsigned)st.status);
            g_fg_status = (uint32_t)st.status;
            g_fg_cool = 300;
        } else if (g_fg_on == 1 && g_fg_status && g_fg_status != ~0u) {
            g_fg_status = 0;
        }
    }
    // Out of date and suboptimal come with every change of the window's size, and vkQueuePresentKHR says so as well.
    int err = g_api_error.exchange(0, std::memory_order_relaxed);
    if (err && err != VK_ERROR_OUT_OF_DATE_KHR && err != VK_SUBOPTIMAL_KHR && fails++ < 5)
        rt_log("frame generation: a present of Streamline's returned %d", err);
    static uint64_t seen;
    if (g_verbose && seen++ % 120 == 0)   // TOTK_DLSS_LOG=verbose: what DLSS-G says of itself, every two seconds
        rt_log("frame generation: present %llu of frame %u: scene %d, generate %d, mode %s, %u to generate; DLSS-G status "
               "%#x, %u frames shown since the last present, smallest side %u, image %ux%u, picture at %d,%d %dx%d",
               (unsigned long long)seen, frame, (int)scene, p->generate, g_fg_on == 1 ? "on" : "off", n,
               (unsigned)st.status, st.numFramesActuallyPresented, st.minWidthOrHeight, p->width, p->height, p->rect[0],
               p->rect[1], p->rect[2], p->rect[3]);

    // The rate for the overlay (half a second of presents) and a log line now and then
    uint64_t now = plat_now_ns();
    if (!g_win_t0) g_win_t0 = now;
    g_win_presents++, g_win_shown += shown;
    if (now - g_win_t0 >= 500000000ull) {
        g_rate_milli.store((uint32_t)(g_win_shown * 1000 / g_win_presents), std::memory_order_relaxed);
        g_win_presents = g_win_shown = 0, g_win_t0 = now;
    }
    // A line two seconds after the first generated frame, then one a minute; and once, when DLSS-G was on for two
    // seconds and added nothing. The count starts anew at the first generated frame: its line is about those seconds.
    if (shown > 1 && !g_stat_begun) g_stat_begun = true, g_stat_t0 = 0;
    if (!g_stat_t0) {   // the presents after this one are counted, each with the time since the one before
        g_stat_presents = g_stat_shown = g_stat_on = g_stat_gen = 0, g_stat_t0 = now;
        return res;
    }
    g_stat_presents++, g_stat_shown += shown, g_stat_on += g_fg_on == 1, g_stat_gen += shown > 1;
    double secs = (double)(now - g_stat_t0) / 1e9;
    bool first = g_stat_begun && !g_stat_logged && g_stat_gen >= 120, idle = !g_stat_begun && !g_stat_idle && g_stat_on >= 120;
    if (first || idle || secs >= 60) {
        if (g_stat_gen) {
            rt_log("frame generation: DLSS-G %ux, %.1f frames per second shown for %.1f rendered (generated frames at "
                   "%llu of %llu presents)", n + 1, (double)g_stat_shown / secs, (double)g_stat_presents / secs,
                   (unsigned long long)g_stat_gen, (unsigned long long)g_stat_presents);
            g_stat_logged++;
        } else if (g_stat_on && !g_stat_idle) {   // DLSS-G adds frames only while the game's window is the one in front
            rt_log("frame generation: DLSS-G %ux is on and showed no extra frame in %llu presents (it adds them only "
                   "while the game's window is in front)", n + 1, (unsigned long long)g_stat_on);
            g_stat_idle = true;
        }
        g_stat_presents = g_stat_shown = g_stat_on = g_stat_gen = 0, g_stat_t0 = now;
    }
    return res;
}

}   // namespace

extern "C" int aod_dlss_init(const char *dir, const char *ngx, int want, int fg_factor) {
    if (g_mod) return g_loaded;
    char why[512] = "";
    g_dir = widen(dir);
    g_search.assign(1, g_dir);
    for (const char *p = ngx; p && *p;) {   // the folders of NVIDIA's own files, each once
        const char *end = strchr(p, '|');
        std::wstring d = widen(std::string(p, end ? (size_t)(end - p) : strlen(p)).c_str());
        bool seen = d.empty();
        for (const std::wstring &s : g_search) seen |= s == d;
        if (!seen && g_search.size() < 4) g_search.push_back(d);
        p = end ? end + 1 : nullptr;
    }
    g_fg_want = fg_factor < 2 ? 2 : fg_factor > 6 ? 6 : fg_factor;
    if (!signed_modules({L"sl.interposer.dll", L"sl.common.dll"}, dir, why, sizeof why)) return off(want, why), 0;
    if ((want & AOD_SL_DLSS) && (!signed_modules({L"sl.dlss.dll"}, dir, why, sizeof why) ||
                                 !player_file(L"nvngx_dlss.dll", why, sizeof why)))
        off(AOD_SL_DLSS, why), want &= ~AOD_SL_DLSS;
    if ((want & AOD_SL_FG) && (!signed_modules({L"sl.dlss_g.dll", L"sl.reflex.dll", L"sl.pcl.dll"}, dir, why, sizeof why) ||
                               !player_file(L"nvngx_dlssg.dll", why, sizeof why)))
        off(AOD_SL_FG, why), want &= ~AOD_SL_FG;
    if (!want) return 0;
    std::wstring interposer = g_dir + L"\\sl.interposer.dll";
    g_mod = LoadLibraryExW(interposer.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_mod) {
        snprintf(why, sizeof why, "sl.interposer.dll could not be loaded (error %lu)", GetLastError());
        return off(want, why), 0;
    }
    if (!get(sl_init, "slInit") || !get(sl_shutdown, "slShutdown") || !get(sl_supported, "slIsFeatureSupported") ||
        !get(sl_requirements, "slGetFeatureRequirements") || !get(sl_set_vulkan, "slSetVulkanInfo") ||
        !get(sl_feature_fn, "slGetFeatureFunction") || !get(sl_frame_token, "slGetNewFrameToken") ||
        !get(sl_constants, "slSetConstants") || !get(sl_tag, "slSetTagForFrame") ||
        !get(sl_evaluate, "slEvaluateFeature") || !get(sl_free, "slFreeResources") ||
        !get(sl_set_loaded, "slSetFeatureLoaded")) {
        off(want, "sl.interposer.dll lacks an entry point");
        unload();
        return 0;
    }
    static sl::Feature features[4];
    uint32_t nf = 0;
    if (want & AOD_SL_DLSS) features[nf++] = sl::kFeatureDLSS;
    if (want & AOD_SL_FG) features[nf++] = sl::kFeatureDLSS_G, features[nf++] = sl::kFeatureReflex, features[nf++] = sl::kFeaturePCL;
    // Streamline hands its plugin paths to NGX, which looks there for NVIDIA's nvngx files
    for (size_t i = 0; i < g_search.size(); i++) g_paths[i] = g_search[i].c_str();
    sl::Preferences pref{};
    pref.showConsole = false;
    const char *log = cfg_get("TOTK_DLSS_LOG");   // developer switch: "verbose" for Streamline's full log
    g_verbose = log && !strcmp(log, "verbose");
    pref.logLevel = g_verbose ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
    pref.pathsToPlugins = g_paths;
    pref.numPathsToPlugins = (uint32_t)g_search.size();
    pref.pathToLogsAndData = nullptr;   // no log file: messages go to the game log
    pref.logMessageCallback = sl_log;
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking |
                 sl::PreferenceFlags::eUseFrameBasedResourceTagging | sl::PreferenceFlags::eDisableDebugText;
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = nf;
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "Age of Decomp";
    pref.projectId = k_project;
    pref.renderAPI = sl::RenderAPI::eVulkan;
    sl::Result r = sl_init(pref, sl::kSDKVersion);
    if (r != sl::Result::eOk) {
        snprintf(why, sizeof why, "slInit: %s (%d)", result_name(r), (int)r);
        off(want, why);
        unload();
        return 0;
    }
    char detail[400];
    if ((want & AOD_SL_DLSS) && !requirements(sl::kFeatureDLSS, detail, sizeof detail)) {
        snprintf(why, sizeof why, "DLSS cannot run on this PC: %s", detail);
        off(AOD_SL_DLSS, why), want &= ~AOD_SL_DLSS;
    }
    if (want & AOD_SL_FG) {
        // DLSS-G first: its answer says why a PC without an RTX 40 or 50 card, or with GPU scheduling off, has none
        std::vector<std::string> inst = g_inst_ext, dev = g_dev_ext;
        uint32_t qg = g_q_graphics, qc = g_q_compute;
        const char *part = "DLSS Frame Generation (it needs a GeForce RTX 40 or 50 card)";
        bool ok = requirements(sl::kFeatureDLSS_G, detail, sizeof detail);
        if (ok && !(ok = requirements(sl::kFeatureReflex, detail, sizeof detail))) part = "Reflex, which it needs,";
        if (ok && !(ok = requirements(sl::kFeaturePCL, detail, sizeof detail))) part = "Reflex's latency marks";
        if (!ok) {
            snprintf(why, sizeof why, "%s cannot run on this PC: %s", part, detail);
            off(AOD_SL_FG, why), want &= ~AOD_SL_FG;
            g_inst_ext = inst, g_dev_ext = dev, g_q_graphics = qg, g_q_compute = qc, g_q_flow = 0;
        }
    }
    if (!want) {
        sl_shutdown();
        unload();
        return 0;
    }
    g_loaded = want;
    rt_log("dlss: Streamline %u.%u.%u loaded from %s for %s%s%s: %u instance and %u device extensions, %u graphics + %u "
           "compute + %u optical flow queues of its own", (unsigned)SL_VERSION_MAJOR, (unsigned)SL_VERSION_MINOR,
           (unsigned)SL_VERSION_PATCH, dir, want & AOD_SL_DLSS ? "DLSS" : "", want == (AOD_SL_DLSS | AOD_SL_FG) ? " and " : "",
           want & AOD_SL_FG ? "frame generation" : "", (unsigned)g_inst_ext.size(), (unsigned)g_dev_ext.size(),
           g_q_graphics, g_q_compute, g_q_flow);
    return want;
}

extern "C" int aod_dlss_instance_extensions(const char **names, int max) {
    int n = 0;
    for (const std::string &e : g_inst_ext) if (n < max) names[n++] = e.c_str();
    return n;
}

extern "C" int aod_dlss_device_extensions(const char **names, int max) {
    int n = 0;
    for (const std::string &e : g_dev_ext) if (n < max) names[n++] = e.c_str();
    return n;
}

extern "C" void aod_dlss_queues(uint32_t *graphics, uint32_t *compute, uint32_t *optical_flow) {
    *graphics = g_q_graphics, *compute = g_q_compute, *optical_flow = g_q_flow;
}

extern "C" void aod_dlss_stop(const char *why) {
    if (!g_mod) return;
    off(g_loaded, why);
    sl_shutdown();
    unload();
}

extern "C" int aod_dlss_device(VkInstance instance, VkPhysicalDevice phys, VkDevice device, uint32_t family, uint32_t queues,
                               int flow_family) {
    if (!g_mod) return 0;
    char why[400];
    sl::VulkanInfo info{};
    info.device = device, info.instance = instance, info.physicalDevice = phys;
    info.graphicsQueueFamily = family, info.graphicsQueueIndex = queues;   // where Streamline's own queues start
    info.computeQueueFamily = family, info.computeQueueIndex = queues + g_q_graphics;
    if (flow_family >= 0) {
        info.opticalFlowQueueFamily = (uint32_t)flow_family, info.opticalFlowQueueIndex = 0;
        info.useNativeOpticalFlowMode = true;
    }
    sl::Result r = sl_set_vulkan(info);
    if (r != sl::Result::eOk) {
        snprintf(why, sizeof why, "slSetVulkanInfo: %s (%d)", result_name(r), (int)r);
        aod_dlss_stop(why);
        return 0;
    }
    auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(reinterpret_cast<void *>(GetProcAddress(g_mod, "vkGetDeviceProcAddr")));
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void *>(GetProcAddress(g_mod, "vkGetInstanceProcAddr")));
    g_present = gdpa ? reinterpret_cast<PFN_vkQueuePresentKHR>(gdpa(device, "vkQueuePresentKHR")) : nullptr;
    sl::AdapterInfo adapter{};
    adapter.vkPhysicalDevice = phys;
    if (g_loaded & AOD_SL_DLSS) {
        r = sl_supported(sl::kFeatureDLSS, adapter);
        if (r == sl::Result::eOk && feature_fn(sl::kFeatureDLSS, dlss_options, "slDLSSSetOptions") && g_present) {
            g_ready = 1;
            rt_log("dlss: ready");
        } else {
            rt_log("dlss: off: this GPU or driver cannot run DLSS (%s, %d)", result_name(r), (int)r);
            dlss_options = nullptr;
        }
    }
    if (g_loaded & AOD_SL_FG) {
        const char *what = "DLSS Frame Generation";
        r = sl_supported(sl::kFeatureDLSS_G, adapter);
        if (r == sl::Result::eOk && (r = sl_supported(sl::kFeatureReflex, adapter)) != sl::Result::eOk) what = "Reflex";
        if (r == sl::Result::eOk && (r = sl_supported(sl::kFeaturePCL, adapter)) != sl::Result::eOk) what = "Reflex's latency marks";
        bool fns = r == sl::Result::eOk && feature_fn(sl::kFeatureDLSS_G, fg_options, "slDLSSGSetOptions") &&
                   feature_fn(sl::kFeatureDLSS_G, fg_state, "slDLSSGGetState") &&
                   feature_fn(sl::kFeatureReflex, reflex_options, "slReflexSetOptions") &&
                   feature_fn(sl::kFeatureReflex, reflex_sleep, "slReflexSleep") &&
                   feature_fn(sl::kFeaturePCL, pcl_marker, "slPCLSetMarker");
        if (fns && gdpa && gipa) {
            g_sc_create = reinterpret_cast<PFN_vkCreateSwapchainKHR>(gdpa(device, "vkCreateSwapchainKHR"));
            g_sc_destroy = reinterpret_cast<PFN_vkDestroySwapchainKHR>(gdpa(device, "vkDestroySwapchainKHR"));
            g_sc_images = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(gdpa(device, "vkGetSwapchainImagesKHR"));
            g_sc_acquire = reinterpret_cast<PFN_vkAcquireNextImageKHR>(gdpa(device, "vkAcquireNextImageKHR"));
            g_wait_idle = reinterpret_cast<PFN_vkDeviceWaitIdle>(gdpa(device, "vkDeviceWaitIdle"));
            g_surface_create = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(gipa(instance, "vkCreateWin32SurfaceKHR"));
            g_surface_destroy = reinterpret_cast<PFN_vkDestroySurfaceKHR>(gipa(instance, "vkDestroySurfaceKHR"));
        }
        bool hooks = g_present && g_sc_create && g_sc_destroy && g_sc_images && g_sc_acquire && g_wait_idle &&
                     g_surface_create && g_surface_destroy;
        // Reflex has to run while DLSS-G does (DLSSGStatus::eFailReflexNotDetectedAtRuntime otherwise)
        sl::ReflexOptions ro{};
        ro.mode = sl::ReflexMode::eLowLatency;
        if (fns && hooks && (r = reflex_options(ro)) != sl::Result::eOk) what = "Reflex's low latency mode";
        if (fns && hooks && r == sl::Result::eOk) {
            sl::DLSSGState st{};
            if (fg_state(g_view, st, nullptr) == sl::Result::eOk) {
                if (st.numFramesToGenerateMax) g_fg_max = st.numFramesToGenerateMax;
                g_fg_min = st.minWidthOrHeight;
            }
            g_fg.store(1);
            uint32_t n = frames_to_generate();
            rt_log("frame generation: DLSS-G ready: %ux asked for, this GPU goes up to %ux: %ux in use (%s optical flow "
                   "queue)", (unsigned)g_fg_want, g_fg_max + 1, n + 1, flow_family >= 0 ? "its own" : "no");
        } else {
            if (r != sl::Result::eOk) rt_log("frame generation: off: this GPU or driver cannot run %s (%s, %d)", what,
                                             result_name(r), (int)r);
            else rt_log("frame generation: off: Streamline lacks an entry point for it");
            sl_set_loaded(sl::kFeatureDLSS_G, false);   // its present hook must not see a swapchain it did not make
            fg_options = nullptr, fg_state = nullptr, reflex_options = nullptr, reflex_sleep = nullptr, pcl_marker = nullptr;
        }
    }
    if (!g_ready && !g_fg.load()) {
        sl_shutdown();
        unload();
        return 0;
    }
    return (g_ready ? AOD_SL_DLSS : 0) | (g_fg.load() ? AOD_SL_FG : 0);
}

extern "C" int aod_dlss_ready(void) { return g_ready; }
extern "C" int aod_fg_ready(void) { return g_fg.load(std::memory_order_relaxed); }
extern "C" int aod_fg_factor(void) { return g_fg.load(std::memory_order_relaxed) ? (int)frames_to_generate() + 1 : 0; }
extern "C" int aod_fg_max(void) { return g_fg.load(std::memory_order_relaxed) ? (int)g_fg_max + 1 : 0; }
extern "C" double aod_fg_rate(void) { return g_fg.load(std::memory_order_relaxed) ? g_rate_milli.load(std::memory_order_relaxed) / 1000.0 : 1.0; }

extern "C" VkResult aod_dlss_present(VkQueue queue, const VkPresentInfoKHR *info, const AodFgPresent *p) {
    if (g_fg.load(std::memory_order_relaxed) && p) return fg_present(queue, info, p);
    PFN_vkQueuePresentKHR f = g_ready || g_fg.load(std::memory_order_relaxed) ? g_present : nullptr;
    return f ? f(queue, info) : vkQueuePresentKHR(queue, info);
}

// ---- the swapchain: Streamline's while frame generation is ready
extern "C" VkResult aod_fg_create_swapchain(VkDevice device, VkSwapchainCreateInfoKHR *info, VkSwapchainKHR *swapchain) {
    if (!g_fg.load()) return vkCreateSwapchainKHR(device, info, NULL, swapchain);
    g_fg_on = -1;   // the options are set again for the new swapchain
    return g_sc_create(device, info, NULL, swapchain);
}

extern "C" void aod_fg_destroy_swapchain(VkDevice device, VkSwapchainKHR swapchain) {
    if (g_fg.load()) g_sc_destroy(device, swapchain, NULL);
    else vkDestroySwapchainKHR(device, swapchain, NULL);
}

extern "C" VkResult aod_fg_swapchain_images(VkDevice device, VkSwapchainKHR swapchain, uint32_t *count, VkImage *images) {
    return g_fg.load() ? g_sc_images(device, swapchain, count, images) : vkGetSwapchainImagesKHR(device, swapchain, count, images);
}

extern "C" VkResult aod_fg_acquire(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore,
                                   VkFence fence, uint32_t *index) {
    return g_fg.load(std::memory_order_relaxed) ? g_sc_acquire(device, swapchain, timeout, semaphore, fence, index)
                                                : vkAcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, index);
}

// The present thread, before the swapchain is made again or destroyed: DLSS-G stops generating.
extern "C" void aod_fg_pause(void) {
    if (!g_fg.load() || g_fg_on != 1) return;
    sl::DLSSGOptions o{};
    o.mode = sl::DLSSGMode::eOff;
    o.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
    fg_options(g_view, o);
    g_fg_on = 0;
}

extern "C" VkResult aod_fg_wait_idle(VkDevice device) {
    return g_fg.load(std::memory_order_relaxed) ? g_wait_idle(device) : vkDeviceWaitIdle(device);
}

extern "C" VkResult aod_fg_win32_surface(VkInstance instance, void *hinstance, void *hwnd, VkSurfaceKHR *surface) {
    VkWin32SurfaceCreateInfoKHR si{};
    si.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    si.hinstance = (HINSTANCE)hinstance, si.hwnd = (HWND)hwnd;
    return g_fg.load() ? g_surface_create(instance, &si, NULL, surface) : vkCreateWin32SurfaceKHR(instance, &si, NULL, surface);
}

extern "C" void aod_fg_destroy_surface(VkInstance instance, VkSurfaceKHR surface) {
    if (g_fg.load()) g_surface_destroy(instance, surface, NULL);
    else vkDestroySurfaceKHR(instance, surface, NULL);
}

// ---- the game's frames
extern "C" uint32_t aod_fg_frame_end(void) {
    if (!g_fg.load(std::memory_order_relaxed)) return 0;
    uint32_t f = g_frame.load(std::memory_order_relaxed);
    aod_fg_render_begin();   // a frame without a submission of its own still gets its marks, in order
    mark(sl::PCLMarker::eRenderSubmitEnd, f);
    g_render_begun.store(0, std::memory_order_relaxed);
    g_frame.store(f + 1, std::memory_order_relaxed);
    return f;
}

extern "C" void aod_fg_frame_begin(void) {
    if (!g_fg.load(std::memory_order_relaxed)) return;
    uint32_t f = g_frame.load(std::memory_order_relaxed);
    sl::FrameToken *t = token_of(f);
    if (t) reflex_sleep(*t);   // Reflex holds the frame's start back when the GPU is the limit; no wait otherwise
    mark(sl::PCLMarker::eSimulationStart, f);
    g_sim_frame.store(f, std::memory_order_relaxed);
}

extern "C" void aod_fg_render_begin(void) {
    if (!g_fg.load(std::memory_order_relaxed) || g_render_begun.exchange(1, std::memory_order_relaxed)) return;
    uint32_t f = g_frame.load(std::memory_order_relaxed);
    if (g_sim_frame.exchange(f, std::memory_order_relaxed) != f) mark(sl::PCLMarker::eSimulationStart, f);   // the first frame
    mark(sl::PCLMarker::eSimulationEnd, f);
    mark(sl::PCLMarker::eRenderSubmitStart, f);
}

extern "C" int aod_fg_scene(VkCommandBuffer cb, const AodDlssFrame *f) {
    if (!g_fg.load(std::memory_order_relaxed)) return 0;
    static int fails;
    uint32_t index = g_frame.load(std::memory_order_relaxed);
    sl::FrameToken *token = token_of(index);
    if (!token) return 0;
    uint32_t rw = f->depth.width, rh = f->depth.height;
    // The camera once per frame on frame generation's viewport (DLSS super resolution sets its own, g_view_sr)
    if (g_const_frame.load(std::memory_order_relaxed) != index) {
        if (!set_constants(*token, f, rw, rh)) return 0;
        g_const_frame.store(index, std::memory_order_relaxed);
    }
    const uint32_t general = VK_IMAGE_LAYOUT_GENERAL;
    sl::Resource depth(sl::ResourceType::eTex2d, (void *)f->depth.image, nullptr, (void *)f->depth.view, general);
    sl::Resource mvec(sl::ResourceType::eTex2d, (void *)f->mvec.image, nullptr, (void *)f->mvec.view, general);
    resource(depth, f->depth), resource(mvec, f->mvec);
    sl::SubresourceRange depth_range{};
    depth_range.aspectMask = f->depth.aspect ? f->depth.aspect : VK_IMAGE_ASPECT_DEPTH_BIT;   // an R32F copy: colour
    depth_range.baseMipLevel = 0, depth_range.levelCount = 1, depth_range.baseArrayLayer = 0, depth_range.layerCount = 1;
    depth.next = &depth_range;
    sl::Extent rext{0, 0, rw, rh};
    // Both images are written again by the next frame's scene, which the replay may record before this frame is
    // shown: Streamline copies them here, inside cb.
    sl::ResourceTag tags[] = {
        sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &rext),
        sl::ResourceTag(&mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &rext),
    };
    sl::Result r = sl_tag(*token, g_view, tags, 2, (sl::CommandBuffer *)cb);
    if (r != sl::Result::eOk) {
        if (fails++ < 5) rt_log("frame generation: slSetTagForFrame: %s (%d)", result_name(r), (int)r);
        return 0;
    }
    g_scene[index & 7].store(index, std::memory_order_relaxed);
    return 1;
}

extern "C" int aod_dlss_evaluate(VkCommandBuffer cb, const AodDlssFrame *f) {
    if (!g_ready) return 0;
    static int fails;
    // With frame generation the evaluation belongs to the game's frame, whose token already has the camera, the depth
    // and the motion vectors (aod_fg_scene); alone, to a count of its own.
    bool fg = g_fg.load(std::memory_order_relaxed) != 0;
    uint32_t index = fg ? g_frame.load(std::memory_order_relaxed) : f->frame;
    sl::FrameToken *token = token_of(index);
    if (!token) return 0;
    sl::Result r;
    // w, h: the output; rw, rh: the render size the scene, the depth and the motion vectors have
    uint32_t w = f->color_out.width, h = f->color_out.height, rw = f->color_in.width, rh = f->color_in.height;
    if (w != g_out_w || h != g_out_h || rw != g_in_w || rh != g_in_h || f->hdr != g_hdr) {
        // The mode whose ratio is nearest: 1 DLAA, 1.5 quality, 1.72 balanced, 2 performance, 3 ultra performance.
        double ratio = rw ? (double)w / rw : 1.0;
        sl::DLSSMode mode = ratio < 1.2 ? sl::DLSSMode::eDLAA : ratio < 1.6 ? sl::DLSSMode::eMaxQuality
                          : ratio < 1.85 ? sl::DLSSMode::eBalanced : ratio < 2.5 ? sl::DLSSMode::eMaxPerformance
                          : sl::DLSSMode::eUltraPerformance;
        static const char *const names[] = {"off", "Performance", "Balanced", "Quality", "Ultra Performance", "Ultra Quality",
                                            "DLAA"};
        sl::DLSSOptions o{};
        o.mode = mode;
        o.outputWidth = w, o.outputHeight = h;
        o.colorBuffersHDR = f->hdr ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        o.useAutoExposure = sl::Boolean::eTrue;
        if ((r = dlss_options(g_view_sr, o)) != sl::Result::eOk) {
            if (fails++ < 5) rt_log("dlss: slDLSSSetOptions %ux%u -> %ux%u: %s (%d)", rw, rh, w, h, result_name(r), (int)r);
            return 0;
        }
        g_out_w = w, g_out_h = h, g_in_w = rw, g_in_h = rh, g_hdr = f->hdr;
        rt_log("dlss: %s, %ux%u -> %ux%u, %s colour", names[(uint32_t)mode], rw, rh, w, h, f->hdr ? "HDR" : "display");
        if (!(token = token_of(index))) return 0;
    }
    // The camera for DLSS's own viewport, every frame (frame generation's is set by aod_fg_scene)
    if (!set_constants(*token, f, rw, rh, g_view_sr)) return 0;

    const uint32_t general = VK_IMAGE_LAYOUT_GENERAL;
    sl::Resource in(sl::ResourceType::eTex2d, (void *)f->color_in.image, nullptr, (void *)f->color_in.view, general);
    sl::Resource out(sl::ResourceType::eTex2d, (void *)f->color_out.image, nullptr, (void *)f->color_out.view, general);
    sl::Resource depth(sl::ResourceType::eTex2d, (void *)f->depth.image, nullptr, (void *)f->depth.view, general);
    sl::Resource mvec(sl::ResourceType::eTex2d, (void *)f->mvec.image, nullptr, (void *)f->mvec.view, general);
    sl::Resource bias(sl::ResourceType::eTex2d, (void *)f->bias.image, nullptr, (void *)f->bias.view, general);
    resource(in, f->color_in), resource(out, f->color_out), resource(depth, f->depth), resource(mvec, f->mvec);
    if (f->bias.image) resource(bias, f->bias);
    sl::SubresourceRange depth_range{};
    depth_range.aspectMask = f->depth.aspect ? f->depth.aspect : VK_IMAGE_ASPECT_DEPTH_BIT;   // an R32F copy: colour
    depth_range.baseMipLevel = 0, depth_range.levelCount = 1, depth_range.baseArrayLayer = 0, depth_range.layerCount = 1;
    depth.next = &depth_range;
    sl::Extent ext{0, 0, w, h}, rext{0, 0, rw, rh};
    sl::ResourceTag tags[] = {
        sl::ResourceTag(&in, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &rext),
        sl::ResourceTag(&out, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &ext),
        sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate, &rext),
        sl::ResourceTag(&mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate, &rext),
    };
    // DLSS's own viewport: its depth and motion vectors (which drop a wrong history) are not frame generation's
    if ((r = sl_tag(*token, g_view_sr, tags, 4, (sl::CommandBuffer *)cb)) != sl::Result::eOk) {
        if (fails++ < 5) rt_log("dlss: slSetTagForFrame: %s (%d)", result_name(r), (int)r);
        return 0;
    }
    // Where the history is another surface's (vk_temporal.c), a mask DLSS could take this frame's colour from. Not
    // sent unless TOTK_DLSS_MASK asks for it, as a test: Streamline 2.14's Vulkan layer cannot describe the mask's
    // R8_UNORM (its format table has no such entry), so every frame it logged "Cannot have undefined format" four
    // times, and DLSS changed nothing with it (DLSS_BIAS=all still ghosted on 2026-11-23); the motion vectors that
    // point out of the picture (TEMPORAL_DROP) are what drops such a history. TOTK_DLSS_MASK=bias
    // (kBufferTypeBiasCurrentColorHint), reactive (kBufferTypeReactiveMaskHint) or transparency
    // (kBufferTypeTransparencyAndCompositionMaskHint).
    static const char *mask_ask = [] {
        const char *e = getenv("TOTK_DLSS_MASK");
        return e ? e : getenv("HWAOI_DLSS_MASK");
    }();
    static sl::BufferType mask_type = [] {
        const char *e = mask_ask;
        sl::BufferType t = e && !strcmp(e, "reactive") ? sl::kBufferTypeReactiveMaskHint
                         : e && !strcmp(e, "transparency") ? sl::kBufferTypeTransparencyAndCompositionMaskHint
                                                           : sl::kBufferTypeBiasCurrentColorHint;
        if (e) rt_log("dlss: the history mask goes to DLSS as buffer type %u (TOTK_DLSS_MASK)", (unsigned)t);
        return t;
    }();
    sl::ResourceTag bias_tag(&bias, mask_type, sl::ResourceLifecycle::eValidUntilEvaluate, &rext);
    if (mask_ask && f->bias.image &&
        (r = sl_tag(*token, g_view_sr, &bias_tag, 1, (sl::CommandBuffer *)cb)) != sl::Result::eOk) {
        static int bias_fails;
        if (bias_fails++ < 3) rt_log("dlss: the history mask was not taken: %s (%d)", result_name(r), (int)r);
    }
    const sl::BaseStructure *inputs[] = {&g_view_sr};
    if ((r = sl_evaluate(sl::kFeatureDLSS, *token, inputs, 1, (sl::CommandBuffer *)cb)) != sl::Result::eOk) {
        if (fails++ < 5) rt_log("dlss: slEvaluateFeature: %s (%d)", result_name(r), (int)r);
        return 0;
    }
    return 1;
}

extern "C" void aod_dlss_shutdown(void) {
    if (!g_mod) return;
    if (g_ready && sl_free) sl_free(sl::kFeatureDLSS, g_view_sr);
    if (g_fg.load() && sl_free) sl_free(sl::kFeatureDLSS_G, g_view);
    sl_shutdown();
    unload();
}

#else   // Streamline is Windows-only: DLSS stays off elsewhere

extern "C" int aod_dlss_init(const char *dir, const char *ngx, int want, int fg_factor) {
    (void)dir, (void)ngx, (void)fg_factor;
    if (want & AOD_SL_DLSS) rt_log("dlss: off: DLSS needs Windows");
    if (want & AOD_SL_FG) rt_log("frame generation: off: DLSS Frame Generation needs Windows");
    return 0;
}
extern "C" int aod_dlss_instance_extensions(const char **names, int max) { (void)names, (void)max; return 0; }
extern "C" int aod_dlss_device_extensions(const char **names, int max) { (void)names, (void)max; return 0; }
extern "C" void aod_dlss_queues(uint32_t *graphics, uint32_t *compute, uint32_t *optical_flow) {
    *graphics = *compute = *optical_flow = 0;
}
extern "C" int aod_dlss_device(VkInstance instance, VkPhysicalDevice phys, VkDevice device, uint32_t family, uint32_t queues,
                               int flow_family) {
    (void)instance, (void)phys, (void)device, (void)family, (void)queues, (void)flow_family;
    return 0;
}
extern "C" void aod_dlss_stop(const char *why) { (void)why; }
extern "C" int aod_dlss_ready(void) { return 0; }
extern "C" int aod_fg_ready(void) { return 0; }
extern "C" int aod_fg_factor(void) { return 0; }
extern "C" int aod_fg_max(void) { return 0; }
extern "C" double aod_fg_rate(void) { return 1.0; }
extern "C" VkResult aod_dlss_present(VkQueue queue, const VkPresentInfoKHR *info, const AodFgPresent *p) {
    (void)p;
    return vkQueuePresentKHR(queue, info);
}
extern "C" VkResult aod_fg_create_swapchain(VkDevice device, VkSwapchainCreateInfoKHR *info, VkSwapchainKHR *swapchain) {
    return vkCreateSwapchainKHR(device, info, NULL, swapchain);
}
extern "C" void aod_fg_destroy_swapchain(VkDevice device, VkSwapchainKHR swapchain) { vkDestroySwapchainKHR(device, swapchain, NULL); }
extern "C" VkResult aod_fg_swapchain_images(VkDevice device, VkSwapchainKHR swapchain, uint32_t *count, VkImage *images) {
    return vkGetSwapchainImagesKHR(device, swapchain, count, images);
}
extern "C" VkResult aod_fg_acquire(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore,
                                   VkFence fence, uint32_t *index) {
    return vkAcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, index);
}
extern "C" void aod_fg_pause(void) {}
extern "C" VkResult aod_fg_wait_idle(VkDevice device) { return vkDeviceWaitIdle(device); }
extern "C" VkResult aod_fg_win32_surface(VkInstance instance, void *hinstance, void *hwnd, VkSurfaceKHR *surface) {
    (void)instance, (void)hinstance, (void)hwnd, (void)surface;
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}
extern "C" void aod_fg_destroy_surface(VkInstance instance, VkSurfaceKHR surface) { vkDestroySurfaceKHR(instance, surface, NULL); }
extern "C" uint32_t aod_fg_frame_end(void) { return 0; }
extern "C" void aod_fg_frame_begin(void) {}
extern "C" void aod_fg_render_begin(void) {}
extern "C" int aod_fg_scene(VkCommandBuffer cb, const AodDlssFrame *f) { (void)cb, (void)f; return 0; }
extern "C" int aod_dlss_evaluate(VkCommandBuffer cb, const AodDlssFrame *f) { (void)cb, (void)f; return 0; }
extern "C" void aod_dlss_shutdown(void) {}

#endif
