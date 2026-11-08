// Optional observations of the hash-locked HWAoI resource-job pool. No guest
// values or results are changed. Direct calls bypass this diagnostic; counters
// describe only calls observed through rt_dispatch, not a complete allocation log.
#pragma once
#include "hwaoi_observe_guard.h"

typedef struct {
    uint64_t pool, alloc_ok, alloc_null, frees, job_start, job_done;
} HwaoiPoolRow;
typedef struct {
    uint64_t pc, started, completed, total_ns, max_ns;
} HwaoiJobType;
typedef struct {
    uint64_t id, parent_id, job, pc, vtable, parent_job, pool, since;
    Cpu *cpu;
    uint32_t tid, priority, depth, type;
} HwaoiActiveJob;
static struct {
    int on;
    uint64_t alloc_pc, free_pc, job_pc, executor_lr, factory_lr[6], heap_getter, heap_getter_lr, next_id, jobs_untracked;
    const RtModule *mods;
    int nmods;
    HwaoiPoolRow rows[256];
    HwaoiJobType types[512];
    HwaoiActiveJob active[512];
    unsigned ntypes;
    unsigned n;
    PlatMutex lock;
} hwaoi_pool_diag = {.lock = PLAT_MUTEX_INIT};

static void hwaoi_pool_diag_init(const RtModule *mods, int count) {
    const char *e = getenv("HWAOI_POOL_DIAG");
    if (!e || strcmp(e, "1")) return;
    const char *sha = NULL;
    for (int i = 0; i < rt_exefs_sha256_count; i++)
        if (!strcmp(rt_exefs_sha256[i].file, "main")) sha = rt_exefs_sha256[i].sha256;
    if (!sha || strcmp(sha, "4c33cfd4c818d9ed62822af792782b4ea443417ed24acea6f1cee739cde604c3"))
        rt_fatal("pool-diag: unsupported main hash");
    for (int i = 0; i < count; i++) {
        if (strcmp(mods[i].name, "main")) continue;
        hwaoi_pool_diag.alloc_pc = mods[i].base + 0x1adce64;
        hwaoi_pool_diag.free_pc = mods[i].base + 0x1adcfa8;
        hwaoi_pool_diag.job_pc = mods[i].base + 0x28708;
        hwaoi_pool_diag.executor_lr = mods[i].base + 0x7fc8c;
        // main+27318 calls a resource factory through vtable+0xb8. A null
        // result reaches the deliberate "can't create resource" abort at 275ac.
        const uint64_t factory_returns[] = {0x2731c, 0x703c98, 0x7041d0, 0x708224, 0x708290, 0x6fdfac};
        for (unsigned n = 0; n < 6; n++) hwaoi_pool_diag.factory_lr[n] = mods[i].base + factory_returns[n];
        hwaoi_pool_diag.heap_getter = mods[i].base + 0x1c878;
        hwaoi_pool_diag.heap_getter_lr = mods[i].base + 0x19d9850;
        hwaoi_pool_diag.mods = mods;
        hwaoi_pool_diag.nmods = count;
        hwaoi_pool_diag.on = 1;
        rt_log("pool-diag: observing indirect resource pool allocation/free, inflate jobs and executor job types; "
               "direct calls are not counted, guest behavior unchanged");
        return;
    }
    rt_fatal("pool-diag: linked main module missing");
}

static int hwaoi_pool_diag_watches(uint64_t pc) {
    return pc == hwaoi_pool_diag.alloc_pc || pc == hwaoi_pool_diag.free_pc || pc == hwaoi_pool_diag.job_pc;
}
static int hwaoi_pool_diag_factory_call(const Cpu *c) {
    for (unsigned n = 0; n < 6; n++) if (c->x[30] == hwaoi_pool_diag.factory_lr[n]) return 1;
    return 0;
}
static int hwaoi_pool_diag_watches_call(const Cpu *c) {
    return hwaoi_pool_diag_watches(c->pc) || c->x[30] == hwaoi_pool_diag.executor_lr ||
           hwaoi_pool_diag_factory_call(c) ||
           (c->pc == hwaoi_pool_diag.heap_getter && c->x[30] == hwaoi_pool_diag.heap_getter_lr);
}

