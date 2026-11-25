// Private pipe transport -> SDL virtual gamepad, shared by the game and compiler.
// No system-wide controller emulation or additional device driver is installed.
#include "aod_nintendo.h"
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <SDL3/SDL.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AOD_NINTENDO_DLL
#define API __declspec(dllexport)
#else
#define API
#endif
static HANDLE process, input, output, accepted;
static SDL_JoystickID joystick_id;
static SDL_Joystick *joystick;
static Uint64 last_report;
static bool tried;

static bool SDLCALL rumble(void *unused, Uint16 low, Uint16 high);

static void detach(void) {
    if (joystick) SDL_CloseJoystick(joystick);
    joystick = NULL;
    if (joystick_id) SDL_DetachVirtualJoystick(joystick_id);
    joystick_id = 0;
}

API void aod_nintendo_stop(void) {
    if (output && !process) (void)rumble(NULL, 0, 0);
    detach();
    if (input) CloseHandle(input); // EOF tells the helper to stop scanning, stop rumble and release GATT.
    if (output) CloseHandle(output);
    if (accepted) CloseHandle(accepted);
    if (process) {
        WaitForSingleObject(process, 2000);
        CloseHandle(process);
    }
    input = output = process = accepted = NULL;
    tried = false;
}

static void discard_pending(void) {
    DWORD available;
    if (!PeekNamedPipe(output, NULL, 0, NULL, &available, NULL)) return;
    // Discard complete packets sent while this process did not own the controller.
    unsigned char buffer[4096];
    available -= available % 64;
    while (available) {
        DWORD read, count = available < sizeof buffer ? available : sizeof buffer;
        if (!ReadFile(output, buffer, count, &read, NULL) || !read) return;
        available -= read;
    }
}

#ifdef AOD_NINTENDO_DLL
API bool aod_nintendo_lend(uintptr_t handles[3]) {
    detach();
    if (!process || WaitForSingleObject(process, 0) != WAIT_TIMEOUT) return false;
    (void)rumble(NULL, 0, 0);
    if (!accepted) accepted = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!accepted || !ResetEvent(accepted)) return false;
    handles[0] = (uintptr_t)input;
    handles[1] = (uintptr_t)output;
    handles[2] = (uintptr_t)accepted;
    return true;
}
#endif

static bool adopt(void) {
    const char *value = getenv("AOD_NINTENDO_HANDOFF");
    if (!value) return false;
    unsigned long parent_id;
    unsigned long long handles[3];
    char extra;
    if (sscanf(value, "%lu:%llx:%llx:%llx%c", &parent_id, &handles[0], &handles[1], &handles[2], &extra) != 4)
        return false;

    // Only take handles from the process that started this game.
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry = {0};
    entry.dwSize = sizeof entry;
    BOOL exists = Process32FirstW(snapshot, &entry);
    while (exists && entry.th32ProcessID != GetCurrentProcessId()) exists = Process32NextW(snapshot, &entry);
    CloseHandle(snapshot);
    if (!exists || entry.th32ParentProcessID != parent_id) return false;

    HANDLE parent = OpenProcess(PROCESS_DUP_HANDLE, FALSE, parent_id);
    if (!parent) return false;
    HANDLE copies[3] = {0};
    bool ok = true;
    for (int i = 0; i < 3 && ok; i++)
        ok = DuplicateHandle(parent, (HANDLE)(uintptr_t)handles[i], GetCurrentProcess(), &copies[i],
                             0, FALSE, DUPLICATE_SAME_ACCESS);
    CloseHandle(parent);
    if (ok)
        ok = GetFileType(copies[0]) == FILE_TYPE_PIPE && GetFileType(copies[1]) == FILE_TYPE_PIPE &&
             PeekNamedPipe(copies[1], NULL, 0, NULL, NULL, NULL) && SetEvent(copies[2]);
    if (ok) {
        input = copies[0];
        output = copies[1];
        discard_pending();
        fprintf(stderr, "[Nintendo Bluetooth] Reusing the compiler's controller connection.\n");
    } else {
        for (int i = 0; i < 2; i++) if (copies[i]) CloseHandle(copies[i]);
    }
    if (copies[2]) CloseHandle(copies[2]);
    return ok;
}

