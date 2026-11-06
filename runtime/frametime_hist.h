// Bounded frame-time histogram for perf.c. No allocation or per-frame sorting.
// The first 256 ms retain 0.25 ms buckets. Longer durations use 32 buckets per
// power of two (at most 3.125% width), up to a saturating UINT64_MAX-ns bound.
// Unlike a single overflow bucket, a seconds-long frame cannot become 256 ms.
#pragma once
#include <stdint.h>

#define FT_BIN_NS UINT64_C(250000)
#define FT_LINEAR_BINS 1024u
#define FT_LOG_BINS 32u
#define FT_BINS (FT_LINEAR_BINS + (64u - 10u) * FT_LOG_BINS)

static inline unsigned ft_hist_bin(uint64_t ns) {
    uint64_t units = ns / FT_BIN_NS;
    if (units < FT_LINEAR_BINS) return (unsigned)units;
    unsigned level = 0;
    for (uint64_t n = units >> 10; n > 1; n >>= 1) level++;
    unsigned part = (unsigned)(units >> (5u + level)) - FT_LOG_BINS;
    return FT_LINEAR_BINS + level * FT_LOG_BINS + part;
}

static inline uint64_t ft_hist_upper_ns(unsigned bin) {
    if (bin < FT_LINEAR_BINS) return ((uint64_t)bin + 1) * FT_BIN_NS;
    if (bin >= FT_BINS) return UINT64_MAX;
    unsigned tail = bin - FT_LINEAR_BINS;
    unsigned shift = 5u + tail / FT_LOG_BINS;
    uint64_t units = FT_LOG_BINS + 1u + tail % FT_LOG_BINS;
    if (units > (UINT64_MAX >> shift)) return UINT64_MAX;
    units <<= shift;
    if (units > UINT64_MAX / FT_BIN_NS) return UINT64_MAX;
    return units * FT_BIN_NS;
}

// Preserve the existing cutoff definition: the fastest frame among the slowest
// ceil(N / 100) frames. This is not the mean of those frames. The reciprocal of
// this bucket's upper bound is a conservative FPS lower bound for that cutoff.
static inline uint64_t ft_hist_low_cutoff_ns(const uint32_t hist[FT_BINS], uint64_t frames) {
    if (!frames) return 0;
    uint64_t cut = frames / 100 + (frames % 100 != 0), count = 0;
    for (unsigned b = FT_BINS; b-- > 0;) {
        count += hist[b];
        if (count >= cut) return ft_hist_upper_ns(b);
    }
    return 0; // inconsistent/empty input; production adds one count per frame
}

static inline double ft_hist_low_fps(uint64_t cutoff_ns) {
    // Truncate at three decimal places, rather than rounding a lower bound up.
    return cutoff_ns ? (UINT64_C(1000000000000) / cutoff_ns) / 1000.0 : 0.0;
}
