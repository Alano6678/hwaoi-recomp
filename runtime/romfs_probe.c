// HWAoI read-only probe of the real RomFS storage, including offsets above 4 GiB.
#include <stdio.h>
#include <stdint.h>
#include "fs.h"
#include "host.h"
#include "romfs_config.h"

int probe_romfs_storage(void) {
    fs_init();
    if (romfs_mount("rom")) return 1;
    uint64_t count = romfs_file_count();
    uint64_t samples[3] = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
    uint64_t small[2] = {UINT64_MAX, UINT64_MAX}, highest = 0;
    for (uint64_t i = 0; i < count; i++) {
        uint64_t offset, size;
        if (!romfs_entry(i, &offset, &size) || !size) continue;
        int storage = 0;
#ifdef ROMFS_NUP_MODE
        storage = offset >= ROMFS_NUP_SIZE;
#endif
        if (size < small[storage]) small[storage] = size, samples[storage] = i;
        if (offset > highest) highest = offset, samples[2] = i;
    }
    unsigned checked = 0;
    for (int k = 0; k < 3; k++) {
        uint64_t i = samples[k];
        if (i == UINT64_MAX) continue;
        int duplicate = 0;
        for (int j = 0; j < k; j++) if (samples[j] == i) duplicate = 1;
        if (duplicate) continue;
        uint64_t offset, size;
        const char *name = romfs_entry(i, &offset, &size);
        char path[4096];
        if (!name || snprintf(path, sizeof path, "rom:/%s", name) >= (int)sizeof path) return 1;
        uint64_t handle = 0, got = 0;
        int64_t measured = -1;
        uint8_t buffer[64];
        if (romfs_open(&handle, path, FS_OPEN_READ) || romfs_size(&measured, handle) || (uint64_t)measured != size) return 1;
        uint64_t want = size < sizeof buffer ? size : sizeof buffer;
        if (romfs_read(&got, handle, 0, buffer, want) || got != want) return 1;
        if (romfs_read(&got, handle, (int64_t)size, buffer, sizeof buffer) || got != 0) return 1;
        romfs_close(handle);
        printf("RomFS storage probe: %s, %llu bytes, logical offset %#llx: ok\n", name,
               (unsigned long long)size, (unsigned long long)offset);
        checked++;
    }
    printf("RomFS storage probe: %u files checked; %llu indexed files\n", checked, (unsigned long long)count);
    return checked ? 0 : 1;
}