static int hwaoi_pool_diag_readable(uint64_t a, uint64_t n, int module) {
    if (!a || a >= RT_ADDRESS_LIMIT || n > RT_ADDRESS_LIMIT - a) return 0;
    int known = 0;
    if (module) {
        for (int i = 0; i < hwaoi_pool_diag.nmods; i++) {
            const RtModule *m = &hwaoi_pool_diag.mods[i];
            if (a >= m->base && n <= m->size && a - m->base <= m->size - n) known = 1;
        }
    } else {
        RtRange h = mem_layout()->heap;
        known = a >= h.base && n <= h.size && a - h.base <= h.size - n;
    }
    if (!known) return 0;
    uint64_t end = a + n;
    while (a < end) {
        RtMemoryInfo info;
        if (mem_query(a, &info) || !(info.perm & 1) || a < info.base || a - info.base >= info.size ||
            info.size > UINT64_MAX - info.base) return 0;
        uint64_t next = info.base + info.size;
        if (next <= a) return 0;
        a = next < end ? next : end;
    }
    return 1;
}

// Validate the actual vtable dispatch. A tail helper can retain the executor's
// LR while changing x0; LR alone is not enough to interpret x0 as a job object.
static int hwaoi_pool_diag_job(Cpu *c, HwaoiActiveJob *out) {
    if (c->x[30] != hwaoi_pool_diag.executor_lr || !hwaoi_pool_diag_readable(c->x[0], 0x38, 0)) return 0;
    uint64_t vtable = LD64(c->x[0]);
    if (!hwaoi_pool_diag_readable(vtable, 0x18, 1) || LD64(vtable + 0x10) != c->pc) return 0;
    *out = (HwaoiActiveJob){.job = c->x[0], .pc = c->pc, .vtable = vtable,
        .parent_job = LD64(c->x[0] + 0x10), .pool = LD64(c->x[0] + 0x18),
        .priority = LD32(c->x[0] + 0x20), .cpu = c, .tid = plat_thread_id(), .since = kern_now_ns()};
    return 1;
}

