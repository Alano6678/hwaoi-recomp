// Original host adapter for the profile-locked 16 KiB resource job vfunc.
// Default off. "verify" executes the lifted job and compares its output; "1"
// uses native zlib only for validated packets. No guest z_stream ABI is shared
// with the host library. zlib's public uncompress2 ABI uses C unsigned long.
#pragma once
#include "hwaoi_observe_guard.h"

typedef int (*HwaoiUncompress2)(unsigned char *, unsigned long *, const unsigned char *, unsigned long *);
static struct {
    int mode, trace;
    uint64_t pc, keys;
    GuestFn original;
    HwaoiUncompress2 uncompress;
    uint64_t verified, accelerated, fallback, entered;
    uint64_t key_values[32];
    int keys_ready;
    PlatMutex keys_lock;
} hwaoi_inflate = {.keys_lock = PLAT_MUTEX_INIT};

static int hwaoi_inflate_absolute(const char *p) {
#ifdef _WIN32
    return p && ((p[0] && p[1] == ':' && (p[2] == '/' || p[2] == '\\')) ||
                 (p[0] == '\\' && p[1] == '\\'));
#else
    return p && p[0] == '/';
#endif
}

static void hwaoi_inflate_init(const RtModule *mods, int count) {
    const char *e = getenv("HWAOI_NATIVE_INFLATE");
    if (!e || !*e || !strcmp(e, "0")) return;
    int mode = !strcmp(e, "verify") ? 1 : !strcmp(e, "1") ? 2 : 0;
    if (!mode) rt_fatal("native-inflate: HWAOI_NATIVE_INFLATE must be 0, verify or 1");
    const char *sha = NULL;
    for (int i = 0; i < rt_exefs_sha256_count; i++)
        if (!strcmp(rt_exefs_sha256[i].file, "main")) sha = rt_exefs_sha256[i].sha256;
    if (!sha || strcmp(sha, "4c33cfd4c818d9ed62822af792782b4ea443417ed24acea6f1cee739cde604c3"))
        rt_fatal("native-inflate: unsupported main hash");
    for (int i = 0; i < count; i++) {
        if (strcmp(mods[i].name, "main")) continue;
        if (mods[i].size < 0x1f36ad0 || mods[i].size > RT_ADDRESS_LIMIT || mods[i].base > RT_ADDRESS_LIMIT - mods[i].size)
            rt_fatal("native-inflate: invalid main module extent");
        hwaoi_inflate.pc = mods[i].base + 0x28708;
        hwaoi_inflate.keys = mods[i].base + 0x1f369d0;
        for (uint64_t j = 0; j < mods[i].count; j++)
            if (mods[i].addrs[j] == hwaoi_inflate.pc) hwaoi_inflate.original = mods[i].fns[j];
    }
    if (!hwaoi_inflate.original) rt_fatal("native-inflate: linked resource job function missing");
    const char *path = getenv("HWAOI_ZLIB_DLL");
    if (!hwaoi_inflate_absolute(path)) rt_fatal("native-inflate: HWAOI_ZLIB_DLL needs an absolute library path");
    void *lib = plat_lib_open(path);
    if (!lib) rt_fatal("native-inflate: cannot open requested zlib library");
    hwaoi_inflate.uncompress = (HwaoiUncompress2)plat_lib_sym(lib, "uncompress2");
    const char *(*version)(void) = (const char *(*)(void))plat_lib_sym(lib, "zlibVersion");
    if (!hwaoi_inflate.uncompress || !version) rt_fatal("native-inflate: cannot load zlib uncompress2/version");
    hwaoi_inflate.mode = mode;
    const char *trace = getenv("HWAOI_NATIVE_INFLATE_TRACE");
    hwaoi_inflate.trace = trace && !strcmp(trace, "1");
    rt_log("native-inflate: %s; zlib %s from %s; unsupported packets retain original guest path",
           mode == 1 ? "VERIFY against lifted job" : "EXPERIMENTAL native 16 KiB jobs", version(), path);
}

