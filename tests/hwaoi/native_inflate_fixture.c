#include "host.h"
#include "platform/platform.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

static RtFileHash test_hashes[] = {{"main", "4c33cfd4c818d9ed62822af792782b4ea443417ed24acea6f1cee739cde604c3"}};
static int test_hash_count = 1, calls, readonly_output, uncommitted_input, bad_query, bad_keys, split_query;
int kern_host_track = 1;
void kern_host_left(void *t) { (void)t; }
uint64_t kern_now_ns(void) { return GetTickCount64() * 1000000ull; }
#define rt_exefs_sha256 test_hashes
#define rt_exefs_sha256_count test_hash_count
static uint8_t *region, expected[0x4000];
static uint64_t base = 0x2000000000ull;
static RtGuestLayout layout = {.heap = {.base = 0x2000000000ull, .size = 0x100000}};
const RtGuestLayout *mem_layout(void) { return &layout; }
void rt_fatal(const char *fmt, ...) { (void)fmt; exit(70); }
void rt_log(const char *fmt, ...) { (void)fmt; }
void *plat_lib_open(const char *path) { return (void *)LoadLibraryA(path); }
void *plat_lib_sym(void *lib, const char *name) { return lib ? (void *)GetProcAddress((HMODULE)lib, name) : NULL; }
int plat_mem_readable(uint64_t a, uint64_t n) {
    if (bad_keys && a == base + 0x8000) return 0;
    return a >= base && a - base < 0x100000 && n <= 0x100000 - (a - base);
}
uint32_t mem_query(uint64_t a, RtMemoryInfo *out) {
    if (!plat_mem_readable(a, 1)) return 1;
    *out = (RtMemoryInfo){.base = base, .size = 0x100000, .perm = readonly_output ? 1 : 3};
    if (uncommitted_input && a == base + 0x10000) out->perm = 0;
    if (bad_query) out->size = 0;
    if (split_query) { out->base = a & ~0xfffull; out->size = 0x1000; }
    return 0;
}
static void original(Cpu *c) {
    if (c->thread && *kern_host_depth(c->thread)) abort();
    calls++;
    memcpy(region + 0x30000, expected, sizeof expected);
    c->x[0] = 0;
}
#include "hwaoi_native_inflate.h"
GuestFn rt_dispatch_lookup(uint64_t pc) {
    return hwaoi_inflate.mode && pc == hwaoi_inflate.pc ? hwaoi_inflate_call : original;
}
void plat_mutex_lock(PlatMutex *m) { (void)m; }
void plat_mutex_unlock(PlatMutex *m) { (void)m; }
uint32_t plat_thread_id(void) { return GetCurrentThreadId(); }
int plat_mem_read(uint64_t a, void *out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (const void *)(uintptr_t)a, out, n, &got) && got == n;
}
#include "hwaoi_pool_diag.h"
static void check(int p) { if (!p) abort(); }
int main(int argc, char **argv) {
    check(argc == 5);
    uint8_t short_packet[16] = {0};
    uint64_t short_key = 0xfedcba9876543210ull;
    memcpy(short_packet, &short_key, 8);
    check(hwaoi_inflate_checksum(short_packet, 8, short_key));
    check(hwaoi_inflate_checksum(short_packet, 15, short_key));
    check(!hwaoi_inflate_checksum(short_packet, 7, short_key));
    check(!hwaoi_inflate_checksum(short_packet, 8, short_key ^ 1));
    check(!hwaoi_inflate_checksum(short_packet, 65536, short_key));
    const char *mode = argv[1];
    _putenv_s("HWAOI_NATIVE_INFLATE", !strcmp(mode, "off") ? "0" : !strcmp(mode, "verify") || !strcmp(mode, "mismatch") ? "verify" : "1");
    _putenv_s("HWAOI_ZLIB_DLL", argv[4]);
    uint64_t address = base + 0x8000 - 0x1f369d0 + 0x28708;
    GuestFn fn = original;
    RtModule m = {.name = "main", .base = base + 0x8000 - 0x1f369d0, .size = 0x3000000,
                  .addrs = &address, .fns = &fn, .count = 1};
    if (!strcmp(mode, "hash")) test_hashes[0].sha256 = "wrong";
    if (!strcmp(mode, "module")) m.name = "sdk";
    if (!strcmp(mode, "extent")) m.size = 0;
    if (!strcmp(mode, "overflow")) m.size = UINT64_MAX;
    if (!strcmp(mode, "path")) _putenv_s("HWAOI_ZLIB_DLL", "zlib1.dll");
    if (!strcmp(mode, "library")) _putenv_s("HWAOI_ZLIB_DLL", "F:/does-not-exist/zlib.dll");
    hwaoi_inflate_init(&m, 1);
    if (!strcmp(mode, "compose")) {
        _putenv_s("HWAOI_POOL_DIAG", "1");
        hwaoi_pool_diag_init(&m, 1);
        check(hwaoi_pool_diag_watches(address));
        Cpu watched = {.pc = address};
        check(hwaoi_pool_diag_watches_call(&watched));
    }
    if (!strcmp(mode, "off")) { check(!hwaoi_inflate.mode); puts("check passed"); return 0; }
    region = VirtualAlloc((void *)(uintptr_t)base, 0x100000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    check(region != NULL);
    FILE *file = fopen(argv[2], "rb"); check(file != NULL);
    size_t n = fread(region + 0x10000, 1, 65535, file); fclose(file); check(n >= 8);
    file = fopen(argv[3], "rb"); check(file != NULL);
    check(fread(expected, 1, sizeof expected, file) == sizeof expected); fclose(file);
    uint64_t *keys = (uint64_t *)(region + 0x8000);
    for (unsigned i = 0; i < 32; i++) keys[i] = 0x1020304050607080ull + i;
    uint64_t job = base + 0x1000;
    ST64(job + 0x18, base + 0x50000);
    ST32(base + 0x50028, 128);
    ST64(job + 0x38, base + 0x30000); ST64(job + 0x40, base + 0x10000); ST32(job + 0x48, (uint32_t)n);
    int fallback = !strcmp(mode, "checksum") || !strcmp(mode, "readonly") || !strcmp(mode, "overlap") ||
                   !strcmp(mode, "length") || !strcmp(mode, "input") || !strcmp(mode, "adler");
    fallback |= !strcmp(mode, "nonheap") || !strcmp(mode, "uncommitted") || !strcmp(mode, "query") ||
                !strcmp(mode, "keys") || !strcmp(mode, "jobregion");
    if (!strcmp(mode, "checksum")) region[0x10000] ^= 1;
    if (!strcmp(mode, "readonly")) readonly_output = 1;
    if (!strcmp(mode, "overlap")) ST64(job + 0x38, base + 0x10000);
    if (!strcmp(mode, "length")) ST32(job + 0x48, 0xffffffffu);
    if (!strcmp(mode, "input")) ST64(job + 0x40, RT_ADDRESS_LIMIT - 2);
    if (!strcmp(mode, "nonheap")) layout.heap.size = 0x10000;
    if (!strcmp(mode, "jobregion")) layout.heap.base = base + 0x2000;
    if (!strcmp(mode, "uncommitted")) uncommitted_input = 1;
    if (!strcmp(mode, "query")) bad_query = 1;
    if (!strcmp(mode, "keys")) bad_keys = 1;
    if (!strcmp(mode, "split")) split_query = 1;
    if (!strcmp(mode, "adler")) {
        // Payload corruption outside sampled custom-checksum bytes: the zlib
        // decoder must still reject its own stream checksum (or custom check).
        region[0x10000 + n - 1] ^= 1;
    }
    if (!strcmp(mode, "mismatch")) expected[0] ^= 1;
    Cpu c = {0};
    int thread[2] = {0}; c.thread = thread;
    for (int i = 0; i < 31; i++) c.x[i] = 0x88770000u + i;
    for (int i = 0; i < 32; i++) c.v[i] = (__uint128_t)0x12345678u + i;
    c.x[0] = job; c.sp = 0x5555; c.pc = address; c.tpidr = 0x6666; c.tpidrro = 0x7777;
    Cpu before = c;
    if (!strcmp(mode, "compose")) hwaoi_pool_diag_call(&c);
    else hwaoi_inflate_call(&c);
    check(c.x[0] == 0); before.x[0] = 0;
    check(thread[1] == 0);
    check(!memcmp(&before, &c, sizeof c));
    check(!memcmp(region + 0x30000, expected, sizeof expected));
    if (fallback) check(calls == 1 && hwaoi_inflate.fallback == 1 && !hwaoi_inflate.accelerated);
    else if (!strcmp(mode, "verify")) check(calls == 1 && hwaoi_inflate.verified == 1 && !hwaoi_inflate.fallback);
    else check(calls == 0 && hwaoi_inflate.accelerated == 1 && !hwaoi_inflate.fallback);
    if (!strcmp(mode, "compose"))
        check(hwaoi_pool_diag.n == 1 && hwaoi_pool_diag.rows[0].job_start == 1 && hwaoi_pool_diag.rows[0].job_done == 1);
    VirtualFree(region, 0, MEM_RELEASE);
    puts("check passed"); return 0;
}
