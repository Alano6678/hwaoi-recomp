// The in-game menu core: Dear ImGui state, the event queue the window adapter fills, gamepad navigation, the Vulkan back
// end on our device and the menu's pages. Free of any window system (menu.h). ImGui runs on the present path only,
// under gfx_present.c's present lock; the event functions only queue.
//
// The Vulkan set-up follows LAS Recomp's runtime/nvn/vulkan/imgui_glue.cpp (https://github.com/peiikai/LASRecomp @
// da35b83a9611d046a9a681aae668d7085954d37f, MIT, Copyright (c) 2026 Peiikai): dynamic rendering into the swapchain
// image, an internal descriptor pool, vertex buffers rotated over more frames than are in flight.
//
// The look and layout are Age of Decomp's "Seal of the Imprisoned", after its launcher. Page switching with L / R and
// Q / E and the full performance panel follow SavageWindRecomp's runtime/ui/menu.mm
// (https://github.com/Revivedx/SavageWindRecomp @
// 575ad88e02f9a89a9a885d90039d48af33c982f4, MIT, Copyright (c) 2026 Revivedx and the Savage Wind Recomp contributors,
// Copyright (c) 2026 Peiikai). This file came from TotK Recomp (https://github.com/valentderah/totk-pc-recomp,
// MIT, Copyright (c) 2026 Valiantsin Dzerakh) through DKB Recomp.
#include <atomic>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "imgui_internal.h"   // SetFocusID, NavCursorVisible
#include "backends/imgui_impl_vulkan.h"
// A private copy of stb_truetype, the parser Dear ImGui builds its font atlas with, to check a system font before the
// atlas gets it (font_usable). Its asserts only mark the font bad here.
static int g_font_bad;
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_assert(x) ((x) ? (void)0 : (void)(g_font_bad = 1))
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include "imstb_truetype.h"
#pragma clang diagnostic pop
#include "ui/binds_page.h"
#include "ui/option_rows.h"
#include "ui/theme.h"
#include "game_target.h"

extern "C" {
#include "cfg.h"
#include "color_presets.h"
#include "input.h"
#include "input_map.h"
#include "perfmon.h"
#include "platform/platform.h"
#include "sysinfo.h"
#include "ui/menu.h"
#include "ui_text.h"
// host.h and vk.h pull in cpu.h, which is C only: the functions the menu needs from them, as declared there.
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int vk_ffx_lib_present(void);   // vk_ffx.h
void vk_ffx_fetch_async(void);  // vk_ffx_fetch.h
int vk_ffx_fetch_state(void);
int vk_dlss_offered(void);      // vk_dlss.h
int vk_dlss_failed(void);
int vk_fg_wanted(void);         // frame generation (vk_dlss.h)
int vk_fg_offered(void);
int vk_fg_failed(void);
int vk_fg_factor(void);
int vk_fg_max(void);
double vk_fg_rate(void);
int vk_hdr_offered(void);       // vk.h: the screen offers an HDR10 picture (1, 0, -1 not known yet)
void vk_queue_lock(void);
void vk_queue_unlock(void);
void res_sizes(unsigned out[2], unsigned scene[2]);
void res_auto_size(unsigned out[2]);
int rt_patch_set(const char *name);
double frame_step_limit_hz(void);
extern const int rt_game_version;
extern const char rt_port_version[];
}

// Inter and Cinzel (OFL-1.1, ui/fonts/OFL-Inter.txt and OFL-Cinzel.txt).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"   // #embed in C++
static const unsigned char inter_ttf[] = {
#embed "fonts/Inter-Variable.ttf"
};
static const unsigned char cinzel_ttf[] = {
#embed "fonts/Cinzel-Variable.ttf"
};
#pragma clang diagnostic pop

// ---- open state, any thread
static std::atomic<int> g_open;     // the menu is open
static std::atomic<int> g_reset;    // opened or closed since the last laid-out frame: drop stale input

// g_open, the input block and the log line change together under open_lock, so they cannot disagree when the window thread
// (F1, Esc), the polling thread (combo) and the present path (Continue, B) race.
static PlatMutex open_lock = PLAT_MUTEX_INIT;

// want 0 or 1 sets the state; -1 flips it. Returns the new state.
static int change_open(int want) {
    plat_mutex_lock(&open_lock);
    int open = want < 0 ? !g_open.load() : want;
    if (g_open.load() != open) {
        g_open.store(open);
        g_reset.store(1);
        input_menu(open);
        rt_log("menu: %s", open ? "open" : "closed");
    }
    plat_mutex_unlock(&open_lock);
    return open;
}

static void set_open(int open) { change_open(open); }

static int g_failed_flag();
extern "C" int menu_is_open(void) { return g_open.load(std::memory_order_relaxed); }
extern "C" void menu_toggle(void) {
    if (!g_failed_flag()) change_open(-1);
}

// ---- events from the window adapter, queued while the menu is open; the present path hands them to ImGui
enum { EV_KEY, EV_POS, EV_BUTTON, EV_WHEEL, EV_FOCUS, EV_MAX = 256 };
struct Ev { int type, a, b; float x, y; };
static PlatMutex ev_lock = PLAT_MUTEX_INIT;
static Ev ev_q[EV_MAX];
static int ev_n;

static void push(Ev e) {
    if (!menu_is_open()) return;
    plat_mutex_lock(&ev_lock);
    if (e.type == EV_POS && ev_n && ev_q[ev_n - 1].type == EV_POS) ev_q[ev_n - 1] = e;   // only the newest position
    else if (ev_n < EV_MAX) ev_q[ev_n++] = e;
    plat_mutex_unlock(&ev_lock);
}

// A key, a mouse button or the wheel makes the keyboard and mouse the device in use (the hints show keys); moving the
// pointer alone does not, so a nudged mouse leaves a controller's hints in place.
extern "C" void menu_event_key(int key, int down) {
    if (down) input_note_keyboard();
    push({EV_KEY, key, down, 0, 0});
}
extern "C" void menu_event_mouse_pos(float x, float y) { push({EV_POS, 0, 0, x, y}); }
extern "C" void menu_event_mouse_button(int button, int down) {
    if (down) input_note_keyboard();
    push({EV_BUTTON, button, down, 0, 0});
}
extern "C" void menu_event_wheel(float dx, float dy) {
    input_note_keyboard();
    push({EV_WHEEL, 0, 0, dx, dy});
}
extern "C" void menu_event_focus(int focused) { push({EV_FOCUS, focused, 0, 0, 0}); }

static void drain_events(ImGuiIO &io) {
    Ev q[EV_MAX];
    plat_mutex_lock(&ev_lock);
    int n = ev_n;
    for (int i = 0; i < n; i++) q[i] = ev_q[i];
    ev_n = 0;
    plat_mutex_unlock(&ev_lock);
    for (int i = 0; i < n; i++) {
        const Ev &e = q[i];
        switch (e.type) {
        case EV_KEY: io.AddKeyEvent((ImGuiKey)e.a, e.b != 0); break;
        case EV_POS: io.AddMousePosEvent(e.x, e.y); break;
        case EV_BUTTON: io.AddMouseButtonEvent(e.a, e.b != 0); break;
        case EV_WHEEL: io.AddMouseWheelEvent(e.x, e.y); break;
        case EV_FOCUS: io.AddFocusEvent(e.a != 0); break;
        }
    }
}

// ---- gamepad navigation: the Switch buttons of our SDL3 gamepad to ImGui's gamepad keys. Nintendo layout: A (east)
// activates, B (south) cancels; the other way round while the game's menus confirm with B (input_confirm_bottom), so
// this menu confirms with the button the game's menus confirm with.
static const struct { int bit; ImGuiKey key; } pad_keys[] = {
    {B_A, ImGuiKey_GamepadFaceDown}, {B_B, ImGuiKey_GamepadFaceRight}, {B_X, ImGuiKey_GamepadFaceUp},
    {B_Y, ImGuiKey_GamepadFaceLeft}, {B_L, ImGuiKey_GamepadL1}, {B_R, ImGuiKey_GamepadR1},
    {B_ZL, ImGuiKey_GamepadL2}, {B_ZR, ImGuiKey_GamepadR2}, {B_LEFT, ImGuiKey_GamepadDpadLeft},
    {B_UP, ImGuiKey_GamepadDpadUp}, {B_RIGHT, ImGuiKey_GamepadDpadRight}, {B_DOWN, ImGuiKey_GamepadDpadDown},
};
static uint64_t pad_prev;   // buttons at the last frame; a button held when the menu opened counts once released
static int stick_dir = -1;  // the D-pad button the left stick holds, -1 none
static bool pad_have;

// The left stick moves the cursor as the D-pad does (ImGui's own stick handling only scrolls the page): past half way
// it holds the D-pad direction of its larger axis, kept until that axis falls under 0.35; ImGui repeats a held one.
static int stick_to_dpad(int32_t lx, int32_t ly) {
    float x = lx / 32767.0f, y = ly / 32767.0f;   // y up
    float along = stick_dir == B_LEFT ? -x : stick_dir == B_RIGHT ? x : stick_dir == B_UP ? y : stick_dir == B_DOWN ? -y : 0;
    if (stick_dir >= 0 && along > 0.35f) return stick_dir;
    if (fmaxf(fabsf(x), fabsf(y)) < 0.5f) return -1;
    return fabsf(x) > fabsf(y) ? (x < 0 ? B_LEFT : B_RIGHT) : (y > 0 ? B_UP : B_DOWN);
}

static void pad_events(ImGuiIO &io, int reset) {
    uint64_t b;
    int32_t lx, ly;
    pad_have = input_pad_read(&b, &lx, &ly) != 0;
    if (pad_have) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    else io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    stick_dir = stick_to_dpad(lx, ly);
    if (stick_dir >= 0) b |= 1ull << stick_dir;
    if (input_confirm_bottom()) {   // A and B change places (a change while one is held reads as a release and a press)
        uint64_t a = b >> B_A & 1, bb = b >> B_B & 1;
        b = (b & ~(1ull << B_A | 1ull << B_B)) | a << B_B | bb << B_A;
    }
    if (reset) {
        pad_prev = b;
        return;
    }
    for (const auto &k : pad_keys) {
        uint64_t m = 1ull << k.bit;
        if ((b ^ pad_prev) & m) io.AddKeyEvent(k.key, (b & m) != 0);
    }
    pad_prev = b;
}

// ---- ImGui and its Vulkan back end (present path)
static int g_ctx_ok;              // context made
static std::atomic<int> g_failed; // set-up failed for good (the menu stays closed)
static int g_failed_flag() { return g_failed.load(std::memory_order_relaxed); }
static int g_vk_ok;               // back end initialised for g_vk_fmt
static theme::Fonts g_fonts;
const theme::Fonts &theme::fonts() { return g_fonts; }
static VkFormat g_vk_fmt;
static float g_font_scale;        // the scale of the uploaded font atlas; 0 when none is uploaded
static float g_style_scale;       // the scale the style was last sized for
static ImGuiStyle g_base_style;   // the menu's style at 1080 lines
static uint64_t g_last_ns;
static uint64_t g_cost_ns, g_cost_frames;   // CPU time of layout and recording while open, logged per 600 frames

// Sizes are tuned for 1080 lines and follow the window's height. Under 720 lines at 100% display scale they stay at the
// 720 size; that floor grows with the window's DPI, so a small window on a 150% or 200% display (sizes are physical
// pixels) still gets text as large as the display scale asks for.
static float menu_scale(uint32_t h) {
    float least = 720.0f * (float)gfx_window_dpi() / 96.0f;
    return ((float)h >= least ? (float)h : least) / 1080.0f;
}

static void style_init(ImGuiStyle &s) {
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 20; s.FrameRounding = 10; s.GrabRounding = 10; s.PopupRounding = 14; s.ChildRounding = 0;
    s.WindowPadding = ImVec2(32, 32); s.FramePadding = ImVec2(14, 8); s.ItemSpacing = ImVec2(14, 8);
    s.WindowBorderSize = 1; s.PopupBorderSize = 1; s.ScrollbarSize = 14;
    ImVec4 *c = s.Colors;
    for (int i = 0; i < ImGuiCol_COUNT; i++) c[i] = theme::colour(theme::surface3);
    c[ImGuiCol_WindowBg] = theme::colour(theme::surface1);
    c[ImGuiCol_PopupBg] = theme::colour(theme::surface2);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Border] = theme::colour(theme::line_strong);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Text] = theme::colour(theme::text);
    c[ImGuiCol_TextDisabled] = theme::colour(theme::text_dim);
    c[ImGuiCol_FrameBg] = theme::colour(theme::surface3);
    c[ImGuiCol_FrameBgHovered] = c[ImGuiCol_FrameBgActive] = theme::colour(theme::surface4);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = theme::colour(theme::surface1);
    c[ImGuiCol_MenuBarBg] = theme::colour(theme::surface2);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = theme::colour(theme::surface4);
    c[ImGuiCol_ScrollbarGrabHovered] = c[ImGuiCol_ScrollbarGrabActive] = theme::colour(theme::line_strong);
    c[ImGuiCol_CheckMark] = c[ImGuiCol_SliderGrab] = c[ImGuiCol_SliderGrabActive] = theme::colour(theme::focus);
    c[ImGuiCol_Button] = theme::colour(theme::surface3);
    c[ImGuiCol_ButtonHovered] = theme::colour(theme::surface4);
    c[ImGuiCol_ButtonActive] = theme::colour(theme::surface4);
    c[ImGuiCol_Header] = theme::colour(theme::surface3);
    c[ImGuiCol_HeaderHovered] = c[ImGuiCol_HeaderActive] = theme::colour(theme::surface4);
    c[ImGuiCol_Tab] = c[ImGuiCol_TabDimmed] = theme::colour(theme::surface2);
    c[ImGuiCol_TabHovered] = c[ImGuiCol_TabSelected] = c[ImGuiCol_TabDimmedSelected] = theme::colour(theme::surface3);
    c[ImGuiCol_TabSelectedOverline] = c[ImGuiCol_TabDimmedSelectedOverline] = theme::colour(theme::focus);
    c[ImGuiCol_PlotLines] = c[ImGuiCol_PlotLinesHovered] = c[ImGuiCol_PlotHistogram] =
        c[ImGuiCol_PlotHistogramHovered] = theme::colour(theme::focus);
    c[ImGuiCol_TextSelectedBg] = theme::colour(theme::focus_soft);
    c[ImGuiCol_TextLink] = c[ImGuiCol_DragDropTarget] = theme::colour(theme::focus);
    c[ImGuiCol_NavCursor] = theme::colour(theme::focus);
    c[ImGuiCol_Separator] = theme::colour(theme::line);
    c[ImGuiCol_SeparatorHovered] = c[ImGuiCol_SeparatorActive] = theme::colour(theme::line_strong);
    c[ImGuiCol_TableHeaderBg] = theme::colour(theme::surface2);
    c[ImGuiCol_TableBorderStrong] = theme::colour(theme::line_strong);
    c[ImGuiCol_TableBorderLight] = theme::colour(theme::line);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = theme::colour(theme::surface2);
    c[ImGuiCol_NavWindowingHighlight] = theme::colour(theme::focus);
    c[ImGuiCol_NavWindowingDimBg] = theme::colour(theme::alpha(theme::bg, 140.0f / 255));
    c[ImGuiCol_ModalWindowDimBg] = theme::colour(theme::alpha(theme::bg, 140.0f / 255));
}

