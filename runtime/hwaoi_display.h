// Native HWAoI presets. Window dimensions are independent of these allocations.
#pragma once
#include <stdint.h>
#include <string.h>

#define HWAOI_TEXTURE_POOL_BASE UINT64_C(0x122f00000)

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

// The measured 4K scene exhausts the original pool. Keep the smaller presets'
// original capacity; reserve matching application memory for the enlarged pool.
// The diagnostic override can force either plan without changing game files.
static inline int hwaoi_display_pool_plan(const unsigned scene[2], const char *override,
                                          uint64_t *child, uint64_t *parent, uint64_t *application_extra) {
    int large = scene[0] == 3840 && scene[1] == 2160;
    uint64_t bytes = large ? UINT64_C(5632) << 20 : HWAOI_TEXTURE_POOL_BASE;
    if (override) {
        if (!strcmp(override, "1")) bytes = UINT64_C(6) << 30;
        else if (!strcmp(override, "0")) bytes = HWAOI_TEXTURE_POOL_BASE;
        else {
            unsigned mib = 0;
            for (const unsigned char *p = (const unsigned char *)override; *p; p++) {
                if (*p < '0' || *p > '9') return 0;
                mib = mib * 10 + (*p - '0');
                if (mib > 6144) return 0;
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
