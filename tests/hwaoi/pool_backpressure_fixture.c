#include "host.h"
#include "platform/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <windows.h>

static RtFileHash hashes[] = {{"main", "4c33cfd4c818d9ed62822af792782b4ea443417ed24acea6f1cee739cde604c3"}};
static int hash_count = 1;
#define rt_exefs_sha256 hashes
#define rt_exefs_sha256_count hash_count
static RtGuestLayout layout = {.heap = {0x2000000000ull, 0x400000}};
static uint64_t clock_ns;
static unsigned calls, waits, successes_after = 2, permission = 3;
static int away, thread[2];
static Cpu incoming;
static uint64_t parent;
static uint32_t parent_count;
static uint64_t wrapper_stack, unreadable_frame;
static uint8_t stack_snapshot[0x590];
static unsigned frame_reads;
static void check(int p) { if (!p) abort(); }
int kern_host_track = 1;
void kern_host_left(void *t) { check(t == thread && !thread[1]); }
uint64_t kern_now_ns(void) { return clock_ns; }
const RtGuestLayout *mem_layout(void) { return &layout; }
uint32_t mem_query(uint64_t a, RtMemoryInfo *info) {
    if (a < layout.heap.base || a - layout.heap.base >= layout.heap.size) return 1;
    *info = (RtMemoryInfo){.base = layout.heap.base, .size = layout.heap.size, .perm = permission};
    return 0;
}
int kern_core_away(void) { check(thread[1] == 1 && !away); away = 1; return 1; }
void kern_core_back(int *a) { check(*a == 1 && away && thread[1] == 1); away = 0; *a = 0; }
static void preserved(void) {
    if (parent) check(LD32(parent + 0x34) == parent_count);
    if (wrapper_stack) check(!memcmp((const void *)(uintptr_t)wrapper_stack, stack_snapshot, sizeof stack_snapshot));
}
int plat_mem_read(uint64_t address, void *out, size_t size) {
    check(thread[1] == 1 && !away);
    frame_reads++;
    if (!wrapper_stack || address < wrapper_stack || size > sizeof stack_snapshot ||
        address - wrapper_stack > sizeof stack_snapshot - size || address == unreadable_frame) return 0;
    memcpy(out, (const void *)(uintptr_t)address, size);
    return 1;
}
void plat_sleep_ns(uint64_t ns) {
    check(away && thread[1] == 1 && ns == 250000); preserved(); clock_ns += ns; waits++;
}
void rt_fatal(const char *fmt, ...) {
    preserved();
    fprintf(stderr, "%s\n", fmt); exit(70);
}
void rt_log(const char *fmt, ...) { (void)fmt; }
static void original(Cpu *c) {
    check(!thread[1] && !away);
    check(!memcmp(c, &incoming, sizeof *c));
    preserved();
    calls++;
    c->x[0] = calls > successes_after ? 0x2000010000ull : 0;
    c->x[1] = 0x222; c->x[8] = 0x888; c->x[9] = 0x999;
    c->pc = 0x7001add040ull;
    c->v[0] = 0x123456;
}
#include "hwaoi_pool_backpressure.h"

