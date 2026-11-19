// Stubs for systems without AMD's Vulkan DLLs; Windows has vk_ffx_fetch_win.c.
#ifndef _WIN32
#include "vk_ffx_fetch.h"

int vk_ffx_fetch_blocking(void) { return 0; }
void vk_ffx_fetch_async(void) {}
int vk_ffx_fetch_state(void) { return VK_FFX_FETCH_IDLE; }
#endif
