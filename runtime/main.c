// totk.exe entry: reserves the guest address space, parses the command line and runs one mode.
//
//   totk.exe [--exefs DIR] [--romfs DIR] [--profile DIR] <mode>
//     --selftest        layout, dispatch table and host-function registry; needs no game files
//     --probe-loader    load and relocate the modules, check them against the measured 1.2.1 values
//     --probe-kernel, --probe-ipc, --probe-ipc-lifetime, --probe-services, --probe-fs    host probes
//     --probe-dpi       the process DPI awareness (per-monitor v2) and the primary monitor's size; no game files
//     --probe-gfx-abi   NVN object layouts against nvn.h's reserved sizes; needs no game files
//     --probe-nvn, --probe-nvn-tex, --probe-window, --probe-shader, --probe-vkmem, --probe-vkexec    graphics probes
//     --probe-astc      the compute ASTC decoder against astc_decode_ldr; needs no game files
//     --probe-movie     host VP9 movie::Decoder on one RomFS movie; skips without game data
//     --boot            run the game to the first NVN call
//     --seconds S       run the game (implies --boot's start-up) until the world clock (game time since the loaded
//                       save's first frame) passed S seconds, then exit 0 at that present
//     --shot PNG        with --seconds: save that frame; env TOTK_GPU=null|vulkan selects the back end
//     (no mode)         run the game
//
// Also here: the runtime services the generated code calls besides rt_dispatch and rt_svc (rt_brk, rt_udf,
// rt_ticks, the function profiler hook), logging, SHA-256, and the crash reporter (a vectored exception handler that
// names the lifted function, the guest registers and the guest frame-pointer chain).
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "sha256.h"

#define MAX_MODULES 16

// ---- logging, tracing, hashing
unsigned rt_trace;
int g_hwaoi_render_probe;

// Log file. A game run whose stderr is not a file already (started from Explorer, a shortcut or a console) also
// writes its log to logs\totk.log next to totk.exe, so a player has one to attach to a report; run_game.py and
// `2> file` keep their own file. Each start moves the earlier logs to totk.1.log .. totk.4.log. A file stops at 64 MiB
// (TOTK_TRACE can write that much). Lines from before log_open wait in a static buffer: mem_init runs first so that
// nothing else lands in the guest address space, and an earlier open would put the CRT's heap there.
#define LOG_KEEP 5
#define LOG_CAP (64ull << 20)
enum { LOG_EARLY, LOG_FILE, LOG_OFF };
static int g_log_state;
static PlatAppend *g_log_file;
static char g_log_path[PLAT_PATH_MAX * 3];
static char g_log_early[32768];
static size_t g_log_early_n;
static uint64_t g_log_bytes;

void rt_log_raw(const char *p, size_t n) {
    fwrite(p, 1, n, stderr);
    int state = __atomic_load_n(&g_log_state, __ATOMIC_ACQUIRE);
    if (state == LOG_EARLY) {
        size_t at = __atomic_fetch_add(&g_log_early_n, n, __ATOMIC_RELAXED);
        if (at + n <= sizeof g_log_early) memcpy(g_log_early + at, p, n);
    } else if (state == LOG_FILE) {
        uint64_t at = __atomic_fetch_add(&g_log_bytes, n, __ATOMIC_RELAXED);
        if (at + n <= LOG_CAP) {
            plat_append_write(g_log_file, p, n);
        } else if (at <= LOG_CAP) {   // the first write past the cap says so, once
            static const char cut[] = "[host] log: the file reached 64 MiB, the rest of this run goes to stderr only\n";
            plat_append_write(g_log_file, cut, sizeof cut - 1);
        }
    }
}

void rt_vlogf(const char *fmt, va_list ap) {
    char buf[2048];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n > 0) rt_log_raw(buf, n < (int)sizeof buf ? (size_t)n : sizeof buf - 1);
}

void rt_logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    rt_vlogf(fmt, ap);
    va_end(ap);
}

void rt_log(const char *fmt, ...) {
    char buf[2048];
    int n = snprintf(buf, sizeof buf, "[host] ");
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(buf + n, sizeof buf - n - 1, fmt, ap);
    va_end(ap);
    n = k < 0 ? n : n + k < (int)sizeof buf - 1 ? n + k : (int)sizeof buf - 2;
    buf[n++] = '\n';
    rt_log_raw(buf, n);
}

const char *rt_log_path(void) {
    return __atomic_load_n(&g_log_state, __ATOMIC_ACQUIRE) == LOG_FILE ? g_log_path : NULL;
}

// Opens the log file for a game run (game = 1) and writes the buffered lines to it; anything else drops them. Runs on
// the main thread before the boot starts other threads.
static void log_open(int game) {
    char dir[PLAT_PATH_MAX * 3], from[PLAT_PATH_MAX * 3 + 16], to[PLAT_PATH_MAX * 3 + 16];
    char *cut = NULL;
    if (game && !plat_stderr_path(dir, sizeof dir) && plat_exe_path(dir, sizeof dir)) {
        cut = plat_last_sep(dir);
        char *f = strrchr(dir, '/');
        if (f > cut) cut = f;
    }
    if (cut && (size_t)(cut - dir) + sizeof "/logs" <= sizeof dir) {
        strcpy(cut, "/logs");
        plat_file_mkdir_p(dir);
        for (int i = LOG_KEEP - 1; i > 0; i--) {   // totk.3.log -> totk.4.log, ..., totk.log -> totk.1.log
            if (i > 1) snprintf(from, sizeof from, "%s/totk.%d.log", dir, i - 1);
            else snprintf(from, sizeof from, "%s/totk.log", dir);
            snprintf(to, sizeof to, "%s/totk.%d.log", dir, i);
            if (plat_file_exists(from)) plat_file_replace(from, to);
        }
        snprintf(g_log_path, sizeof g_log_path, "%s/totk.log", dir);
        g_log_file = plat_append_open(g_log_path);
    }
    if (!g_log_file) {
        __atomic_store_n(&g_log_state, LOG_OFF, __ATOMIC_RELEASE);
        return;
    }
    size_t n = __atomic_load_n(&g_log_early_n, __ATOMIC_RELAXED);
    if (n > sizeof g_log_early) n = sizeof g_log_early;
    plat_append_write(g_log_file, g_log_early, n);
    g_log_bytes = n;
    __atomic_store_n(&g_log_state, LOG_FILE, __ATOMIC_RELEASE);
    rt_log("log: %s", g_log_path);
}