int main(int argc, char **argv) {
    check(argc == 2);
    const char *mode = argv[1];
    uint64_t addr = 0x7001adce64ull;
    GuestFn fn = original;
    RtModule module = {.name = "main", .base = 0x7000000000ull, .size = 0x3000000,
                       .addrs = &addr, .fns = &fn, .count = 1};
    _putenv_s("HWAOI_POOL_BACKPRESSURE", !strcmp(mode, "off") ? "0" : !strcmp(mode, "badmode") ? "yes" : "1");
    if (!strcmp(mode, "unset")) _putenv_s("HWAOI_POOL_BACKPRESSURE", "");
    if (!strcmp(mode, "hash")) hashes[0].sha256 = "wrong";
    if (!strcmp(mode, "nohash")) hash_count = 0;
    if (!strcmp(mode, "module")) module.name = "sdk";
    if (!strcmp(mode, "extent")) module.size = 0x1adce64;
    if (!strcmp(mode, "caller-extent")) module.size = 0x1af5e64;
    if (!strcmp(mode, "caller-partial-instruction")) module.size = 0x1af5e67;
    if (!strcmp(mode, "caller-full-instruction")) module.size = 0x1af5e68;
    if (!strcmp(mode, "overflow")) module.base = RT_ADDRESS_LIMIT - 32;
    if (!strcmp(mode, "function")) module.count = 0;
    hwaoi_pool_wait_init(&module, 1);
    if (!strcmp(mode, "off") || !strcmp(mode, "unset")) { check(!hwaoi_pool_wait.on); return 0; }
    check(hwaoi_pool_wait.on && hwaoi_pool_wait.original == original);
    void *heap = VirtualAlloc((void *)(uintptr_t)layout.heap.base, layout.heap.size,
                             MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    check(heap != NULL);
    uint64_t pool = layout.heap.base;
    ST64(pool + 0x18, pool + 0x10000); ST32(pool + 0x28, 128); ST32(pool + 0x2c, 16000);
    memset(&incoming, 0xa5, sizeof incoming);
    incoming.pc = addr; incoming.x[0] = pool; incoming.x[1] = 0x50;
    incoming.x[30] = 0x70000289b0ull; incoming.thread = thread;
    const uint64_t submit[] = {0x1ec13c, 0x1ec29c, 0x1ec454, 0x1ec5c8, 0x1ec75c, 0x1ec8dc,
                              0x1eca5c, 0x1ecbdc, 0x1ecd7c, 0x1ecf44, 0x1ed0f8};
    if (!strncmp(mode, "submit-", 7)) {
        unsigned index = (unsigned)strtoul(mode + 7, NULL, 10);
        check(index < sizeof submit / sizeof *submit);
        incoming.x[30] = module.base + submit[index];
    }
    if (!strcmp(mode, "near-submit")) incoming.x[30] = module.base + 0x1eca60;
    if (!strcmp(mode, "wrong-submit-size")) { incoming.x[30] = module.base + 0x1eca5c; incoming.x[1] = 0x70; }
    if (!strcmp(mode, "parent")) { incoming.x[30] = module.base + 0x26454; incoming.x[1] = 0x70; }
    if (!strcmp(mode, "parent-size")) incoming.x[30] = module.base + 0x26454;
    if (!strcmp(mode, "parent-buffer")) { incoming.x[30] = module.base + 0x261a8; incoming.x[1] = 0x70; }
    if (!strncmp(mode, "child-", 6)) {
        incoming.x[30] = module.base + 0x1668b4; incoming.x[1] = 0x58;
        parent = pool + 0x4000; parent_count = 3;
        incoming.x[26] = parent; ST32(parent + 0x34, parent_count);
        if (!strcmp(mode, "child-second")) incoming.x[30] = module.base + 0x1669e8;
        if (!strcmp(mode, "child-size")) incoming.x[1] = 0x50;
        if (!strcmp(mode, "child-near")) incoming.x[30] += 4;
        if (!strcmp(mode, "child-other")) incoming.x[30] = module.base + 0x1672ec;
        if (!strcmp(mode, "child-timeout")) successes_after = 100000;
        if (!strcmp(mode, "child-closing")) ST32(pool + 0x20, 1u << 30);
    }
    unsigned wrapper_bypass = 0, expected_frame_reads = 0;
    if (!strncmp(mode, "wrapper-", 8)) {
        wrapper_stack = pool + 0x300000;
        const uint32_t offsets[] = {0x10, 0x50, 0x510, 0x580};
        const uint32_t returns[] = {0x14c3e24, 0x14dfdac, 0x1af5e64, 0x6c3980};
        incoming.sp = wrapper_stack;
        incoming.x[29] = wrapper_stack + 0x10; incoming.x[19] = wrapper_stack + 0x30;
        incoming.x[2] = wrapper_stack; incoming.x[30] = module.base + 0x806dc; incoming.x[1] = 0x38;
        ST64(wrapper_stack, 0x306e); ST64(wrapper_stack + 8, 0);
        parent = pool + 0x4000; parent_count = 1;
        ST32(parent + 0x34, parent_count);
        ST64(wrapper_stack + 0x30, parent); ST64(wrapper_stack + 0x38, parent);
        ST64(wrapper_stack + 0x40, pool); ST64(wrapper_stack + 0x48, 1);
        for (unsigned i = 0; i < 4; i++) {
            ST64(wrapper_stack + offsets[i], i < 3 ? wrapper_stack + offsets[i + 1] : wrapper_stack + 0x700);
            ST64(wrapper_stack + offsets[i] + 8, module.base + returns[i]);
        }
        expected_frame_reads = 4;
        if (!strcmp(mode, "wrapper-timeout")) successes_after = 100000;
        else if (!strcmp(mode, "wrapper-closing")) ST32(pool + 0x20, 1u << 30);
        else if (!strcmp(mode, "wrapper-success")) successes_after = 0;
        else if (!strcmp(mode, "wrapper-lr")) { incoming.x[30] += 4; wrapper_bypass = 1; expected_frame_reads = 0; }
        else if (!strcmp(mode, "wrapper-size")) { incoming.x[1]++; wrapper_bypass = 1; expected_frame_reads = 0; }
        else if (!strcmp(mode, "wrapper-sp-zero")) { incoming.sp = 0; wrapper_bypass = 1; expected_frame_reads = 0; }
        else if (!strcmp(mode, "wrapper-sp-align")) { incoming.sp++; wrapper_bypass = 1; expected_frame_reads = 0; }
        else if (!strcmp(mode, "wrapper-sp-overflow")) { incoming.sp = UINT64_MAX - 15; wrapper_bypass = 1; expected_frame_reads = 0; }
        else if (!strcmp(mode, "wrapper-sp-limit")) { incoming.sp = RT_ADDRESS_LIMIT - 0x580; wrapper_bypass = 1; expected_frame_reads = 0; }
        else if (!strcmp(mode, "wrapper-fp")) { incoming.x[29] += 16; wrapper_bypass = 1; expected_frame_reads = 0; }
        else if (!strcmp(mode, "wrapper-args")) { incoming.x[19] += 16; wrapper_bypass = 1; expected_frame_reads = 0; }
        else if (!strcmp(mode, "wrapper-tag")) { incoming.x[2] += 16; wrapper_bypass = 1; expected_frame_reads = 0; }
        else if (!strncmp(mode, "wrapper-return-", 15)) {
            unsigned index = (unsigned)strtoul(mode + 15, NULL, 10); check(index < 4);
            ST64(wrapper_stack + offsets[index] + 8, module.base + returns[index] + 4);
            wrapper_bypass = 1; expected_frame_reads = index + 1;
        } else if (!strncmp(mode, "wrapper-chain-", 14)) {
            unsigned index = (unsigned)strtoul(mode + 14, NULL, 10); check(index < 3);
            ST64(wrapper_stack + offsets[index], wrapper_stack + offsets[index + 1] + 16);
            wrapper_bypass = 1; expected_frame_reads = index + 1;
        } else if (!strncmp(mode, "wrapper-unreadable-", 19)) {
            unsigned index = (unsigned)strtoul(mode + 19, NULL, 10); check(index < 4);
            unreadable_frame = wrapper_stack + offsets[index];
            wrapper_bypass = 1; expected_frame_reads = index + 1;
        } else check(!strcmp(mode, "wrapper-recover"));
        memcpy(stack_snapshot, (const void *)(uintptr_t)wrapper_stack, sizeof stack_snapshot);
    }
    if (!strcmp(mode, "success")) successes_after = 0;
    if (!strcmp(mode, "lr")) incoming.x[30] += 4;
    if (!strcmp(mode, "size")) incoming.x[1]++;
    if (!strcmp(mode, "timeout")) successes_after = 100000;
    if (!strcmp(mode, "closing")) ST32(pool + 0x20, 1u << 30);
    if (!strcmp(mode, "empty")) ST32(pool + 0x2c, 0);
    if (!strcmp(mode, "count")) ST32(pool + 0x2c, 16001);
    if (!strcmp(mode, "slot")) ST32(pool + 0x28, 64);
    if (!strcmp(mode, "backing")) ST64(pool + 0x18, pool + layout.heap.size - 8);
    if (!strcmp(mode, "nonheap")) incoming.x[0] = 0x7000000000ull;
    if (!strcmp(mode, "unaligned")) incoming.x[0]++;
    if (!strcmp(mode, "readonly")) permission = 1;
    Cpu c = incoming;
    hwaoi_pool_wait_call(&c);
    unsigned expected_calls = (wrapper_bypass || !strcmp(mode, "wrapper-success") ||
                               !strcmp(mode, "success") || !strcmp(mode, "lr") || !strcmp(mode, "size") ||
                               !strcmp(mode, "near-submit") || !strcmp(mode, "wrong-submit-size") ||
                               !strcmp(mode, "child-size") || !strcmp(mode, "child-near") || !strcmp(mode, "child-other") ||
                               !strcmp(mode, "parent-size") || !strcmp(mode, "parent-buffer")) ? 1 : 3;
    check(calls == expected_calls && waits == expected_calls - 1 && !away && !thread[1]);
    check(frame_reads == expected_frame_reads); preserved();
    Cpu expected = incoming;
    expected.x[0] = calls > successes_after ? 0x2000010000ull : 0;
    expected.x[1] = 0x222; expected.x[8] = 0x888; expected.x[9] = 0x999;
    expected.pc = 0x7001add040ull; expected.v[0] = 0x123456;
    check(!memcmp(&c, &expected, sizeof c));
    if (parent) check(LD32(parent + 0x34) == parent_count);
    check(hwaoi_pool_wait.recovered == (expected_calls > 1 ? 1u : 0u));
    VirtualFree(heap, 0, MEM_RELEASE);
    puts("pool backpressure checks passed");
}
