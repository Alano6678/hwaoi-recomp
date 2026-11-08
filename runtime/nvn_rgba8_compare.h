// Narrow, exact normalized-value conversion for CPU-authoritative grayscale RGBA8 comparison textures.
#pragma once
#include <stdint.h>
#include <string.h>

// Bounded fallback for small CPU-authored defaults, including the observed 8x8
// texture. Keep larger/mipped/GPU-written images on a visibly unsupported path.
#define NVN_RGBA8_COMPARE_MAX_EDGE 8u
#define NVN_RGBA8_COMPARE_MAX_TEXELS (6u * NVN_RGBA8_COMPARE_MAX_EDGE * NVN_RGBA8_COMPARE_MAX_EDGE)

static inline uint32_t nvn_rgba8_compare_count(uint32_t width, uint32_t height, uint32_t layers) {
    if (!width || !height || width > NVN_RGBA8_COMPARE_MAX_EDGE || height > NVN_RGBA8_COMPARE_MAX_EDGE ||
        (layers != 1 && layers != 6)) return 0;
    return width * height * layers;
}

static inline int nvn_rgba8_compare_texels(const uint8_t rgba[][4], uint32_t count, uint16_t *depth) {
    if (!rgba || !depth || !count || count > NVN_RGBA8_COMPARE_MAX_TEXELS) return 0;
    // Validate the entire image before publishing any converted texel.
    for (uint32_t i = 0; i < count; i++)
        if (rgba[i][0] != rgba[i][1] || rgba[i][0] != rgba[i][2] || rgba[i][0] != rgba[i][3]) return 0;
    for (uint32_t i = 0; i < count; i++) depth[i] = (uint16_t)((uint32_t)rgba[i][0] * 257u);
    return 1;
}

static inline int nvn_rgba8_compare_depth(const uint8_t rgba[6][4], uint32_t layers, uint16_t depth[6]) {
    if (layers != 1 && layers != 6) return 0;
    uint16_t next[6] = {0};
    // (v * 257) / 65535 == v / 255 for every UNORM8 value.
    if (!nvn_rgba8_compare_texels(rgba, layers, next)) return 0;
    memcpy(depth, next, sizeof next);
    return 1;
}