static void parse_trace(void) {
    const char *e = cfg_get("TOTK_TRACE");   // no default: nothing is traced when unset
    if (!e) return;
    static const struct { const char *name; unsigned bit; } names[] = {
        {"svc", RT_TRACE_SVC}, {"host", RT_TRACE_HOSTCALL}, {"ipc", RT_TRACE_IPC}, {"fs", RT_TRACE_FS},
        {"gfx", RT_TRACE_GFX}, {"perf", RT_TRACE_PERF}, {"sched", RT_TRACE_SCHED}, {"audio", RT_TRACE_AUDIO},
        {"present", RT_TRACE_PRESENT}, {"codec", RT_TRACE_CODEC}, {"all", ~0u}};
    for (const char *p = e; *p;) {
        size_t n = strcspn(p, ",");
        int known = 0;
        for (unsigned i = 0; i < sizeof names / sizeof *names; i++)
            if (strlen(names[i].name) == n && !strncmp(p, names[i].name, n)) rt_trace |= names[i].bit, known = 1;
        if (!known && n) rt_log("TOTK_TRACE: unknown item '%.*s' (svc, host, ipc, fs, gfx, perf, sched, audio, present, codec, all)", (int)n, p);
        p += n + (p[n] == ',');
    }
}

void rt_sha256(const void *p, size_t n, uint8_t out[32]) { sha256(p, n, out); }

// ---- configuration
static RtConfig cfg;
const RtConfig *rt_config(void) { return &cfg; }

// ---- runtime services the generated code calls (cpu.h)
volatile int g_fnprof = 0;
void fnprof_hit(uint64_t addr, uint64_t lr, uint64_t x0) { (void)addr, (void)lr, (void)x0; }

// Guest assertions also reach UDF/BRK deliberately; retain their caller chain
// before exiting, just as the native exception reporter does for a host fault.
static void guest_abort_diagnostic(Cpu *c, uint64_t pc) {
    if (!c) return;
    for (int i = 0; i < 28; i += 4)
        rt_logf("guest abort tid %u: x%d=%#llx x%d=%#llx x%d=%#llx x%d=%#llx\n", plat_thread_id(),
                i, (unsigned long long)c->x[i], i + 1, (unsigned long long)c->x[i + 1],
                i + 2, (unsigned long long)c->x[i + 2], i + 3, (unsigned long long)c->x[i + 3]);
    char at[160];
    uint64_t fp = c->x[29];
    for (unsigned n = 0; n < 24 && fp && !(fp & 15); n++) {
        uint64_t frame[2];
        if (fp > UINT64_MAX - sizeof frame || !plat_mem_read(fp, frame, sizeof frame)) break;
        rt_log("guest abort frame %u: %s", n, ldr_describe(frame[1], at, sizeof at));
        if (frame[0] <= fp || frame[0] - fp > (64u << 20)) break;
        fp = frame[0];
    }
    if (RT_TARGET_HWAOI && rt_patch_set("hwaoi_display") >= 0) {
        const RtLoaded *main = ldr_module(0);
        // Exact-build main+112bc formats into this buffer before UDF #8002.
        // Diagnostics only: no guest memory or instruction is changed.
        if (main && !strcmp(main->name, "main") && pc == main->base + 0x11334) {
            char message[1024];
            if (plat_mem_read(main->base + 0x2897fb8, message, sizeof message)) {
                message[sizeof message - 1] = 0;
                rt_log("HWAoI game assertion: %s", message);
            }
        }
    }
}

void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) {
    char at[128], lr[128];
    guest_abort_diagnostic(c, pc);
    rt_fatal("brk #%#x at %s (lr %s)", imm, ldr_describe(pc, at, sizeof at), ldr_describe(c->x[30], lr, sizeof lr));
}

void rt_udf(Cpu *c, uint64_t pc) {
    char at[128], lr[128];
    guest_abort_diagnostic(c, pc);
    rt_fatal("undefined instruction at %s (lr %s)", ldr_describe(pc, at, sizeof at),
             ldr_describe(c->x[30], lr, sizeof lr));
}

// CNTPCT_EL0: the Switch counter runs at 19.2 MHz, the Switch 2's at 31.25 MHz (Bananza's SDK converts with 31250000 /
// 10^9 in nn::os::detail::TickManager::ConvertToTick). With 19.2 MHz ticks Bananza measured its frames 1.6 times too
// long and paced itself at 20 FPS. The scale is set before main, so no thread reads it half written.
static PlatScale tick_scale;