bool theme::button(const char *label, ImVec2 size, float scale, bool primary) {
    ImVec2 p = ImGui::GetCursorScreenPos(), end(p.x + size.x, p.y + size.y);
    ImGui::PushID(label);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    bool pressed = ImGui::InvisibleButton("##button", size, ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    bool hover = ImGui::IsItemHovered();
    bool focus = ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float r = 10 * scale;
    if (primary) {
        dl->AddRectFilled(p, end, theme::gold, r);
        dl->AddRectFilledMultiColor(ImVec2(p.x + r, p.y), ImVec2(end.x - r, end.y),
                                    theme::gold_strong, theme::gold_strong, theme::gold, theme::gold);
    } else {
        if (hover) dl->AddRectFilled(p, end, theme::surface3, r);
        dl->AddRect(p, end, theme::line_strong, r);
    }
    if (focus) {
        dl->AddRect(ImVec2(p.x - 2 * scale, p.y - 2 * scale), ImVec2(end.x + 2 * scale, end.y + 2 * scale),
                    theme::focus_soft, r, 0, 6 * scale);
        dl->AddRect(ImVec2(p.x - 2 * scale, p.y - 2 * scale), ImVec2(end.x + 2 * scale, end.y + 2 * scale),
                    theme::focus, r, 0, 2 * scale);
    }
    ImVec2 text_size = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p.x + (size.x - text_size.x) * .5f, p.y + (size.y - text_size.y) * .5f),
                primary ? theme::gold_ink : theme::text, label);
    ImGui::PopID();
    return pressed;
}

static void make_context(void) {
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendPlatformName = "totk_menu";
    style_init(g_base_style);
    ImGui::GetStyle() = g_base_style;
    g_ctx_ok = 1;
}

static void vk_down(void) {
    if (!g_vk_ok) return;
    ImGui_ImplVulkan_Shutdown();
    g_vk_ok = 0;
    g_font_scale = 0;
}

static int vk_up(const MenuTarget *t) {
    if (g_vk_ok && g_vk_fmt == t->format) return 1;
    if (g_vk_ok) {   // the swapchain changed format: pipelines for the new one
        vk_queue_lock();
        vkQueueWaitIdle(t->queue);
        vk_queue_unlock();
        vk_down();
    }
    ImGui_ImplVulkan_InitInfo ii = {};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = t->instance;
    ii.PhysicalDevice = t->phys;
    ii.Device = t->device;
    ii.QueueFamily = t->queue_family;
    ii.Queue = t->queue;
    ii.DescriptorPoolSize = 16;
    ii.MinImageCount = 2;
    ii.ImageCount = 4;   // vertex buffers rotate per recorded frame: more than gfx_present.c's 2 frames in flight
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.UseDynamicRendering = true;
    g_vk_fmt = t->format;
    ii.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    ii.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineRenderingCreateInfo.pColorAttachmentFormats = &g_vk_fmt;
    if (!ImGui_ImplVulkan_Init(&ii)) return 0;
    g_vk_ok = 1;
    return 1;
}

// The text fonts cover Latin, Cyrillic, punctuation and arrows. Language names merge their system fonts into Inter.
static const ImWchar kRanges[] = {0x0020, 0x00FF, 0x0400, 0x052F, 0x2010, 0x2027, 0x2190, 0x2193, 0};
static const ImWchar kCyrillic[] = {0x0400, 0x052F, 0};
static const struct { const char *lang; UiTextId name; } kCjk[] = {
    {"ja", UI_MENU_LANGNAME_JA}, {"ko", UI_MENU_LANGNAME_KO}, {"zh-Hans", UI_MENU_LANGNAME_ZH_HANS},
    {"zh-Hant", UI_MENU_LANGNAME_ZH_HANT},
};

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// 1 when Dear ImGui can take this font file: more than the 100 bytes AddFontFromMemoryTTF asserts on, every table of its
// directory inside the file (stb_truetype reads tables without bounds checks, so a cut-off file faults when the glyphs
// are made), and stb_truetype parses it. ImGui asserts on a font it cannot parse, and an assert ends the process with
// 0xc0000409 and no line in the log.
static int font_usable(const uint8_t *d, uint64_t n) {
    if (!d || n <= 100 || n > INT_MAX) return 0;
    int off = stbtt_GetFontOffsetForIndex(d, 0);
    if (off < 0 || (uint64_t)off + 12 > n) return 0;
    uint32_t tables = (uint32_t)d[off + 4] << 8 | d[off + 5];
    if ((uint64_t)off + 12 + 16ull * tables > n) return 0;
    for (uint32_t i = 0; i < tables; i++) {
        const uint8_t *r = d + off + 12 + 16 * i;
        if ((uint64_t)be32(r + 8) + be32(r + 12) > n) return 0;
    }
    stbtt_fontinfo fi;
    g_font_bad = 0;
    return stbtt_InitFont(&fi, d, off) && !g_font_bad;
}

extern "C" int menu_font_check(void) {
    uint64_t n = sizeof inter_ttf;
    uint8_t *bad = (uint8_t *)malloc(n);
    if (!bad) return 0;
    memcpy(bad, inter_ttf, n);
    uint32_t tables = (uint32_t)bad[4] << 8 | bad[5];
    for (uint32_t i = 0; i < tables; i++)
        if (!memcmp(bad + 12 + 16 * i, "cmap", 4)) memset(bad + 12 + 16 * i, 0, 4);   // no character map
    int ok = font_usable(inter_ttf, n) && !font_usable(inter_ttf, 4096) && !font_usable(inter_ttf, 100) &&
             !font_usable(bad, n);
    memset(bad, 0, 4);             // an unknown font tag
    ok = ok && !font_usable(bad, n);
    free(bad);
    return ok;
}

// Inter at 25 / 21 px and Cinzel at 38 / 24 px per 1080 lines; Inter supplies the Cyrillic glyphs Cinzel lacks.
// The Japanese, Korean and Chinese language names merge into the text font from a system font when there is one (each
// language is named in its own script). Made again when the window height changes; the old texture goes after the queue
// is idle (CreateFontsTexture).
static int fonts_up(float scale) {
    enum { N = sizeof kCjk / sizeof *kCjk };
    ImFontAtlas *fa = ImGui::GetIO().Fonts;
    fa->Clear();
    ImFontConfig fc;
    fc.FontDataOwnedByAtlas = false;
    g_fonts.text = fa->AddFontFromMemoryTTF((void *)inter_ttf, (int)sizeof inter_ttf, 25.0f * scale, &fc, kRanges);
    uint8_t *data[N] = {};
    ImVector<ImWchar> ranges[N];
    int merged = 0;
    for (int i = 0; i < N; i++) {
        char path[512];
        uint64_t size = 0;
        if (!plat_font_path(kCjk[i].lang, path, sizeof path) || !(data[i] = plat_file_read(path, &size))) continue;
        if (!font_usable(data[i], size)) {
            rt_log("menu: system font %s left out: the font parser cannot read it (%llu bytes)", path,
                   (unsigned long long)size);
            free(data[i]);
            data[i] = NULL;
            continue;
        }
        merged++;
        ImFontGlyphRangesBuilder b;
        b.AddText(ui_text_in("en", kCjk[i].name));
        if (!strcmp(kCjk[i].lang, "zh-Hans"))
            for (int id = UI_MENU_FIRST; id <= UI_MENU_LAST_ID; id++)
                b.AddText(ui_text_in("zh-Hans", (UiTextId)id));
        b.BuildRanges(&ranges[i]);
        ImFontConfig mc;
        mc.MergeMode = true;
        mc.FontDataOwnedByAtlas = false;
        fa->AddFontFromMemoryTTF(data[i], (int)size, 25.0f * scale, &mc, ranges[i].Data);
    }
    g_fonts.small = fa->AddFontFromMemoryTTF((void *)inter_ttf, (int)sizeof inter_ttf, 21.0f * scale, &fc, kRanges);
    g_fonts.title = fa->AddFontFromMemoryTTF((void *)cinzel_ttf, (int)sizeof cinzel_ttf, 38.0f * scale, &fc, kRanges);
    ImFontConfig merge;
    merge.MergeMode = true;
    merge.FontDataOwnedByAtlas = false;
    fa->AddFontFromMemoryTTF((void *)inter_ttf, (int)sizeof inter_ttf, 38.0f * scale, &merge, kCyrillic);
    g_fonts.brand = fa->AddFontFromMemoryTTF((void *)cinzel_ttf, (int)sizeof cinzel_ttf, 24.0f * scale, &fc, kRanges);
    fa->AddFontFromMemoryTTF((void *)inter_ttf, (int)sizeof inter_ttf, 24.0f * scale, &merge, kCyrillic);
    ImGui::GetIO().FontDefault = g_fonts.text;
    vk_queue_lock();   // the upload submits on the queue and waits for it
    int ok = ImGui_ImplVulkan_CreateFontsTexture();
    vk_queue_unlock();
    fa->ClearInputData();   // the glyphs are in the texture: the font files and the pixels are not needed again
    fa->ClearTexData();
    for (uint8_t *d : data) free(d);
    rt_log("menu: font atlas at %.1f px, merged %d of %d CJK system fonts", 25.0f * scale, merged, (int)N);
    g_font_scale = ok ? scale : 0;
    return ok;
}

extern "C" void menu_vk_release(void) {
    if (g_ctx_ok) vk_down();
}

// ---- test hooks
// TOTK_MENU_TEST=<frame>[:<frames per page>]: at that presented frame the menu opens on its first tab; each tab stays
// <frames per page> frames (default 30) and its last frame's swapchain image goes to TOTK_MENU_TEST_DIR (default: the
// working directory) as menu-test-<frame>-<page>.png; after the last tab the menu closes.
// TOTK_MENU_TEST_SET=<frame>:<key>=<value>[,<key>=<value>...][;<frame>:...]: cfg_set of each key (TOTK_ prefix
// optional) at that presented frame; changes live settings with no input, menu open or not. totk.ini is written only by
// the pseudo-key SAVE=1 (cfg_save, as a row change does; a failed write shows the menu's save note). The pseudo-key
// RESTART=1 does what the action bar's Restart does, only while that button would show (so a game started again with
// the same hook does not restart a second time).
enum { TAB_SCREEN, TAB_GAME, TAB_SOUND, TAB_CONTROLS, TAB_MENU, TAB_ADVANCED, TAB_COUNT };
enum { TEST_PAGE_FRAMES = 30, TEST_SETS = 16 };
struct TestSet { uint64_t frame; char key[64], value[64]; };
static struct {
    int parsed;
    uint64_t at;          // TOTK_MENU_TEST frame; 0 none
    uint64_t per_page;
    uint64_t opened;      // the frame the hook opened the menu; 0 not yet
    int page;             // the tab being shot; -1 none
    TestSet sets[TEST_SETS];
    int n_sets, next_set; // TOTK_MENU_TEST_SET, in frame order
} g_test = {0, 0, 0, 0, -1, {}, 0, 0};

static void test_parse(void) {
    g_test.parsed = 1;
    if (const char *e = cfg_get("TOTK_MENU_TEST")) {
        long long at = atoll(e), per = 0;
        if (const char *c = strchr(e, ':')) per = atoll(c + 1);
        g_test.at = at > 0 ? (uint64_t)at : 0;
        g_test.per_page = per > 0 ? (uint64_t)per : TEST_PAGE_FRAMES;
    }
    const char *s = cfg_get("TOTK_MENU_TEST_SET");
    while (s && *s) {   // groups "<frame>:<key>=<value>,..." separated by ';'
        char *end;
        unsigned long long f = strtoull(s, &end, 10);
        if (*end != ':') break;
        s = end + 1;
        while (*s && *s != ';' && g_test.n_sets < TEST_SETS) {
            size_t n = strcspn(s, ",;");
            const char *eq = (const char *)memchr(s, '=', n);
            if (eq) {
                TestSet &t = g_test.sets[g_test.n_sets++];
                t.frame = f;
                int kn = (int)(eq - s), vn = (int)(s + n - eq - 1);
                snprintf(t.key, sizeof t.key, "%s%.*s", strncmp(s, "TOTK_", 5) ? "TOTK_" : "", kn, s);
                snprintf(t.value, sizeof t.value, "%.*s", vn, eq + 1);
            }
            s += n;
            if (*s == ',') s++;
        }
        while (*s && *s != ';') s++;
        if (*s == ';') s++;
    }
    if (g_test.at || g_test.n_sets)
        rt_log("menu: test hook: open at frame %llu, %llu frames per page, %d settings changes",
               (unsigned long long)g_test.at, (unsigned long long)g_test.per_page, g_test.n_sets);
}

static void test_apply_sets(uint64_t frame) {
    for (; g_test.next_set < g_test.n_sets && g_test.sets[g_test.next_set].frame <= frame; g_test.next_set++) {
        const TestSet &t = g_test.sets[g_test.next_set];
        rt_log("menu: test hook: frame %llu: %s=%s", (unsigned long long)frame, t.key, t.value);
        if (!strcmp(t.key, "TOTK_SAVE")) rt_log("menu: test hook: cfg_save %s", cfg_save() ? "wrote" : "failed");
        else if (!strcmp(t.key, "TOTK_RESTART")) {
            bool shown = opt_restart_needed() && gfx_restart_available();
            rt_log("menu: test hook: Restart %s", shown ? "pressed" : "not shown");
            if (shown && cfg_save()) gfx_request_restart();
        } else cfg_set(t.key, t.value);
    }
}

// ---- the lists the Screen and Game tabs build
enum { MAX_GPUS = 8 };
static char g_gpu_names[MAX_GPUS][VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
static int g_gpu_n = -1;   // -1: not listed yet

// The Vulkan devices in vkEnumeratePhysicalDevices order, which TOTK_GPU_INDEX counts in (vk_device.c pick_device).
static void list_gpus(VkInstance inst) {
    if (g_gpu_n >= 0) return;
    VkPhysicalDevice pd[MAX_GPUS];
    uint32_t n = MAX_GPUS;
    if (vkEnumeratePhysicalDevices(inst, &n, pd) < 0) n = 0;   // VK_INCOMPLETE keeps the first MAX_GPUS
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        snprintf(g_gpu_names[i], sizeof g_gpu_names[i], "%s", p.deviceName);
    }
    g_gpu_n = (int)n;
}

