/* Original synthetic per-stage binding tests. The lookup provider deliberately
 * uses small tagged records, not real NVN shader controls or game bytes. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../runtime/nvn_program_bind.h"

static void fixture_check(int ok, int line, const char *what) {
    if (!ok) { fprintf(stderr, "program bind assertion at %d: %s\n", line, what); exit(1); }
}
#define CHECK(x) fixture_check(!!(x), __LINE__, #x)

typedef struct { unsigned magic, stage; } FixtureControl;
enum { CONTROL_MAGIC = 0x1234ABCD };
static FixtureControl controls[3][6];
static unsigned char ownership[3][32];
static unsigned serial_calls, lookup_calls;
static uint64_t next_serial = 0x10000;
static int failing_stage = -1;

static uint64_t serial_new(void) { serial_calls++; return ++next_serial; }

static int lookup(const prog_t *p, unsigned stage, NVNshaderData *out) {
    lookup_calls++;
    if (stage >= 6 || p->magic != MAGIC('P', 'R', 'O', 'G') || p->nstage > 6) return -1;
    if ((int)stage == failing_stage) return -1;
    int found = 0;
    for (unsigned i = 0; i < p->nstage; i++) {
        const FixtureControl *control = p->sd[i].control;
        if (!control || control->magic != CONTROL_MAGIC || control->stage >= 6) return -1;
        if (control->stage == stage) {
            if (found) return -1;
            *out = p->sd[i]; found = 1;
        }
    }
    return found;
}

static prog_t program(unsigned identity, uint32_t mask, uint64_t serial) {
    CHECK(identity < 3 && !(mask & ~63u));
    prog_t p = {.magic = MAGIC('P', 'R', 'O', 'G'), .serial = serial,
                .owned_control = ownership[identity]};
    for (unsigned stage = 0; stage < 6; stage++) {
        controls[identity][stage] = (FixtureControl){CONTROL_MAGIC, stage};
        if (mask & (1u << stage)) {
            p.sd[p.nstage++] = (NVNshaderData){0x1000u * (identity + 1) + stage * 0x100u,
                                              &controls[identity][stage]};
        }
    }
    return p;
}

static int merged_has(const NvnProgramBindings *s, unsigned stage, const prog_t *source) {
    NVNshaderData actual = {0}, expected = {0};
    int a = lookup(&s->merged, stage, &actual);
    if (!source) return a == 0;
    int e = lookup(source, stage, &expected);
    return a == e && (a == 0 || (actual.data == expected.data && actual.control == expected.control));
}

static void sequence(void) {
    NvnProgramBindings s = {0};
    prog_t vs = program(0, 1, 10), fs = program(1, 2, 20);
    const struct { const prog_t *program; uint32_t mask; } calls[] = {
        {&vs, 0x01}, {NULL, 0x08}, {NULL, 0x10}, {NULL, 0x04}, {&fs, 0x02}, {NULL, 0x20}
    };
    for (unsigned i = 0; i < sizeof calls / sizeof calls[0]; i++) {
        CHECK(nvn_program_bind(&s, calls[i].program, calls[i].mask) == 1);
        CHECK(nvn_program_refresh(&s, lookup, serial_new) == (i == 0 || i == 4));
        CHECK(s.source[0] == &vs && merged_has(&s, 0, &vs));
    }
    CHECK(s.mask == 3 && s.merged.nstage == 2);
    CHECK(s.source[1] == &fs && merged_has(&s, 1, &fs));
    for (unsigned i = 2; i < 6; i++) CHECK(s.source[i] == NULL && merged_has(&s, i, NULL));
    CHECK(serial_calls == 2 && s.merged.owned_control == NULL);
}

static void compute(void) {
    NvnProgramBindings s = {0};
    prog_t graphics = program(0, 3, 10), cs = program(1, 32, 20), fs = program(2, 2, 30);
    CHECK(nvn_program_bind(&s, &graphics, 3));
    CHECK(nvn_program_bind(&s, &cs, 32));
    CHECK(nvn_program_refresh(&s, lookup, serial_new) == 1);
    CHECK(s.mask == 35 && s.merged.nstage == 3);
    CHECK(merged_has(&s, 0, &graphics) && merged_has(&s, 1, &graphics) && merged_has(&s, 5, &cs));
    CHECK(nvn_program_bind(&s, &fs, 2));
    CHECK(nvn_program_refresh(&s, lookup, serial_new) == 1);
    CHECK(merged_has(&s, 0, &graphics) && merged_has(&s, 1, &fs) && merged_has(&s, 5, &cs));
    CHECK(nvn_program_bind(&s, NULL, 31));
    CHECK(nvn_program_refresh(&s, lookup, serial_new) == 1);
    CHECK(s.mask == 32 && s.merged.nstage == 1 && merged_has(&s, 5, &cs));
    CHECK(nvn_program_bind(&s, &graphics, 3));
    CHECK(nvn_program_bind(&s, NULL, 32));
    CHECK(nvn_program_refresh(&s, lookup, serial_new) == 1);
    CHECK(s.mask == 3 && s.merged.nstage == 2 && merged_has(&s, 5, NULL));
}

static void masks(void) {
    NvnProgramBindings s = {0};
    prog_t a = program(0, 63, 1), b = program(1, 63, 2);
    CHECK(nvn_program_bind(&s, &a, 63));
    CHECK(nvn_program_refresh(&s, lookup, serial_new) == 1);
    CHECK(nvn_program_bind(&s, &b, (1u << 1) | (1u << 5)));
    for (unsigned stage = 0; stage < 6; stage++)
        CHECK(s.source[stage] == ((stage == 1 || stage == 5) ? &b : &a));
    CHECK(nvn_program_refresh(&s, lookup, serial_new) == 1);
    for (unsigned stage = 0; stage < 6; stage++) CHECK(merged_has(&s, stage, s.source[stage]));
    NvnProgramBindings snapshot = s;
    CHECK(nvn_program_bind(&s, NULL, 0) == 1 && !memcmp(&s, &snapshot, sizeof s));
    CHECK(nvn_program_bind(&s, &a, 0) == 1 && !memcmp(&s, &snapshot, sizeof s));
    const uint32_t bad[] = {64, 65, 0x80000000u, UINT32_MAX};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(nvn_program_bind(&s, NULL, bad[i]) == 0);
        CHECK(!memcmp(&s, &snapshot, sizeof s));
    }
    CHECK(nvn_program_refresh(&s, lookup, serial_new) == 0);
    CHECK(serial_calls == 2);
}

static void unchanged(void) {
    NvnProgramBindings s = {0};
    prog_t a = program(0, 3, 1);
    CHECK(nvn_program_refresh(&s, lookup, serial_new) == 0 && serial_calls == 0 && lookup_calls == 0);
    CHECK(nvn_program_bind(&s, &a, 3));
    CHECK(nvn_program_refresh(&s, lookup, serial_new) == 1);
    unsigned calls = lookup_calls;
    uint64_t serial = s.merged.serial;
    for (unsigned i = 0; i < 20; i++) {
        CHECK(nvn_program_bind(&s, &a, 3));
        CHECK(nvn_program_refresh(&s, lookup, serial_new) == 0);
    }
    CHECK(serial_calls == 1 && lookup_calls == calls && s.merged.serial == serial);
}

static void serial_change(void) {
    NvnProgramBindings s = {0};
    prog_t a = program(0, 3, 100);
    CHECK(nvn_program_bind(&s, &a, 3));
    CHECK(nvn_program_refresh(&s, lookup, serial_new) == 1);
    uint64_t before = s.merged.serial;
    a.sd[0].data += 0x80; a.serial = 101;
    CHECK(!s.dirty); // Same pointer, no rebind: serial alone must invalidate.
    CHECK(nvn_program_refresh(&s, lookup, serial_new) == 1);
    CHECK(s.merged.serial != before && s.seen_serial[0] == 101 && s.seen_serial[1] == 101);
    CHECK(merged_has(&s, 0, &a) && merged_has(&s, 1, &a));
    CHECK(nvn_program_refresh(&s, lookup, serial_new) == 0);
    before = s.merged.serial;
    a = program(1, 3, 200); // Finalize/reinitialize the same source address.
    CHECK(nvn_program_bind(&s, &a, 1));
    CHECK(nvn_program_refresh(&s, lookup, serial_new) == 1);
    CHECK(s.merged.serial != before && s.source[0] == &a && s.source[1] == &a);
    CHECK(merged_has(&s, 0, &a) && merged_has(&s, 1, &a));
    CHECK(serial_calls == 3);
}

static void all_clear(void) {
    NvnProgramBindings s = {0};
    prog_t all = program(0, 63, 5), graphics = program(1, 31, 6);
    CHECK(nvn_program_bind(&s, &all, 63));
    CHECK(nvn_program_refresh(&s, lookup, serial_new) == 1);
    CHECK(nvn_program_bind(&s, &graphics, 31));
    CHECK(nvn_program_refresh(&s, lookup, serial_new) == 1);
    CHECK(s.mask == 63 && s.merged.nstage == 6 && s.source[5] == &all);
    for (unsigned stage = 0; stage < 5; stage++) CHECK(merged_has(&s, stage, &graphics));
    CHECK(merged_has(&s, 5, &all));
    CHECK(nvn_program_bind(&s, NULL, 63));
    CHECK(nvn_program_refresh(&s, lookup, serial_new) == 1);
    CHECK(s.mask == 0 && s.merged.nstage == 0 && s.merged.owned_control == NULL);
    for (unsigned stage = 0; stage < 6; stage++) {
        CHECK(!s.source[stage] && !s.seen[stage] && !s.seen_serial[stage]);
        CHECK(!s.merged.sd[stage].data && !s.merged.sd[stage].control);
    }
    CHECK(nvn_program_bind(&s, NULL, 63));
    CHECK(nvn_program_refresh(&s, lookup, serial_new) == 0 && serial_calls == 3);
}

static void ownership_test(void) {
    NvnProgramBindings s = {0};
    prog_t a = program(0, 1, 10), b = program(1, 2, 20);
    prog_t a_before = a, b_before = b;
    memset(ownership[0], 0xA5, sizeof ownership[0]);
    memset(ownership[1], 0x5A, sizeof ownership[1]);
    CHECK(nvn_program_bind(&s, &a, 1) && nvn_program_bind(&s, &b, 2));
    CHECK(nvn_program_refresh(&s, lookup, serial_new) == 1);
    CHECK(!memcmp(&a, &a_before, sizeof a) && !memcmp(&b, &b_before, sizeof b));
    CHECK(s.merged.owned_control == NULL && a.owned_control == ownership[0] && b.owned_control == ownership[1]);
    CHECK(s.merged.sd[0].control == a.sd[0].control && s.merged.sd[1].control == b.sd[0].control);
    CHECK(s.merged.serial != a.serial && s.merged.serial != b.serial);
    s.merged.sd[0].data ^= 0xFFFF;
    CHECK(a.sd[0].data == a_before.sd[0].data); // Record values do not alias the source array.
    CHECK(nvn_program_bind(&s, NULL, 63) && nvn_program_refresh(&s, lookup, serial_new) == 1);
    for (unsigned i = 0; i < sizeof ownership[0]; i++)
        CHECK(ownership[0][i] == 0xA5 && ownership[1][i] == 0x5A);
    CHECK(!memcmp(&a, &a_before, sizeof a) && !memcmp(&b, &b_before, sizeof b));
}

static void lookup_failure(void) {
    NvnProgramBindings s = {0};
    prog_t a = program(0, 1, 1), b = program(1, 32, 2);
    CHECK(nvn_program_bind(&s, &a, 3)); // Valid source lacks the selected fragment stage.
    CHECK(nvn_program_refresh(&s, lookup, serial_new) == 1);
    CHECK(s.merged.nstage == 1 && merged_has(&s, 0, &a) && merged_has(&s, 1, NULL));
    CHECK(s.source[1] == &a); // Missing shader is not a malformed binding.
    CHECK(nvn_program_bind(&s, &b, 32));
    NvnProgramBindings before = s;
    unsigned calls_before = serial_calls;
    failing_stage = 5; // Fails after earlier stages have been assembled in the temporary view.
    CHECK(nvn_program_refresh(&s, lookup, serial_new) == -1);
    CHECK(!memcmp(&s, &before, sizeof s) && serial_calls == calls_before && s.dirty);
    failing_stage = -1;
    CHECK(nvn_program_refresh(&s, lookup, serial_new) == 1);
    CHECK(merged_has(&s, 5, &b) && serial_calls == calls_before + 1);
    before = s;
    b.nstage = 7; b.serial++;
    CHECK(nvn_program_refresh(&s, lookup, serial_new) == -1);
    CHECK(!memcmp(&s, &before, sizeof s) && serial_calls == calls_before + 1);
    b.nstage = 1;
    CHECK(nvn_program_refresh(&s, lookup, serial_new) == 1);
    CHECK(s.seen_serial[5] == b.serial && merged_has(&s, 5, &b));
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    if (!strcmp(argv[1], "sequence")) sequence();
    else if (!strcmp(argv[1], "compute")) compute();
    else if (!strcmp(argv[1], "masks")) masks();
    else if (!strcmp(argv[1], "unchanged")) unchanged();
    else if (!strcmp(argv[1], "serial")) serial_change();
    else if (!strcmp(argv[1], "all-clear")) all_clear();
    else if (!strcmp(argv[1], "ownership")) ownership_test();
    else if (!strcmp(argv[1], "lookup-failure")) lookup_failure();
    else CHECK(0);
    puts("program binding check passed");
    return 0;
}
