// See vk_ffx_fetch.h (Windows implementation; vk_ffx_fetch.c has the stubs for other systems). The files come from AMD's FidelityFX-SDK repository at a pinned tag, are checked against SHA-256
// sums recorded here, and are written next to the executable only when the sum matches. The download itself is Windows'
// own curl.exe (Windows 10 1803 and later), so no network code is compiled in.
#include "vk_ffx_fetch.h"

#include "host.h"

#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <string.h>

#include "platform/platform.h"

#define SDK_RAW "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/Kits/FidelityFX/signedbin/"

static const struct {
    const char *name, *sha256;
} FILES[] = {
    {"amd_fidelityfx_loader_vk.dll", "38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9"},
    {"amd_fidelityfx_upscaler_vk.dll", "4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7"},
};

static volatile LONG g_state = VK_FFX_FETCH_IDLE;

static int sha256_hex(const char *path, char out[65]) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    unsigned char sum[32], buf[65536];
    int ok = 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) >= 0 &&
        BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) >= 0) {
        size_t n;
        ok = 1;
        while (ok && (n = fread(buf, 1, sizeof buf, f)) > 0) ok = BCryptHashData(h, buf, (ULONG)n, 0) >= 0;
        if (ok) ok = BCryptFinishHash(h, sum, sizeof sum, 0) >= 0;
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    fclose(f);
    if (!ok) return 0;
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", sum[i]);
    return 1;
}

static int have(const char *dir, int i) {
    char p[PLAT_PATH_MAX + 64], hex[65];
    snprintf(p, sizeof p, "%s/%s", dir, FILES[i].name);
    return sha256_hex(p, hex) && !strcmp(hex, FILES[i].sha256);
}

static int download(const char *dir, int i) {
    char dst[PLAT_PATH_MAX + 64], tmp[PLAT_PATH_MAX + 80], hex[65];
    snprintf(dst, sizeof dst, "%s/%s", dir, FILES[i].name);
    snprintf(tmp, sizeof tmp, "%s.part", dst);
    wchar_t sys[MAX_PATH], cmd[2 * PLAT_PATH_MAX + 512], wtmp[PLAT_PATH_MAX + 80], wurl[256];
    if (!GetSystemDirectoryW(sys, MAX_PATH)) return 0;
    if (!MultiByteToWideChar(CP_UTF8, 0, tmp, -1, wtmp, (int)(sizeof wtmp / sizeof wtmp[0]))) return 0;
    char url[256];
    snprintf(url, sizeof url, SDK_RAW "%s", FILES[i].name);
    MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, (int)(sizeof wurl / sizeof wurl[0]));
    swprintf(cmd, sizeof cmd / sizeof cmd[0], L"\"%ls\\curl.exe\" -fsSL --retry 2 -o \"%ls\" \"%ls\"", sys, wtmp, wurl);
    STARTUPINFOW si = {.cb = sizeof si};
    PROCESS_INFORMATION pi;
    rt_log("fsr: downloading %s from the FidelityFX SDK repository", FILES[i].name);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        rt_log("fsr: cannot start curl.exe (error %lu)", GetLastError());
        return 0;
    }
    WaitForSingleObject(pi.hProcess, 300000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (code != 0 || !sha256_hex(tmp, hex) || strcmp(hex, FILES[i].sha256)) {
        rt_log("fsr: download of %s failed (curl exit %lu, or the SHA-256 differs from the pinned one)", FILES[i].name,
               (unsigned long)code);
        remove(tmp);
        return 0;
    }
    remove(dst);
    if (rename(tmp, dst)) {
        remove(tmp);
        return 0;
    }
    return 1;
}

int vk_ffx_fetch_blocking(void) {
    char exe[PLAT_PATH_MAX];
    if (!plat_exe_path(exe, sizeof exe)) return 0;
    char *last = plat_last_sep(exe);
    if (!last) return 0;
    *last = 0;
    InterlockedExchange(&g_state, VK_FFX_FETCH_RUNNING);
    int ok = 1;
    for (int i = 0; i < 2 && ok; i++)
        if (!have(exe, i)) ok = download(exe, i);
    InterlockedExchange(&g_state, ok ? VK_FFX_FETCH_DONE : VK_FFX_FETCH_FAILED);
    if (ok) rt_log("fsr: the FSR DLLs are in place (FidelityFX SDK 2.4.0)");
    return ok;
}

static void fetch_thread(void *arg) {
    (void)arg;
    vk_ffx_fetch_blocking();
}

void vk_ffx_fetch_async(void) {
    if (InterlockedCompareExchange(&g_state, VK_FFX_FETCH_RUNNING, VK_FFX_FETCH_IDLE) != VK_FFX_FETCH_IDLE) return;
    if (!plat_thread_spawn(fetch_thread, NULL, "ffx-fetch", 0)) InterlockedExchange(&g_state, VK_FFX_FETCH_FAILED);
}

int vk_ffx_fetch_state(void) { return (int)g_state; }