// Window sizes: the common 16:9 sizes that fit the monitor, the 1920x1080 default and the current size.
static const int kSizes[][2] = {{1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3200, 1800},
                                {3840, 2160}};
enum { MAX_SIZES = sizeof kSizes / sizeof *kSizes + 1 };

static void row_window_size(void) {
    int sz[MAX_SIZES][2], n = 0, mw = 0, mh = 0, cw = 0, ch = 0;
    int have = gfx_window_monitor_size(&mw, &mh);
    int cur_ok = sscanf(cfg_value("TOTK_WINDOW_SIZE"), "%dx%d", &cw, &ch) == 2;
    bool listed = false;
    for (size_t i = 0; i < sizeof kSizes / sizeof *kSizes; i++) {
        int w = kSizes[i][0], h = kSizes[i][1];
        bool cur = cur_ok && w == cw && h == ch;
        if (have && (w > mw || h > mh) && !cur && !(w == 1920 && h == 1080)) continue;
        listed |= cur;
        sz[n][0] = w, sz[n][1] = h, n++;
    }
    if (cur_ok && !listed) {   // the current size where its area puts it
        int at = n;
        while (at > 0 && (long)sz[at - 1][0] * sz[at - 1][1] > (long)cw * ch) at--;
        memmove(sz[at + 1], sz[at], sizeof sz[0] * (n - at));
        sz[at][0] = cw, sz[at][1] = ch, n++;
    }
    char vals[MAX_SIZES][24], shown[MAX_SIZES][32];
    const char *pv[MAX_SIZES], *pn[MAX_SIZES];
    for (int i = 0; i < n; i++) {
        snprintf(vals[i], sizeof vals[i], "%dx%d", sz[i][0], sz[i][1]);
        snprintf(shown[i], sizeof shown[i], "%d × %d", sz[i][0], sz[i][1]);   // U+00D7, the multiplication sign
        pv[i] = vals[i];
        pn[i] = shown[i];
    }
    // In fullscreen the window covers the screen and this size waits for Windowed: the row is greyed, as the launcher's
    // (when: fullscreen = 0), and says the size in use.
    static char in_use[96];
    const char *note = nullptr;
    if (cfg_bool("TOTK_FULLSCREEN")) {
        opt_disable_next(UI_MENU_WINDOW_SIZE_WINDOWED_ONLY);
        if (have && !(cur_ok && cw == mw && ch == mh)) {   // as the other "in use" notes: only when it differs
            char screen[32];
            snprintf(screen, sizeof screen, "%d × %d", mw, mh);
            snprintf(in_use, sizeof in_use, menu_text(UI_MENU_PRESENT_IN_USE), screen);
            note = in_use;
        }
    }
    opt_choice_list("TOTK_WINDOW_SIZE", UI_MENU_WINDOW_SIZE, pv, pn, n, UI_MENU_WINDOW_SIZE_DESC, -1, note);
}

// The game resolution (resolution.c): 16:9 sizes from its smallest, 1280x720 (for weak GPUs), up, the common 21:9 and
// 32:9 monitor sizes and the 16:10 screens of the Steam Deck, handhelds and laptops (the camera and the HUD follow
// another shape, resolution.c res_init; the installer lays the HUD out for each, installer/steps.py ASPECT_SIZES), and
// the current value when it is another size.
static void row_output_res(void) {
    if (RT_TARGET_HWAOI) {
        // auto: the screen's size as far as the graphics card's memory allows (resolution.c); the row names what it is
        static const char *const vals[] = {"auto", "1280x720", "1920x1080", "2560x1440", "3840x2160"};
        static char automatic[64];
        unsigned a[2];
        res_auto_size(a);
        snprintf(automatic, sizeof automatic, menu_text(UI_MENU_OUTPUT_RES_AUTO), a[0], a[1]);
        const char *names[] = {automatic, "1280 × 720", "1920 × 1080", "2560 × 1440", "3840 × 2160"};
        opt_choice_list("TOTK_OUTPUT_RES", UI_MENU_OUTPUT_RES, vals, names, 5, UI_MENU_OUTPUT_RES_DESC);
        return;
    }
    static const char *const vals[] = {"1280x720", "1600x900", "1920x1080", "2560x1440", "3200x1800", "3840x2160",
                                       "2560x1080", "3440x1440", "3840x1600", "5120x2160", "3840x1080", "5120x1440",
                                       "1280x800", "1680x1050", "1920x1200", "2560x1600"};
    enum { N = sizeof vals / sizeof *vals };
    char shown[N][32];
    const char *names[N];
    for (int i = 0; i < N; i++) {
        int w = 0, h = 0;
        sscanf(vals[i], "%dx%d", &w, &h);
        snprintf(shown[i], sizeof shown[i], "%d × %d", w, h);   // U+00D7, the multiplication sign
        names[i] = shown[i];
    }
    opt_choice_list("TOTK_OUTPUT_RES", UI_MENU_OUTPUT_RES, vals, names, N, UI_MENU_OUTPUT_RES_DESC);
}

static void row_render_scale(void) {
    if (RT_TARGET_HWAOI) return; // Native presets use 100%; inherited supersampling has no profile here.
    static const char *const vals[] = {"game", "50", "67", "75", "100", "125", "150", "200"};
    enum { N = sizeof vals / sizeof *vals };
    char shown[N][32];
    const char *names[N];
    names[0] = menu_text(UI_MENU_RENDER_SCALE_GAME);
    for (int i = 1; i < N; i++) {
        snprintf(shown[i], sizeof shown[i], "%s %%", vals[i]);
        names[i] = shown[i];
    }
    opt_choice_list("TOTK_RENDER_SCALE", UI_MENU_RENDER_SCALE, vals, names, N, UI_MENU_RENDER_SCALE_DESC);
}

static void row_gpu(void) {
    const char *vals[MAX_GPUS + 1], *names[MAX_GPUS + 1];
    char idx[MAX_GPUS][4];
    vals[0] = "";
    names[0] = menu_text(UI_MENU_GPU_AUTO);
    for (int i = 0; i < g_gpu_n; i++) {
        snprintf(idx[i], sizeof idx[i], "%d", i);
        vals[i + 1] = idx[i];
        names[i + 1] = g_gpu_names[i];
    }
    opt_choice_list("TOTK_GPU_INDEX", UI_MENU_GPU, vals, names, g_gpu_n + 1, UI_MENU_GPU_DESC);
}

// The game language: one entry per language TotK ships (a Switch code that names it), each under its own name. The
// Switch's Portuguese is not among them.
static void row_game_lang(void) {
    static const struct { const char *code; UiTextId name; } langs[] = {
        {"en-US", UI_MENU_LANGNAME_EN}, {"ja", UI_MENU_LANGNAME_JA}, {"fr", UI_MENU_LANGNAME_FR},
        {"de", UI_MENU_LANGNAME_DE}, {"it", UI_MENU_LANGNAME_IT}, {"es", UI_MENU_LANGNAME_ES},
        {"nl", UI_MENU_LANGNAME_NL}, {"ru", UI_MENU_LANGNAME_RU},
        {"zh-Hans", UI_MENU_LANGNAME_ZH_HANS}, {"zh-Hant", UI_MENU_LANGNAME_ZH_HANT}, {"ko", UI_MENU_LANGNAME_KO},
    };
    enum { N = sizeof langs / sizeof *langs + 1 };
    const char *vals[N], *names[N];
    vals[0] = "";
    names[0] = menu_text(UI_MENU_GAME_LANG_AUTO);
    const char *now = cfg_value("TOTK_LANG");
    int cur = *now ? -1 : 0;
    for (int i = 1; i < N; i++) {
        vals[i] = langs[i - 1].code;
        names[i] = menu_text(langs[i - 1].name);
        if (*now && !strcmp(ui_text_lang(now), ui_text_lang(vals[i]))) cur = i;   // en-GB is English too
    }
    opt_choice_list("TOTK_LANG", UI_MENU_GAME_LANG, vals, names, N, UI_MENU_GAME_LANG_DESC, cur);
}

// The frame limit (frame_rate.c): unset, the older fps60 = 0 still means 30. The monitor choice shows the rate it gives.
static void row_fps_limit() {
    if (RT_TARGET_HWAOI) {
        // Above 60 the game's fixed step runs the whole game faster, so players get 30 and 60 (frame_rate.c).
        static const char *const vals[] = {"30", "60"};
        const char *names[] = {menu_text(UI_MENU_FPS_30), menu_text(UI_MENU_FPS_60)};
        opt_choice_list("TOTK_FPS_LIMIT", UI_MENU_FPS_LIMIT, vals, names, 2, UI_MENU_FPS_LIMIT_DESC);
        return;
    }
    // 40 and 45: the Steam Deck's screen runs at 40 to 60 Hz (the OLED to 90); 40 divides 40 and 120 Hz evenly
    static const char *const vals[] = {"30", "40", "45", "60", "90", "120", "144", "display"};
    static const UiTextId ids[] = {UI_MENU_FPS_30, UI_MENU_FPS_40,  UI_MENU_FPS_45,  UI_MENU_FPS_60,
                                   UI_MENU_FPS_90, UI_MENU_FPS_120, UI_MENU_FPS_144, UI_MENU_FPS_DISPLAY};
    enum { N = sizeof vals / sizeof *vals };
    const char *names[N];
    for (int i = 0; i < N; i++) names[i] = menu_text(ids[i]);
    static char display[64];
    double hz = gfx_window_refresh_hz();
    if (hz > 0) {
        snprintf(display, sizeof display, menu_text(UI_MENU_FPS_DISPLAY_HZ), hz);
        names[N - 1] = display;
    }
    const char *set = cfg_get("TOTK_FPS_LIMIT"), *old = cfg_get("TOTK_FPS60");
    int cur = (!set || !*set) && old && !strcmp(old, "0") ? 0 : -1;
    opt_choice_list("TOTK_FPS_LIMIT", UI_MENU_FPS_LIMIT, vals, names, N, UI_MENU_FPS_LIMIT_DESC, cur);
}

// The menu's name of a TOTK_VK_PRESENT mode (gfx_window_present_mode); NULL for another.
static const char *present_name(const char *mode) {
    static const struct { const char *mode; UiTextId name; } names[] = {
        {"mailbox", UI_MENU_PRESENT_MAILBOX}, {"fifo", UI_MENU_PRESENT_VSYNC}, {"immediate", UI_MENU_PRESENT_IMMEDIATE},
    };
    for (const auto &n : names)
        if (!strcmp(n.mode, mode)) return menu_text(n.name);
    return nullptr;
}

// "in use: VSync" under the Presentation value when the swapchain got another mode than the setting (a driver without
// mailbox gets fifo); NULL when they agree or before the first swapchain.
static const char *present_note(void) {
    const char *got = gfx_window_present_mode(), *name = present_name(got);
    if (!*got || !strcmp(got, cfg_value("TOTK_VK_PRESENT")) || !name) return nullptr;
    static char note[96];
    snprintf(note, sizeof note, menu_text(UI_MENU_PRESENT_IN_USE), name);
    return note;
}

// ---- the tabs
// Colour adjustment (vk_color.c): a profile writes the four values (STS78721's presets, issue #136); moving one of them
// makes the profile Custom.
static void rows_color(void) {
    // The profiles' values come from color_presets.h, the table the picture itself applies (vk_color.c).
    static const UiTextId names[] = {UI_MENU_COLOR_DEFAULT, UI_MENU_COLOR_VIVID, UI_MENU_COLOR_CLARITY,
                                     UI_MENU_COLOR_OLED, UI_MENU_COLOR_CUSTOM};   // COLOR_PRESET's order in cfg.c
    opt_group(UI_MENU_COLOR);
    if (opt_choice("TOTK_COLOR_PRESET", UI_MENU_COLOR_PRESET, names, 5, UI_MENU_COLOR_PRESET_DESC)) {
        if (const ColorPreset *e = color_preset_find(cfg_value("TOTK_COLOR_PRESET"))) {
            cfg_set("TOTK_COLOR_BRIGHTNESS", e->brightness), cfg_set("TOTK_COLOR_CONTRAST", e->contrast);
            cfg_set("TOTK_COLOR_SATURATION", e->saturation), cfg_set("TOTK_COLOR_GAMMA", e->gamma);
            cfg_save();
        }
    }
    bool moved = opt_slider("TOTK_COLOR_BRIGHTNESS", UI_MENU_COLOR_BRIGHTNESS, -30, 30, UI_MENU_COLOR_BRIGHTNESS_DESC, 1,
                            "%+.0f %%");
    moved |= opt_slider("TOTK_COLOR_CONTRAST", UI_MENU_COLOR_CONTRAST, 70, 150, UI_MENU_COLOR_CONTRAST_DESC, 5, "%.0f %%");
    moved |= opt_slider("TOTK_COLOR_SATURATION", UI_MENU_COLOR_SATURATION, 50, 200, UI_MENU_COLOR_SATURATION_DESC, 5,
                        "%.0f %%");
    moved |= opt_slider("TOTK_COLOR_GAMMA", UI_MENU_COLOR_GAMMA, 0.8f, 1.2f, UI_MENU_COLOR_GAMMA_DESC, 0.02f, "%.2f");
    if (moved && strcmp(cfg_value("TOTK_COLOR_PRESET"), "custom")) {
        cfg_set("TOTK_COLOR_PRESET", "custom");
        cfg_save();
    }
}

// The upscaler of the 3D scene (TEMPORAL, read at start): AMD FSR (vk_ffx.c) or NVIDIA DLSS (vk_dlss.c), each with its
// own quality key. The window's own scaling (UPSCALER) is another row. 0 off, 1 FSR, 2 DLSS ("dlaa" is 0.1.2's name
// for DLSS at the native size).
static int temporal_now(void) {
    const char *v = cfg_value("TOTK_TEMPORAL");
    return !strcmp(v, "fsr") ? 1 : !strcmp(v, "dlss") || !strcmp(v, "dlaa") ? 2 : 0;
}

