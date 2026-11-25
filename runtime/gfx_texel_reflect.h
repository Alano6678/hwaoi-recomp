// SPDX-License-Identifier: MIT
// Resource numeric types come from SPIR-V, including cached translator output.
// A null texel-buffer view still has to match OpTypeImage's sampled scalar type.
#pragma once
static int gfx_reflect_texel_types(const uint32_t *words, uint64_t count, GfxShaderRes *resources, int nresources) {
    int needed = 0;
    for (int i = 0; i < nresources; i++) {
        resources[i].sampled_type = 0;
        needed |= (resources[i].type & 255) == 2;
    }
    if (!needed) return 1;
    if (!words || count < 5 || words[0] != UINT32_C(0x07230203) || !words[3] || words[3] > (1u << 20)) return 0;
    uint32_t bound = words[3];
    uint32_t *links = calloc(bound, sizeof *links), *bindings = calloc(bound, sizeof *bindings), *sets = calloc(bound, sizeof *sets);
    if (!links || !bindings || !sets) { free(links); free(bindings); free(sets); return 0; }
    enum { IMAGE = 1u << 30, SCALAR = 1u << 29, ID_MASK = (1u << 20) - 1, DECORATED = 1u << 16 };
    int ok = 1;
    for (uint64_t i = 5; i < count;) {
        uint32_t length = words[i] >> 16, op = words[i] & 65535;
        if (!length || length > count - i) { ok = 0; break; }
        const uint32_t *a = words + i + 1;
        if (op == 22 && length >= 3 && a[0] < bound && a[1] == 32) links[a[0]] = SCALAR | GFX_SAMPLED_FLOAT; // OpTypeFloat
        else if (op == 21 && length >= 4 && a[0] < bound && a[1] == 32) links[a[0]] = SCALAR | (a[2] ? GFX_SAMPLED_SINT : GFX_SAMPLED_UINT);
        else if (op == 25 && length >= 9 && a[0] < bound && a[1] < bound && a[2] == 5) links[a[0]] = IMAGE | a[1]; // OpTypeImage DimBuffer
        else if ((op == 27 || op == 28 || op == 29) && length >= 3 && a[0] < bound && a[1] < bound) links[a[0]] = a[1];
        else if (op == 32 && length >= 4 && a[0] < bound && a[2] < bound) links[a[0]] = a[2]; // OpTypePointer
        else if (op == 59 && length >= 4 && a[1] < bound && a[0] < bound) links[a[1]] = a[0]; // OpVariable
        else if (op == 71 && length >= 4 && a[0] < bound && a[2] < 65536) {
            if (a[1] == 33) bindings[a[0]] = DECORATED | a[2];
            else if (a[1] == 34) sets[a[0]] = DECORATED | a[2];
        }
        i += length;
    }
    if (ok) for (uint32_t id = 1; id < bound; id++) {
        if (!(bindings[id] & DECORATED) || !(sets[id] & DECORATED)) continue;
        uint32_t at = id, scalar = 0; int image = 0;
        for (unsigned depth = 0; depth < 32 && at && at < bound; depth++) {
            uint32_t next = links[at];
            if (next & SCALAR) { scalar = next & 255; break; }
            image |= !!(next & IMAGE); at = next & ID_MASK;
        }
        if (!image || !scalar) continue;
        for (int i = 0; i < nresources; i++)
            if ((resources[i].type & 255) == 2 && resources[i].binding == (bindings[id] & 65535) && resources[i].set == (sets[id] & 65535))
                resources[i].sampled_type = (uint8_t)scalar;
    }
    if (ok) for (int i = 0; i < nresources; i++)
        if ((resources[i].type & 255) == 2 && !resources[i].sampled_type) { ok = 0; break; }
    free(links); free(bindings); free(sets); return ok;
}
