// External, read-only thread snapshot for a selected local Windows runtime.
// Each thread is resumed before symbol loading, unwinding, formatting or I/O.
#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define STACK_BYTES (256u << 10)
#define MAX_DEPTH 64
static BYTE stack_copy[STACK_BYTES];
static DWORD64 stack_start, stack_end, stack_allocation;
static HANDLE target;

static void string(const WCHAR *s) {
    char u[32768 * 3];
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, u, sizeof u, NULL, NULL);
    putchar('"');
    if (n) for (int i = 0; i < n - 1; i++) {
        unsigned char c = (unsigned char)u[i];
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c < 32) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

// StackWalk64 runs only after ResumeThread. Never read the live stack: it is
// changing now. Module .pdata/.xdata may be read remotely for unwind metadata.
static BOOL CALLBACK read_memory(HANDLE process, DWORD64 address, PVOID out, DWORD size, LPDWORD got) {
    *got = 0;
    if (address >= stack_start && address <= stack_end && size <= stack_end - address) {
        memcpy(out, stack_copy + (size_t)(address - stack_start), size);
        *got = size;
        return TRUE;
    }
    MEMORY_BASIC_INFORMATION mb;
    if (!VirtualQueryEx(process, (void *)(uintptr_t)address, &mb, sizeof mb) ||
        (DWORD64)(uintptr_t)mb.AllocationBase == stack_allocation) return FALSE;
    SIZE_T n = 0;
    BOOL ok = ReadProcessMemory(process, (void *)(uintptr_t)address, out, size, &n);
    *got = (DWORD)n;
    return ok;
}

static void print_modules(DWORD pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    DWORD inventory_error = snap == INVALID_HANDLE_VALUE ? GetLastError() : 0;
    printf("\"modules\":[");
    int first = 1;
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W m = {.dwSize = sizeof m};
        for (BOOL ok = Module32FirstW(snap, &m); ok; ok = Module32NextW(snap, &m)) {
            if (!first) putchar(',');
            first = 0;
            printf("{\"base\":\"0x%llx\",\"size\":%lu,\"path\":", (unsigned long long)(uintptr_t)m.modBaseAddr, m.modBaseSize);
            string(m.szExePath);
            printf("}");
            SymLoadModuleExW(target, NULL, m.szExePath, NULL, (DWORD64)(uintptr_t)m.modBaseAddr, m.modBaseSize, NULL, 0);
        }
        CloseHandle(snap);
    }
    printf("],\"module_inventory_error\":%lu,", inventory_error);
}

