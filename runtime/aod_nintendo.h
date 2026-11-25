// Age of Decomp integrated Nintendo Bluetooth transport. GPL-3.0-or-later.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#if defined(_WIN32) && defined(AOD_NINTENDO_DLL)
#define AOD_NINTENDO_API __declspec(dllexport)
#else
#define AOD_NINTENDO_API
#endif
#ifdef __cplusplus
extern "C" {
#endif
AOD_NINTENDO_API bool aod_nintendo_start(const char *helper, bool protocol_test);
AOD_NINTENDO_API void aod_nintendo_tick(void);
AOD_NINTENDO_API void aod_nintendo_stop(void);
#if defined(_WIN32) && defined(AOD_NINTENDO_DLL)
AOD_NINTENDO_API bool aod_nintendo_lend(uintptr_t handles[3]);
#endif
#ifdef __cplusplus
}
#endif
