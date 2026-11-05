// Optional host-port backpressure for audited hash-locked resource submission sites.
// Preserve the game's allocator and queue. A failed allocation releases its
// internal lock before returning; retry only after yielding the virtual core.
// This changes timing, not pool capacity or task completion. Default off.
#pragma once
#include "hwaoi_observe_guard.h"

static struct {
    int on;
    uint64_t pc, base, recovered;
    GuestFn original;
} hwaoi_pool_wait;

static void hwaoi_pool_wait_init(const RtModule *mods, int count) {
    const char *e = getenv("HWAOI_POOL_BACKPRESSURE");
    if (!e || !*e || !strcmp(e, "0")) return;
    if (strcmp(e, "1")) rt_fatal("pool-backpressure: expected 0 or 1");
    const char *sha = NULL;
    for (int i = 0; i < rt_exefs_sha256_count; i++)
        if (!strcmp(rt_exefs_sha256[i].file, "main")) sha = rt_exefs_sha256[i].sha256;
    if (!sha || strcmp(sha, "4c33cfd4c818d9ed62822af792782b4ea443417ed24acea6f1cee739cde604c3"))
        rt_fatal("pool-backpressure: unsupported main hash");
    for (int i = 0; i < count; i++) {
        const RtModule *m = &mods[i];
        if (strcmp(m->name, "main")) continue;
        // Include the allocator and all audited saved returns; the farthest is
        // +1af5e64, where the caller resumes after the wrapped submission.
        if (m->size < 0x1af5e68 || m->size > RT_ADDRESS_LIMIT || m->base > RT_ADDRESS_LIMIT - m->size)
            rt_fatal("pool-backpressure: invalid main extent");
        hwaoi_pool_wait.pc = m->base + 0x1adce64;
        hwaoi_pool_wait.base = m->base;
        for (uint64_t j = 0; j < m->count; j++)
            if (m->addrs[j] == hwaoi_pool_wait.pc) hwaoi_pool_wait.original = m->fns[j];
    }
    if (!hwaoi_pool_wait.original) rt_fatal("pool-backpressure: original allocator missing");
    hwaoi_pool_wait.on = 1;
    rt_log("pool-backpressure: EXPERIMENTAL bounded retry at audited resource allocation sites; pool/queues unchanged");
}

static int hwaoi_pool_wait_heap(uint64_t a, uint64_t n) {
    RtRange h = mem_layout()->heap;
    if (!a || a >= RT_ADDRESS_LIMIT || n > RT_ADDRESS_LIMIT - a || a < h.base ||
        n > h.size || a - h.base > h.size - n) return 0;
    uint64_t end = a + n;
    while (a < end) {
        RtMemoryInfo info;
        if (mem_query(a, &info) || (info.perm & 3) != 3 || a < info.base ||
            a - info.base >= info.size || info.size > UINT64_MAX - info.base) return 0;
        uint64_t next = info.base + info.size;
        if (next <= a) return 0;
        a = next < end ? next : end;
    }
    return 1;
}

// main+8069c is a generic wrapper. Its allocator LR/size do not identify a
// producer: require the observed call chain and hash-locked stack-frame shape.
// The wrapper has not initialized or published the job at this point. Read
// through the platform's fault-safe copier; guest stack pointers are untrusted.
static int hwaoi_pool_wait_wrapped_request(Cpu *c) {
    const uint64_t sp = c->sp;
    if (!sp || (sp & 15) || sp > RT_ADDRESS_LIMIT - 0x590 ||
        c->x[29] != sp + 0x10 || c->x[19] != sp + 0x30 || c->x[2] != sp) return 0;
    const uint32_t offsets[4] = {0x10, 0x50, 0x510, 0x580};
    const uint32_t returns[4] = {0x14c3e24, 0x14dfdac, 0x1af5e64, 0x6c3980};
    uint64_t frames[4][2];
    int valid = 1;
    void *host_section = hwaoi_host_section_enter(c);
    for (unsigned i = 0; i < 4; i++) {
        if (!plat_mem_read(sp + offsets[i], frames[i], sizeof frames[i]) ||
            frames[i][1] != hwaoi_pool_wait.base + returns[i] ||
            (i < 3 && frames[i][0] != sp + offsets[i + 1])) {
            valid = 0;
            break;
        }
    }
    hwaoi_host_section_leave(host_section);
    return valid;
}

