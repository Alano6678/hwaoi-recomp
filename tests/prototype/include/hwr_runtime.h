#ifndef HWR_RUNTIME_H
#define HWR_RUNTIME_H
/* Original bounded AArch64 translation support. No OS, GPU or game services. */
#include <stddef.h>
#include <stdint.h>

enum HwrStatus {
    HWR_RUNNING = 0, HWR_EXTERNAL = 1, HWR_RETURN = 2,
    HWR_MEMORY_FAULT = 3, HWR_BUDGET_EXHAUSTED = 4
};

typedef struct HwrContext {
    uint64_t x[31];
    uint64_t sp;
    uint64_t pc;
    uint32_t nzcv; /* Architectural bits N=31, Z=30, C=29, V=28. */
    uint8_t *memory;
    size_t memory_size;
    uint64_t guest_base;
    uint64_t budget; /* Caller supplies a finite instruction allowance. */
    int status;
    uint64_t fault_address;
} HwrContext;

static inline uint64_t hwr_mask(unsigned bits) {
    return bits == 64 ? UINT64_MAX : UINT64_C(0xffffffff);
}
static inline uint64_t hwr_reg(const HwrContext *c, unsigned r, int use_sp) {
    return r == 31 ? (use_sp ? c->sp : 0) : c->x[r];
}
static inline void hwr_write(HwrContext *c, unsigned r, int use_sp,
                             unsigned bits, uint64_t value) {
    value &= hwr_mask(bits);
    if (r != 31) c->x[r] = value;
    else if (use_sp) c->sp = value;
}
static inline uint64_t hwr_shift(uint64_t a, unsigned kind,
                                unsigned count, unsigned bits) {
    uint64_t mask = hwr_mask(bits);
    a &= mask;
    if (!count) return a;
    if (kind == 0) return (a << count) & mask;
    if (kind == 1) return a >> count;
    if (kind == 2) {
        uint64_t result = a >> count;
        if (a & (UINT64_C(1) << (bits - 1)))
            result |= mask ^ (mask >> count);
        return result;
    }
    return ((a >> count) | (a << (bits - count))) & mask;
}
static inline void hwr_arithmetic_flags(HwrContext *c, uint64_t a,
                                        uint64_t b, uint64_t r,
                                        unsigned bits, int subtract) {
    uint64_t mask = hwr_mask(bits), sign = UINT64_C(1) << (bits - 1);
    unsigned carry, overflow;
    a &= mask; b &= mask; r &= mask;
    carry = subtract ? a >= b : r < a;
    overflow = ((subtract ? (a ^ b) : ~(a ^ b)) & (a ^ r) & sign) != 0;
    c->nzcv = ((uint32_t)((r & sign) != 0) << 31)
            | ((uint32_t)(r == 0) << 30) | ((uint32_t)carry << 29)
            | ((uint32_t)overflow << 28);
}
static inline void hwr_logical_flags(HwrContext *c, uint64_t r, unsigned bits) {
    r &= hwr_mask(bits);
    c->nzcv = ((uint32_t)((r >> (bits - 1)) & 1) << 31)
            | ((uint32_t)(r == 0) << 30);
}
static inline int hwr_condition(const HwrContext *c, unsigned cond) {
    unsigned n = (c->nzcv >> 31) & 1, z = (c->nzcv >> 30) & 1;
    unsigned carry = (c->nzcv >> 29) & 1, v = (c->nzcv >> 28) & 1;
    int result;
    switch (cond >> 1) {
    case 0: result = (int)z; break;
    case 1: result = (int)carry; break;
    case 2: result = (int)n; break;
    case 3: result = (int)v; break;
    case 4: result = (int)(carry && !z); break;
    case 5: result = (int)(n == v); break;
    case 6: result = (int)(n == v && !z); break;
    default: result = 1; break;
    }
    return (cond & 1) && cond != 15 ? !result : result;
}
static inline int hwr_memory_range(HwrContext *c, uint64_t address,
                                   size_t count) {
    if (!c->memory || (count && address > UINT64_MAX - (count - 1))
        || address < c->guest_base || count > c->memory_size
        || address - c->guest_base > c->memory_size - count) {
        c->status = HWR_MEMORY_FAULT;
        c->fault_address = address;
        return 0;
    }
    return 1;
}
static inline int hwr_load(HwrContext *c, uint64_t address,
                           unsigned bytes, uint64_t *result) {
    unsigned i;
    uint64_t value = 0;
    if (!hwr_memory_range(c, address, bytes)) return 0;
    for (i = 0; i < bytes; ++i)
        value |= (uint64_t)c->memory[(size_t)(address - c->guest_base) + i] << (i * 8);
    *result = value;
    return 1;
}
static inline int hwr_store(HwrContext *c, uint64_t address,
                            unsigned bytes, uint64_t value) {
    unsigned i;
    if (!hwr_memory_range(c, address, bytes)) return 0;
    for (i = 0; i < bytes; ++i)
        c->memory[(size_t)(address - c->guest_base) + i] = (uint8_t)(value >> (i * 8));
    return 1;
}
#endif