// Optional clock-consumer attribution. The returned guest clock is unchanged.
// A low key bit publishes a slot only after its first stack sample is complete.
typedef struct {
    _Atomic uint64_t key, calls;
    uint64_t parents[4];
    uint32_t first_tid;
} RateClockSite;
static RateClockSite rate_clock_sites[1024];
static _Atomic uint64_t rate_clock_overflow;
static _Atomic int rate_clock_enabled = -1;

// HWAoI's scheduler can take its step from a parent, a measured interval, or
// 1/requested_Hz. Observe those inputs without changing any guest clock/data.
// Only its once-per-node clock call is sampled, never the inner pacing loop.
typedef struct {
    uint64_t node, parent, calls, window_start_ns;
    double sum_seconds;
    uint32_t original_hz, requested_hz, flags;
    float seconds, parent_seconds;
} RateNode;
static RateNode rate_nodes[32];
static PlatMutex rate_node_lock = PLAT_MUTEX_INIT;

static void rate_node_observe(const Cpu *c) {
    if (!RT_TARGET_HWAOI) return;
    const RtLoaded *main = ldr_module(0);
    if (!main || strcmp(main->name, "main") || c->x[30] != main->base + 0x6c2854) return;
    RateNode value = {.node = c->x[27], .requested_hz = (uint32_t)c->x[19]};
    if (!value.node || value.node > UINT64_MAX - 0x34 || c->sp > UINT64_MAX - 0x30 ||
        !plat_mem_read(value.node + 0x30, &value.original_hz, 4) ||
        !plat_mem_read(value.node + 0x18, &value.seconds, 4) ||
        !plat_mem_read(c->sp + 0x28, &value.parent, 8) ||
        !plat_mem_read(c->sp + 0xc, &value.flags, 4)) return;
    if (value.parent && (value.parent > UINT64_MAX - 0x40 ||
        !plat_mem_read(value.parent + 0x3c, &value.parent_seconds, 4))) return;
    plat_mutex_lock(&rate_node_lock);
    for (unsigned i = 0; i < 32; i++) {
        if (rate_nodes[i].node && rate_nodes[i].node != value.node) continue;
        value.calls = rate_nodes[i].calls + 1;
        value.window_start_ns = rate_nodes[i].window_start_ns ? rate_nodes[i].window_start_ns : kern_now_ns();
        value.sum_seconds = rate_nodes[i].sum_seconds + value.seconds;
        rate_nodes[i] = value;
        break;
    }
    plat_mutex_unlock(&rate_node_lock);
}

static int rate_clock_on(void) {
    int on = atomic_load_explicit(&rate_clock_enabled, memory_order_relaxed);
    if (on < 0) {
        const char *value = cfg_get("TOTK_RATE_DIAG");
        on = value && !strcmp(value, "1");
        atomic_store_explicit(&rate_clock_enabled, on, memory_order_relaxed);
    }
    return on;
}

static void rate_clock_observe(void) {
    Cpu *c = kern_cpu();
    if (!c || !c->x[30] || (c->x[30] & 3)) return;
    rate_node_observe(c);
    uint64_t lr = c->x[30];
    unsigned hash = (unsigned)((lr >> 2) ^ (lr >> 14)) & 1023;
    for (unsigned n = 0; n < 8; n++) {
        RateClockSite *s = &rate_clock_sites[(hash + n) & 1023];
        uint64_t key = atomic_load_explicit(&s->key, memory_order_acquire);
        if (!key) {
            uint64_t empty = 0;
            if (atomic_compare_exchange_strong_explicit(&s->key, &empty, lr | 1,
                                                       memory_order_acq_rel, memory_order_acquire)) {
                s->first_tid = plat_thread_id();
                uint64_t fp = c->x[29];
                for (unsigned at = 0; at < 4 && fp && !(fp & 15); at++) {
                    uint64_t frame[2];
                    if (fp > UINT64_MAX - sizeof frame || !plat_mem_read(fp, frame, sizeof frame)) break;
                    s->parents[at] = frame[1];
                    if (frame[0] <= fp || frame[0] - fp > (64u << 20)) break;
                    fp = frame[0];
                }
                atomic_store_explicit(&s->key, lr, memory_order_release);
                key = lr;
            } else key = empty;
        }
        if (key == lr) {
            atomic_fetch_add_explicit(&s->calls, 1, memory_order_relaxed);
            return;
        }
    }
    atomic_fetch_add_explicit(&rate_clock_overflow, 1, memory_order_relaxed);
}

