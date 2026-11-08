// Original GPU-side adapter for array QUADS indirect draws.
#pragma once
#include "vk_exec.h"
typedef struct { VkBuffer indices, arguments; VkDeviceSize offset; } VkQuadIndirect;
// Replay thread only. Source span is already resolved/synchronized by the caller.
// Zero means no converter commands were recorded; the caller can use its CPU fallback.
int vk_quad_indirect_prepare(VkExec *ex, const VkMemSpan *source, uint64_t source_address, VkQuadIndirect *out);
void vk_quad_shutdown(void); // after submission completion callbacks have drained
