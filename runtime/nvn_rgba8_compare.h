// Narrow, exact normalized-value conversion for CPU-authoritative grayscale RGBA8 comparison textures.
#pragma once
#include <stdint.h>
#include <string.h>

static inline int nvn_rgba8_compare_depth(const uint8_t rgba[6][4], uint32_t layers, uint16_t depth[6]) {
    if (layers != 1 && layers != 6) return 0;
    uint16_t next[6] = {0};
    for (uint32_t i = 0; i < layers; i++) {
        // No unproved channel-selection rule: every component must carry the same actual value.
        if (rgba[i][0] != rgba[i][1] || rgba[i][0] != rgba[i][2] || rgba[i][0] != rgba[i][3]) return 0;
        // (v * 257) / 65535 == v / 255, preserving every UNORM8 value exactly as UNORM16.
        next[i] = (uint16_t)((uint32_t)rgba[i][0] * 257u);
    }
    memcpy(depth, next, sizeof next);
    return 1;
}