void hwaoi_rate_diagnostic_frame(uint64_t frame) {
    if (!rate_clock_on() || frame % 120) return;
    RateNode nodes[32];
    uint64_t node_now = kern_now_ns();
    plat_mutex_lock(&rate_node_lock);
    memcpy(nodes, rate_nodes, sizeof nodes);
    for (unsigned i = 0; i < 32; i++) {
        rate_nodes[i].calls = 0;
        rate_nodes[i].sum_seconds = 0;
        rate_nodes[i].window_start_ns = node_now;
    }
    plat_mutex_unlock(&rate_node_lock);
    for (unsigned i = 0; i < 32; i++) {
        const RateNode *s = &nodes[i];
        if (s->calls) rt_log("rate-node: node=%#llx calls=%llu original_hz=%u requested_hz=%u "
                            "last_seconds=%.9g parent=%#llx parent_seconds=%.9g fixed=%u "
                            "step_sum=%.6f wall=%.6f",
                            (unsigned long long)s->node, (unsigned long long)s->calls,
                            s->original_hz, s->requested_hz, (double)s->seconds,
                            (unsigned long long)s->parent, (double)s->parent_seconds, s->flags & 1,
                            s->sum_seconds, node_now > s->window_start_ns ?
                            (node_now - s->window_start_ns) * 1e-9 : 0.0);
    }
    uint64_t counts[20] = {0}, total = 0;
    RateClockSite *sites[20] = {0};
    for (unsigned n = 0; n < 1024; n++) {
        RateClockSite *s = &rate_clock_sites[n];
        uint64_t key = atomic_load_explicit(&s->key, memory_order_acquire);
        if (!key || (key & 1)) continue;
        uint64_t count = atomic_exchange_explicit(&s->calls, 0, memory_order_relaxed);
        total += count;
        for (unsigned i = 0; count && i < 20; i++) {
            if (count <= counts[i]) continue;
            memmove(counts + i + 1, counts + i, (19 - i) * sizeof counts[0]);
            memmove(sites + i + 1, sites + i, (19 - i) * sizeof sites[0]);
            counts[i] = count; sites[i] = s; break;
        }
    }
    uint64_t overflow = atomic_exchange_explicit(&rate_clock_overflow, 0, memory_order_relaxed);
    rt_log("rate-clock: frame=%llu calls=%llu overflow=%llu", (unsigned long long)frame,
           (unsigned long long)total, (unsigned long long)overflow);
    for (unsigned i = 0; i < 20 && sites[i]; i++) {
        RateClockSite *s = sites[i]; char at[128], parent[4][128];
        for (unsigned p = 0; p < 4; p++) ldr_describe(s->parents[p], parent[p], sizeof parent[p]);
        rt_log("rate-clock: calls=%llu first_tid=%u lr=%s parents=%s / %s / %s / %s",
               (unsigned long long)counts[i], s->first_tid,
               ldr_describe(atomic_load_explicit(&s->key, memory_order_relaxed), at, sizeof at),
               parent[0], parent[1], parent[2], parent[3]);
    }
}

uint32_t rt_tick_hz(void) { return rt_game_version ? 19200000u : 31250000u; }

__attribute__((constructor)) static void tick_scale_init(void) { tick_scale = plat_scale(rt_tick_hz(), plat_counter_hz()); }

uint64_t rt_ticks(void) {
    uint64_t ticks = plat_scaled(tick_scale, plat_counter());
    if (rate_clock_on()) rate_clock_observe();
    return ticks;
}
uint64_t rt_ticks_to_ns(uint64_t t) { return (uint64_t)((unsigned __int128)t * 1000000000u / rt_tick_hz()); }
uint64_t rt_ns_to_ticks(uint64_t ns) { return (uint64_t)((unsigned __int128)ns * rt_tick_hz() / 1000000000u); }

// ---- crash reporter
typedef struct { uintptr_t host; uint64_t guest; } FnRev;
static FnRev *fn_rev;
static uint64_t fn_rev_n;

static int by_host(const void *a, const void *b) {
    uintptr_t x = ((const FnRev *)a)->host, y = ((const FnRev *)b)->host;
    return x < y ? -1 : x > y;
}

// The lifted function that contains a host code address, by the nearest function start below it; 0 outside the lifted
// code.
static uint64_t lifted_function(uintptr_t rip) {
    if (!fn_rev) {
        static RtModule mods[MAX_MODULES];
        int n = rt_linked_modules(mods, MAX_MODULES);
        uint64_t total = 0;
        for (int i = 0; i < n; i++) total += mods[i].count;
        fn_rev = malloc((total ? total : 1) * sizeof *fn_rev);
        if (!fn_rev) return 0;
        for (int i = 0; i < n; i++)
            for (uint64_t k = 0; k < mods[i].count; k++)
                fn_rev[fn_rev_n++] = (FnRev){(uintptr_t)mods[i].fns[k], mods[i].addrs[k]};
        qsort(fn_rev, fn_rev_n, sizeof *fn_rev, by_host);
    }
    // The lifted code is one range with the host code outside it (kernel.c ap_init checks that); an address above the
    // last function start is taken for host code, which costs only the last function's body.
    if (!fn_rev_n || rip > fn_rev[fn_rev_n - 1].host) return 0;
    uint64_t lo = 0, hi = fn_rev_n;
    while (lo < hi) {
        uint64_t mid = (lo + hi) / 2;
        if (fn_rev[mid].host <= rip) lo = mid + 1;
        else hi = mid;
    }
    return lo ? fn_rev[lo - 1].guest : 0;
}

static int readable(uint64_t a, uint64_t n) { return plat_mem_readable(a, n); }

