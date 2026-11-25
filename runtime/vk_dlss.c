// NVIDIA DLSS for the temporal path (vk_dlss.h), Age of Decomp's own code. vk_temporal.c finds the scene, jitters its
// draws, makes the camera's motion vectors and an R32F copy of the depth, and calls vk_dlss_dispatch where it would
// call FSR: on the HDR lighting target at the scene's own size (DLAA), or on the tone-mapped picture in place of the
// game's own upscale (DLSS Super Resolution). Streamline wants the camera too; it is taken apart here from the
// view-projection the temporal path captured. World coordinates reach about 5e4, so the matrices are combined in
// double precision. The game's clip space is Streamline's: x right, y up (row 0 of an image is the top), depth
// reversed in 0..1.
#include "vk_dlss.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aod_dlss.h"
#include "cfg.h"
#include "host.h"
#include "platform/platform.h"

typedef struct {
    double vp[16];                       // view-projection, row vectors
    double pos[3], right[3], up[3], fwd[3];
    double p00, p11, a, b;               // projection: x, y scales; z_ndc = a + b / distance
    double nearz, farz;
    int inverted;
} Cam;

// ---- matrices: row major, row vectors (p' = p * M), as Streamline wants them

static void mat_mul(double r[16], const double a[16], const double b[16]) {
    double t[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            t[4 * i + j] = a[4 * i] * b[j] + a[4 * i + 1] * b[4 + j] + a[4 * i + 2] * b[8 + j] + a[4 * i + 3] * b[12 + j];
    memcpy(r, t, sizeof t);
}

static int mat_inv(double r[16], const double m[16]) {   // Gauss-Jordan with partial pivoting
    double a[4][8];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++) a[i][j] = j < 4 ? m[4 * i + j] : (j - 4 == i);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int i = c + 1; i < 4; i++)
            if (fabs(a[i][c]) > fabs(a[p][c])) p = i;
        if (fabs(a[p][c]) < 1e-300) return 0;
        if (p != c)
            for (int j = 0; j < 8; j++) { double t = a[c][j]; a[c][j] = a[p][j]; a[p][j] = t; }
        double inv = 1.0 / a[c][c];
        for (int j = 0; j < 8; j++) a[c][j] *= inv;
        for (int i = 0; i < 4; i++)
            if (i != c && a[i][c] != 0) {
                double f = a[i][c];
                for (int j = 0; j < 8; j++) a[i][j] -= f * a[c][j];
            }
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) r[4 * i + j] = a[i][4 + j];
    return 1;
}

