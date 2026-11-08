// Original host-only NVN per-stage binding state. A binding mask changes only
// selected stages, including null/unbind; graphics and compute are independent.
#pragma once
#include <string.h>
#include "nvn_obj.h"

typedef struct {
    const prog_t *source[6], *seen[6];
    uint64_t seen_serial[6];
    prog_t merged;
    uint32_t mask;
    int dirty;
} NvnProgramBindings;

typedef int (*NvnProgramStageLookup)(const prog_t *, unsigned, NVNshaderData *);

// A stage's immutable shader identity comes from its source program, not the
// merged view's new binding-combination serial. Never return a stale source
// before refresh, after SetShaders/reinitialization, or following an unbind.
static inline const prog_t *nvn_program_stage_source(const NvnProgramBindings *s, unsigned stage) {
    if (!s || stage >= 6 || s->dirty) return NULL;
    const prog_t *p = s->source[stage];
    return p && p == s->seen[stage] && p->serial == s->seen_serial[stage] ? p : NULL;
}

static inline int nvn_program_bind(NvnProgramBindings *s, const prog_t *program, uint32_t mask) {
    if (mask & ~63u) return 0;
    for (unsigned st = 0; st < 6; st++) if (mask & (1u << st)) {
        if (s->source[st] != program) s->dirty = 1;
        s->source[st] = program;
    }
    return 1;
}

// Caller serializes replay. Source objects retain ownership of shader controls;
// this view does not own/free them. Reinitialization at the same address is
// detected by serial, preserving the shader/memo cache's identity contract.
// Validate/build a temporary view before committing: failure changes no view.
static inline int nvn_program_refresh(NvnProgramBindings *s, NvnProgramStageLookup lookup,
                                      uint64_t (*new_serial)(void)) {
    int changed = s->dirty;
    for (unsigned st = 0; st < 6; st++) {
        const prog_t *p = s->source[st];
        if (p != s->seen[st] || (p ? p->serial : 0) != s->seen_serial[st]) changed = 1;
    }
    if (!changed) return 0;
    prog_t view = {0};
    uint32_t mask = 0;
    for (unsigned st = 0; st < 6; st++) {
        const prog_t *p = s->source[st];
        if (!p) continue;
        mask |= 1u << st;
        NVNshaderData data;
        int found = lookup(p, st, &data);
        if (found < 0) return -1;
        if (found) view.sd[view.nstage++] = data;
    }
    view.magic = MAGIC('P', 'R', 'O', 'G');
    view.serial = new_serial();
    s->merged = view;
    s->mask = mask;
    s->dirty = 0;
    for (unsigned st = 0; st < 6; st++) {
        s->seen[st] = s->source[st];
        s->seen_serial[st] = s->source[st] ? s->source[st]->serial : 0;
    }
    return 1;
}