static void rows_temporal(void) {
    static const UiTextId names[] = {UI_MENU_TEMPORAL_OFF, UI_MENU_TEMPORAL_FSR, UI_MENU_TEMPORAL_DLSS};
    static const int at_start = temporal_now();
    if (!strcmp(cfg_value("TOTK_TEMPORAL"), "dlaa")) {   // the same thing under the newer names
        cfg_set("TOTK_TEMPORAL", "dlss");
        cfg_set("TOTK_DLSS_QUALITY", "native");
        opt_save_soon();
    }
    int cur = temporal_now();
    static char in_use[96];
    const char *note = nullptr;
#ifdef _WIN32
    // Windows: AMD's DLLs are downloaded from its SDK repository when FSR is picked (or at start, if it already is)
    bool fsr = true;
    if (cur == 1 && !vk_ffx_lib_present()) note = menu_text(UI_MENU_FFX_DOWNLOAD);
#else
    bool fsr = vk_ffx_lib_present() != 0;
    if (!fsr) note = menu_text(UI_MENU_FFX_MISSING);
#endif
    // DLSS was asked for at start and does not run (no RTX card, a DLL missing, Streamline failed)
    if (cur == 2 && at_start == 2 && vk_dlss_failed()) {
        snprintf(in_use, sizeof in_use, menu_text(UI_MENU_PRESENT_IN_USE), menu_text(UI_MENU_TEMPORAL_OFF));
        note = in_use;
    }
    // DLSS is offered on NVIDIA cards that have its files beside the game (and shown while it is the setting)
    int n = vk_dlss_offered() || cur == 2 ? 3 : 2;
    int pick = opt_select("TOTK_TEMPORAL", UI_MENU_TEMPORAL, names, n, cur, UI_MENU_TEMPORAL_DESC, 0, fsr ? -1 : 1,
                          cur != at_start, note);
    if (pick >= 0) {
        if (pick == 1 && !vk_ffx_lib_present()) vk_ffx_fetch_async();
        cfg_set("TOTK_TEMPORAL", pick == 1 ? "fsr" : pick == 2 ? "dlss" : "off");
        opt_save_soon();
        cur = pick;
    }
    static const UiTextId fq[] = {UI_MENU_FSR_Q_NATIVE, UI_MENU_FSR_Q_QUALITY, UI_MENU_FSR_Q_BALANCED,
                                  UI_MENU_FSR_Q_PERFORMANCE};
    static const UiTextId dq[] = {UI_MENU_DLSS_Q_NATIVE, UI_MENU_FSR_Q_QUALITY, UI_MENU_FSR_Q_BALANCED,
                                  UI_MENU_FSR_Q_PERFORMANCE};
    if (!cur) opt_disable_next(UI_MENU_NEEDS_UPSCALING);
    if (cur == 2) opt_choice("TOTK_DLSS_QUALITY", UI_MENU_TEMPORAL_QUALITY, dq, 4, UI_MENU_TEMPORAL_QUALITY_DESC);
    else opt_choice("TOTK_FSR_QUALITY", UI_MENU_TEMPORAL_QUALITY, fq, 4, UI_MENU_TEMPORAL_QUALITY_DESC);
    if (cur != 1) return;   // FSR's own rows
    static const char *const fv[] = {"auto", "3.1"};
    const char *fn[] = {menu_text(UI_MENU_FSR_V_AUTO), menu_text(UI_MENU_FSR_V_31)};
    opt_choice_list("TOTK_FSR_VERSION", UI_MENU_FSR_VERSION, fv, fn, 2, UI_MENU_FSR_VERSION_DESC);
    static const char *const sv[] = {"", "game", "0", "50", "100"};
    const char *sn[] = {menu_text(UI_MENU_SCENE_SHARP_AUTO), menu_text(UI_MENU_SCENE_SHARP_GAME), "0 %", "50 %", "100 %"};
    opt_choice_list("TOTK_SCENE_SHARPNESS", UI_MENU_SCENE_SHARP, sv, sn, 5, UI_MENU_SCENE_SHARP_DESC);
}

// NVIDIA DLSS frame generation (FRAME_GEN, read at start; vk_dlss.c): frames shown per frame the game draws. Offered
// on NVIDIA cards that have its files beside the game, and shown while it is the setting. Once it runs, the list ends
// at the multiplier the graphics card reaches.
static void row_frame_gen(void) {
    static const char *const all[] = {"off", "2", "3", "4", "5", "6"};
    static const char *const all_names[] = {nullptr, "2x", "3x", "4x", "5x", "6x"};
    static const int at_start = vk_fg_wanted();
    int now = atoi(cfg_value("TOTK_FRAME_GEN"));   // "off" reads as 0
    if (!vk_fg_offered() && now < 2) return;
    // Off, then 2x up to what the card reaches (6x until that is known), and the value set if it is beyond that
    const char *values[6], *names[6];
    int max = vk_fg_max(), top = max >= 2 ? max : 6, n = 0;
    for (int i = 0; i < 6; i++) {
        int factor = i ? i + 1 : 0;
        if (factor > top && factor != now) continue;
        values[n] = all[i], names[n++] = i ? all_names[i] : menu_text(UI_MENU_TEMPORAL_OFF);
    }
    static char in_use[96];
    const char *note = nullptr;
    // asked for at start and it does not run (no RTX 40 or 50 card, a file missing), or the card stops below it
    int running = vk_fg_factor();
    if (at_start && now == at_start && (vk_fg_failed() || (running && running != at_start))) {
        snprintf(in_use, sizeof in_use, menu_text(UI_MENU_PRESENT_IN_USE),
                 running >= 2 && running <= 6 ? all_names[running - 1] : menu_text(UI_MENU_TEMPORAL_OFF));
        note = in_use;
    }
    opt_choice_list("TOTK_FRAME_GEN", UI_MENU_FRAME_GEN, values, names, n, UI_MENU_FRAME_GEN_DESC, -1, note);
}

static void tab_screen(float scale) {
    opt_begin("##screen", scale);
    static const UiTextId fullscreen[] = {UI_MENU_WINDOWED, UI_MENU_FULLSCREEN_MODE};   // TOTK_FULLSCREEN 0, 1
    opt_choice("TOTK_FULLSCREEN", UI_MENU_FULLSCREEN, fullscreen, 2, UI_MENU_FULLSCREEN_DESC);
    row_window_size();
    static const UiTextId present[] = {UI_MENU_PRESENT_MAILBOX, UI_MENU_PRESENT_VSYNC, UI_MENU_PRESENT_IMMEDIATE};
    opt_choice("TOTK_VK_PRESENT", UI_MENU_PRESENT, present, 3, UI_MENU_PRESENT_DESC, present_note());
    row_gpu();
    static const UiTextId perf[] = {UI_MENU_PERF_OFF, UI_MENU_PERF_FPS, UI_MENU_PERF_FULL};
    opt_choice("TOTK_PERF_OVERLAY", UI_MENU_PERF_OVERLAY, perf, 3, UI_MENU_PERF_OVERLAY_DESC);
    opt_group(UI_MENU_GRAPHICS);
    row_output_res();
    row_render_scale();
    if (RT_TARGET_HWAOI) rows_temporal();
    if (RT_TARGET_HWAOI) row_frame_gen();
    static const UiTextId up[] = {UI_MENU_UPSCALER_FSR, UI_MENU_UPSCALER_BILINEAR};
    opt_choice("TOTK_UPSCALER", UI_MENU_UPSCALER, up, 2, UI_MENU_UPSCALER_DESC);
    opt_slider("TOTK_SHARPNESS", UI_MENU_SHARPNESS, 0, 100, UI_MENU_SHARPNESS_DESC, 5, "%.0f %%");
    static const UiTextId aspect[] = {UI_MENU_ASPECT_FIT, UI_MENU_ASPECT_STRETCH};
    opt_choice("TOTK_ASPECT", UI_MENU_ASPECT, aspect, 2, UI_MENU_ASPECT_DESC);
    // Only where this game version has the field-of-view patch (res_fov_live); without it the row would do nothing.
    if (rt_patch_set("fov") >= 0) opt_slider("TOTK_FOV", UI_MENU_FOV, 70, 140, UI_MENU_FOV_DESC, 5, "%.0f %%");
    if (RT_TARGET_HWAOI) {   // SMAA on the present (vk_smaa.c)
        static const UiTextId aa[] = {UI_MENU_AA_OFF, UI_MENU_AA_SMAA};
        opt_choice("TOTK_ANTIALIASING", UI_MENU_AA, aa, 2, UI_MENU_AA_DESC);
    }
    rows_color();
    if (RT_TARGET_HWAOI) {   // HDR10 output (vk_hdr.c)
        opt_group(UI_MENU_HDR_GROUP);
        // Greyed while the window's screen offers no HDR10 picture (HDR off in the system, or a screen without it), as
        // the launcher greys them; the brightness rows also while HDR output is off (the launcher's when: hdr = 1).
        bool no_screen = vk_hdr_offered() == 0, grey = no_screen || !cfg_bool("TOTK_HDR");
        UiTextId why = no_screen ? UI_MENU_HDR_NO_SCREEN : UI_MENU_NEEDS_HDR;
        static char in_use[96];   // set on such a screen: the picture is SDR, as the other rows' "in use: Off"
        const char *note = nullptr;
        if (no_screen && cfg_bool("TOTK_HDR")) {
            snprintf(in_use, sizeof in_use, menu_text(UI_MENU_PRESENT_IN_USE), menu_text(UI_MENU_TEMPORAL_OFF));
            note = in_use;
        }
        if (no_screen) opt_disable_next(why);
        opt_toggle("TOTK_HDR", UI_MENU_HDR, UI_MENU_HDR_DESC, false, note);
        if (grey) opt_disable_next(why);
        opt_slider("TOTK_HDR_PEAK_NITS", UI_MENU_HDR_PEAK, 400, 10000, UI_MENU_HDR_PEAK_DESC, 50, "%.0f nits");
        if (grey) opt_disable_next(why);
        opt_slider("TOTK_HDR_GAME_NITS", UI_MENU_HDR_GAME, 80, 500, UI_MENU_HDR_GAME_DESC, 5, "%.0f nits");
        if (grey) opt_disable_next(why);
        opt_slider("TOTK_HDR_UI_NITS", UI_MENU_HDR_UI, 80, 500, UI_MENU_HDR_UI_DESC, 5, "%.0f nits");
    }
    opt_end();
}

static void tab_game(float scale) {
    opt_begin("##game", scale);
    row_fps_limit();
    if (!RT_TARGET_HWAOI) {
    opt_toggle("TOTK_REALTIME", UI_MENU_REALTIME, UI_MENU_REALTIME_DESC);
    opt_toggle("TOTK_CLOTH_REALTIME", UI_MENU_CLOTH_REALTIME, UI_MENU_CLOTH_REALTIME_DESC);
    opt_toggle("TOTK_HIGHFPS_FIX", UI_MENU_HIGHFPS_FIX, UI_MENU_HIGHFPS_FIX_DESC);
    }
    row_game_lang();
    static const UiTextId mode[] = {UI_MENU_MODE_DOCKED, UI_MENU_MODE_HANDHELD};
    opt_choice("TOTK_OPERATION_MODE", UI_MENU_OPERATION_MODE, mode, 2, UI_MENU_OPERATION_MODE_DESC);
    opt_toggle("TOTK_ASSUME_FOCUS", UI_MENU_ASSUME_FOCUS, UI_MENU_ASSUME_FOCUS_DESC);
    opt_end();
}

static void tab_sound(float scale) {
    opt_begin("##sound", scale);
    opt_slider("TOTK_VOLUME", UI_MENU_VOLUME, 0, 300, UI_MENU_VOLUME_DESC, 5, "%.0f %%");
    opt_toggle("TOTK_MUTE_UNFOCUSED", UI_MENU_MUTE_UNFOCUSED, UI_MENU_MUTE_UNFOCUSED_DESC);
    static const UiTextId latency[] = {UI_MENU_AUDIO_LATENCY_LOW, UI_MENU_AUDIO_LATENCY_NORMAL,
                                       UI_MENU_AUDIO_LATENCY_HIGH};
    opt_choice("TOTK_AUDIO_LATENCY", UI_MENU_AUDIO_LATENCY, latency, 3, UI_MENU_AUDIO_LATENCY_DESC);
    opt_end();
}

// A player's input device (input.c assign): Auto, the keyboard and mouse, each connected gamepad by name, and None for
// player 2. A saved gamepad that is not connected keeps its value and shows as such.
static void row_player_device(int p) {
    enum { MAX = 12 };
    InputDeviceChoice pads[8];
    int npads = input_device_choices(pads, 8);
    const char *vals[MAX], *names[MAX];
    int n = 0;
    vals[n] = "auto", names[n++] = menu_text(UI_MENU_DEVICE_AUTO);
    vals[n] = "keyboard", names[n++] = menu_text(UI_MENU_DEVICE_KEYBOARD);
    for (int i = 0; i < npads && n < MAX - 2; i++) vals[n] = pads[i].value, names[n++] = pads[i].name;
    if (p > 0) vals[n] = "none", names[n++] = menu_text(UI_MENU_DEVICE_NONE);
    char key[32];
    snprintf(key, sizeof key, "TOTK_PLAYER%d_DEVICE", p + 1);
    const char *now = cfg_value(key);
    int cur = -1;
    for (int i = 0; i < n; i++)
        if (!strcmp(vals[i], now)) cur = i;
    static char missing[INPUT_PLAYERS][64];
    if (cur < 0 && !strncmp(now, "pad:", 4)) {
        snprintf(missing[p], sizeof missing[p], "%s", now);
        vals[n] = missing[p], names[n] = menu_text(UI_MENU_DEVICE_MISSING), cur = n++;
    }
    const char *note = p > 0 && !input_player_connected(p) ? menu_text(UI_MENU_PLAYER2_OFF_NOTE) : nullptr;
    opt_choice_list(key, p ? UI_MENU_PLAYER2_DEVICE : UI_MENU_PLAYER1_DEVICE, vals, names, n, UI_MENU_PLAYER_DEVICE_DESC,
                    cur, note);
}

// Players (co-op, two Switch controllers fed by separate devices) and the console mode (docked / handheld, live).
static void players_rows() {
    opt_group(UI_MENU_PLAYERS);
    static const UiTextId mode[] = {UI_MENU_CONSOLE_DOCKED, UI_MENU_CONSOLE_HANDHELD};
    opt_choice("TOTK_OPERATION_MODE", UI_MENU_CONSOLE_MODE, mode, 2, UI_MENU_CONSOLE_MODE_DESC);
    static const UiTextId types[] = {UI_MENU_TYPE_PRO, UI_MENU_TYPE_DUAL};
    for (int p = 0; p < INPUT_PLAYERS; p++) {
        row_player_device(p);
        opt_choice(p ? "TOTK_PLAYER2_TYPE" : "TOTK_PLAYER1_TYPE", p ? UI_MENU_PLAYER2_TYPE : UI_MENU_PLAYER1_TYPE, types, 2,
                   UI_MENU_PLAYER_TYPE_DESC);
    }
    opt_group(UI_MENU_TAB_CONTROLS);
}

// The connected devices (input.c input_pads): the keyboard and mouse, then every gamepad with what SDL reports of it
// (nothing is assumed from its name). A or a click on a gamepad makes it rumble, to tell two apart.
static InputPadInfo g_pads[8];
static int g_npads;