static void crash_handler(const PlatCrash *x) {
    static volatile int32_t busy;
    if (plat_xchg32(&busy, 1)) plat_exit(71);
    char d[160];
    uintptr_t rip = x->rip;
    rt_logf("FATAL: %s (%#x) at host %p (totk+%#llx)", x->what, x->code, (void *)rip, (unsigned long long)x->offset);
    if (x->access >= 0)
        rt_logf(", %s address %#llx", x->access == 1 ? "writing" : x->access == 8 ? "executing" : "reading",
                (unsigned long long)x->addr);
    rt_log_raw("\n", 1);
    uint64_t f = lifted_function(rip);
    if (f) rt_logf("  in lifted function f_%llx = %s\n", (unsigned long long)f, ldr_describe(f, d, sizeof d));
    for (int i = 0; i < x->nframes; i++) {   // abort and invalid parameter: where the host code called it
        uint64_t g = lifted_function(x->frames[i]);
        rt_logf("  host frame %2d: totk+%#llx", i, (unsigned long long)(x->frames[i] - (rip - x->offset)));
        if (g) rt_logf(" in lifted function f_%llx", (unsigned long long)g);
        rt_log_raw("\n", 1);
    }
    Cpu *c = kern_cpu();
    if (c) {
        for (int i = 0; i < 31; i++)
            rt_logf("  x%-2d %016llx%s", i, (unsigned long long)c->x[i], i % 4 == 3 ? "\n" : "");
        rt_logf("  sp  %016llx\n  pc  %016llx  lr %s\n", (unsigned long long)c->sp, (unsigned long long)c->pc,
                ldr_describe(c->x[30], d, sizeof d));
        uint64_t fp = c->x[29];
        for (int n = 0; n < 32 && fp && !(fp & 7) && readable(fp, 16); n++) {
            uint64_t next = LD64(fp), lr = LD64(fp + 8);
            rt_logf("  guest frame %2d: %s\n", n, ldr_describe(lr, d, sizeof d));
            if (next <= fp || next - fp > (64u << 20)) break;
            fp = next;
        }
    } else {
        rt_logf("  not on a guest thread\n");
    }
    fflush(stderr);
    plat_exit(71);
}

// ---- self-test: layout, dispatch table, host-function registry, without game files
static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FAIL: " __VA_ARGS__); putchar('\n'); } } while (0)

static double now_s(void) { return (double)plat_now_ns() / 1e9; }

static volatile int host_probe_hits;
static void host_probe(Cpu *c) { host_probe_hits++; c->x[0] = 42; }

static void check_cpu(void) {
    __builtin_cpu_init();
    int ok = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma") && __builtin_cpu_supports("bmi2");
    printf("cpu: x86-64-v3 (AVX2, FMA, BMI2) %s\n", ok ? "present" : "MISSING");
    CHECK(ok, "the lifted code is built for x86-64-v3");
}

// Layout: ascending, no overlap, below 2^39, and inside the window mem_init reserved.
static void check_layout(void) {
    const RtLayoutEntry *lay = rt_layout;
    int n = rt_layout_count;
    printf("layout: %d modules\n", n);
    for (int i = 0; i < n; i++) {
        printf("  %-8s base %#llx  size %#10llx  end %#llx\n", lay[i].name, (unsigned long long)lay[i].base,
               (unsigned long long)lay[i].size, (unsigned long long)(lay[i].base + lay[i].size));
        CHECK(lay[i].size > 0, "%s has size 0", lay[i].name);
        CHECK(lay[i].base % 0x10000 == 0, "%s base is not 64 KiB aligned", lay[i].name);
        CHECK(lay[i].base + lay[i].size <= RT_KERNEL_BASE, "%s reaches into the kernel region", lay[i].name);
        if (i) {
            CHECK(lay[i].base > lay[i - 1].base, "%s base is not above %s", lay[i].name, lay[i - 1].name);
            CHECK(lay[i - 1].base + lay[i - 1].size <= lay[i].base, "%s overlaps %s", lay[i - 1].name, lay[i].name);
        }
        uint64_t end;
        int ours = plat_mem_state(lay[i].base, lay[i].base + 1, &end) == PLAT_MEM_RESERVED;
        CHECK(ours, "%s is not inside the reserved module window", lay[i].name);
    }
    const RtGuestLayout *g = mem_layout();
    printf("guest memory: aslr %#llx+%#llx stack %#llx heap %#llx alias %#llx+%#llx kernel %#llx host functions %#llx\n",
           (unsigned long long)g->aslr.base, (unsigned long long)g->aslr.size, (unsigned long long)g->stack.base,
           (unsigned long long)g->heap.base, (unsigned long long)g->alias.base, (unsigned long long)g->alias.size,
           (unsigned long long)g->kernel.base, (unsigned long long)g->hostfn.base);
    CHECK(g->alias.base + g->alias.size <= lay[0].base && g->heap.base + g->heap.size <= g->alias.base,
          "guest regions overlap");
}

// Every function start dispatches to its own function. The word after a start (unless that is a start too), a
// misaligned address and the word below the module dispatch to nothing.
static void check_modules(const RtModule *mods, int n) {
    uint64_t total = 0;
    for (int i = 0; i < n; i++) {
        const RtModule *m = &mods[i];
        const RtLayoutEntry *l = NULL;
        for (int k = 0; k < rt_layout_count; k++)
            if (!strcmp(rt_layout[k].name, m->name)) l = &rt_layout[k];
        CHECK(l && l->base == m->base && l->size == m->size, "%s: linked base/size differ from the layout", m->name);

        uint64_t bad = 0, bad_neg = 0;
        for (uint64_t k = 0; k < m->count; k++) {
            uint64_t a = m->addrs[k];
            if (rt_dispatch_lookup(a) != m->fns[k] && bad++ < 5)
                printf("  FAIL: %s_fn_addrs[%llu] %#llx does not dispatch to its function\n", m->name,
                       (unsigned long long)k, (unsigned long long)a);
            int next_is_start = k + 1 < m->count && m->addrs[k + 1] == a + 4;
            if ((!next_is_start && rt_dispatch_lookup(a + 4)) || rt_dispatch_lookup(a + 2)) bad_neg++;
        }
        if (rt_dispatch_lookup(m->base - 4)) bad_neg++;
        failures += bad > 0;
        CHECK(!bad_neg, "%s: %llu addresses that are not function starts dispatch to a function", m->name,
              (unsigned long long)bad_neg);
        uint64_t lo = m->count ? m->addrs[0] : 0, hi = m->count ? m->addrs[m->count - 1] : 0;
        printf("  %-8s %8llu functions, first %#llx, last %#llx: %s\n", m->name, (unsigned long long)m->count,
               (unsigned long long)lo, (unsigned long long)hi, bad || bad_neg ? "FAIL" : "ok");
        total += m->count;
    }
    printf("  total    %8llu functions\n", (unsigned long long)total);
}

