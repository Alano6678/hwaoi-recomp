// Game and platform seams the standalone menu preview cannot use without starting the game or a window.
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "cfg.h"
#include "input.h"
#include "input_map.h"
#include "perfmon.h"
#include "sysinfo.h"
#include "ui/menu.h"
}

unsigned preview_dpi = 96;
bool preview_quit = false;
bool preview_restart = false;
uint64_t preview_buttons = 0;
int32_t preview_lx = 0, preview_ly = 0;
int preview_capture_key = 0;
int preview_capture_pad = 0;

extern "C" {
extern const int rt_game_version = 100;
extern const char rt_port_version[] = "preview";

void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
[[noreturn]] void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}
void vk_queue_lock(void) {}
void vk_queue_unlock(void) {}
void res_sizes(unsigned out[2], unsigned scene[2]) {
    out[0] = scene[0] = 1920;
    out[1] = scene[1] = 1080;
}
void res_auto_size(unsigned out[2]) { out[0] = 2560, out[1] = 1440; }   // as on the preview's 2560x1440 monitor
double frame_step_limit_hz(void) { return 60; }
void gfx_window_apply_settings(void) {}
void gfx_window_request_close(void) { preview_quit = true; }
// The Restart button shows only when the launcher restarts the game (AOD_RESTART_EXIT), as gfx_present.c.
int gfx_restart_available(void) { const char *v = getenv("AOD_RESTART_EXIT"); return v && atoi(v) > 0 && atoi(v) < 255; }
void gfx_request_restart(void) { if (gfx_restart_available()) preview_restart = preview_quit = true; }
// No generated patch sets in the preview: rows of game-version patches (field of view) are hidden.
int rt_patch_set(const char *) { return -1; }
// The Upscaling row as on an NVIDIA GPU with DLSS's files; MENU_PREVIEW_NO_DLSS=1 hides DLSS, as on any other GPU.
int vk_dlss_offered(void) { const char *v = getenv("MENU_PREVIEW_NO_DLSS"); return !(v && *v == '1'); }
// MENU_PREVIEW_DLSS_OFF=1: as after a start where DLSS was asked for and could not run (the "in use: Off" note).
int vk_dlss_failed(void) { const char *v = getenv("MENU_PREVIEW_DLSS_OFF"); return v && *v == '1'; }
// The Frame generation row is hidden, as on a PC without an RTX 40 or 50 card or without NVIDIA's file, unless
// MENU_PREVIEW_FG=<n> makes the preview a run started with FRAME_GEN=<n> on a card that reaches 4x.
static int preview_fg(void) { const char *v = getenv("MENU_PREVIEW_FG"); return v ? atoi(v) : 0; }
int vk_fg_offered(void) { return preview_fg() != 0; }
// A screen that shows HDR; MENU_PREVIEW_NO_HDR=1 makes it one without (HDR off in the system): the HDR rows greyed.
int vk_hdr_offered(void) { const char *v = getenv("MENU_PREVIEW_NO_HDR"); return v && *v == '1' ? 0 : 1; }
int vk_fg_wanted(void) { return preview_fg(); }
int vk_fg_failed(void) { return 0; }
int vk_fg_max(void) { return preview_fg() ? 4 : 0; }
int vk_fg_factor(void) { int n = preview_fg(); return n > 4 ? 4 : n; }
double vk_fg_rate(void) { return vk_fg_factor() ? vk_fg_factor() : 1.0; }
// AMD's FSR library: present, so the row shows no download note; nothing is fetched in the preview.
int vk_ffx_lib_present(void) { return 1; }
void vk_ffx_fetch_async(void) {}
int vk_ffx_fetch_state(void) { return 0; }
int gfx_window_monitor_size(int *w, int *h) { *w = 2560; *h = 1440; return 1; }
double gfx_window_refresh_hz(void) { return 60; }
unsigned gfx_window_dpi(void) { return preview_dpi; }
const char *gfx_window_present_mode(void) { return "mailbox"; }
void input_menu(int) {}
int input_pad_read(uint64_t *b, int32_t *x, int32_t *y) {
    *b = preview_buttons; *x = preview_lx; *y = preview_ly; return 1;
}
int input_pad_layout(void) { return -1; }
int input_pad_type(void) { const char *type = getenv("AOD_PREVIEW_PAD_TYPE"); return type ? atoi(type) : 3; }
int input_pad_label(int p) {
    if (input_pad_type() >= 4 && input_pad_type() <= 6) {
        if (p == PAD_SOUTH) return PAD_LABEL_CROSS;
        if (p == PAD_EAST) return PAD_LABEL_CIRCLE;
        if (p == PAD_WEST) return PAD_LABEL_SQUARE;
        if (p == PAD_NORTH) return PAD_LABEL_TRIANGLE;
    }
    if (p == PAD_SOUTH) return PAD_LABEL_A;
    if (p == PAD_EAST) return PAD_LABEL_B;
    if (p == PAD_WEST) return PAD_LABEL_X;
    if (p == PAD_NORTH) return PAD_LABEL_Y;
    return PAD_LABEL_NONE;
}
int input_device_choices(InputDeviceChoice *, int) { return 0; }
// The preview's connected gamepads: AOD_PREVIEW_PADS lists them by kind, "xbox,pro2,ps5" (unset: one Xbox pad; empty:
// none). The first is player 1's, the second player 2's.
int input_pads(InputPadInfo *out, int max) {
    const char *list = getenv("AOD_PREVIEW_PADS");
    if (!list) list = "xbox";
    int n = 0;
    for (const char *s = list; *s && n < max;) {
        const char *e = strchr(s, ',');
        size_t len = e ? (size_t)(e - s) : strlen(s);
        InputPadInfo p = {};
        if (len == 4 && !strncmp(s, "xbox", 4)) {
            snprintf(p.name, sizeof p.name, "Xbox Wireless Controller");
            p.type = 3, p.family = PAD_FAMILY_XBOX, p.wired = 1, p.battery = -1, p.rumble = p.trigger_rumble = 1;
            p.layout = PAD_LAYOUT_NINTENDO;   // the default on every pad (input_pad_layout_pick)
            const int labels[4] = {PAD_LABEL_A, PAD_LABEL_B, PAD_LABEL_X, PAD_LABEL_Y};
            memcpy(p.labels, labels, sizeof labels);
        } else if (len == 3 && !strncmp(s, "ps5", 3)) {
            snprintf(p.name, sizeof p.name, "DualSense Wireless Controller");
            p.type = 6, p.family = PAD_FAMILY_PLAYSTATION, p.wired = 0, p.battery = 70, p.rumble = p.motion = p.light = 1;
            p.layout = PAD_LAYOUT_NINTENDO;
            const int labels[4] = {PAD_LABEL_CROSS, PAD_LABEL_CIRCLE, PAD_LABEL_SQUARE, PAD_LABEL_TRIANGLE};
            memcpy(p.labels, labels, sizeof labels);
        } else if (len == 4 && !strncmp(s, "pro2", 4)) {
            snprintf(p.name, sizeof p.name, "Nintendo Switch 2 Pro Controller");
            p.type = 7, p.family = PAD_FAMILY_NINTENDO, p.switch2 = 1, p.wired = 0, p.battery = 85;
            p.rumble = p.motion = p.back_buttons = 1, p.layout = PAD_LAYOUT_NINTENDO;
            const int labels[4] = {PAD_LABEL_B, PAD_LABEL_A, PAD_LABEL_Y, PAD_LABEL_X};
            memcpy(p.labels, labels, sizeof labels);
        }
        if (p.name[0]) {
            p.player = n < 2 ? n : -1;
            snprintf(p.value, sizeof p.value, "pad:preview%d:0", n);
            out[n++] = p;
        }
        s = e ? e + 1 : s + len;
    }
    return n;
}
int input_player_pad(int player, InputPadInfo *out) {
    InputPadInfo pads[8];
    int n = input_pads(pads, 8);
    for (int i = 0; i < n; i++)
        if (pads[i].player == player) { *out = pads[i]; return 1; }
    return 0;
}
int input_keyboard_player(void) { return 0; }
int input_pad_identify(int) { return 1; }
int input_rumble_preview(void) { return 1; }
// AOD_PREVIEW_KEYBOARD=1: as when the keyboard or the mouse was used last (the hints show keys).
int input_keyboard_active(void) { const char *v = getenv("AOD_PREVIEW_KEYBOARD"); return v && *v == '1'; }
// AOD_PREVIEW_CONFIRM_BOTTOM=1: as when the game's menus confirm with the bottom button (input.c input_confirm_bottom:
// confirm_button = auto with this Xbox pad in use); unset: the game's own A confirms, as the pad scenarios press.
int input_confirm_bottom(void) { const char *v = getenv("AOD_PREVIEW_CONFIRM_BOTTOM"); return v && *v == '1'; }
void input_note_keyboard(void) {}
int input_player_connected(int player) {
    InputPadInfo pad;
    return player == 0 || input_player_pad(player, &pad);
}
uint32_t input_key_char(int) { return 0; }
void input_map_snapshot_player(int player, InputMap *m) {
    static unsigned seen = ~0u;
    if (seen != cfg_version()) {
        input_map_configure();
        seen = cfg_version();
    }
    *m = *input_map_player(player);
}
int input_capture(int reset, int *id) {
    if (reset) { preview_capture_key = preview_capture_pad = 0; return INPUT_CAPTURE_NONE; }
    if (preview_capture_key) {
        *id = preview_capture_key;
        preview_capture_key = 0;
        return *id == KEY_ESCAPE ? INPUT_CAPTURE_CANCEL : INPUT_CAPTURE_KEY;
    }
    if (preview_capture_pad) {
        *id = preview_capture_pad;
        preview_capture_pad = 0;
        return INPUT_CAPTURE_PAD;
    }
    return INPUT_CAPTURE_NONE;
}
void perfmon_mode(int) {}
void perfmon_set_gpu(const char *, const uint8_t *, int, float) {}
void perfmon_snapshot(PerfStats *s) {
    memset(s, 0, sizeof *s);
    s->fps = 60; s->frame_ms_avg = 16.67f; s->frame_ms_max = 18;
    for (float &v : s->frame_ms) v = 16.67f;
    snprintf(s->cpu_name, sizeof s->cpu_name, "Synthetic CPU");
    snprintf(s->gpu_name, sizeof s->gpu_name, "Synthetic GPU");
}
int sysinfo_text(char *buf, size_t n) { return snprintf(buf, n, "Menu preview"); }
void sysinfo_cpu_name(char *out, size_t n) { snprintf(out, n, "Synthetic CPU"); }
int sysinfo_log_folder(char *out, size_t n) { if (n) *out = 0; return 0; }
const char *ov_language(void) { return "en-US"; }
}