static const char *pad_family_name(const InputPadInfo &p) {
    switch (p.family) {
    case PAD_FAMILY_XBOX: return "Xbox";
    case PAD_FAMILY_PLAYSTATION: return "PlayStation";
    case PAD_FAMILY_NINTENDO: return p.switch2 ? "Nintendo Switch 2" : "Nintendo Switch";
    }
    return menu_text(UI_MENU_DEVICE_GENERIC);
}

static const char *player_name(int player) {
    return menu_text(player < 0 ? UI_MENU_DEVICE_UNUSED : player ? UI_BINDS_PLAYER_2 : UI_BINDS_PLAYER_1);
}

static void devices_rows(void) {
    opt_group(UI_MENU_DEVICES);
    g_npads = input_pads(g_pads, 8);
    opt_info("##dev_keyboard", menu_text(UI_MENU_DEVICE_KEYBOARD), player_name(input_keyboard_player()),
             menu_text(UI_MENU_DEVICE_KEYBOARD_DESC));
    for (int i = 0; i < g_npads; i++) {
        const InputPadInfo &p = g_pads[i];
        char value[128], has[192], desc[UI_DESC_BYTES], id[24];
        int n = snprintf(value, sizeof value, "%s", player_name(p.player));
        if (p.wired >= 0 && n < (int)sizeof value)
            n += snprintf(value + n, sizeof value - n, " · %s", menu_text(p.wired ? UI_MENU_DEVICE_WIRED : UI_MENU_DEVICE_WIRELESS));
        if (p.battery >= 0 && n < (int)sizeof value) snprintf(value + n, sizeof value - n, " · %d %%", p.battery);
        const struct { int on; UiTextId name; } caps[] = {
            {p.rumble, UI_MENU_CAP_RUMBLE}, {p.trigger_rumble, UI_MENU_CAP_TRIGGER_RUMBLE}, {p.motion, UI_MENU_CAP_MOTION},
            {p.back_buttons, UI_MENU_CAP_BACK_BUTTONS}, {p.light, UI_MENU_CAP_LIGHT},
        };
        int h = 0;
        has[0] = 0;
        for (const auto &c : caps)
            if (c.on && h < (int)sizeof has) h += snprintf(has + h, sizeof has - h, "%s%s", h ? ", " : "", menu_text(c.name));
        if (!h) snprintf(has, sizeof has, "%s", menu_text(UI_MENU_CAP_NONE));
        int d = snprintf(desc, sizeof desc, menu_text(UI_MENU_DEVICE_DESC), pad_family_name(p), has);
        if (p.rumble && d > 0 && d < (int)sizeof desc) snprintf(desc + d, sizeof desc - d, " %s", menu_text(UI_MENU_DEVICE_IDENTIFY));
        snprintf(id, sizeof id, "##dev_pad%d", i);
        if (opt_info(id, p.name, value, desc)) input_pad_identify(i);
    }
    if (!g_npads) opt_text(UI_MENU_DEVICES_NONE);
}

static void tab_controls(float scale) {
    opt_begin("##controls", scale);
    devices_rows();
    players_rows();
    // Only the rows that do something with what is connected: a row for a feature no connected gamepad has would be
    // an option that changes nothing.
    // Each row's description ends with the connected devices it is for.
    enum { FOR_ALL, FOR_RUMBLE, FOR_TRIGGERS, FOR_MOTION, FOR_COUNT };
    static char used[FOR_COUNT][256];
    bool any[FOR_COUNT] = {};
    for (int k = 0; k < FOR_COUNT; k++) {
        char names[200];
        int n = 0;
        names[0] = 0;
        for (int i = 0; i < g_npads; i++) {
            const InputPadInfo &p = g_pads[i];
            bool has = k == FOR_ALL || (k == FOR_RUMBLE && p.rumble) || (k == FOR_TRIGGERS && p.trigger_rumble) ||
                       (k == FOR_MOTION && p.motion);
            if (!has) continue;
            any[k] = true;
            if (n < (int)sizeof names) n += snprintf(names + n, sizeof names - n, "%s%s", n ? ", " : "", p.name);
        }
        snprintf(used[k], sizeof used[k], menu_text(UI_MENU_USED_BY), names);
    }
    // No Button layout row (gone in 0.2.0): every button is where a Switch controller has it (input_pad_layout_pick).
    {   // Whose buttons the game's own pictures show (hud_prompts.c). Also with the keyboard alone: its automatic
        // choice shows the keys then, and a player may still want a maker's buttons or the game's own.
        static const UiTextId prompts[] = {UI_MENU_PAD_LAYOUT_AUTO, UI_MENU_BUTTON_PROMPTS_XBOX,
                                           UI_MENU_BUTTON_PROMPTS_PLAYSTATION, UI_MENU_BUTTON_PROMPTS_SWITCH,
                                           UI_MENU_BUTTON_PROMPTS_GAME};
        opt_choice("TOTK_BUTTON_PROMPTS", UI_MENU_BUTTON_PROMPTS, prompts, 5, UI_MENU_BUTTON_PROMPTS_DESC);
    }
    if (any[FOR_ALL]) {   // The button the game's menus confirm with (input_confirm_bottom): gamepads only.
        static const UiTextId confirm[] = {UI_MENU_PAD_LAYOUT_AUTO, UI_PAD_SOUTH, UI_PAD_EAST};   // cfg.c's values
        opt_describe_next(used[FOR_ALL]);
        opt_choice("TOTK_CONFIRM_BUTTON", UI_MENU_CONFIRM_BUTTON, confirm, 3, UI_MENU_CONFIRM_BUTTON_DESC);
    }
    // Motion stays in sight, greyed with the reason, while no connected gamepad has a gyro and an accelerometer SDL can
    // read (an Xbox controller, and a controller that works as one, sends none): players look for this row.
    if (any[FOR_MOTION]) opt_describe_next(used[FOR_MOTION]);
    else opt_disable_next(UI_MENU_MOTION_NONE);
    opt_toggle("TOTK_MOTION", UI_MENU_MOTION, UI_MENU_MOTION_DESC);
    if (any[FOR_RUMBLE]) {
        // Each change rumbles the gamepads at once with the new values, so it is felt (input_rumble_preview).
        opt_describe_next(used[FOR_RUMBLE]);
        if (opt_slider("TOTK_RUMBLE_STRENGTH", UI_MENU_RUMBLE_STRENGTH, 0, 300, UI_MENU_RUMBLE_STRENGTH_DESC, 10, "%.0f %%"))
            input_rumble_preview();
        opt_describe_next(used[FOR_RUMBLE]);
        if (opt_slider("TOTK_RUMBLE_CURVE", UI_MENU_RUMBLE_CURVE, 0.2f, 1.5f, UI_MENU_RUMBLE_CURVE_DESC, 0.05f, "%.2f"))
            input_rumble_preview();
    }
    if (any[FOR_TRIGGERS]) {
        opt_describe_next(used[FOR_TRIGGERS]);
        if (opt_toggle("TOTK_RUMBLE_TRIGGERS", UI_MENU_RUMBLE_TRIGGERS, UI_MENU_RUMBLE_TRIGGERS_DESC))
            input_rumble_preview();
    }
    if (any[FOR_ALL]) {
        opt_describe_next(used[FOR_ALL]);
        opt_slider("TOTK_STICK_DEADZONE", UI_MENU_STICK_DEADZONE, 0, 50, UI_MENU_STICK_DEADZONE_DESC, 1, "%.0f %%");
    }
    static char mouse[128];
    snprintf(mouse, sizeof mouse, menu_text(UI_MENU_USED_BY), menu_text(UI_MENU_DEVICE_KEYBOARD));
    opt_describe_next(mouse);
    opt_slider("TOTK_MOUSE_SENS", UI_MENU_MOUSE_SENS, 0, 10, UI_MENU_MOUSE_SENS_DESC, 0.1f, "%.1f");
    binds_rows(scale);
    opt_end();
    binds_after_page();
}

static void tab_menu(float scale) {
    opt_begin("##menu", scale);
    static const UiTextId langs[] = {   // the order of MENU_LANG's values in cfg.c
        UI_MENU_MENU_LANG_AUTO, UI_MENU_LANGNAME_EN, UI_MENU_LANGNAME_RU, UI_MENU_LANGNAME_IT, UI_MENU_LANGNAME_ZH_HANS,
    };
    opt_choice("TOTK_MENU_LANG", UI_MENU_MENU_LANG, langs, sizeof langs / sizeof *langs, UI_MENU_MENU_LANG_DESC);
    opt_end();
}

// ---- Advanced: the testing and troubleshooting keys, the bug report helpers and the reset of every setting
static bool g_modal;   // the reset confirmation is open: B and Esc close it, not the menu; L / R stay on the page

// A thread count with no fixed default: Automatic (unset), then the common counts.
static void row_threads(const char *key, UiTextId label, UiTextId desc) {
    static const char *const vals[] = {"", "1", "2", "4", "6", "8", "12", "16", "24", "32", "48", "64"};
    enum { N = sizeof vals / sizeof *vals };
    const char *names[N];
    names[0] = menu_text(UI_MENU_AUTO);
    for (int i = 1; i < N; i++) names[i] = vals[i];
    opt_choice_list(key, label, vals, names, N, desc);
}

// A free-text key shown as Off (unset) / On (value set): On writes on_value, any set value reads as On.
static void row_set_or_not(const char *key, UiTextId label, UiTextId desc, const char *on_value) {
    const char *vals[] = {"", on_value}, *names[] = {menu_text(UI_MENU_OFF), menu_text(UI_MENU_ON)};
    opt_choice_list(key, label, vals, names, 2, desc, *cfg_value(key) ? 1 : 0);
}

static bool validation_layer_present(void) {
    static int have = -1;
    if (have < 0) {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        VkLayerProperties *l = (VkLayerProperties *)calloc(n ? n : 1, sizeof *l);
        if (l && vkEnumerateInstanceLayerProperties(&n, l) >= 0)
            for (uint32_t i = 0; i < n && have <= 0; i++) have = !strcmp(l[i].layerName, "VK_LAYER_KHRONOS_validation");
        free(l);
        if (have < 0) have = 0;
    }
    return have != 0;
}

// Every setting the menu shows back to its default (the environment's keys stay), written to totk.ini.
static void reset_all(void) {
    const char *keys[128];
    int n = 0;
    opt_restart_needed();   // the values at start are taken before the reset changes any
    for (const CfgMeta *m; n < 128 && (m = cfg_meta_at(n)); n++) keys[n] = m->key;
    int done = cfg_reset(keys, n);
    opt_save_soon();
    rt_log("menu: reset %d settings to their defaults", done);
}

// A button's result shown in its value for a few seconds: 1 done (done_text), -1 failed.
struct ActionNote { uint64_t until; int result; };

static const char *action_value(const ActionNote &a, UiTextId done_text) {
    if (!a.result || plat_now_ns() > a.until) return "";
    return menu_text(a.result > 0 ? done_text : UI_MENU_ACTION_FAILED);
}

static void action_done(ActionNote &a, bool ok) { a = {plat_now_ns() + 3000000000ull, ok ? 1 : -1}; }