static void bench(const RtModule *mods, int n) {
    uint64_t total = 0, sink = 0;
    double t0 = now_s();
    for (int i = 0; i < n; i++) {
        for (uint64_t k = 0; k < mods[i].count; k++) sink += (uintptr_t)rt_dispatch_lookup(mods[i].addrs[k]);
        total += mods[i].count;
    }
    double dt = now_s() - t0;
    __asm__ volatile("" ::"r"(sink));
    if (total) printf("lookup: %.1f ns per function start (address order)\n", dt * 1e9 / (double)total);
}

// Memory SVCs without modules: heap, physical memory in the alias region, a stack move, QueryMemory, GetInfo.
static void check_memory(void) {
    const RtGuestLayout *g = mem_layout();
    RtMemoryInfo q;
    uint64_t heap = 0, v = 0;
    int before = failures;
    CHECK(mem_set_heap_size(0x400000, &heap) == 0 && heap == g->heap.base, "SetHeapSize 4 MiB");
    memset((void *)(uintptr_t)heap, 0xAB, 0x400000);
    mem_query(heap + 0x1000, &q);
    CHECK(q.base == heap && q.size == 0x400000 && q.state == 5 && q.perm == 3, "QueryMemory on the heap");
    CHECK(mem_set_heap_size(0x300000, &heap) != 0, "SetHeapSize accepts a size that is not a 2 MiB multiple");

    uint64_t a = g->alias.base + 0x100000;
    CHECK(mem_map_physical(a, 0x10000) == 0, "MapPhysicalMemory");
    ST64(a + 0x8000, 0x1234);
    CHECK(mem_map_physical(a - 0x4000, 0x20000) == 0 && LD64(a + 0x8000) == 0x1234,
          "MapPhysicalMemory over a mapped range lost its contents");
    mem_query(a, &q);
    CHECK(q.base == a - 0x4000 && q.size == 0x20000 && q.state == 5, "QueryMemory on mapped alias pages");
    mem_info(7, &v);
    CHECK(v == 0x400000 + 0x20000, "GetInfo 7 (used memory) %#llx", (unsigned long long)v);

    uint64_t stack = g->stack.base + 0x100000;
    ST64(a + 8, 0x99);
    CHECK(mem_map(stack, a, 0x4000) == 0 && LD64(stack + 8) == 0x99, "MapMemory alias -> stack did not copy");
    ST64(stack + 0x10, 0x5678);
    mem_query(stack, &q);
    CHECK(q.base == stack && q.size == 0x4000 && q.state == 0xB, "QueryMemory on a moved stack");
    mem_query(a, &q);
    CHECK(q.base == a && q.state == 5 && q.perm == 0, "QueryMemory on a moved stack's source");
    CHECK(mem_unmap_physical(a, 0x4000) != 0, "UnmapPhysicalMemory under a moved stack");
    CHECK(mem_unmap(stack, a, 0x4000) == 0 && LD64(a + 0x10) == 0x5678, "UnmapMemory did not copy the stack back");

    CHECK(mem_unmap_physical(a - 0x4000, 0x20000) == 0, "UnmapPhysicalMemory");
    mem_query(a, &q);
    CHECK(q.state == 0 && q.base == g->alias.base && q.size == g->alias.size, "QueryMemory on the free alias region");
    CHECK(mem_map_physical(a, 0x1000) == 0 && LD64(a + 0x10) == 0 && mem_unmap_physical(a, 0x1000) == 0,
          "remapped physical memory is not zeroed");
    CHECK(mem_info(6, &v) && v == g->pool, "GetInfo 6 (total memory) %#llx", (unsigned long long)v);
    uint64_t beyond_pool = (g->pool + 0x1000ull) & ~0xfffull;
    CHECK(beyond_pool > g->pool && a >= g->alias.base && a - g->alias.base <= g->alias.size &&
          beyond_pool <= g->alias.size - (a - g->alias.base) && mem_map_physical(a, beyond_pool) == 0x10801,
          "MapPhysicalMemory beyond the pool is not LimitReached");
    mem_query(RT_HOSTFN_BASE + 0x40, &q);
    CHECK(q.state == 0x10 && q.base == RT_HOSTFN_BASE, "QueryMemory on the host-function window");
    mem_query(g->alias.base + g->alias.size + 0x1000, &q);
    CHECK(q.state == 3 && q.base == g->alias.base + g->alias.size, "QueryMemory on host memory");
    CHECK(mem_set_heap_size(0, &heap) == 0, "SetHeapSize 0");
    printf("memory: heap, physical memory, stack move, QueryMemory, GetInfo: %s\n", failures > before ? "FAIL" : "ok");
}