API bool aod_nintendo_start(const char *helper, bool test) {
    if (output) { discard_pending(); return true; }
    if (tried) return false;
    tried = true;
    if (!test && adopt()) return true;
    if (!helper || !*helper) return false;
    wchar_t path[32768], command[32768];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, helper, -1, path, 32768) ||
        GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES ||
        wcschr(path, L'"') || swprintf(command, 32768, L"\"%ls\"%ls", path, test ? L" --protocol-test" : L"") < 0)
        return false;
    HANDLE child_in = NULL, child_out = NULL, child_err = NULL;
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    STARTUPINFOEXW si = {0};
    PROCESS_INFORMATION pi = {0};
    bool ok = false;
    bool attributes_ready = false;
    SIZE_T bytes = 0;
    if (!CreatePipe(&child_in, &input, &sa, 65536) || !CreatePipe(&output, &child_out, &sa, 65536)) goto done;
    SetHandleInformation(input, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(output, HANDLE_FLAG_INHERIT, 0);
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (!err || err == INVALID_HANDLE_VALUE ||
        !DuplicateHandle(GetCurrentProcess(), err, GetCurrentProcess(), &child_err, 0, TRUE, DUPLICATE_SAME_ACCESS))
        child_err = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    HANDLE handles[] = {child_in, child_out, child_err};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = child_in;
    si.StartupInfo.hStdOutput = child_out;
    si.StartupInfo.hStdError = child_err;
    InitializeProcThreadAttributeList(NULL, 1, 0, &bytes);
    si.lpAttributeList = HeapAlloc(GetProcessHeap(), 0, bytes);
    if (!si.lpAttributeList || !InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &bytes)) goto done;
    attributes_ready = true;
    if (!UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof handles, NULL, NULL)) goto done;
    if (!CreateProcessW(path, command, NULL, NULL, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                        NULL, NULL, &si.StartupInfo, &pi)) goto done;
    process = pi.hProcess;
    CloseHandle(pi.hThread);
    ok = true;
done:
    if (si.lpAttributeList) {
        if (attributes_ready) DeleteProcThreadAttributeList(si.lpAttributeList);
        HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
    }
    if (child_in) CloseHandle(child_in);
    if (child_out) CloseHandle(child_out);
    if (child_err && child_err != INVALID_HANDLE_VALUE) CloseHandle(child_err);
    if (!ok) { aod_nintendo_stop(); tried = true; }
    return ok;
}

static bool SDLCALL rumble(void *unused, Uint16 low, Uint16 high) {
    (void)unused;
    if (!input) return false;
    unsigned char b[16] = "AODRUM01";
    memcpy(b + 10, &low, 2);
    memcpy(b + 12, &high, 2);
    DWORD written;
    return WriteFile(input, b, sizeof b, &written, NULL) && written == sizeof b;
}
static bool SDLCALL sensors(void *unused, bool enabled) { (void)unused; (void)enabled; return true; }