static void tab_advanced(float scale) {
    static ActionNote copy_note, log_note;
    opt_begin("##advanced", scale);
    opt_text(UI_MENU_ADV_WARNING, true);

    opt_group(UI_MENU_GROUP_SCHED);
    static const UiTextId preempt[] = {UI_MENU_PREEMPT_OFF, UI_MENU_PREEMPT_ON, UI_MENU_PREEMPT_INHERIT};   // 0|1|2
    opt_choice("TOTK_ASYNC_PREEMPT", UI_MENU_ASYNC_PREEMPT, preempt, 3, UI_MENU_ASYNC_PREEMPT_DESC);
    opt_toggle("TOTK_REPLAY_ASYNC", UI_MENU_REPLAY_ASYNC, UI_MENU_REPLAY_ASYNC_DESC);
    opt_toggle("TOTK_TIMER_1MS", UI_MENU_TIMER_1MS, UI_MENU_TIMER_1MS_DESC);

    opt_group(UI_MENU_GROUP_SHADERS);
    static const UiTextId wait[] = {UI_MENU_PIPE_WAIT_DEFAULT, UI_MENU_PIPE_WAIT_DRAW};
    opt_choice("TOTK_PIPE_WAIT", UI_MENU_PIPE_WAIT, wait, 2, UI_MENU_PIPE_WAIT_DESC);
    static const UiTextId skip[] = {UI_MENU_PIPE_SKIP_STRICT, UI_MENU_PIPE_SKIP_ANY, UI_MENU_PIPE_SKIP_FULLSCREEN};
    opt_choice("TOTK_PIPE_SKIP", UI_MENU_PIPE_SKIP, skip, 3, UI_MENU_PIPE_SKIP_DESC);
    opt_toggle("TOTK_LIB_PREBUILD", UI_MENU_LIB_PREBUILD, UI_MENU_LIB_PREBUILD_DESC);
    row_threads("TOTK_LIB_PREBUILD_THREADS", UI_MENU_LIB_PREBUILD_THREADS, UI_MENU_LIB_PREBUILD_THREADS_DESC);
    opt_toggle("TOTK_PIPE_PREBUILD", UI_MENU_PIPE_PREBUILD, UI_MENU_PIPE_PREBUILD_DESC);
    row_threads("TOTK_PREWARM_XLATE", UI_MENU_PREWARM_XLATE, UI_MENU_PREWARM_XLATE_DESC);
    opt_toggle("TOTK_GPL", UI_MENU_GPL, UI_MENU_GPL_DESC);

    opt_group(UI_MENU_GROUP_TEXMEM);
    static const UiTextId astc[] = {UI_MENU_ASTC_AUTO, UI_MENU_ASTC_GPU, UI_MENU_ASTC_NATIVE, UI_MENU_ASTC_CPU};
    opt_choice("TOTK_ASTC", UI_MENU_ASTC, astc, 4, UI_MENU_ASTC_DESC);
    static const UiTextId mem[] = {UI_MENU_MEMORY_AUTO, UI_MENU_MEMORY_IMPORT, UI_MENU_MEMORY_MIRROR,
                                   UI_MENU_MEMORY_VERIFY};
    opt_choice("TOTK_MEMORY", UI_MENU_MEMORY, mem, 4, UI_MENU_MEMORY_DESC);
    opt_toggle("TOTK_TEX_ALIAS", UI_MENU_TEX_ALIAS, UI_MENU_TEX_ALIAS_DESC);
    opt_slider("TOTK_TEX_VRAM_PRESSURE", UI_MENU_TEX_VRAM_PRESSURE, 0, 100, UI_MENU_TEX_VRAM_PRESSURE_DESC, 1, "%.0f%%");
    opt_toggle("TOTK_ASTC_BC", UI_MENU_ASTC_BC, UI_MENU_ASTC_BC_DESC);

    opt_group(UI_MENU_GROUP_FIXES);
    opt_toggle("TOTK_LOD_GUARD", UI_MENU_LOD_GUARD, UI_MENU_LOD_GUARD_DESC);
    opt_toggle("TOTK_FIX_C0", UI_MENU_FIX_C0, UI_MENU_FIX_C0_DESC);
    opt_toggle("TOTK_FIX_ZERO", UI_MENU_FIX_ZERO, UI_MENU_FIX_ZERO_DESC);
    opt_toggle("TOTK_BIND_SKIP", UI_MENU_BIND_SKIP, UI_MENU_BIND_SKIP_DESC);
    opt_toggle("TOTK_SHADER_MEMO", UI_MENU_SHADER_MEMO, UI_MENU_SHADER_MEMO_DESC);
    opt_toggle("TOTK_PIPE_MEMO", UI_MENU_PIPE_MEMO, UI_MENU_PIPE_MEMO_DESC);
    opt_toggle("TOTK_TEX_MEMO", UI_MENU_TEX_MEMO, UI_MENU_TEX_MEMO_DESC);
    opt_toggle("TOTK_DESC_TEMPLATE", UI_MENU_DESC_TEMPLATE, UI_MENU_DESC_TEMPLATE_DESC);
    opt_toggle("TOTK_PUSH_SKIP", UI_MENU_PUSH_SKIP, UI_MENU_PUSH_SKIP_DESC);
    opt_slider("TOTK_SPLIT_DRAWS", UI_MENU_SPLIT_DRAWS, 0, 1000, UI_MENU_SPLIT_DRAWS_DESC, 8, "%.0f");

    opt_group(UI_MENU_GROUP_VULKAN);
    if (!validation_layer_present() && !cfg_bool("TOTK_VK_VALIDATION")) opt_disable_next(UI_MENU_VK_VALIDATION_MISSING);
    opt_toggle("TOTK_VK_VALIDATION", UI_MENU_VK_VALIDATION, UI_MENU_VK_VALIDATION_DESC);
    static const UiTextId dyn[] = {UI_MENU_AUTO, UI_MENU_OFF, UI_MENU_ON};   // unset, 0, 1
    opt_choice("TOTK_VK_DYN_VERTEX", UI_MENU_VK_DYN_VERTEX, dyn, 3, UI_MENU_VK_DYN_VERTEX_DESC);
    opt_choice("TOTK_VK_DYN_A2C", UI_MENU_VK_DYN_A2C, dyn, 3, UI_MENU_VK_DYN_A2C_DESC);
    opt_toggle("TOTK_VK_SUBGROUP_PIN", UI_MENU_VK_SUBGROUP_PIN, UI_MENU_VK_SUBGROUP_PIN_DESC);

    opt_group(UI_MENU_GROUP_DIAG);
    row_set_or_not("TOTK_TRACE", UI_MENU_TRACE, UI_MENU_TRACE_DESC, "perf");
    opt_toggle("TOTK_FLIGHT", UI_MENU_FLIGHT, UI_MENU_FLIGHT_DESC);
    opt_slider("TOTK_FLIGHT_SPIKE_MS", UI_MENU_FLIGHT_SPIKE_MS, 20, 1000, UI_MENU_FLIGHT_SPIKE_MS_DESC, 10, "%.0f");
    static char rec_path[600];   // the recording goes beside the log
    if (!rec_path[0]) {
        char dir[512];
        snprintf(rec_path, sizeof rec_path, "%s/input-record.txt", sysinfo_log_folder(dir, sizeof dir) ? dir : ".");
    }
    row_set_or_not("TOTK_INPUT_RECORD", UI_MENU_INPUT_RECORD, UI_MENU_INPUT_RECORD_DESC, rec_path);
    if (opt_button("##open_log", UI_MENU_OPEN_LOG, UI_MENU_OPEN_LOG_DESC, action_value(log_note, UI_MENU_OPEN_LOG))) {
        char dir[512];
        bool ok = sysinfo_log_folder(dir, sizeof dir) && plat_open_folder(dir);
        if (!ok) action_done(log_note, false);   // success shows as the opened folder
    }
    if (opt_button("##copy_info", UI_MENU_COPY_INFO, UI_MENU_COPY_INFO_DESC,
                   action_value(copy_note, UI_MENU_COPY_INFO_DONE))) {
        static char text[16384];
        action_done(copy_note, sysinfo_text(text, sizeof text) > 0 && plat_clipboard_set(text));
    }
    bool ask = opt_button("##reset", UI_MENU_RESET, UI_MENU_RESET_DESC);
    opt_end();
    if (ask) ImGui::OpenPopup("##reset_confirm");   // at the level BeginPopupModal looks it up

    // the confirmation, over the menu; Cancel has the cursor first
    g_modal = false;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("##reset_confirm", nullptr,
                               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize)) {
        g_modal = true;
        ImGui::TextUnformatted(menu_text(UI_MENU_RESET_CONFIRM));
        ImGui::Spacing();
        const char *yes = menu_text(UI_MENU_RESET_YES), *no = menu_text(UI_MENU_RESET_NO);
        const ImGuiStyle &st = ImGui::GetStyle();
        float bw = fmaxf(ImGui::CalcTextSize(yes).x, ImGui::CalcTextSize(no).x) + st.FramePadding.x * 4;
        bool close = false;
        if (theme::button(no, ImVec2(bw, 50 * scale), scale, false)) close = true;
        if (ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (theme::button(yes, ImVec2(bw, 50 * scale), scale, true)) {
            reset_all();
            close = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
            close = true;
        if (close) {
            ImGui::CloseCurrentPopup();
            opt_focus_key("##reset");   // back on the row it came from
        }
        ImGui::EndPopup();
    }
}

static const UiTextId kTabNames[TAB_COUNT] = {UI_MENU_TAB_SCREEN, UI_MENU_TAB_GAME, UI_MENU_TAB_SOUND,
                                              UI_MENU_TAB_CONTROLS, UI_MENU_TAB_MENU, UI_MENU_TAB_ADVANCED};
static const UiTextId kPageSummary[TAB_COUNT] = {UI_MENU_PAGE_SCREEN, UI_MENU_PAGE_GAME, UI_MENU_PAGE_SOUND,
                                                 UI_MENU_PAGE_CONTROLS, UI_MENU_PAGE_MENU, UI_MENU_PAGE_ADVANCED};
static int g_tab;               // the tab shown
static int g_tab_request = -1;  // select this tab at the next layout; -1 none
static int g_focus_page = -1;   // this tab's first row takes the cursor once it is shown; -1 none
static uint64_t g_open_anim_ns;

static float glyph_width(const char *label, float scale) {
    return fmaxf(28 * scale, ImGui::CalcTextSize(label).x + 14 * scale);
}

static float glyph(ImDrawList *dl, ImVec2 p, const char *label, float scale, bool circle, bool dpad = false, int face = 0) {
    float fs = ImGui::GetFontSize(), height = 30 * scale;
    float width = dpad || circle ? height : glyph_width(label, scale);
    ImVec2 end(p.x + width, p.y + height);
    float radius = circle || dpad ? height * .5f : 6 * scale;
    dl->AddRectFilled(p, end, theme::surface3, radius);
    dl->AddRect(p, end, theme::line_strong, radius, 0, 1.5f * scale);
    if (dpad) {
        float x = p.x + width * .5f, y = p.y + height * .5f, r = 7 * scale;
        dl->AddLine(ImVec2(x - r, y), ImVec2(x + r, y), theme::text, 2.5f * scale);
        dl->AddLine(ImVec2(x, y - r), ImVec2(x, y + r), theme::text, 2.5f * scale);
    } else if (face >= PAD_LABEL_CROSS && face <= PAD_LABEL_TRIANGLE) {
        ImU32 colour = theme::text;
        if (input_pad_type() == 4 || input_pad_type() == 5) {
            static const ImU32 classic[] = {IM_COL32(113,182,255,255), IM_COL32(255,119,114,255),
                                            IM_COL32(239,152,209,255), IM_COL32(121,223,184,255)};
            colour = classic[face - PAD_LABEL_CROSS];
        }
        float x = p.x + width * .5f, y = p.y + height * .5f, r = 7 * scale, stroke = 2 * scale;
        if (face == PAD_LABEL_CROSS) {
            dl->AddLine(ImVec2(x-r,y-r), ImVec2(x+r,y+r), colour, stroke);
            dl->AddLine(ImVec2(x+r,y-r), ImVec2(x-r,y+r), colour, stroke);
        } else if (face == PAD_LABEL_CIRCLE) dl->AddCircle(ImVec2(x,y), r, colour, 24, stroke);
        else if (face == PAD_LABEL_SQUARE) dl->AddRect(ImVec2(x-r,y-r), ImVec2(x+r,y+r), colour, 0, 0, stroke);
        else dl->AddTriangle(ImVec2(x,y-r), ImVec2(x+r,y+r), ImVec2(x-r,y+r), colour, stroke);
    } else {
        ImU32 colour = theme::text;
        if (circle && (input_pad_type() == 2 || input_pad_type() == 3)) {
            if (face == PAD_LABEL_A) colour = IM_COL32(119,217,101,255);
            if (face == PAD_LABEL_B) colour = IM_COL32(255,108,100,255);
            if (face == PAD_LABEL_X) colour = IM_COL32(113,182,255,255);
            if (face == PAD_LABEL_Y) colour = IM_COL32(246,214,107,255);
        }
        ImVec2 sz = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2(p.x + (width - sz.x) * .5f, p.y + (height - fs) * .5f), colour, label);
    }
    return width;
}

static void status_text(ImDrawList *dl, ImVec2 p, float width, int status, float scale) {
    if (!status || width <= 0) return;
    // A save failure stays until a save succeeds; the settings hold for the session.
    UiTextId id = status == 3 ? UI_MENU_SAVE_FAILED : status == 2 ? UI_MENU_STATUS_RESTART : UI_MENU_STATUS_SAVED;
    ImU32 c = status == 3 ? theme::err : status == 2 ? theme::warn : theme::ok;
    float fs = ImGui::GetFontSize();
    dl->AddCircleFilled(ImVec2(p.x + 4 * scale, p.y + fs * .5f), 4 * scale, c);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::colour(c));
    ImGui::RenderTextEllipsis(dl, ImVec2(p.x + 14 * scale, p.y), ImVec2(p.x + width, p.y + fs),
                              p.x + width, p.x + width, menu_text(id), nullptr, nullptr);
    ImGui::PopStyleColor();
}

static void fade_vertices(ImDrawList *dl, int start, float alpha) {
    for (int i = start; i < dl->VtxBuffer.Size; i++) {
        ImU32 c = dl->VtxBuffer[i].col;
        unsigned a = c >> IM_COL32_A_SHIFT;
        dl->VtxBuffer[i].col = (c & ~IM_COL32_A_MASK) | ((ImU32)(a * alpha) << IM_COL32_A_SHIFT);
    }
}

