#include "host.h"
#include "platform/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <windows.h>

static RtFileHash test_hashes[] = {{"main", "4c33cfd4c818d9ed62822af792782b4ea443417ed24acea6f1cee739cde604c3"}};
static int test_hash_count = 1, locked, log_count, failures, custom, fiber_case, active_lines, snapshots;
static uint64_t clock_ns;
uint64_t kern_now_ns(void) { clock_ns += 1000000; return clock_ns; }
static RtGuestLayout test_layout = {.heap = {0x2000000000ull, 0x10000}};
const RtGuestLayout *mem_layout(void) { return &test_layout; }
uint32_t mem_query(uint64_t a, RtMemoryInfo *out) {
    if (a >= 0x2000000000ull && a < 0x2000010000ull)
        *out = (RtMemoryInfo){.base = 0x2000000000ull, .size = 0x10000, .perm = 3};
    else if (a >= 0x7000000000ull && a < 0x7000010000ull)
        *out = (RtMemoryInfo){.base = 0x7000000000ull, .size = 0x10000, .perm = 1};
    else return 1;
    return 0;
}
int kern_host_track = 1;
void kern_host_left(void *t) { (void)t; }
#define rt_exefs_sha256 test_hashes
#define rt_exefs_sha256_count test_hash_count
void rt_fatal(const char *fmt, ...) { (void)fmt; exit(70); }
void rt_log(const char *fmt, ...) {
    char b[1024]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    log_count++; if (strstr(b, " FAILED")) failures++;
    if (strstr(b, "pool-diag: active id=")) active_lines++;
    if (strstr(b, "executor snapshot active=2")) snapshots++;
}
void plat_mutex_lock(PlatMutex *m) { (void)m; if (locked++) abort(); }
void plat_mutex_unlock(PlatMutex *m) { (void)m; if (--locked) abort(); }
uint32_t plat_thread_id(void) { return 7; }
static GuestFn target;
static void parent_job(Cpu *c), child_job(Cpu *c), tail_helper(Cpu *c), null_alloc(Cpu *c);
GuestFn rt_dispatch_lookup(uint64_t pc) {
    if (custom) {
        if (pc == 0x7000000100ull) return parent_job;
        if (pc == 0x7000000200ull) return child_job;
        if (pc == 0x7000000300ull) return tail_helper;
        if (pc == 0x7001adce64ull) return null_alloc;
    }
    return target;
}
#include "hwaoi_pool_diag.h"