static bool attach(void) {
    SDL_VirtualJoystickSensorDesc sensors_desc[] = {{SDL_SENSOR_GYRO, 250}, {SDL_SENSOR_ACCEL, 250}};
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id = 0x057e; desc.product_id = 0x2069;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = 16;
    desc.button_mask = 0xffff;
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    desc.nsensors = 2; desc.sensors = sensors_desc;
    desc.name = "Nintendo Switch 2 Pro Controller (Bluetooth)";
    desc.Rumble = rumble;
    desc.SetSensorsEnabled = sensors;
    joystick_id = SDL_AttachVirtualJoystick(&desc);
    if (!joystick_id || !(joystick = SDL_OpenJoystick(joystick_id))) { detach(); return false; }
    // Explicit physical positions: SDL labels and auto layout must remain Nintendo.
    SDL_GUID guid = SDL_GetJoystickGUIDForID(joystick_id);
    char guid_text[33], mapping[512];
    SDL_GUIDToString(guid, guid_text, sizeof guid_text);
    snprintf(mapping, sizeof mapping, "%s,Nintendo Switch 2 Pro Controller (Bluetooth),"
        "a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,start:b6,leftstick:b7,rightstick:b8,"
        "leftshoulder:b9,rightshoulder:b10,dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,misc1:b15,"
        "leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,type:nintendoswitchpro,", guid_text);
    SDL_AddGamepadMapping(mapping);
    return true;
}

static bool apply(const unsigned char b[64]) {
    uint16_t product; memcpy(&product, b + 10, 2);
    if (memcmp(b, "AODPAD01", 8) || b[8] != 0 || b[9] > 1 || product != 0x2069) return false;
    if (!b[9]) { detach(); return true; }
    float values[10]; memcpy(values, b + 20, sizeof values);
    for (int i = 0; i < 10; i++) if (!isfinite(values[i]) || fabsf(values[i]) > 1000) return false;
    if (!joystick && !attach()) return false;
    uint32_t buttons; memcpy(&buttons, b + 16, 4);
    // Nintendo report bits -> SDL physical positions, never Xbox-remapped in the transport.
    const uint32_t masks[] = {4, 8, 1, 2, 0x100, 0x1000, 0x200, 0x800, 0x400,
                              0x400000, 0x40, 0x20000, 0x10000, 0x80000, 0x40000, 0x2000};
    for (int i = 0; i < 16; i++) SDL_SetJoystickVirtualButton(joystick, i, (buttons & masks[i]) != 0);
    for (int i = 0; i < 4; i++) {
        float v = fmaxf(-1, fminf(1, values[i])) * (i & 1 ? -1 : 1);
        SDL_SetJoystickVirtualAxis(joystick, i, (Sint16)lroundf(v * 32767));
    }
    SDL_SetJoystickVirtualAxis(joystick, 4, buttons & 0x800000 ? 32767 : -32768);
    SDL_SetJoystickVirtualAxis(joystick, 5, buttons & 0x80 ? 32767 : -32768);
    Uint64 timestamp = SDL_GetTicksNS();
    SDL_SendJoystickVirtualSensorData(joystick, SDL_SENSOR_GYRO, timestamp, values + 4, 3);
    SDL_SendJoystickVirtualSensorData(joystick, SDL_SENSOR_ACCEL, timestamp, values + 7, 3);
    last_report = SDL_GetTicks();
    return true;
}

API void aod_nintendo_tick(void) {
    if (!output) return;
    for (int n = 0; n < 256; n++) {
        DWORD available;
        if (!PeekNamedPipe(output, NULL, 0, NULL, &available, NULL)) { aod_nintendo_stop(); tried = true; return; }
        if (available < 64) break;
        unsigned char b[64]; DWORD read;
        if (!ReadFile(output, b, sizeof b, &read, NULL) || read != sizeof b || !apply(b)) {
            fprintf(stderr, "[Nintendo Bluetooth] Invalid transport packet; controller released.\n");
            aod_nintendo_stop(); tried = true; return;
        }
    }
    // A stalled transport must never leave a held button or stick in the game.
    if (joystick && SDL_GetTicks() - last_report > 3000) detach();
    if (process && WaitForSingleObject(process, 0) == WAIT_OBJECT_0) { aod_nintendo_stop(); tried = true; }
}
#else
bool aod_nintendo_start(const char *helper, bool test) { (void)helper; (void)test; return false; }
void aod_nintendo_tick(void) {}
void aod_nintendo_stop(void) {}
#endif