// ---- the window: rail, page column, detail strip and action bar. L / R or Q / E switch pages; B and Esc close it.
// The rail is mouse-only; pages and action buttons remain in one controller navigation scope.
static void layout_menu(uint32_t w, uint32_t h, float scale) {
    ImDrawList *bg = ImGui::GetBackgroundDrawList();
    int bg_start = bg->VtxBuffer.Size;
    bg->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2((float)w, (float)h),
                                theme::alpha(theme::bg, 150.0f / 255), theme::alpha(theme::bg, 150.0f / 255),
                                theme::alpha(theme::bg, 205.0f / 255), theme::alpha(theme::bg, 205.0f / 255));
    float ww = fminf(w * .92f, 1560 * scale), wh = fminf(h * .90f, 920 * scale);
    ImVec2 origin((w - ww) * .5f, (h - wh) * .5f), end(origin.x + ww, origin.y + wh);
    for (int i = 3; i >= 1; i--) {
        float d = i * 4 * scale;
        bg->AddRect(ImVec2(origin.x - d, origin.y - d), ImVec2(end.x + d, end.y + d),
                    IM_COL32(0, 0, 0, i == 1 ? 60 : i == 2 ? 35 : 15), 20 * scale, 0, 4 * scale);
    }
    ImGui::SetNextWindowPos(origin, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(ww, wh), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(32 * scale, 0));
    ImGui::Begin("##menu", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, end, theme::alpha(theme::surface1, 250.0f / 255), 20 * scale);
    dl->AddRect(origin, end, theme::line, 20 * scale);
    float rail = 300 * scale, bar = 64 * scale, bar_y = end.y - bar;
    dl->AddRectFilledMultiColor(ImVec2(origin.x + 1, origin.y + 20 * scale), ImVec2(origin.x + rail, bar_y),
                                theme::surface2, theme::surface2, IM_COL32(13, 15, 19, 255), IM_COL32(13, 15, 19, 255));
    dl->AddRectFilled(ImVec2(origin.x + 1, origin.y + 1), ImVec2(origin.x + rail, origin.y + 22 * scale),
                      theme::surface2, 20 * scale, ImDrawFlags_RoundCornersTopLeft);
    dl->AddLine(ImVec2(origin.x + rail, origin.y), ImVec2(origin.x + rail, bar_y), theme::line);
    dl->AddRectFilled(ImVec2(origin.x + 1, bar_y), ImVec2(end.x - 1, end.y - 1),
                      IM_COL32(13, 15, 19, 255), 20 * scale, ImDrawFlags_RoundCornersBottom);
    dl->AddLine(ImVec2(origin.x, bar_y), ImVec2(end.x, bar_y), theme::line);

    const char *launcher_pad = getenv("RECOMPLAUNCHER_PAD");
    // The hints follow the device used last: a controller's own buttons, or keys while the keyboard or the mouse is
    // in use (also with controllers connected).
    bool pad = !input_keyboard_active() &&
               (pad_have || (launcher_pad && (!strcmp(launcher_pad, "xbox") || !strcmp(launcher_pad, "playstation"))));
    ImGui::PushFont(theme::fonts().brand);
    dl->AddText(ImVec2(origin.x + 24 * scale, origin.y + 28 * scale), theme::text, "Age of Decomp");
    ImGui::PopFont();
    ImGui::PushFont(theme::fonts().small);
    float hint_y = origin.y + 72 * scale;
    const char *prev = pad ? binds_pad_hint(B_L, "L") : "Q", *next = pad ? binds_pad_hint(B_R, "R") : "E";
    glyph(dl, ImVec2(origin.x + 24 * scale, hint_y), prev, scale, false);
    float next_w = glyph_width(next, scale);
    glyph(dl, ImVec2(origin.x + rail - 24 * scale - next_w, hint_y), next, scale, false);
    const char *settings = menu_text(UI_MENU_TITLE);
    dl->AddText(ImVec2(origin.x + (rail - ImGui::CalcTextSize(settings).x) * .5f, hint_y + 4 * scale),
                theme::text_faint, settings);
    ImGui::PopFont();

    int step = g_modal || binds_modal() ? 0
             : ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false) ? 1
             : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false) ? -1 : 0;
    if (step) g_tab_request = (g_tab + step + TAB_COUNT) % TAB_COUNT;
    float item_h = 2.1f * theme::fonts().text->FontSize, item_x = origin.x + 18 * scale;
    float item_w = rail - 36 * scale;
    for (int k = 0; k < TAB_COUNT; k++) {
        ImVec2 p(item_x, origin.y + 123 * scale + k * item_h);
        ImGui::SetCursorScreenPos(p);
        ImGui::PushID(k);
        bool clicked = ImGui::InvisibleButton("##rail", ImVec2(item_w, item_h));
        bool hover = ImGui::IsItemHovered();
        if (clicked && !g_modal && !binds_modal()) g_tab_request = k;
        bool active = k == (g_tab_request >= 0 ? g_tab_request : g_tab);
        if (active || hover) dl->AddRectFilled(p, ImVec2(p.x + item_w, p.y + item_h),
                                               active ? theme::focus_soft : theme::surface3, 10 * scale);
        if (active) dl->AddRect(p, ImVec2(p.x + item_w, p.y + item_h), theme::alpha(theme::focus, 90.0f / 255),
                                10 * scale);
        const char *name = menu_text(kTabNames[k]);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::colour(active || hover ? theme::text : theme::text_dim));
        ImGui::RenderTextEllipsis(dl, ImVec2(p.x + 18 * scale, p.y + (item_h - ImGui::GetFontSize()) * .5f),
                                  ImVec2(p.x + item_w - 14 * scale, p.y + item_h),
                                  p.x + item_w - 14 * scale, p.x + item_w - 14 * scale, name, nullptr, nullptr);
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    if (g_tab_request >= 0) { g_tab = g_tab_request; g_focus_page = g_tab; g_tab_request = -1; }
    if (g_focus_page == g_tab) { opt_focus_first(); g_focus_page = -1; }
    char version[96];
    snprintf(version, sizeof version, menu_text(UI_MENU_VERSION), rt_port_version);
    ImGui::PushFont(theme::fonts().small);
    // Too wide for the rail: the version goes on a line of its own (word wrap would break it at its dots).
    char *split = strrchr(version, ' ');
    if (split && ImGui::CalcTextSize(version).x > rail - 48 * scale) *split = '\n';
    dl->AddText(theme::fonts().small, ImGui::GetFontSize(), ImVec2(origin.x + 24 * scale, bar_y - 52 * scale),
                theme::text_faint, version);
    ImGui::PopFont();

    float page_x = origin.x + rail + 32 * scale, page_w = end.x - page_x - 32 * scale;
    float y = origin.y + 28 * scale;
    ImGui::PushFont(theme::fonts().title);
    dl->AddText(ImVec2(page_x, y), theme::text, menu_text(kTabNames[g_tab]));
    y += ImGui::GetFontSize() + 8 * scale;
    ImGui::PopFont();
    ImGui::PushFont(theme::fonts().small);
    const char *summary = menu_text(kPageSummary[g_tab]);
    ImVec2 summary_size = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, page_w, summary);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(page_x, y), theme::text_dim, summary, nullptr, page_w);
    y += summary_size.y + 18 * scale;
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(page_x, y));
    float detail_h = 3 * theme::fonts().small->FontSize * 1.13f + 16 * scale;
    opt_reserve_bottom(bar + detail_h + 22 * scale);
    switch (g_tab) {
    case TAB_SCREEN: tab_screen(scale); break;
    case TAB_GAME: tab_game(scale); break;
    case TAB_SOUND: tab_sound(scale); break;
    case TAB_CONTROLS: tab_controls(scale); break;
    case TAB_MENU: tab_menu(scale); break;
    case TAB_ADVANCED: tab_advanced(scale); break;
    }

    const char *cont = menu_text(UI_MENU_CONTINUE), *quit = menu_text(UI_MENU_QUIT);
    float bh = 2.5f * ImGui::GetFontSize();
    float cont_w = ImGui::CalcTextSize(cont).x + 40 * scale, quit_w = ImGui::CalcTextSize(quit).x + 40 * scale;
    float cont_x = end.x - 24 * scale - cont_w, quit_x = cont_x - 12 * scale - quit_w;
    float button_y = bar_y + (bar - bh) * .5f;
    // Restart: while a setting read at start differs from its value at start, and the launcher that started the game
    // starts it again (AOD_RESTART_EXIT). The pending change is written first; a failed write keeps the game running.
    bool restart = opt_restart_needed() && gfx_restart_available();
    const char *again = menu_text(UI_MENU_RESTART_NOW);
    float again_w = restart ? ImGui::CalcTextSize(again).x + 40 * scale : 0;
    float left_x = restart ? quit_x - 12 * scale - again_w : quit_x;
    if (restart) {
        ImGui::SetCursorScreenPos(ImVec2(left_x, button_y));
        if (theme::button(again, ImVec2(again_w, bh), scale, false)) {
            opt_save_tick(1);
            if (cfg_save()) gfx_request_restart();
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(quit_x, button_y));
    if (theme::button(quit, ImVec2(quit_w, bh), scale, false)) {
        opt_save_tick(1);   // a change made a moment ago is kept
        gfx_window_request_close();
    }
    ImGui::SetCursorScreenPos(ImVec2(cont_x, button_y));
    if (theme::button(cont, ImVec2(cont_w, bh), scale, true)) set_open(0);

    ImGui::PushFont(theme::fonts().small);
    const char *labels[] = {menu_text(UI_MENU_HINT_MOVE), menu_text(UI_MENU_HINT_CHANGE),
                            menu_text(UI_MENU_HINT_CLOSE), menu_text(UI_MENU_HINT_PAGES)};
    int face_labels[2] = {};
    bool bottom = pad && input_confirm_bottom();   // the game's B confirms (pad_events)
    const char *keys[] = {pad ? "" : "↑↓",
                          pad ? binds_pad_hint(bottom ? B_B : B_A, bottom ? "B" : "A", &face_labels[0]) : menu_text(UI_KEY_ENTER),
                          pad ? binds_pad_hint(bottom ? B_A : B_B, bottom ? "A" : "B", &face_labels[1]) : "Esc", prev, next};
    float gap = 8 * scale, group_gap = 18 * scale, glyph_h = 30 * scale, total = 0;
    for (int i = 0; i < 4; i++) {
        bool image = pad && (i == 0 || ((i == 1 || i == 2) && face_labels[i-1]));
        total += (image ? glyph_h : glyph_width(keys[i], scale)) + gap + ImGui::CalcTextSize(labels[i]).x;
        if (i == 3) total += glyph_width(keys[4], scale) + gap;
        if (i < 3) total += group_gap;
    }
    float hx = left_x - 20 * scale - total, hy = bar_y + (bar - glyph_h) * .5f;
    float min_hx = origin.x + 20 * scale;
    if (hx >= min_hx) {
        for (int i = 0; i < 4; i++) {
            int face = pad && (i == 1 || i == 2) ? face_labels[i-1] : 0;
            hx += glyph(dl, ImVec2(hx, hy), keys[i], scale, face != 0, pad && i == 0, face) + gap;
            if (i == 3) hx += glyph(dl, ImVec2(hx, hy), keys[4], scale, false) + gap;
            dl->AddText(ImVec2(hx, hy + (glyph_h - ImGui::GetFontSize()) * .5f), theme::text_dim, labels[i]);
            hx += ImGui::CalcTextSize(labels[i]).x + group_gap;
        }
    }
    float status_width = fmaxf(0, left_x - total - 50 * scale - min_hx);
    status_text(dl, ImVec2(min_hx, bar_y + (bar - ImGui::GetFontSize()) * .5f), status_width, opt_status(), scale);
    ImGui::PopFont();
    bool was_modal = g_modal || binds_modal();
    if (g_tab != TAB_ADVANCED) g_modal = false;
    if (!was_modal && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)))
        set_open(0);
    ImGui::End();
    float fade = g_open_anim_ns ? fminf(1, (float)(plat_now_ns() - g_open_anim_ns) / 140000000.0f) : 1;
    if (fade < 1) {
        fade_vertices(bg, bg_start, fade);
        ImGuiWindow *root = ImGui::FindWindowByName("##menu");
        for (ImGuiWindow *window : GImGui->Windows) {
            ImGuiWindow *parent = window;
            while (parent && parent != root) parent = parent->ParentWindow;
            if (parent == root && window->Active) fade_vertices(window->DrawList, 0, fade);
        }
    }
}

static int g_save_pending;   // a changed setting still waits for its write to totk.ini (opt_save_tick)

// ---- the performance overlay (TOTK_PERF_OVERLAY), in the top right corner. "fps": the frame rate over the presents of
// the last half second. "full": a panel of perfmon.c's numbers, in the style of SavageWindRecomp's perf_overlay_draw
// (menu.mm): the machine's load and temperatures, the frame rate and frame times with a graph, the render thread's waits.
// Off, it costs a cfg_version load per present.
static unsigned g_perf_ver = ~0u;
static int g_perf_mode;                  // 0 off, 1 fps, 2 full
static std::atomic<int> g_perf_save;     // F10 changed the mode: totk.ini is written on the present thread
static uint64_t g_perf_last_ns, g_perf_win_ns, g_perf_max_ns;
static int g_perf_frames;
static char g_perf_text[96];
// The overlay's top edge at 1080p, times the menu scale: below the small label HWAoI keeps in the top right corner of
// every screen (its own "NORMAL", about 18..36 px down at 1080p, anchored to the window's right edge), which the fps
// box at 12 px used to cover.
static const float PERF_TOP = 46;

static int perf_mode_of(const char *s) { return !strcmp(s, "full") ? 2 : !strcmp(s, "fps") ? 1 : 0; }

extern "C" void menu_perf_cycle(void) {
    static const char *const modes[] = {"off", "fps", "full"};
    cfg_set("TOTK_PERF_OVERLAY", modes[(perf_mode_of(cfg_value("TOTK_PERF_OVERLAY")) + 1) % 3]);
    g_perf_save.store(1);
}

// The device the game renders on, for perfmon's GPU counters (by LUID), NVML (by name) and ADL (by PCI bus, else
// name); once, before full mode.
static void perf_gpu_once(const MenuTarget *t) {
    static bool done;
    if (done) return;
    done = true;
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(t->phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = (VkExtensionProperties *)calloc(ne ? ne : 1, sizeof *ep);
    bool has_bus = false;
    if (ep && vkEnumerateDeviceExtensionProperties(t->phys, NULL, &ne, ep) >= 0)
        for (uint32_t i = 0; i < ne && !has_bus; i++) has_bus = !strcmp(ep[i].extensionName, VK_EXT_PCI_BUS_INFO_EXTENSION_NAME);
    free(ep);
    VkPhysicalDevicePCIBusInfoPropertiesEXT bus = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT};
    VkPhysicalDeviceIDProperties id = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, has_bus ? &bus : nullptr};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
    vkGetPhysicalDeviceProperties2(t->phys, &p2);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(t->phys, &mp);
    uint64_t vram = 0;
    for (uint32_t h = 0; h < mp.memoryHeapCount; h++)
        if (mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) vram += mp.memoryHeaps[h].size;
    perfmon_set_gpu(p2.properties.deviceName, id.deviceLUIDValid ? id.deviceLUID : NULL, has_bus ? (int)bus.pciBus : -1,
                    (float)(vram >> 20));
}

static void perf_tick(uint64_t now, const MenuTarget *t) {
    if (cfg_version() != g_perf_ver) {
        g_perf_ver = cfg_version();
        int m = perf_mode_of(cfg_value("TOTK_PERF_OVERLAY"));
        if (m != g_perf_mode) {
            g_perf_last_ns = 0, g_perf_text[0] = 0;
            if (m == 2) perf_gpu_once(t);
            perfmon_mode(m);
        }
        g_perf_mode = m;
    }
    if (g_perf_mode != 1) return;
    uint64_t d = now - g_perf_last_ns;
    if (!g_perf_last_ns || d > 2000000000ull) {   // first frame, or a stall the window should not average in
        g_perf_win_ns = g_perf_max_ns = 0;
        g_perf_frames = 0;
    } else {
        g_perf_win_ns += d;
        g_perf_frames++;
        if (d > g_perf_max_ns) g_perf_max_ns = d;
        if (g_perf_win_ns >= 500000000ull) {
            // with frame generation: the frames shown, then the ones the game drew
            double fps = g_perf_frames * 1e9 / g_perf_win_ns, gen = vk_fg_rate();
            if (gen > 1.05) snprintf(g_perf_text, sizeof g_perf_text, menu_text(UI_PERF_LINE_FPS_GEN), fps * gen, fps);
            else snprintf(g_perf_text, sizeof g_perf_text, menu_text(UI_PERF_LINE_FPS), fps);
            g_perf_win_ns = g_perf_max_ns = 0;
            g_perf_frames = 0;
        }
    }
    g_perf_last_ns = now;
}

// The panel keeps its width while the numbers change: every column is as wide as the widest text it can show, measured
// on samples of 8s (Roboto's digits share one width), and the numbers are right-aligned in it.
struct PerfCol { UiTextId fmt; float sample; };

static float perf_text_w(UiTextId fmt, float v) {
    char b[64];
    snprintf(b, sizeof b, menu_text(fmt), v);
    return ImGui::CalcTextSize(b).x;
}

// A fixed column as wide as the widest of the formats (with their samples) and labels given.
static void perf_column(const char *id, const PerfCol *cols, int n, const UiTextId *labels = nullptr, int nl = 0) {
    float w = ImGui::CalcTextSize(menu_text(UI_MENU_OV_NONE)).x;
    for (int i = 0; i < n; i++) w = fmaxf(w, perf_text_w(cols[i].fmt, cols[i].sample));
    for (int i = 0; i < nl; i++) w = fmaxf(w, ImGui::CalcTextSize(menu_text(labels[i])).x);
    ImGui::TableSetupColumn(id, ImGuiTableColumnFlags_WidthFixed, w);
}

static void perf_right(const char *s, bool dim) {
    float pad = ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(s).x;
    if (pad > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad);
    if (dim) ImGui::TextDisabled("%s", s);
    else ImGui::TextUnformatted(s);
}

// One cell of the full panel's table: fmt (a catalog format, one number) with v, or "-" when the machine gives none.
static void perf_cell(UiTextId fmt, float v) {
    ImGui::TableNextColumn();
    if (v < 0) return perf_right(menu_text(UI_MENU_OV_NONE), true);
    char b[64];
    snprintf(b, sizeof b, menu_text(fmt), v);
    perf_right(b, false);
}

static void perf_label(UiTextId label, const ImVec4 &c) {
    ImGui::TableNextColumn();
    ImGui::TextColored(c, "%s", menu_text(label));
}