static uint64_t result;
static unsigned invokes;
static void check(int p) { if (!p) abort(); }
static unsigned active_count(void) {
    unsigned n = 0;
    for (unsigned i = 0; i < 512; i++) n += hwaoi_pool_diag.active[i].id != 0;
    return n;
}
static void null_alloc(Cpu *c) { check(!locked && !*kern_host_depth(c->thread)); c->x[0] = 0; }
static void tail_helper(Cpu *c) {
    check(active_count() == 1 && !locked && !*kern_host_depth(c->thread));
    c->x[0] = 0x333;
}
static void child_job(Cpu *c) {
    check(!locked && !*kern_host_depth(c->thread) && active_count() == 2);
    const HwaoiActiveJob *child = NULL, *parent = NULL;
    for (unsigned i = 0; i < 512; i++) {
        const HwaoiActiveJob *p = &hwaoi_pool_diag.active[i];
        if (p->id && p->pc == 0x7000000200ull) child = p;
        if (p->id && p->pc == 0x7000000100ull) parent = p;
    }
    check(child && parent && child->priority == 8 && parent->priority == 7);
    check(child->parent_job == parent->job);
    check(child->depth == (fiber_case ? 0u : 1u));
    check(child->parent_id == (fiber_case ? 0 : parent->id));
    Cpu saved = *c;
    c->pc = 0x7001adce64ull; c->x[0] = 0x2000004000ull; c->x[1] = 0x50; c->x[30] = 0x123;
    hwaoi_pool_diag_call(c); // Failure snapshot must retain both nested jobs.
    check(c->x[0] == 0 && snapshots == 1 && active_lines == 2);
    *c = saved;
    c->x[0] = 0x222;
}
static void parent_job(Cpu *c) {
    check(!locked && !*kern_host_depth(c->thread) && active_count() == 1);
    Cpu saved = *c;
    c->pc = 0x7000000300ull; // Same LR and object, but not the vtable target.
    hwaoi_pool_diag_call(c);
    check(active_count() == 1 && hwaoi_pool_diag.ntypes == 1 && c->x[0] == 0x333);
    *c = saved;
    Cpu other = *c, *nested = fiber_case ? &other : c;
    nested->pc = 0x7000000200ull; nested->x[0] = 0x2000002000ull;
    hwaoi_pool_diag_call(nested);
    check(nested->x[0] == 0x222 && active_count() == 1);
    *c = saved;
    c->x[0] = 0x111;
}
static void original(Cpu *c) {
    if (locked) abort();
    if (c->thread && *kern_host_depth(c->thread)) abort();
    invokes++;
    c->x[0] = result; c->x[8] = 0x1234;
    c->pc = 0x777; // Nested calls may change pc; observer must preserve it.
}
int main(int argc, char **argv) {
    check(argc == 2);
    RtModule m = {.name = "main", .base = 0x7000000000ull, .size = 0x10000};
    if (!strcmp(argv[1], "disabled")) {
        _putenv_s("HWAOI_POOL_DIAG", "0");
        hwaoi_pool_diag_init(&m, 1);
        check(!hwaoi_pool_diag.on && !log_count);
        puts("check passed"); return 0;
    }
    _putenv_s("HWAOI_POOL_DIAG", "1");
    if (!strcmp(argv[1], "hash")) test_hashes[0].sha256 = "wrong";
    if (!strcmp(argv[1], "module")) m.name = "sdk";
    hwaoi_pool_diag_init(&m, 1);
    check(hwaoi_pool_diag.on);
    check(hwaoi_pool_diag_watches(m.base + 0x28708));
    check(!hwaoi_pool_diag_watches(m.base + 0x2870c));
    Cpu not_job = {.pc = 0x999};
    check(!hwaoi_pool_diag_watches_call(&not_job));
    not_job.x[30] = hwaoi_pool_diag.executor_lr;
    check(hwaoi_pool_diag_watches_call(&not_job));
    if (!strcmp(argv[1], "nested") || !strcmp(argv[1], "fibers")) {
        custom = 1; fiber_case = !strcmp(argv[1], "fibers");
        void *heap = VirtualAlloc((void *)0x2000000000ull, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        void *module = VirtualAlloc((void *)0x7000000000ull, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        check(heap && module);
        ST64(0x2000001000ull, 0x7000001000ull); ST64(0x7000001010ull, 0x7000000100ull);
        ST64(0x2000002000ull, 0x7000002000ull); ST64(0x7000002010ull, 0x7000000200ull);
        ST64(0x2000002010ull, 0x2000001000ull);
        ST64(0x2000001018ull, 0x2000004000ull); ST64(0x2000002018ull, 0x2000004000ull);
        ST32(0x2000001020ull, 7); ST32(0x2000002020ull, 8);
        ST32(0x2000004028ull, 128); ST32(0x200000402cull, 16000);
        memset((void *)0x2000004030ull, 0xff, 2000);
        int thread[2] = {0}; Cpu c = {.pc = 0x7000000100ull, .thread = thread};
        c.x[0] = 0x2000001000ull; c.x[30] = hwaoi_pool_diag.executor_lr; c.x[19] = 0x999; c.sp = 0xaaa;
        Cpu expected = c; expected.x[0] = 0x111;
        hwaoi_pool_diag_call(&c);
        check(!memcmp(&c, &expected, sizeof c) && !active_count() && !thread[1]);
        check(hwaoi_pool_diag.ntypes == 2 && hwaoi_pool_diag.next_id == 2 && !hwaoi_pool_diag.jobs_untracked);
        for (unsigned i = 0; i < 2; i++)
            check(hwaoi_pool_diag.types[i].started == 1 && hwaoi_pool_diag.types[i].completed == 1 &&
                  hwaoi_pool_diag.types[i].total_ns > 0 && hwaoi_pool_diag.types[i].max_ns > 0);
        check(hwaoi_pool_diag.rows[0].job_start == 0 && hwaoi_pool_diag.rows[0].job_done == 0);
        VirtualFree(heap, 0, MEM_RELEASE); VirtualFree(module, 0, MEM_RELEASE);
        puts("check passed"); return 0;
    }
    if (!strcmp(argv[1], "capacity")) {
        for (unsigned i = 0; i < 512; i++) hwaoi_pool_diag.active[i].id = i + 1;
        HwaoiActiveJob a = {.pc = 0x123, .cpu = &not_job};
        check(hwaoi_pool_diag_job_enter(a) == -1 && hwaoi_pool_diag.jobs_untracked == 1);
        memset(hwaoi_pool_diag.active, 0, sizeof hwaoi_pool_diag.active);
        hwaoi_pool_diag.ntypes = 512;
        check(hwaoi_pool_diag_job_enter(a) == -1 && hwaoi_pool_diag.jobs_untracked == 2);
        puts("check passed"); return 0;
    }
    uint64_t pool_words[0x820 / 8] = {0}, job_words[10] = {0};
    uint64_t pool = (uint64_t)(uintptr_t)pool_words;
    ((uint32_t *)pool_words)[0x28 / 4] = 128;
    ((uint32_t *)pool_words)[0x2c / 4] = 16000;
    for (unsigned i = 0; i < 500; i++) ((uint32_t *)pool_words)[0x30 / 4 + i] = 0xffffffffu;
    uint8_t before[sizeof pool_words]; memcpy(before, pool_words, sizeof before);
    job_words[3] = pool;
    target = original;
    const uint64_t pcs[] = {hwaoi_pool_diag.alloc_pc, hwaoi_pool_diag.free_pc,
                            hwaoi_pool_diag.job_pc, hwaoi_pool_diag.alloc_pc};
    for (unsigned i = 0; i < 4; i++) {
        int thread[2] = {0};
        Cpu c = {0}; c.pc = pcs[i]; c.x[0] = i == 2 ? (uint64_t)(uintptr_t)job_words : pool;
        c.thread = thread;
        c.x[1] = 0x50; c.x[30] = 0x99; c.sp = 0x123456; c.x[19] = 0xfedc;
        result = i == 3 ? 0 : 0xabcdef;
        Cpu expected = c; original(&expected); invokes--;
        hwaoi_pool_diag_call(&c);
        check(!memcmp(&c, &expected, sizeof c));
        check(thread[1] == 0);
        check(!memcmp(pool_words, before, sizeof before));
    }
    check(invokes == 4 && failures == 1 && !locked && hwaoi_pool_diag.n == 1);
    HwaoiPoolRow *r = &hwaoi_pool_diag.rows[0];
    check(r->alloc_ok == 1 && r->alloc_null == 1 && r->frees == 1 && r->job_start == 1 && r->job_done == 1);
    puts("check passed"); return 0;
}