static double dot3(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// A perspective view-projection without jitter or skew, else 0.
static int cam_from(const double m[16], Cam *c) {
    double cx[3] = {m[0], m[4], m[8]}, cy[3] = {m[1], m[5], m[9]}, cz[3] = {m[2], m[6], m[10]}, f[3] = {m[3], m[7], m[11]};
    double lf = sqrt(dot3(f, f)), p00 = sqrt(dot3(cx, cx)), p11 = sqrt(dot3(cy, cy));
    if (!(lf > 0.98 && lf < 1.02) || !(p00 > 0.05 && p00 < 50) || !(p11 > 0.05 && p11 < 50)) return 0;
    if (fabs(dot3(cx, f)) > 1e-2 * p00 || fabs(dot3(cy, f)) > 1e-2 * p11 || fabs(dot3(cx, cy)) > 1e-2 * p00 * p11) return 0;
    double k = dot3(cz, f) / (lf * lf), off = 0;
    for (int i = 0; i < 3; i++) off += (cz[i] - k * f[i]) * (cz[i] - k * f[i]);
    if (sqrt(off) > 1e-4 + 1e-2 * fabs(k)) return 0;
    double a = k, b = m[14] - k * m[15];   // z_clip = a * w + b with w = the distance along f
    if (fabs(b) < 1e-9) return 0;
    memcpy(c->vp, m, sizeof c->vp);
    c->p00 = p00, c->p11 = p11, c->a = a, c->b = b;
    c->inverted = b > 0;
    if (c->inverted) c->nearz = b / (1 - a), c->farz = a < 0 ? -b / a : 1e30;
    else c->nearz = a > 0 ? -b / a : 0, c->farz = b / (1 - a);
    for (int i = 0; i < 3; i++) c->right[i] = cx[i] / p00, c->up[i] = cy[i] / p11, c->fwd[i] = f[i] / lf;
    // The camera sits where clip x, y and w are all 0: three planes, solved by Cramer's rule.
    double r0[3] = {cx[0], cx[1], cx[2]}, r1[3] = {cy[0], cy[1], cy[2]}, r2[3] = {f[0], f[1], f[2]};
    double rhs[3] = {-m[12], -m[13], -m[15]};
    double det = r0[0] * (r1[1] * r2[2] - r1[2] * r2[1]) - r0[1] * (r1[0] * r2[2] - r1[2] * r2[0]) +
                 r0[2] * (r1[0] * r2[1] - r1[1] * r2[0]);
    if (fabs(det) < 1e-12) return 0;
    for (int col = 0; col < 3; col++) {
        double t[3][3] = {{r0[0], r0[1], r0[2]}, {r1[0], r1[1], r1[2]}, {r2[0], r2[1], r2[2]}};
        for (int row = 0; row < 3; row++) t[row][col] = rhs[row];
        c->pos[col] = (t[0][0] * (t[1][1] * t[2][2] - t[1][2] * t[2][1]) - t[0][1] * (t[1][0] * t[2][2] - t[1][2] * t[2][0]) +
                       t[0][2] * (t[1][0] * t[2][1] - t[1][1] * t[2][0])) / det;
    }
    return 1;
}

// ---- the setting and the files

static int g_asked;   // TEMPORAL asked for DLSS when the device was made (vk_dlss_started)

static const char *mode_at_start(void) {
    static char mode[16];
    if (!mode[0]) snprintf(mode, sizeof mode, "%s", cfg_value("TOTK_TEMPORAL"));
    return mode;
}

int vk_dlss_wanted(void) { return !strcmp(mode_at_start(), "dlss") || !strcmp(mode_at_start(), "dlaa"); }

const char *vk_dlss_quality(void) {
    static char q[24];
    if (!q[0]) snprintf(q, sizeof q, "%s", !strcmp(mode_at_start(), "dlss") ? cfg_value("TOTK_DLSS_QUALITY") : "native");
    return q;
}

void vk_dlss_started(int asked) { g_asked = asked; }

int vk_dlss_available(void) { return aod_dlss_ready(); }

int vk_dlss_failed(void) { return g_asked && !aod_dlss_ready(); }

int vk_dlss_dir(char *dir, size_t cap) {
    const char *d = cfg_get("TOTK_DLSS_DIR");
    if (d && *d) return snprintf(dir, cap, "%s", d) < (int)cap;
    int len = cap > 16 ? plat_exe_path(dir, cap - 16) : 0;
    char *cut = len ? plat_last_sep(dir) : NULL;
    if (!cut) return 0;
    strcpy(cut + 1, "streamline");
    return 1;
}

// NVIDIA's own files (nvngx_dlss.dll for DLSS, nvngx_dlssg.dll for frame generation) are the player's: in the folders
// TOTK_DLSS_NGX and TOTK_FRAME_GEN_NGX name, in the ngx folder beside the executable, or beside Streamline's DLLs.
static int ngx_has(const char *dir, const char *name) {
    char f[PLAT_PATH_MAX * 2 + 32];
    snprintf(f, sizeof f, "%s/%s", dir, name);
    return plat_file_exists(f);
}

// Adds dir to the '|' separated list unless it is there already.
static void ngx_add(char *dirs, size_t cap, const char *dir) {
    size_t n = strlen(dirs), d = strlen(dir);
    for (const char *p = dirs; *p;) {
        const char *end = strchr(p, '|');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == d && !strncmp(p, dir, d)) return;
        p += len + (end != NULL);
    }
    if (d && n + d + 2 <= cap) snprintf(dirs + n, cap - n, "%s%s", n ? "|" : "", dir);
}

int vk_dlss_ngx_dir(char *dirs, size_t cap) {
    char dir[PLAT_PATH_MAX * 2];
    dirs[0] = 0;
    static const char *const keys[] = {"TOTK_DLSS_NGX", "TOTK_FRAME_GEN_NGX"};
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
        const char *e = cfg_get(keys[i]);
        if (!e || !*e || snprintf(dir, sizeof dir, "%s", e) >= (int)sizeof dir) continue;
        if (plat_file_exists(dir)) {   // the DLL itself: its folder
            char *cut = plat_last_sep(dir);
            if (!cut) continue;
            *cut = 0;
        }
        ngx_add(dirs, cap, dir);
    }
    int len = plat_exe_path(dir, sizeof dir - 16);
    char *cut = len ? plat_last_sep(dir) : NULL;
    if (cut) {
        strcpy(cut + 1, "ngx");
        if (ngx_has(dir, "nvngx_dlss.dll") || ngx_has(dir, "nvngx_dlssg.dll")) ngx_add(dirs, cap, dir);
    }
    return dirs[0] != 0;
}

