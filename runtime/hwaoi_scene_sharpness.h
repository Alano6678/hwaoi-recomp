// Original, explicit 3D scene sharpness preference for one verified game RCAS
// kernel. This changes its supplied strength, not the original shader algorithm.
// Native control/code SHA256: ae3465623ca09a95f8d0a6b9493a1ca7cf92cb72eabd0b46bd81f7eef8a801cf
// / 51797365960eab9a10d8d0c88a54c745445f70faf9e882ece261db4015ef68f8.
// Production-wrapper lowering matches the captured RCAS SPIR-V byte-for-byte:
// 19dbcb50d2cb7237345577eb6bc343871f6e8a69f1476694f5ff0989d92248df.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define HWAOI_SCENE_RCAS_ORIGIN UINT64_C(0x2771af0c1ea82a4c)
#define HWAOI_SCENE_RCAS_CONTROL_BYTES 0x880u
#define HWAOI_SCENE_RCAS_CODE_BYTES 14736u

static inline int hwaoi_scene_sharpness_parse(const char *value, int *percent) {
    if (!percent) return 0;
    if (!value) value = "game";   // private-copy mode remains opt-in pending full-runtime fault investigation
    if (!strcmp(value, "game")) { *percent = -1; return 1; }
    if (!*value) return 0;
    unsigned n = 0;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p < '0' || *p > '9') return 0;
        n = n * 10 + (unsigned)(*p - '0');
        if (n > 100) return 0;
    }
    *percent = (int)n;
    return 1;
}

static inline int hwaoi_scene_sharpness_words(int percent, uint32_t words[2]) {
    if (!words || percent < 0 || percent > 100) return 0;
    float strength = (float)percent / 100.0f;
    uint32_t bits;
    memcpy(&bits, &strength, sizeof bits);
    // Positive 1..100 percent lies in binary16's normal finite range. Round
    // the discarded significand to nearest/even; zero is handled separately.
    uint32_t half = 0;
    if (percent) {
        half = (bits - UINT32_C(0x38000000)) >> 13;
        uint32_t discarded = bits & 0x1fff;
        if (discarded > 0x1000 || (discarded == 0x1000 && (half & 1))) half++;
    }
    words[0] = bits;
    words[1] = half | (half << 16);
    return 1;
}

static inline int hwaoi_scene_sharpness_match(const uint8_t *control, size_t control_size,
                                             const uint8_t *code, size_t code_size) {
    static const uint8_t control_sha[32] = {0xc0,0x9e,0x90,0xf4,0x9a,0x98,0x4d,0x19,0xc5,0xf9,0x46,0x14,0x80,0x95,0x72,0x46,0xa4,0xd1,0x0b,0xa2,0x71,0x22,0x17,0xee,0xf3,0x6a,0xa2,0xa8,0xce,0x4b,0x23,0xe7};
    static const uint8_t code_sha[32] = {0xae,0x2c,0x8d,0x81,0xd7,0x9b,0xb6,0xdc,0xc1,0xdf,0x13,0x70,0xcd,0xd1,0xe3,0x6e,0xd1,0xda,0x60,0x2c,0x35,0x73,0xac,0x9c,0x8f,0xeb,0xf3,0xe7,0x5b,0x1b,0x8e,0xad};
    if (!control || !code || control_size != HWAOI_SCENE_RCAS_CONTROL_BYTES ||
        code_size != HWAOI_SCENE_RCAS_CODE_BYTES) return 0;
    // This kernel has no HIR1 register-bank extension. The complete control
    // digest also fixes DKC1/root layout, local size and all compiler offsets.
    static const uint8_t no_ir_banks[4] = {0};
    if (memcmp(control + 0x850, no_ir_banks, 4)) return 0;
    uint8_t digest[32];
    rt_sha256(control, control_size, digest);
    if (memcmp(digest, control_sha, sizeof digest)) return 0;
    rt_sha256(code, code_size, digest);
    return !memcmp(digest, code_sha, sizeof digest);
}