static uint64_t hwaoi_inflate_u64(const uint8_t *p) { uint64_t n; memcpy(&n, p, 8); return n; }
static uint32_t hwaoi_inflate_u32(const uint8_t *p) { uint32_t n; memcpy(&n, p, 4); return n; }

// The game's packet has an 8-byte custom checksum before an ordinary zlib
// stream. Main+0x28760 samples at most 24 bytes using a loaded 32-entry key table.
// Bounds below validate every sample independently; arithmetic is unsigned.
static int hwaoi_inflate_checksum(const uint8_t *packet, uint32_t bytes, uint64_t key) {
    if (bytes < 8 || bytes > 65535) return 0;
    uint32_t n = bytes - 8;
    uint64_t sample;
    if (n >= 16) {
        sample = (hwaoi_inflate_u32(packet + 16) ^ (uint32_t)key) & 31u;
        if (sample > n) sample = n;
        sample &= 24u;
    } else sample = n & ~7u;
    uint64_t offset = n >= 8 ? (hwaoi_inflate_u64(packet + 8) ^ key) % n : key ^ n;
    offset &= ~7ull;
    if (offset + sample > n) offset = 0;
    if (sample && (offset > n || sample > n - offset)) return 0;
    for (uint64_t i = 0; i < sample; i += 8) key += hwaoi_inflate_u64(packet + 8 + offset + i);
    return key == hwaoi_inflate_u64(packet);
}

// Windows VirtualQuery scans the protections of a very large contiguous region:
// the real probe measured 49-57 ms per input/output query. For these packets,
// use the runtime's committed heap map, including moved-stack permissions.
// Reject every non-heap region rather than trusting mem_query's generic holes.
static int hwaoi_inflate_heap_access(uint64_t a, uint64_t n, unsigned permission, int trace, const char *role) {
    const RtRange heap = mem_layout()->heap;
    if (!a || a >= RT_ADDRESS_LIMIT || n > RT_ADDRESS_LIMIT - a || a < heap.base ||
        n > heap.size || a - heap.base > heap.size - n) return 0;
    uint64_t start = trace ? kern_now_ns() : 0;
    uint64_t end = a + n;
    while (a < end) {
        RtMemoryInfo info;
        uint32_t result = mem_query(a, &info);
        if (result || (info.perm & permission) != permission || a < info.base ||
            a - info.base >= info.size || info.size > UINT64_MAX - info.base) return 0;
        uint64_t next = info.base + info.size;
        if (next <= a) return 0;
        a = next < end ? next : end;
    }
    if (trace) rt_log("native-inflate: heap %s validation time=%.3f ms", role, (kern_now_ns() - start) / 1e6);
    return 1;
}

static int hwaoi_inflate_keys(void) {
    if (__atomic_load_n(&hwaoi_inflate.keys_ready, __ATOMIC_ACQUIRE)) return 1;
    plat_mutex_lock(&hwaoi_inflate.keys_lock);
    if (!hwaoi_inflate.keys_ready) {
        // Init bounds this exact range to the hash-locked main module. Its
        // readonly table is snapshotted only after the loaded memory is readable.
        if (!plat_mem_readable(hwaoi_inflate.keys, sizeof hwaoi_inflate.key_values)) {
            plat_mutex_unlock(&hwaoi_inflate.keys_lock);
            return 0;
        }
        memcpy(hwaoi_inflate.key_values, (const void *)(uintptr_t)hwaoi_inflate.keys,
               sizeof hwaoi_inflate.key_values);
        __atomic_store_n(&hwaoi_inflate.keys_ready, 1, __ATOMIC_RELEASE);
    }
    plat_mutex_unlock(&hwaoi_inflate.keys_lock);
    return 1;
}