// Streamline's module sl_dll and NVIDIA's file ngx_dll are in place.
static int files_present(const char *sl_dll, const char *ngx_dll) {
    char dir[PLAT_PATH_MAX * 2], ngx[PLAT_PATH_MAX * 6];
    if (!vk_dlss_dir(dir, sizeof dir) || !ngx_has(dir, "sl.interposer.dll") || !ngx_has(dir, sl_dll)) return 0;
    if (ngx_has(dir, ngx_dll)) return 1;
    if (!vk_dlss_ngx_dir(ngx, sizeof ngx)) return 0;
    for (char *p = ngx; p && *p;) {
        char *end = strchr(p, '|');
        if (end) *end = 0;
        if (ngx_has(p, ngx_dll)) return 1;
        p = end ? end + 1 : NULL;
    }
    return 0;
}

int vk_dlss_files_present(void) {
    static int known = -1;
    if (known < 0) known = files_present("sl.dlss.dll", "nvngx_dlss.dll");   // the folders' files decide once per run
    return known;
}

int vk_dlss_offered(void) {
    VkCtx *c = vk_ctx();
    return c && c->props.vendorID == 0x10de && vk_dlss_files_present();
}

// ---- frame generation (FRAME_GEN, read at start like TEMPORAL: Streamline starts before the Vulkan instance)

static int g_fg_asked;   // FRAME_GEN asked for it when the device was made (vk_fg_started)

int vk_fg_wanted(void) {
    static int factor = -1;
    if (factor < 0) {
        int n = atoi(cfg_value("TOTK_FRAME_GEN"));   // "off" reads as 0
        factor = n >= 2 && n <= 6 ? n : 0;
    }
    return factor;
}

void vk_fg_started(int asked) { g_fg_asked = asked; }

int vk_fg_available(void) { return aod_fg_ready(); }

int vk_fg_failed(void) { return g_fg_asked && !aod_fg_ready(); }

int vk_fg_files_present(void) {
    static int known = -1;
    if (known < 0) known = files_present("sl.dlss_g.dll", "nvngx_dlssg.dll");
    return known;
}

int vk_fg_offered(void) {
    VkCtx *c = vk_ctx();
    return c && c->props.vendorID == 0x10de && vk_fg_files_present();
}

int vk_fg_factor(void) { return aod_fg_factor(); }
int vk_fg_max(void) { return aod_fg_max(); }
double vk_fg_rate(void) { return aod_fg_rate(); }

// ---- one frame