// Holding the diagnostic lock. Active slots are stable until the corresponding
// wrapper returns. Cpu identity distinguishes guest fibers on one host thread.
static int hwaoi_pool_diag_job_enter(HwaoiActiveJob value) {
    int slot = -1;
    for (unsigned i = 0; i < 512; i++) {
        HwaoiActiveJob *p = &hwaoi_pool_diag.active[i];
        if (!p->id) { if (slot < 0) slot = (int)i; continue; }
        if (p->cpu != value.cpu) continue;
        if (p->job == value.job && p->pc == value.pc) return -1; // same-frame tail recursion
        if (p->depth >= value.depth) { value.depth = p->depth + 1; value.parent_id = p->id; }
    }
    if (slot < 0) { hwaoi_pool_diag.jobs_untracked++; return -1; }
    unsigned type;
    for (type = 0; type < hwaoi_pool_diag.ntypes; type++) if (hwaoi_pool_diag.types[type].pc == value.pc) break;
    if (type == 512) { hwaoi_pool_diag.jobs_untracked++; return -1; }
    if (type == hwaoi_pool_diag.ntypes) {
        hwaoi_pool_diag.types[type].pc = value.pc;
        hwaoi_pool_diag.ntypes++;
    }
    value.id = ++hwaoi_pool_diag.next_id;
    value.type = type;
    hwaoi_pool_diag.active[slot] = value;
    hwaoi_pool_diag.types[type].started++;
    return slot;
}
static void hwaoi_pool_diag_job_leave(int slot) {
    if (slot < 0) return;
    HwaoiActiveJob *p = &hwaoi_pool_diag.active[slot];
    uint64_t elapsed = kern_now_ns() - p->since;
    HwaoiJobType *t = &hwaoi_pool_diag.types[p->type];
    t->completed++; t->total_ns += elapsed;
    if (elapsed > t->max_ns) t->max_ns = elapsed;
    p->id = 0;
}
static void hwaoi_pool_diag_dump_jobs(void) {
    uint64_t now = kern_now_ns();
    unsigned active = 0, shown = 0;
    for (unsigned i = 0; i < 512; i++) active += hwaoi_pool_diag.active[i].id != 0;
    rt_log("pool-diag: executor snapshot active=%u types=%u untracked=%llu (inclusive wall times)",
           active, hwaoi_pool_diag.ntypes, (unsigned long long)hwaoi_pool_diag.jobs_untracked);
    for (unsigned i = 0; i < 512 && shown < 128; i++) {
        const HwaoiActiveJob *p = &hwaoi_pool_diag.active[i];
        if (!p->id) continue;
        shown++;
        rt_log("pool-diag: active id=%llu parent_id=%llu tid=%lu cpu=%p depth=%u job=%#llx pc=%#llx "
               "vtable=%#llx priority=%u parent_job=%#llx pool=%#llx elapsed=%.3f ms",
               (unsigned long long)p->id, (unsigned long long)p->parent_id, (unsigned long)p->tid, (void *)p->cpu,
               p->depth, (unsigned long long)p->job, (unsigned long long)p->pc, (unsigned long long)p->vtable,
               p->priority, (unsigned long long)p->parent_job, (unsigned long long)p->pool, (now - p->since) / 1e6);
    }
    if (active > shown) rt_log("pool-diag: %u additional active jobs omitted", active - shown);
    for (unsigned i = 0; i < hwaoi_pool_diag.ntypes && i < 128; i++) {
        const HwaoiJobType *t = &hwaoi_pool_diag.types[i];
        rt_log("pool-diag: type pc=%#llx started=%llu completed=%llu active=%llu total=%.3f ms max=%.3f ms",
               (unsigned long long)t->pc, (unsigned long long)t->started, (unsigned long long)t->completed,
               (unsigned long long)(t->started - t->completed), t->total_ns / 1e6, t->max_ns / 1e6);
    }
    if (hwaoi_pool_diag.ntypes > 128) rt_log("pool-diag: %u additional job types omitted", hwaoi_pool_diag.ntypes - 128);
}

// Caller holds the diagnostic lock; pool identities never move or get reused
// within this table, so concurrent snapshots cannot invalidate a row pointer.
static HwaoiPoolRow *hwaoi_pool_diag_row(uint64_t pool) {
    for (unsigned i = 0; i < hwaoi_pool_diag.n; i++)
        if (hwaoi_pool_diag.rows[i].pool == pool) return &hwaoi_pool_diag.rows[i];
    if (hwaoi_pool_diag.n == 256) rt_fatal("pool-diag: diagnostic pool table full");
    HwaoiPoolRow *r = &hwaoi_pool_diag.rows[hwaoi_pool_diag.n++];
    r->pool = pool;
    return r;
}