static int selftest(void) {
    static RtModule mods[MAX_MODULES];
    int n = rt_linked_modules(mods, MAX_MODULES);

    check_cpu();
    check_layout();
    uint64_t probe = hostfn_register("<selftest probe>", host_probe);

    double t0 = now_s();
    hostfn_seal();
    double dt = now_s() - t0;
    RtDispatchStats s = rt_dispatch_stats();
    printf("dispatch: %d modules linked, table covers %#llx-%#llx: %llu slots, %.1f MiB reserved, built in %.0f ms\n",
           n, (unsigned long long)s.lo, (unsigned long long)s.hi, (unsigned long long)s.slots,
           (double)s.table_bytes / (1 << 20), dt * 1e3);
    check_modules(mods, n);
    CHECK(s.functions > 0, "no functions linked");
    Cpu *c = plat_aligned_alloc(sizeof(Cpu), 64);
    memset(c, 0, sizeof *c);
    uint64_t r = rt_call(c, probe, 1, (uint64_t)7);
    CHECK(rt_dispatch_lookup(probe) && r == 42 && host_probe_hits == 1 && c->x[30] == 0,
          "a registered host function does not dispatch through rt_call");
    CHECK(!rt_dispatch_lookup(probe + 16), "an unregistered host-function address dispatches");
    CHECK(rt_dispatch_lookup(RT_RET_HOST), "the rt_call return sentinel is not registered");
    plat_aligned_free(c);
    bench(mods, n);

    void *k1 = mem_kernel_alloc(0x200, 16), *k2 = mem_kernel_alloc(0x2000, 0x1000);
    CHECK((uintptr_t)k1 - RT_KERNEL_BASE < RT_KERNEL_SIZE && !((uintptr_t)k2 & 0xFFF) && k1 != k2,
          "kernel-region allocation");
    mem_kernel_free(k1, 0x200);
    CHECK(mem_kernel_alloc(0x200, 16) == k1, "a freed kernel-region block is not reused");
    check_memory();
    CHECK(!vk_backend_selftest(), "the replay write-back deduplication");

    float used, total, proc;
    plat_mem_info(&used, &total, &proc);
    if (proc >= 0) printf("process: working set %.0f MiB\n", proc);
    printf("selftest: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
}

static int usage(const char *argv0) {
    fprintf(stderr, "usage: %s [--exefs DIR] [--romfs DIR] [--profile DIR] [--selftest | --probe-loader | "
                    "--probe-cfg | --probe-dpi | --probe-kernel | --probe-ipc | --probe-ipc-lifetime | --probe-services | --probe-input | --probe-fs | --probe-gfx-abi | "
                    "--probe-nvn | --probe-nvn-tex | --probe-window | --probe-present-color | --probe-shader | --probe-shader-geometry | --probe-vkmem | "
                    "--probe-vkexec | --probe-vkgeometry | --probe-vkgeometry-passthrough | --probe-vkgeometry-passthrough-ordering | --probe-vkclear | --probe-vkcopy-astc | --probe-vkpool | --probe-vkreset | --probe-astc | --probe-movie | --prepare-shaders | --warm-shaders | --play-experimental | --boot |--seconds S [--shot PNG]]\n", argv0);
    return 2;
}

// Paths for a copied folder (tools/package_build.py). Per path: TOTK_EXEFS, TOTK_ROMFS, TOTK_PROFILE_DIR, else the
// folder next to totk.exe (exefs with main.npdm, romfs with romfs.bin, any profile folder) if it is there, else the
// path baked in at build time. A relative value is relative to totk.exe's folder.
static char pp_exefs[PLAT_PATH_MAX * 2], pp_romfs[PLAT_PATH_MAX * 2], pp_profile[PLAT_PATH_MAX * 2];

static const char *portable_path(const char *dir, const char *var, const char *sub, const char *must, char *out, size_t cap,
                                 const char *fallback) {
    const char *e = cfg_get(var);
    if (e && *e) {
        int abs_path = e[0] == '\\' || e[0] == '/' || e[1] == ':';
        if (abs_path) snprintf(out, cap, "%s", e);
        else snprintf(out, cap, "%s/%s", dir, e);
        return out;
    }
    char probe[PLAT_PATH_MAX * 3];
    snprintf(out, cap, "%s/%s", dir, sub);
    if (!must) return plat_dir_exists(out) ? out : fallback;   // the folder itself is the sign
    snprintf(probe, sizeof probe, "%s/%s", out, must);
    return plat_file_exists(probe) ? out : fallback;
}

static void portable_paths(RtConfig *c) {
    char exe[PLAT_PATH_MAX * 2];
    int n = plat_exe_path(exe, sizeof exe);
    char *cut = n && n < sizeof exe ? plat_last_sep(exe) : NULL;
    if (!cut) return;
    *cut = 0;
    c->exefs = portable_path(exe, "TOTK_EXEFS", "exefs", "main.npdm", pp_exefs, sizeof pp_exefs, c->exefs);
    c->romfs = portable_path(exe, "TOTK_ROMFS", "romfs", "romfs.bin", pp_romfs, sizeof pp_romfs, c->romfs);
    c->profile = portable_path(exe, "TOTK_PROFILE_DIR", "profile", NULL, pp_profile, sizeof pp_profile, c->profile);
}

int main(int argc, char **argv) {
    mem_init();   // first: claim the guest address space before anything else can land there; reads totk.ini
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    plat_crash_hook(crash_handler);
    parse_trace();
    prof_start();
    // Timed guest waits (kernel.c block_inner) sleep in whole milliseconds; at the default 15.6 ms tick a 1 ms wait
    // costs a full tick. TOTK_TIMER_1MS=0 keeps the default for comparison.
    if (cfg_bool("TOTK_TIMER_1MS")) plat_timer_1ms();
    cfg = (RtConfig){.exefs = rt_default_exefs, .romfs = rt_default_romfs, .profile = rt_default_profile,
                     .gpu = cfg_get("TOTK_GPU")};
    portable_paths(&cfg);   // command-line paths below still win
    if (cfg.gpu && strcmp(cfg.gpu, "null") && strcmp(cfg.gpu, "vulkan")) {
        rt_logf("TOTK_GPU must be null or vulkan, not '%s'\n", cfg.gpu);
        return 2;
    }
    const char *mode = NULL;
    for (int i = 1; i < argc; i++) {
        const char **path = !strcmp(argv[i], "--exefs") ? &cfg.exefs : !strcmp(argv[i], "--romfs") ? &cfg.romfs
                          : !strcmp(argv[i], "--profile") ? &cfg.profile : NULL;
        if (path) {
            if (++i == argc) return usage(argv[0]);
            *path = argv[i];
        } else if (!strcmp(argv[i], "--seconds")) {
            char *end;
            if (++i == argc) return usage(argv[0]);
            double s = strtod(argv[i], &end);
            if (*end || !(s > 0) || s > 1e6) return usage(argv[0]);
            cfg.seconds = s;
        } else if (!strcmp(argv[i], "--shot")) {
            if (++i == argc) return usage(argv[0]);
            cfg.shot = argv[i];
        } else if (!mode && !strncmp(argv[i], "--", 2)) {
            mode = argv[i];
        } else {
            return usage(argv[0]);
        }
    }
    if (cfg.shot && !cfg.seconds) return usage(argv[0]);
    if (cfg.seconds && mode) return usage(argv[0]);
#if RT_TARGET_HWAOI && !HWAOI_ALLOW_EXPERIMENTAL_BOOT
    if (!mode || !strcmp(mode, "--boot")) {
        fprintf(stderr, "HWAoI runtime port is not boot-qualified yet. CPU generation does not establish game compatibility.\n");
        return 2;
    }
#endif
    log_open(!mode);
    if (!mode) {
        cfg.boot = cfg.seconds > 0;   // --seconds starts the game like --boot, with its checkpoints
        rt_boot();
        return 0;
    }
    if (!strcmp(mode, "--selftest")) return selftest();
    if (!strcmp(mode, "--probe-loader")) return probe_loader();
    if (!strcmp(mode, "--probe-cfg")) return probe_cfg();
#ifdef _WIN32
    if (!strcmp(mode, "--probe-dpi")) return probe_dpi();   // the Win32 DPI awareness of totk.manifest
#endif
    if (!strcmp(mode, "--probe-kernel")) return probe_kernel();
    if (!strcmp(mode, "--probe-ipc")) return probe_ipc();
    if (!strcmp(mode, "--probe-ipc-lifetime")) return probe_ipc_lifetime();
    if (!strcmp(mode, "--probe-services")) return probe_services();
    if (!strcmp(mode, "--probe-input")) return probe_input_services();
    if (!strcmp(mode, "--probe-fs")) return probe_fs();
    if (!strcmp(mode, "--probe-storage")) return probe_save_storage();
    if (!strcmp(mode, "--probe-romfs")) return probe_romfs_storage();
    if (!strcmp(mode, "--probe-boot")) { cfg.boot = 1; rt_boot(); return 0; }
    if (!strcmp(mode, "--probe-render")) { g_hwaoi_render_probe = 1; cfg.boot = 0; rt_boot(); return 0; }
    if (RT_TARGET_HWAOI && !strcmp(mode, "--play-experimental")) {
        rt_log("HWAoI: interactive experimental run; loading and gameplay compatibility remain incomplete");
        cfg.boot = 0;
        rt_boot();
        return 0;
    }
    if (!strcmp(mode, "--probe-gfx-abi")) return probe_gfx_abi();
    if (!strcmp(mode, "--probe-nvn")) return probe_nvn();
    if (!strcmp(mode, "--probe-nvn-tex")) return probe_nvn_tex();
    if (!strcmp(mode, "--probe-window")) return probe_window();
    if (!strcmp(mode, "--probe-present-color")) return probe_present_color();
    if (!strcmp(mode, "--probe-shader")) return probe_shader();
    if (!strcmp(mode, "--probe-shader-geometry")) return probe_geometry_shader();
    if (!strcmp(mode, "--probe-vkmem")) return probe_vkmem();
    if (!strcmp(mode, "--probe-vkexec")) return probe_vkexec();
    if (!strcmp(mode, "--probe-vkgeometry")) return probe_vkgeometry();
    if (!strcmp(mode, "--probe-vkgeometry-passthrough")) return probe_vkgeometry_passthrough();
    if (!strcmp(mode, "--probe-vkgeometry-passthrough-ordering")) return probe_vkgeometry_ordering();
    if (!strcmp(mode, "--probe-vkclear")) return probe_vkclear();
    if (!strcmp(mode, "--probe-vkcopy-astc")) return probe_vkcopy_astc();
    if (!strcmp(mode, "--probe-vkpool")) return probe_vkpool();
    if (!strcmp(mode, "--probe-vkreset")) return probe_vkreset();
    if (!strcmp(mode, "--probe-astc")) return probe_astc();
    if (!strcmp(mode, "--probe-vkdevice")) return probe_vkdevice();
    if (!strcmp(mode, "--probe-movie")) return probe_movie();
    if (!strcmp(mode, "--prepare-shaders")) return vk_pipeline_prepare_shaders();
    if (!strcmp(mode, "--warm-shaders")) return vk_pipeline_warm_shaders();
    if (!strcmp(mode, "--boot")) { cfg.boot = 1; rt_boot(); return 0; }
    return usage(argv[0]);
}