static void snapshot_thread(DWORD tid) {
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    printf("{\"tid\":%lu", tid);
    if (!thread) { printf(",\"error\":\"OpenThread\",\"winerror\":%lu}", GetLastError()); return; }
    if (GetProcessIdOfThread(thread) != GetProcessId(target)) {
        printf(",\"error\":\"thread process identity changed\"}"); CloseHandle(thread); return;
    }
    WCHAR *description = NULL;
    typedef HRESULT (WINAPI *GetDescription)(HANDLE, PWSTR *);
    GetDescription describe = (GetDescription)(uintptr_t)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
    if (describe && SUCCEEDED(describe(thread, &description)) && description) {
        printf(",\"name\":"); string(description); LocalFree(description);
    }
    ULONG64 cycles = 0;
    QueryThreadCycleTime(thread, &cycles);
    CONTEXT context __attribute__((aligned(16))) = {0};
    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    stack_start = stack_end = stack_allocation = 0;
    DWORD context_error = 0, stack_error = 0, resume_error = 0;
    LARGE_INTEGER begin, end, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&begin);
    DWORD previous = SuspendThread(thread);
    if (previous == (DWORD)-1) {
        printf(",\"error\":\"SuspendThread\",\"winerror\":%lu}", GetLastError()); CloseHandle(thread); return;
    }
    // Only OS query/read calls while stopped. Every success of SuspendThread
    // has exactly one matching ResumeThread, including all read failure paths.
    BOOL have_context = GetThreadContext(thread, &context);
    if (!have_context) context_error = GetLastError();
    MEMORY_BASIC_INFORMATION mb;
    if (have_context && VirtualQueryEx(target, (void *)(uintptr_t)context.Rsp, &mb, sizeof mb) &&
        mb.State == MEM_COMMIT && !(mb.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
        stack_start = context.Rsp;
        stack_allocation = (DWORD64)(uintptr_t)mb.AllocationBase;
        SIZE_T size = (uintptr_t)mb.BaseAddress + mb.RegionSize - (uintptr_t)context.Rsp;
        if (size > STACK_BYTES) size = STACK_BYTES;
        SIZE_T copied = 0;
        if (!ReadProcessMemory(target, (void *)(uintptr_t)stack_start, stack_copy, size, &copied)) stack_error = GetLastError();
        stack_end = stack_start + copied;
    } else if (have_context) stack_error = GetLastError() ? GetLastError() : ERROR_NOACCESS;
    if (ResumeThread(thread) == (DWORD)-1) resume_error = GetLastError();
    QueryPerformanceCounter(&end);
    printf(",\"cycles\":\"%llu\",\"previous_suspend_count\":%lu,\"capture_ms\":%.3f,\"context_error\":%lu,\"stack_error\":%lu,\"resume_error\":%lu",
           (unsigned long long)cycles, previous, (end.QuadPart - begin.QuadPart) * 1000.0 / freq.QuadPart,
           context_error, stack_error, resume_error);
    if (!have_context || resume_error) { printf("}"); CloseHandle(thread); return; }
    printf(",\"stack_start\":\"0x%llx\",\"stack_bytes\":%llu,\"registers\":{",
           (unsigned long long)stack_start, (unsigned long long)(stack_end - stack_start));
#define REG(name, field) printf("\"" name "\":\"0x%llx\",", (unsigned long long)context.field)
    REG("rax", Rax); REG("rbx", Rbx); REG("rcx", Rcx); REG("rdx", Rdx); REG("rsi", Rsi); REG("rdi", Rdi);
    REG("rbp", Rbp); REG("rsp", Rsp); REG("r8", R8); REG("r9", R9); REG("r10", R10); REG("r11", R11);
    REG("r12", R12); REG("r13", R13); REG("r14", R14); REG("r15", R15);
#undef REG
    printf("\"rip\":\"0x%llx\"},\"frames\":[{\"pc\":\"0x%llx\",\"sp\":\"0x%llx\"}",
           (unsigned long long)context.Rip, (unsigned long long)context.Rip, (unsigned long long)context.Rsp);
    STACKFRAME64 frame = {0};
    frame.AddrPC.Offset = context.Rip; frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp; frame.AddrStack.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
    DWORD64 prev_pc = context.Rip, prev_sp = context.Rsp;
    unsigned frames = 1;
    if (stack_end > stack_start) for (int n = 0; n < MAX_DEPTH; n++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, target, thread, &frame, &context, read_memory,
                         SymFunctionTableAccess64, SymGetModuleBase64, NULL) || !frame.AddrPC.Offset) break;
        if (frame.AddrPC.Offset == prev_pc && frame.AddrStack.Offset == prev_sp) { if (n) break; else continue; }
        printf(",{\"pc\":\"0x%llx\",\"sp\":\"0x%llx\"}",
               (unsigned long long)frame.AddrPC.Offset, (unsigned long long)frame.AddrStack.Offset);
        prev_pc = frame.AddrPC.Offset; prev_sp = frame.AddrStack.Offset; frames++;
    }
    printf("],\"depth_limit_reached\":%s}", frames >= MAX_DEPTH ? "true" : "false");
    CloseHandle(thread);
}

int wmain(int argc, WCHAR **argv) {
    if (argc != 3 || sizeof(void *) != 8) { fputs("usage: snapshot PID EXPECTED_EXE\n", stderr); return 2; }
    WCHAR *end;
    unsigned long number = wcstoul(argv[1], &end, 10);
    if (!number || *end || number == GetCurrentProcessId()) { fputs("invalid PID\n", stderr); return 2; }
    target = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, (DWORD)number);
    if (!target) { fprintf(stderr, "OpenProcess: %lu\n", GetLastError()); return 2; }
    WCHAR actual[32768]; DWORD length = sizeof actual / sizeof *actual;
    BOOL wow = FALSE;
    if (!QueryFullProcessImageNameW(target, 0, actual, &length) || _wcsicmp(actual, argv[2]) ||
        !IsWow64Process(target, &wow) || wow) {
        fputs("process executable identity/architecture mismatch\n", stderr); CloseHandle(target); return 2;
    }
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS | SYMOPT_IGNORE_CVREC);
    if (!SymInitializeW(target, L"", FALSE)) { fprintf(stderr, "SymInitialize: %lu\n", GetLastError()); CloseHandle(target); return 2; }
    FILETIME created = {0}, exited = {0}, kernel = {0}, user = {0};
    if (!GetProcessTimes(target, &created, &exited, &kernel, &user)) {
        fprintf(stderr, "GetProcessTimes: %lu\n", GetLastError()); SymCleanup(target); CloseHandle(target); return 2;
    }
    printf("{\"pid\":%lu,\"creation_filetime\":\"%llu\",", (DWORD)number,
           (unsigned long long)(((uint64_t)created.dwHighDateTime << 32) | created.dwLowDateTime));
    print_modules((DWORD)number);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    DWORD inventory_error = snap == INVALID_HANDLE_VALUE ? GetLastError() : 0;
    printf("\"threads\":[");
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te = {.dwSize = sizeof te}; int first = 1;
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != number) continue;
            if (!first) putchar(','); first = 0;
            snapshot_thread(te.th32ThreadID);
            fflush(stdout);
        }
        CloseHandle(snap);
    }
    printf("],\"thread_inventory_error\":%lu}\n", inventory_error);
    SymCleanup(target); CloseHandle(target);
    return 0;
}
