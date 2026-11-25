// Native Windows entry point for the portable compiler. No game data is embedded.
#include <windows.h>
#include <wchar.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR arguments, int show) {
    (void)instance; (void)previous; (void)show;
    wchar_t folder[32768], python[32768], command[32768];
    DWORD n = GetModuleFileNameW(NULL, folder, 32768);
    if (!n || n >= 32768) return 1;
    while (n && folder[n - 1] != L'\\') n--;
    if (!n) return 1;
    folder[n - 1] = 0;
#ifdef HWAOI_BUNDLE_DIR
    // A stand-alone launcher kept apart from the compiler files: built with the folder that holds them.
    if (swprintf(folder, 32768, L"%ls", HWAOI_BUNDLE_DIR) < 0) return 1;
#endif
    int headless = wcsncmp(arguments, L"cli", 3) == 0 || wcsncmp(arguments, L"--play", 6) == 0;
    if (swprintf(python, 32768, L"%ls\\python\\python%s.exe", folder, headless ? L"" : L"w") < 0 ||
        swprintf(command, 32768, L"\"%ls\" -E -s \"%ls\\source\\compiler\\%ls.py\" --bundle \"%ls\" %ls",
                 python, folder, headless ? L"cli" : L"gui", folder, arguments) < 0)
        return 1;
    STARTUPINFOW startup = {.cb = sizeof startup};
    PROCESS_INFORMATION process;
    DWORD flags = headless ? CREATE_NO_WINDOW : 0;
    if (!CreateProcessW(python, command, NULL, NULL, FALSE, flags, NULL, folder, &startup, &process)) {
        MessageBoxW(NULL, L"Extract the complete compiler ZIP, then open AgeOfDecomp-Compiler.exe again.",
                    L"Age of Decomp Compiler", MB_OK | MB_ICONERROR);
        return 1;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
}
