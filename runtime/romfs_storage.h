// HWAoI virtual storage routing. Physical source files are read-only.
#pragma once
#include <stdint.h>
static inline int romfs_storage_route(uint64_t source_size, uint64_t cache_size,
                                      uint64_t logical, uint32_t requested,
                                      int *cache, uint64_t *physical, uint32_t *amount) {
    if (cache_size > UINT64_MAX - source_size) return 0;
    uint64_t total = source_size + cache_size;
    if (logical > total) return 0;
    *cache = logical >= source_size;
    *physical = *cache ? logical - source_size : logical;
    uint64_t available = *cache ? total - logical : source_size - logical;
    *amount = available < requested ? (uint32_t)available : requested;
    return 1;
}