static int hwaoi_pool_wait_request(Cpu *c) {
    if (c->x[30] < hwaoi_pool_wait.base) return 0;
    uint64_t lr = c->x[30] - hwaoi_pool_wait.base;
    // hang-05: decompression-parent job. Its in-flight count is transferred
    // into the job after allocation, and publication has not started here.
    if (lr == 0x26454) return c->x[1] == 0x70;
    // hang-10: one/two resource children of the current executor job. Both
    // free-list locks are released; the parent's children+1 count stays live.
    // The first child is published before the second allocation. The original
    // helping loop begins only after both; retry remains bounded, not a promise
    // of progress when every executor is a blocked producer.
    if (lr == 0x1668b4 || lr == 0x1669e8) return c->x[1] == 0x58;
    // hang-17: main+14c3dc0 allocates one 0x38 child via the generic wrapper.
    // Parent counter exchange and publication occur after allocation returns.
    if (lr == 0x806dc) return c->x[1] == 0x38 && hwaoi_pool_wait_wrapped_request(c);
    if (c->x[1] != 0x50) return 0;
    // main+1ec0c0: each prior job is fully published before the next allocation.
    // All eleven unrolled/tail sites have the same 0x50-byte request. Evidence:
    // work/analysis/pool-submit-1ec0c0-audit.md; hang-02 observed LR+1eca5c fail.
    switch (lr) {
    case 0x289b0:
    case 0x1ec13c: case 0x1ec29c: case 0x1ec454: case 0x1ec5c8:
    case 0x1ec75c: case 0x1ec8dc: case 0x1eca5c: case 0x1ecbdc:
    case 0x1ecd7c: case 0x1ecf44: case 0x1ed0f8:
        return 1;
    default: return 0;
    }
}

static void hwaoi_pool_wait_call(Cpu *c) {
    if (!hwaoi_pool_wait_request(c))
        __attribute__((musttail)) return hwaoi_pool_wait.original(c);
    Cpu request = *c;
    hwaoi_pool_wait.original(c);
    if (c->x[0]) return;
    void *host_section = hwaoi_host_section_enter(c);
    uint64_t pool = request.x[0];
    if ((pool & 7) || !hwaoi_pool_wait_heap(pool, 0x820) || LD32(pool + 0x28) != 128 ||
        !hwaoi_pool_wait_heap(LD64(pool + 0x18), 0x1f4000))
        rt_fatal("pool-backpressure: unsupported resource pool layout");
    uint64_t start = kern_now_ns();
    unsigned retries = 0;
    for (;;) {
        uint32_t state = __atomic_load_n((uint32_t *)(uintptr_t)(pool + 0x20), __ATOMIC_ACQUIRE);
        uint32_t used = __atomic_load_n((uint32_t *)(uintptr_t)(pool + 0x2c), __ATOMIC_ACQUIRE);
        if ((state & (1u << 30)) || used < 1 || used > 16000)
            rt_fatal("pool-backpressure: invalid/closing pool state=%#x occupied=%u", state, used);
        if (kern_now_ns() - start >= 5000000000ull || retries == 20000)
            rt_fatal("pool-backpressure: resource pool made no allocatable space within 5 seconds (%u retries)", retries);
        int away = kern_core_away();
        plat_sleep_ns(250000);
        kern_core_back(&away);
        // The original allocator clobbers caller-saved registers even on failure.
        // Restore the entire incoming context before each independent retry.
        *c = request;
        hwaoi_host_section_leave(host_section);
        hwaoi_pool_wait.original(c);
        host_section = hwaoi_host_section_enter(c);
        retries++;
        if (c->x[0]) break;
    }
    uint64_t n = __atomic_add_fetch(&hwaoi_pool_wait.recovered, 1, __ATOMIC_RELAXED);
    if (n <= 8 || !(n & 255))
        rt_log("pool-backpressure: recovered %llu allocation(s), latest %u retries %.3f ms",
               (unsigned long long)n, retries, (kern_now_ns() - start) / 1e6);
    hwaoi_host_section_leave(host_section);
}