static void hwaoi_inflate_call(Cpu *c) {
    void *host_section = hwaoi_host_section_enter(c);
    uint64_t seq = __atomic_add_fetch(&hwaoi_inflate.entered, 1, __ATOMIC_RELAXED);
    int trace = hwaoi_inflate.trace && (seq <= 64 || !(seq & 4095));
    int query_trace = hwaoi_inflate.trace && seq <= 4;
    uint64_t job = c->x[0];
    if (trace) rt_log("native-inflate: trace %llu tid=%lu job=%#llx begin", (unsigned long long)seq,
                      (unsigned long)plat_thread_id(), (unsigned long long)job);
    if (!hwaoi_inflate_heap_access(job, 0x50, 1, query_trace, "job")) {
        uint64_t n = __atomic_add_fetch(&hwaoi_inflate.fallback, 1, __ATOMIC_RELAXED);
        if (n <= 8 || !(n & 4095)) rt_log("native-inflate: original-path fallback %llu (job region)", (unsigned long long)n);
        hwaoi_host_section_leave(host_section);
        hwaoi_inflate.original(c);
        return;
    }
    uint64_t dst = LD64(job + 0x38), src = LD64(job + 0x40);
    uint32_t bytes = LD32(job + 0x48);
    uint8_t packet[65535], output[0x4000];
    int ok = bytes >= 8 && bytes <= sizeof packet && src && src < RT_ADDRESS_LIMIT &&
             bytes <= RT_ADDRESS_LIMIT - src && hwaoi_inflate_heap_access(src, bytes, 1, query_trace, "source") &&
             hwaoi_inflate_heap_access(dst, sizeof output, 2, query_trace, "output") &&
             !(src < dst + sizeof output && dst < src + bytes) &&
             !(job < dst + sizeof output && dst < job + 0x50) &&
             hwaoi_inflate_keys();
    if (ok) {
        memcpy(packet, (const void *)(uintptr_t)src, bytes);
        uint64_t key = hwaoi_inflate.key_values[(bytes - 8) & 31u];
        ok = hwaoi_inflate_checksum(packet, bytes, key);
    }
    if (trace) rt_log("native-inflate: trace %llu bounds/checksum=%d bytes=%u", (unsigned long long)seq, ok, bytes);
    if (ok) {
        unsigned long out = sizeof output, in = bytes - 8;
        ok = hwaoi_inflate.uncompress(output, &out, packet + 8, &in) == 0 && out == sizeof output;
        // The original one-shot inflate accepts trailing input after stream end.
    }
    if (trace) rt_log("native-inflate: trace %llu decode=%d", (unsigned long long)seq, ok);
    if (!ok) {
        uint64_t n = __atomic_add_fetch(&hwaoi_inflate.fallback, 1, __ATOMIC_RELAXED);
        if (n <= 8 || !(n & 4095)) rt_log("native-inflate: original-path fallback %llu", (unsigned long long)n);
        hwaoi_host_section_leave(host_section);
        hwaoi_inflate.original(c);
        return;
    }
    if (hwaoi_inflate.mode == 1) {
        hwaoi_host_section_leave(host_section);
        hwaoi_inflate.original(c);
        host_section = hwaoi_host_section_enter(c);
        if (trace) rt_log("native-inflate: trace %llu guest-return", (unsigned long long)seq);
        if (c->x[0] || memcmp((const void *)(uintptr_t)dst, output, sizeof output) ||
            memcmp((const void *)(uintptr_t)src, packet, bytes))
            rt_fatal("native-inflate: native/lifted mismatch or input changed, job=%#llx", (unsigned long long)job);
        uint64_t n = __atomic_add_fetch(&hwaoi_inflate.verified, 1, __ATOMIC_RELAXED);
        if (n == 1 || !(n & 4095)) rt_log("native-inflate: verified %llu jobs against lifted output", (unsigned long long)n);
    } else {
        memcpy((void *)(uintptr_t)dst, output, sizeof output);
        c->x[0] = 0;
        uint64_t n = __atomic_add_fetch(&hwaoi_inflate.accelerated, 1, __ATOMIC_RELAXED);
        if (n == 1 || !(n & 4095)) rt_log("native-inflate: accelerated %llu validated jobs", (unsigned long long)n);
    }
    hwaoi_host_section_leave(host_section);
}