// What Streamline wants to know of a frame, from the temporal path's: the camera taken apart, the reprojection, the
// jitter and the depth and motion images. 1 when the scene gave a camera.
static int frame_of(AodDlssFrame *f, const VkDlssDispatch *d) {
    memset(f, 0, sizeof *f);
    static const double identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    Cam c;
    int cam = d->vp && cam_from(d->vp, &c);
    double inv[16];
    if (cam && d->prev_vp && !d->reset && mat_inv(inv, c.vp)) {
        mat_mul(f->clip_to_prev, inv, d->prev_vp);   // clip -> world -> previous clip
        if (!mat_inv(f->prev_to_clip, f->clip_to_prev)) memcpy(f->prev_to_clip, identity, sizeof identity);
    } else {
        memcpy(f->clip_to_prev, identity, sizeof identity);
        memcpy(f->prev_to_clip, identity, sizeof identity);
    }
    if (cam) {
        double proj[16] = {c.p00, 0, 0, 0, 0, c.p11, 0, 0, 0, 0, c.a, 1, 0, 0, c.b, 0};   // view: x right, y up, z ahead
        memcpy(f->view_to_clip, proj, sizeof proj);
        if (!mat_inv(f->clip_to_view, proj)) memcpy(f->clip_to_view, identity, sizeof identity);
        memcpy(f->cam_pos, c.pos, sizeof f->cam_pos);
        memcpy(f->cam_up, c.up, sizeof f->cam_up);
        memcpy(f->cam_right, c.right, sizeof f->cam_right);
        memcpy(f->cam_fwd, c.fwd, sizeof f->cam_fwd);
        f->cam_near = c.nearz, f->cam_far = c.farz;
        f->cam_fov = 2 * atan(1 / c.p11), f->cam_aspect = c.p11 / c.p00;
        f->depth_inverted = c.inverted;
    } else {   // no camera this frame: a still one of the game's usual shape, and a new history
        double p11 = 1 / tan(41.0 * 3.14159265358979 / 360), aspect = (double)d->depth.width / d->depth.height;
        double proj[16] = {p11 / aspect, 0, 0, 0, 0, p11, 0, 0, 0, 0, 0, 1, 0, 0, 64, 0};
        memcpy(f->view_to_clip, proj, sizeof proj);
        if (!mat_inv(f->clip_to_view, proj)) memcpy(f->clip_to_view, identity, sizeof identity);
        f->cam_up[1] = 1, f->cam_right[0] = 1, f->cam_fwd[2] = 1;
        f->cam_near = 64, f->cam_far = 210000, f->cam_fov = 41.0 * 3.14159265358979 / 180, f->cam_aspect = aspect;
        f->depth_inverted = 1;
    }
    static double sign = 0;
    if (!sign) {   // developer switch while a convention is checked: TOTK_DLSS_JITTER_SIGN=-1 flips the offset
        const char *e = cfg_get("TOTK_DLSS_JITTER_SIGN");
        sign = e && atof(e) < 0 ? -1 : 1;
    }
    f->jitter[0] = (float)(sign * d->jx), f->jitter[1] = (float)(sign * d->jy);
    f->reset = d->reset || !cam;
    f->hdr = d->hdr;
    f->mvec_camera = 1;   // zeros when the camera is unknown or did not move: still what the picture did
    f->depth = (AodDlssImage){d->depth.image, d->depth.view, d->depth.format, d->depth.width, d->depth.height,
                              d->depth.usage, VK_IMAGE_ASPECT_COLOR_BIT};
    f->mvec = (AodDlssImage){d->mv.image, d->mv.view, d->mv.format, d->mv.width, d->mv.height, d->mv.usage,
                             VK_IMAGE_ASPECT_COLOR_BIT};
    f->bias = (AodDlssImage){d->bias.image, d->bias.view, d->bias.format, d->bias.width, d->bias.height,
                             d->bias.usage, VK_IMAGE_ASPECT_COLOR_BIT};
    return cam;
}

int vk_fg_scene(VkCommandBuffer cb, const VkDlssDispatch *d) {
    static int told;
    AodDlssFrame f;
    int cam = frame_of(&f, d);
    int ok = aod_fg_scene(cb, &f);
    if (!told && ok) {
        told = 1;
        rt_log("frame generation: first scene: depth and motion vectors %ux%u, camera %s (near %.1f, %s Z), motion "
               "vectors %s", d->depth.width, d->depth.height, cam ? "from the scene" : "unknown", f.cam_near,
               f.depth_inverted ? "reversed" : "standard", d->moving ? "from the camera" : "zero");
    }
    return ok;
}

int vk_dlss_dispatch(VkCommandBuffer cb, const VkDlssDispatch *d) {
    static uint32_t frames;
    static int told;
    AodDlssFrame f;
    int cam = frame_of(&f, d);
    f.frame = ++frames;
    f.color_in = (AodDlssImage){d->color.image, d->color.view, d->color.format, d->color.width, d->color.height,
                                d->color.usage & ~(VkImageUsageFlags)VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_ASPECT_COLOR_BIT};
    f.color_out = (AodDlssImage){d->out.image, d->out.view, d->out.format, d->out.width, d->out.height, d->out.usage,
                                 VK_IMAGE_ASPECT_COLOR_BIT};
    if (!told) {
        told = 1;
        rt_log("dlss: first frame: %ux%u -> %ux%u, camera %s (near %.1f, %s Z), motion vectors %s", d->color.width,
               d->color.height, d->out.width, d->out.height, cam ? "from the scene" : "unknown", f.cam_near,
               f.depth_inverted ? "reversed" : "standard", d->moving ? "from the camera" : "zero");
    }
    return aod_dlss_evaluate(cb, &f);
}
