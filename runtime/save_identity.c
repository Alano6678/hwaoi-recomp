// The account of the save data in a profile folder (save_identity.h). A player who copies the save folder of another
// game folder into this one, or whose account.uid is gone, has a save here that the account of this start does not
// own: the game would begin anew beside it. load_uid asks here before it makes or keeps such an account.
#include "save_identity.h"

#include <stdio.h>
#include <string.h>

#include "platform/platform.h"

#define PATH_CAP 1100
#define MAX_DEPTH 32   // a save is a few levels deep; the bound keeps a damaged folder from being walked for ever

typedef struct {
    const char *dir;
    int depth, found;
} Walk;

static int holds_a_file(const char *dir, int depth);

static int walk_entry(const char *name, int kind, uint64_t size, void *ctx) {
    Walk *w = ctx;
    (void)size;
    char path[PATH_CAP];
    if (kind == PLAT_FS_FILE) w->found = 1;
    else if (kind == PLAT_FS_DIR && w->depth < MAX_DEPTH &&
             snprintf(path, sizeof path, "%s/%s", w->dir, name) < (int)sizeof path)
        w->found = holds_a_file(path, w->depth + 1);
    return !w->found;   // 0 stops the listing
}

// A file at any depth under dir. Links (symbolic links, reparse points) count as nothing and are not entered.
static int holds_a_file(const char *dir, int depth) {
    Walk w = {dir, depth, 0};
    plat_fs_list(dir, walk_entry, &w);
    return w.found;
}

// The id an account folder's name stands for: 32 lower-case hex digits, the high half first, as fs_host.c writes it.
// 0 for any other name and for the id 0, which is no account.
static int uid_of_name(const char *name, uint64_t uid[2]) {
    uint64_t half[2] = {0, 0};
    if (strlen(name) != 32) return 0;
    for (int i = 0; i < 32; i++) {
        char c = name[i];
        int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (digit < 0) return 0;
        half[i / 16] = half[i / 16] << 4 | (uint64_t)digit;
    }
    if (!half[0] && !half[1]) return 0;
    uid[0] = half[1], uid[1] = half[0];
    return 1;
}

int save_identity_has(const char *profile, const uint64_t uid[2]) {
    char path[PATH_CAP];
    if (snprintf(path, sizeof path, "%s/save/%016llx%016llx", profile, (unsigned long long)uid[1],
                 (unsigned long long)uid[0]) >= (int)sizeof path)
        return 0;
    return plat_fs_kind(path) == PLAT_FS_DIR && holds_a_file(path, 0);
}

typedef struct {
    const char *dir;
    int found;
} Pending;

static int pending_entry(const char *name, int kind, uint64_t size, void *ctx) {
    Pending *p = ctx;
    (void)size;
    if (kind != PLAT_FS_DIR) return 1;
    int commit = !strcmp(name, "new") || !strcmp(name, "old");
    if (!commit && !strncmp(name, "ready-", 6) && name[6]) {
        commit = 1;
        for (const char *c = name + 6; *c; c++)
            if (*c < '0' || *c > '9') commit = 0;
    }
    char path[PATH_CAP];
    if (commit && snprintf(path, sizeof path, "%s/%s", p->dir, name) < (int)sizeof path) p->found = holds_a_file(path, 0);
    return !p->found;
}

int save_identity_pending(const char *profile) {
    char dir[PATH_CAP];
    if (snprintf(dir, sizeof dir, "%s/save-tx", profile) >= (int)sizeof dir) return 0;
    Pending p = {dir, 0};
    plat_fs_list(dir, pending_entry, &p);
    return p.found;
}

typedef struct {
    const char *dir;
    int n;
    uint64_t uid[2];
    char name[33];
} Saves;

static int saves_entry(const char *name, int kind, uint64_t size, void *ctx) {
    Saves *s = ctx;
    (void)size;
    uint64_t uid[2];
    char path[PATH_CAP];
    if (kind != PLAT_FS_DIR || !uid_of_name(name, uid)) return 1;
    if (snprintf(path, sizeof path, "%s/%s", s->dir, name) >= (int)sizeof path || !holds_a_file(path, 0)) return 1;
    if (!s->n++) {
        s->uid[0] = uid[0], s->uid[1] = uid[1];
        memcpy(s->name, name, sizeof s->name);
    }
    return 1;
}

int save_identity_saves(const char *profile, uint64_t uid[2], char name[33]) {
    char dir[PATH_CAP];
    if (snprintf(dir, sizeof dir, "%s/save", profile) >= (int)sizeof dir) return 0;
    Saves s = {.dir = dir};
    plat_fs_list(dir, saves_entry, &s);
    if (s.n == 1) {
        uid[0] = s.uid[0], uid[1] = s.uid[1];
        memcpy(name, s.name, sizeof s.name);
    }
    return s.n;
}
