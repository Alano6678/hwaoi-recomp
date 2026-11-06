// Presentation-rate experiment. This does not assert qualified game/physics timing.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>

static inline int hwaoi_rate_period(const char *value, uint64_t *period) {
    if (!value || !*value) value = "60";
    char *end; errno = 0;
    long rate = strtol(value, &end, 10);
    if (errno || end == value || *end || (rate != 30 && rate != 60 && rate != 90 && rate != 120)) return 0;
    *period = (UINT64_C(1000000000) + (uint64_t)rate / 2) / (uint64_t)rate;
    return 1;
}
