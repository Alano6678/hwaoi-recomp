// Original opt-in diagnostic controller. Commands expire on the host clock;
// malformed/stale updates never refresh a held input. No game memory is patched.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <ctype.h>

typedef struct { uint64_t sequence, until, buttons; int32_t axis[4]; unsigned fps, width, height; } InputFeed;

static inline int input_feed_accept(InputFeed *state, const char *line, uint64_t now) {
    uint64_t fields[10] = {0};
    const char *p = line;
    for (int i = 0; i < 10; i++) {
        while (isspace((unsigned char)*p)) p++;
        if (i == 7 && !*p) break; // original seven-field input-only protocol
        if (!*p || (*p == '-' && (i < 2 || i >= 6))) return 0;
        char *end; errno = 0;
        if (i >= 2 && i < 6) {
            long long v = strtoll(p, &end, 10);
            if (errno || end == p || v < -32767 || v > 32767) return 0;
            fields[i] = (uint64_t)v;
        } else {
            fields[i] = strtoull(p, &end, i == 1 ? 16 : 10);
            if (errno || end == p) return 0;
        }
        if (*end && !isspace((unsigned char)*end)) return 0;
        p = end;
    }
    while (isspace((unsigned char)*p)) p++;
    if (*p || !fields[0] || fields[0] <= state->sequence || fields[1] > 0xffff || fields[6] > 10000) return 0;
    if (fields[7] && fields[7] != 30 && fields[7] != 60 && fields[7] != 90 && fields[7] != 120) return 0;
    if ((fields[8] || fields[9]) && (fields[8] < 640 || fields[8] > 3840 || fields[9] < 360 || fields[9] > 2160)) return 0;
    uint64_t duration = fields[6] * UINT64_C(1000000);
    if (duration > UINT64_MAX - now) return 0;
    state->sequence = fields[0]; state->buttons = fields[1]; state->until = now + duration;
    for (int i = 0; i < 4; i++) state->axis[i] = (int32_t)(int64_t)fields[i + 2];
    state->fps = (unsigned)fields[7]; state->width = (unsigned)fields[8]; state->height = (unsigned)fields[9];
    return 1;
}

static inline void input_feed_merge(const InputFeed *state, uint64_t now, uint64_t *buttons, int32_t l[2], int32_t r[2]) {
    if (now >= state->until) return;
    *buttons |= state->buttons;
    for (int i = 0; i < 4; i++) {
        int32_t *dst = i < 2 ? &l[i] : &r[i - 2];
        int32_t v = state->axis[i];
        if (llabs((long long)v) > llabs((long long)*dst)) *dst = v;
    }
}