// Failure-only snapshots at the exact-build resource factory call. Reading the
// descriptors before the call retains stack-backed input even if the factory
// rewrites it. No retries, allocator substitutions or guest writes.
static void hwaoi_resource_factory_diag(Cpu *c) {
    GuestFn fn = rt_dispatch_lookup(c->pc);
    if (!fn) rt_fatal("resource-factory: original function missing");
    uint64_t pc = c->pc, lr = c->x[30], args[4], bytes[4][12] = {{0}};
    int readable[4];
    void *host_section = hwaoi_host_section_enter(c);
    for (unsigned i = 0; i < 4; i++) {
        args[i] = c->x[i];
        readable[i] = args[i] && args[i] <= UINT64_MAX - sizeof bytes[i] &&
                      plat_mem_read(args[i], bytes[i], sizeof bytes[i]);
    }
    hwaoi_host_section_leave(host_section);
    fn(c);
    if (c->x[0]) return;
    host_section = hwaoi_host_section_enter(c);
    static unsigned failures;
    unsigned sequence = __atomic_add_fetch(&failures, 1, __ATOMIC_RELAXED);
    if (sequence <= 16) {
        rt_log("resource-factory: FAILED tid=%u sequence=%u pc=%#llx lr=%#llx args=%#llx,%#llx,%#llx,%#llx",
               plat_thread_id(), sequence, (unsigned long long)pc, (unsigned long long)lr, (unsigned long long)args[0],
               (unsigned long long)args[1], (unsigned long long)args[2], (unsigned long long)args[3]);
        for (unsigned i = 0; i < 4; i++) {
            if (!readable[i]) continue;
            for (unsigned at = 0; at < 12; at += 4)
                rt_log("resource-factory: arg%u+%#x %#llx %#llx %#llx %#llx", i, at * 8,
                       (unsigned long long)bytes[i][at], (unsigned long long)bytes[i][at + 1],
                       (unsigned long long)bytes[i][at + 2], (unsigned long long)bytes[i][at + 3]);
        }
        // The observed GPU-pool backing allocator is a module-resident
        // synchronized wrapper. Follow only this exact-build wrapper type;
        // the first different parent is recorded, never interpreted or changed.
        if (hwaoi_pool_diag.executor_lr > 0x7fc8c && readable[0]) {
            uint64_t base = hwaoi_pool_diag.executor_lr - 0x7fc8c;
            uint64_t parent = bytes[0][2], table = bytes[0][0];
            for (unsigned depth = 0; depth < 3 && table == base + 0x27ec4c8; depth++) {
                uint64_t state[24];
                if (!parent || parent > UINT64_MAX - sizeof state || !plat_mem_read(parent, state, sizeof state)) break;
                rt_log("resource-factory: heap-parent depth=%u address=%#llx", depth, (unsigned long long)parent);
                for (unsigned at = 0; at < 24; at += 4)
                    rt_log("resource-factory: parent+%#x %#llx %#llx %#llx %#llx", at * 8,
                           (unsigned long long)state[at], (unsigned long long)state[at + 1],
                           (unsigned long long)state[at + 2], (unsigned long long)state[at + 3]);
                table = state[0]; parent = state[2];
            }
        }
    }
    hwaoi_host_section_leave(host_section);
}

static void hwaoi_resource_heap_diag(Cpu *c) {
    GuestFn fn = rt_dispatch_lookup(c->pc);
    if (!fn) rt_fatal("resource-heap: original getter missing");
    fn(c);
    if (!c->x[0]) return;
    void *host_section = hwaoi_host_section_enter(c);
    static uint64_t seen[32];
    uint64_t heap = c->x[0], words[24];
    int show = 0;
    plat_mutex_lock(&hwaoi_pool_diag.lock);
    for (unsigned i = 0; i < 32; i++) {
        if (seen[i] == heap) break;
        if (!seen[i]) { seen[i] = heap; show = 1; break; }
    }
    plat_mutex_unlock(&hwaoi_pool_diag.lock);
    if (show && heap <= UINT64_MAX - sizeof words && plat_mem_read(heap, words, sizeof words)) {
        rt_log("resource-heap: selected heap=%#llx", (unsigned long long)heap);
        for (unsigned n = 0; n < 24; n += 4)
            rt_log("resource-heap: +%#x %#llx %#llx %#llx %#llx", n * 8, (unsigned long long)words[n],
                   (unsigned long long)words[n + 1], (unsigned long long)words[n + 2], (unsigned long long)words[n + 3]);
    }
    hwaoi_host_section_leave(host_section);
}

