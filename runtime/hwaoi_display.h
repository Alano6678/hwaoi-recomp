// Native HWAoI presets. Window dimensions are independent of these allocations.
#pragma once
#include <stdint.h>
#include <string.h>

#define HWAOI_TEXTURE_POOL_BASE UINT64_C(0x122f00000)

// Whether the native-display patch sets (hwaoi_display, hwaoi_native1080: scene = output, both scale policies 1.0) are
// wanted. TOTK_NATIVE_1080 decides when set; otherwise they are, except with RENDER_SCALE=game: the game's own path
// then runs (its 1600x900 scene, dynamic resolution and temporal upscaler to 1080p, which also smooths edges).
static inline int hwaoi_native_wanted(const char *native_1080, const char *render_scale) {
    if (native_1080 && *native_1080) return strcmp(native_1080, "0") != 0;
    return !render_scale || strcmp(render_scale, "game") != 0;
}

static inline int hwaoi_display_size(const char *value, unsigned size[2]) {
    static const struct { const char *name; unsigned w, h; } presets[] = {
        {"1280x720", 1280, 720}, {"1920x1080", 1920, 1080},
        {"2560x1440", 2560, 1440}, {"3840x2160", 3840, 2160},
    };
    for (unsigned i = 0; i < sizeof presets / sizeof *presets; i++) {
        if (value && !strcmp(value, presets[i].name)) {
            size[0] = presets[i].w; size[1] = presets[i].h;
            return 1;
        }
    }
    return 0;
}

static inline int hwaoi_display_dimensions(const char *output, const char *scene_override,
                                            unsigned out[2], unsigned scene[2]) {
    unsigned o[2], s[2];
    if (!hwaoi_display_size(output, o)) return 0;
    s[0] = o[0]; s[1] = o[1];
    if (scene_override && (!hwaoi_display_size(scene_override, s) || s[0] > o[0] || s[1] > o[1])) return 0;
    out[0] = o[0]; out[1] = o[1]; scene[0] = s[0]; scene[1] = s[1];
    return 1;
}

// The scene for the temporal upscaler (HWAOI_TEMPORAL=fsr, HWAOI_FSR_QUALITY): the output divided by 1.5 (quality),
// 1.7 (balanced) or 2 (performance) on each side, rounded to even sizes (1920x1080: 1280x720, 1130x636, 960x540).
// Smaller than the output, so the presets' texture pool plan covers it.
static inline int hwaoi_display_fsr_scene(const unsigned out[2], const char *quality, unsigned scene[2]) {
    unsigned tenths = !quality ? 0 : !strcmp(quality, "quality") ? 15 : !strcmp(quality, "balanced") ? 17
                    : !strcmp(quality, "performance") ? 20 : 0;
    if (!tenths) return 0;
    for (int i = 0; i < 2; i++) scene[i] = 2 * ((out[i] * 10 + tenths) / (2 * tenths));
    return 1;
}

// Keep the original pool through 1080p and grow it for larger presets.
// Reserve matching application memory for any increase in pool capacity.
// The diagnostic override can set the capacity without changing game files.
static inline int hwaoi_display_pool_plan(const unsigned scene[2], const char *override,
                                          uint64_t *child, uint64_t *parent, uint64_t *application_extra) {
    // The render targets scale with the scene's pixel count: 838 MiB more per extra 1080p worth of pixels (3840x2160 is
    // 3 extra: 7169 MiB; 5632 MiB failed loading a stage and 6144 MiB was full after 15-20 minutes, when a movie's
    // 2 MiB surface found 1 MiB free in 27 blocks). The pool is address space until used (resident memory does not
    // grow); the ceiling is the guest heap window (memory.c HEAP_CAP 12 GiB, 9 GiB of it the base application pool).
    uint64_t px = (uint64_t)scene[0] * scene[1], base_px = UINT64_C(1920) * 1080;
    uint64_t bytes = HWAOI_TEXTURE_POOL_BASE;
    if (px > base_px) bytes += ((UINT64_C(838) * (px - base_px)) / base_px) << 20;
    if (bytes > (UINT64_C(7680) << 20)) bytes = UINT64_C(7680) << 20;
    if (override) {
        if (!strcmp(override, "1")) bytes = UINT64_C(6) << 30;
        else if (!strcmp(override, "0")) bytes = HWAOI_TEXTURE_POOL_BASE;
        else {
            unsigned mib = 0;
            for (const unsigned char *p = (const unsigned char *)override; *p; p++) {
                if (*p < '0' || *p > '9') return 0;
                mib = mib * 10 + (*p - '0');
                if (mib > 7680) return 0;   // the 12 GiB guest heap window (memory.c HEAP_CAP) minus the 9 GiB base pool
            }
            if (mib < 4655) return 0;
            bytes = (uint64_t)mib << 20;
        }
    }
    *child = bytes;
    *parent = bytes + (UINT64_C(395) << 20) + 0x5008;
    *application_extra = (bytes - HWAOI_TEXTURE_POOL_BASE + 0x1fffff) & ~UINT64_C(0x1fffff);
    return 1;
}
