// Native fixture for runtime/save_identity.c: the two platform calls it makes, over the C library, and one line per
// question for tests/hwaoi/test_save_identity.py, which builds the profile folders.
//   save_identity_fixture <profile> [<account folder name>]
#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "platform/platform.h"
#include "save_identity.h"

#ifdef _WIN32
#define entry_stat stat   // no symbolic links are made by the test on Windows
#else
#define entry_stat lstat
#endif

static int kind_of(const char *path) {
    struct stat st;
    if (entry_stat(path, &st)) return PLAT_FS_MISSING;
#ifdef S_ISLNK
    if (S_ISLNK(st.st_mode)) return PLAT_FS_LINK;
#endif
    return S_ISDIR(st.st_mode) ? PLAT_FS_DIR : PLAT_FS_FILE;
}

int plat_fs_kind(const char *path) { return kind_of(path); }

int plat_fs_list(const char *dir, int (*cb)(const char *name, int kind, uint64_t size, void *ctx), void *ctx) {
    DIR *d = opendir(dir);
    if (!d) return PLAT_FS_NOT_FOUND;
    for (struct dirent *e; (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char path[2048];
        if (snprintf(path, sizeof path, "%s/%s", dir, e->d_name) >= (int)sizeof path) continue;
        int kind = kind_of(path);
        if (kind == PLAT_FS_MISSING) continue;
        if (!cb(e->d_name, kind, 0, ctx)) break;
    }
    closedir(d);
    return PLAT_FS_OK;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    uint64_t uid[2] = {0x1111, 0x2222};
    char name[33] = "untouched";
    int n = save_identity_saves(argv[1], uid, name);
    printf("saves %d name %s lo %016" PRIx64 " hi %016" PRIx64 "\n", n, name, uid[0], uid[1]);
    printf("pending %d\n", save_identity_pending(argv[1]));
    if (argc > 2) {
        uint64_t own[2] = {0, 0};
        if (strlen(argv[2]) != 32 || sscanf(argv[2], "%16" SCNx64 "%16" SCNx64, &own[1], &own[0]) != 2) return 2;
        printf("has %d\n", save_identity_has(argv[1], own));
    }
    return 0;
}