static void hwaoi_pool_diag_call(Cpu *c) {
    void *host_section = hwaoi_host_section_enter(c);
    GuestFn fn = rt_dispatch_lookup(c->pc);
    if (!fn) rt_fatal("pool-diag: original function missing");
    if (c->pc == hwaoi_pool_diag.heap_getter && c->x[30] == hwaoi_pool_diag.heap_getter_lr) {
        hwaoi_host_section_leave(host_section);
        __attribute__((musttail)) return hwaoi_resource_heap_diag(c);
    }
    if (hwaoi_pool_diag_factory_call(c)) {
        hwaoi_host_section_leave(host_section);
        __attribute__((musttail)) return hwaoi_resource_factory_diag(c);
    }
    uint64_t pc = c->pc, lr = c->x[30], arg = c->x[1];
    int job = pc == hwaoi_pool_diag.job_pc, alloc = pc == hwaoi_pool_diag.alloc_pc;
    int known = job || alloc || pc == hwaoi_pool_diag.free_pc;
    HwaoiActiveJob active;
    int generic = hwaoi_pool_diag_job(c, &active);
    uint64_t pool = job ? LD64(c->x[0] + 0x18) : known ? c->x[0] : 0;
    plat_mutex_lock(&hwaoi_pool_diag.lock);
    int active_slot = generic ? hwaoi_pool_diag_job_enter(active) : -1;
    HwaoiPoolRow *r = known ? hwaoi_pool_diag_row(pool) : NULL;
    if (job) r->job_start++;
    plat_mutex_unlock(&hwaoi_pool_diag.lock);
    hwaoi_host_section_leave(host_section);
    if (!known && active_slot < 0) __attribute__((musttail)) return fn(c);
    fn(c);
    host_section = hwaoi_host_section_enter(c);
    if (!known) {
        plat_mutex_lock(&hwaoi_pool_diag.lock);
        hwaoi_pool_diag_job_leave(active_slot);
        plat_mutex_unlock(&hwaoi_pool_diag.lock);
        hwaoi_host_section_leave(host_section);
        return;
    }
    // Atomic reads are observations of guest state, not an atomic snapshot of
    // the allocator; other workers may change the pool while these are read.
    uint32_t slot = __atomic_load_n((uint32_t *)(uintptr_t)(pool + 0x28), __ATOMIC_RELAXED);
    uint32_t used = __atomic_load_n((uint32_t *)(uintptr_t)(pool + 0x2c), __ATOMIC_RELAXED);
    plat_mutex_lock(&hwaoi_pool_diag.lock);
    hwaoi_pool_diag_job_leave(active_slot);
    if (job) r->job_done++;
    else if (alloc) {
        if (c->x[0]) r->alloc_ok++;
        else r->alloc_null++;
    } else r->frees++;
    uint64_t calls = r->alloc_ok + r->alloc_null + r->frees + r->job_done;
    int failed = alloc && !c->x[0];
    if (failed) hwaoi_pool_diag_dump_jobs();
    if (failed || (alloc && r->alloc_ok == 1) || !(calls & 1023)) {
        unsigned bits = 0, words = slot ? ((0x1f4000u / slot) + 31) / 32 : 0;
        if (failed && words <= 500)
            for (unsigned i = 0; i < words; i++)
                bits += (unsigned)__builtin_popcount(__atomic_load_n(
                    (uint32_t *)(uintptr_t)(pool + 0x30 + i * 4), __ATOMIC_RELAXED));
        rt_log("pool-diag: pool=%#llx tid=%lu op=%s lr=%#llx arg=%#llx result=%#llx "
               "slot=%u used=%u alloc=%llu null=%llu free=%llu jobs=%llu/%llu bitmap=%u/%u%s",
               (unsigned long long)pool, (unsigned long)plat_thread_id(), job ? "job" : alloc ? "alloc" : "free",
               (unsigned long long)lr, (unsigned long long)arg, (unsigned long long)c->x[0], slot, used,
               (unsigned long long)r->alloc_ok, (unsigned long long)r->alloc_null, (unsigned long long)r->frees,
               (unsigned long long)r->job_done, (unsigned long long)r->job_start, bits,
               failed && words <= 500 ? words * 32 : 0, failed ? " FAILED" : "");
    }
    plat_mutex_unlock(&hwaoi_pool_diag.lock);
    hwaoi_host_section_leave(host_section);
}