static float perf_gb(float mb) { return mb < 0 ? -1 : mb / 1024; }

// The picture lines: the window, the game image the present shows and the 3D scene it holds (resolution.c), how the
// present scales the image to the window (gfx_present.c, the last present), the frame limit and the presentation mode
// in use.
static void perf_picture(const MenuTarget *t, const ImVec4 &c) {
    char v[7][64];
    snprintf(v[0], sizeof v[0], menu_text(UI_MENU_OV_FMT_SIZE), t->width, t->height);
    snprintf(v[1], sizeof v[1], menu_text(UI_MENU_OV_FMT_SIZE), (unsigned)t->image_w, (unsigned)t->image_h);
    unsigned out[2], scene[2];
    res_sizes(out, scene);
    if (scene[0] && scene[1] && out[0])
        snprintf(v[2], sizeof v[2], menu_text(UI_MENU_OV_FMT_SCENE), scene[0], scene[1], scene[0] * 100.0 / out[0]);
    else snprintf(v[2], sizeof v[2], "%s", menu_text(UI_MENU_OV_UNMEASURED));
    if (t->scaler == 2) {
        int s = cfg_int("TOTK_SHARPNESS");
        snprintf(v[3], sizeof v[3], menu_text(UI_MENU_OV_FMT_FSR), s < 0 ? 0 : s > 100 ? 100 : s);
    } else {
        snprintf(v[3], sizeof v[3], "%s", menu_text(t->scaler ? UI_MENU_OV_SCALE_BILINEAR : UI_MENU_OV_SCALE_NONE));
    }
    double hz = frame_step_limit_hz();
    snprintf(v[4], sizeof v[4], "%s", menu_text(UI_MENU_OV_NONE));
    if (hz > 0) snprintf(v[4], sizeof v[4], menu_text(UI_MENU_OV_FPS), hz);
    const char *pm = present_name(gfx_window_present_mode());
    snprintf(v[5], sizeof v[5], "%s", pm ? pm : menu_text(UI_MENU_OV_NONE));
    // frame generation, while it is loaded: the multiplier, and the frames the screen gets per second right now
    int rows = 6, fg = vk_fg_factor();
    if (fg) {
        PerfStats ps;
        perfmon_snapshot(&ps);
        snprintf(v[rows++], sizeof v[0], menu_text(UI_MENU_OV_FMT_FRAME_GEN), fg, ps.fps * vk_fg_rate());
    }
    static const UiTextId labels[7] = {UI_MENU_OV_WINDOW, UI_MENU_OV_OUTPUT, UI_MENU_OV_SCENE,   UI_MENU_OV_SCALING,
                                       UI_MENU_OV_LIMIT,  UI_MENU_OV_PRESENT, UI_MENU_OV_FRAME_GEN};
    if (!ImGui::BeginTable("##perfp", 2, ImGuiTableFlags_SizingFixedFit)) return;
    float vw = 0;
    for (int i = 0; i < rows; i++) vw = fmaxf(vw, ImGui::CalcTextSize(v[i]).x);
    ImGui::TableSetupColumn("##p0", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("##p1", ImGuiTableColumnFlags_WidthFixed, vw);
    for (int i = 0; i < rows; i++) {
        perf_label(labels[i], c);
        ImGui::TableNextColumn();
        perf_right(v[i], false);
    }
    ImGui::EndTable();
}

static void perf_full_draw(const MenuTarget *t, float scale) {
    float w = (float)t->width;
    PerfStats s;
    perfmon_snapshot(&s);
    ImGui::SetNextWindowPos(ImVec2(w - 12 * scale, PERF_TOP * scale), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowBgAlpha(204.0f / 255);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10 * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1 * scale);
    ImGui::PushStyleColor(ImGuiCol_Border, theme::colour(theme::line));
    ImGui::Begin("##perf", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    // under the menu while it is open (the settings stay readable), else over everything
    if (g_open.load(std::memory_order_relaxed)) ImGui::BringWindowToDisplayBack(ImGui::GetCurrentWindow());
    else ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImGui::SetWindowFontScale(0.72f);
    const ImVec4 gpu_c = theme::colour(theme::text_dim), cpu_c = gpu_c, ram_c = gpu_c,
                 fps_c = theme::colour(theme::text), wait_c = gpu_c, pic_c = gpu_c;
    if (ImGui::BeginTable("##perft", 4, ImGuiTableFlags_SizingFixedFit)) {
        static const UiTextId labels[] = {UI_MENU_OV_GPU, UI_MENU_OV_VRAM, UI_MENU_OV_CPU, UI_MENU_OV_RAM};
        static const PerfCol c0[] = {{UI_MENU_OV_FPS, 888}};
        static const PerfCol c1[] = {{UI_MENU_OV_FMT_PCT, 100}, {UI_MENU_OV_FMT_GB, 888.8f}, {UI_MENU_OV_FRAME_MS, 888.8f}};
        static const PerfCol c2[] = {{UI_MENU_OV_FMT_TEMP, 188}, {UI_MENU_OV_FMT_GB_OF, 888.8f}, {UI_MENU_OV_MAX_MS, 8888.8f}};
        static const PerfCol c3[] = {{UI_MENU_OV_FMT_POWER, 888}, {UI_MENU_OV_FMT_MHZ, 8888}, {UI_MENU_OV_GAME_PCT, 100},
                                     {UI_MENU_OV_GAME_GB, 888.8f}};
        perf_column("##c0", c0, 1, labels, 4);
        perf_column("##c1", c1, 3);
        perf_column("##c2", c2, 3);
        perf_column("##c3", c3, 4);
        perf_label(UI_MENU_OV_GPU, gpu_c);
        perf_cell(UI_MENU_OV_FMT_PCT, s.gpu_pct);
        perf_cell(UI_MENU_OV_FMT_TEMP, s.gpu_temp_c);
        perf_cell(UI_MENU_OV_FMT_POWER, s.gpu_power_w);
        perf_label(UI_MENU_OV_VRAM, gpu_c);
        perf_cell(UI_MENU_OV_FMT_GB, perf_gb(s.vram_used_mb));
        perf_cell(UI_MENU_OV_FMT_GB_OF, perf_gb(s.vram_total_mb));
        perf_cell(UI_MENU_OV_FMT_MHZ, s.gpu_clock_mhz);
        perf_label(UI_MENU_OV_CPU, cpu_c);
        perf_cell(UI_MENU_OV_FMT_PCT, s.cpu_total_pct);
        perf_cell(UI_MENU_OV_FMT_TEMP, s.cpu_temp_c);
        perf_cell(UI_MENU_OV_GAME_PCT, s.cpu_game_pct);
        perf_label(UI_MENU_OV_RAM, ram_c);
        perf_cell(UI_MENU_OV_FMT_GB, perf_gb(s.ram_used_mb));
        perf_cell(UI_MENU_OV_FMT_GB_OF, perf_gb(s.ram_total_mb));
        perf_cell(UI_MENU_OV_GAME_GB, perf_gb(s.ram_game_mb));
        ImGui::TableNextColumn();   // the frame row: no label, the rate in its place
        char b[64];
        snprintf(b, sizeof b, menu_text(UI_MENU_OV_FPS), s.fps);
        ImGui::TextColored(fps_c, "%s", b);
        perf_cell(UI_MENU_OV_FRAME_MS, s.frame_ms_avg);
        perf_cell(UI_MENU_OV_MAX_MS, s.frame_ms_max);
        ImGui::EndTable();
    }
    float top = s.frame_ms_max > 50 ? s.frame_ms_max : 50;
    // as wide as the panel the tables size; the plot does not count toward that width, so the panel can still shrink
    ImGuiWindow *win = ImGui::GetCurrentWindow();
    float max_x = win->DC.CursorMaxPos.x;
    ImGui::PlotLines("##ft", s.frame_ms, PERF_HISTORY, 0, nullptr, 0, top, ImVec2(-FLT_MIN, 48 * scale));
    win->DC.CursorMaxPos.x = max_x;
    // per frame, averaged over the last half second; the labels' column stretches, so the numbers end at the panel's
    // right edge, and the table still asks for no more than its widest label and number
    if (ImGui::BeginTable("##perfw", 2, ImGuiTableFlags_SizingFixedFit)) {
        static const PerfCol c1[] = {{UI_MENU_OV_FMT_MS, 888.88f}, {UI_MENU_OV_PER_FRAME, 888.8f}};
        ImGui::TableSetupColumn("##w0", ImGuiTableColumnFlags_WidthStretch);
        perf_column("##w1", c1, 2);
        perf_label(UI_MENU_OV_REPLAY, wait_c);
        perf_cell(UI_MENU_OV_FMT_MS, s.replay_ms);
        perf_label(UI_MENU_OV_GPU_TIME, wait_c);
        perf_cell(UI_MENU_OV_FMT_MS, s.gpu_ms);
        perf_label(UI_MENU_OV_PIPE_WAIT, wait_c);
        perf_cell(UI_MENU_OV_FMT_MS, s.pipe_wait_ms);
        perf_label(UI_MENU_OV_SHADER_WAIT, wait_c);
        perf_cell(UI_MENU_OV_FMT_MS, s.xlate_ms);
        perf_label(UI_MENU_OV_TEX_UPLOADS, wait_c);
        perf_cell(UI_MENU_OV_PER_FRAME, s.tex_uploads);
        ImGui::EndTable();
    }
    perf_picture(t, pic_c);
    if (s.gpu_name[0]) ImGui::TextDisabled("%s", s.gpu_name);
    if (s.cpu_name[0]) ImGui::TextDisabled("%s", s.cpu_name);
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

static void perf_draw(const MenuTarget *t, float scale) {
    if (g_perf_mode == 2) return perf_full_draw(t, scale);
    float w = (float)t->width;
    if (!g_perf_mode || !g_perf_text[0]) return;
    // with the menu open: under its window, over the dimming layout_menu drew before
    ImDrawList *dl = g_open.load(std::memory_order_relaxed) ? ImGui::GetBackgroundDrawList() : ImGui::GetForegroundDrawList();
    ImVec2 sz = ImGui::CalcTextSize(g_perf_text);
    float pad = 6 * scale;
    ImVec2 p(w - 12 * scale - sz.x - pad * 2, PERF_TOP * scale);   // the top right corner, under the game's label
    dl->AddRectFilled(p, ImVec2(p.x + sz.x + pad * 2, p.y + sz.y + pad * 2),
                      theme::alpha(theme::surface1, 200.0f / 255), 6 * scale);
    dl->AddText(ImVec2(p.x + pad, p.y + pad), theme::text, g_perf_text);
}

extern "C" int menu_overlay_frame(uint64_t frame, const MenuTarget *t, const char **shot) {
    *shot = NULL;
    if (!g_test.parsed) {
        opt_restart_needed();   // the first frame: the settings read at start are still as the game started
        test_parse();
    }
    if (g_test.next_set < g_test.n_sets && frame >= g_test.sets[g_test.next_set].frame) test_apply_sets(frame);
    if (g_perf_save.exchange(0)) cfg_save();
    perf_tick(plat_now_ns(), t);
    bool open = g_open.load(std::memory_order_relaxed);
    if (!open) {
        if (g_save_pending) g_save_pending = opt_save_tick(1);   // closed: the last change goes to totk.ini now
        if (g_test.at && !g_test.opened && frame >= g_test.at) {
            g_test.opened = frame;
            g_test.page = 0;
            g_tab_request = 0;
            set_open(1);
            open = true;
        } else if (!g_perf_mode) {
            return 0;
        }
    }
    if (g_failed_flag()) {
        set_open(0);
        return 0;
    }
    uint64_t t0 = plat_now_ns();
    if (!g_ctx_ok) make_context();
    float scale = menu_scale(t->height);
    if (!vk_up(t) || (g_font_scale != scale && !fonts_up(scale))) {
        rt_log("menu: Dear ImGui Vulkan back end set-up failed; the menu stays closed");
        g_failed.store(1);
        set_open(0);
        return 0;
    }
    if (g_style_scale != scale) {
        ImGui::GetStyle() = g_base_style;
        ImGui::GetStyle().ScaleAllSizes(scale);
        g_style_scale = scale;
    }
    list_gpus(t->instance);
    ImGuiIO &io = ImGui::GetIO();
    int reset = g_reset.exchange(0);
    if (reset) {
        io.ClearInputKeys();
        io.ClearInputMouse();
        g_last_ns = 0;
        g_open_anim_ns = open ? t0 : 0;
        g_focus_page = g_tab_request >= 0 ? g_tab_request : g_tab;   // opening: the first row has the cursor
        if (GImGui->OpenPopupStack.Size) ImGui::ClosePopupToLevel(0, false);   // a confirmation left open on closing
        g_modal = false;
    }
    io.DisplaySize = ImVec2((float)t->width, (float)t->height);
    double dt = g_last_ns ? (t0 - g_last_ns) / 1e9 : 1 / 60.0;
    io.DeltaTime = (float)(dt < 1e-4 ? 1e-4 : dt > 0.1 ? 0.1 : dt);
    g_last_ns = t0;
    if (!open) {   // the overlay alone: no input reaches ImGui
    } else if (g_test.page < 0) {
        drain_events(io);
        pad_events(io, reset);
    } else {   // the test hook's pages: the user's input in a background test window must not change settings
        plat_mutex_lock(&ev_lock);
        ev_n = 0;
        plat_mutex_unlock(&ev_lock);
        pad_events(io, 1);
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (open) layout_menu(t->width, t->height, scale);
    perf_draw(t, scale);
    ImGui::Render();
    if (open) g_save_pending = opt_save_tick(0);
    g_cost_ns += plat_now_ns() - t0;
    if (g_test.page >= 0 && frame >= g_test.opened + g_test.per_page * (uint64_t)(g_test.page + 1)) {
        static char path[512];   // this frame's image goes to a file; then the next tab, or the menu closes
        const char *dir = cfg_get("TOTK_MENU_TEST_DIR");
        snprintf(path, sizeof path, "%s/menu-test-%llu-%d.png", dir ? dir : ".", (unsigned long long)frame, g_test.page);
        *shot = path;
        if (++g_test.page == TAB_COUNT) {
            g_test.page = -1;
            set_open(0);   // the next frame lays out nothing
        } else {
            g_tab_request = g_test.page;
        }
    }
    return ImGui::GetDrawData()->TotalVtxCount > 0;
}

extern "C" void menu_overlay_draw(VkCommandBuffer cb) {
    uint64_t t0 = plat_now_ns();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cb);
    g_cost_ns += plat_now_ns() - t0;
    if (++g_cost_frames == 600) {
        rt_log("menu: %.3f ms CPU per open frame (layout and recording, mean of 600)", g_cost_ns / 600 / 1e6);
        g_cost_ns = g_cost_frames = 0;
    }
}
