/* plan.h - profiles, sampling plan and cost model (no I/O). */
#ifndef HD_PLAN_H
#define HD_PLAN_H

#include <sys/types.h>

struct hd_profile {
    const char *name;
    off_t default_block;
    off_t seek_bytes;           /* cost of a seek as equivalent sequential bytes */
};

/* Profile constants by HD_PROFILE_* number (opts.h); -1 from plan_profile_number if the
 * name is unknown. */
const struct hd_profile *plan_profile(int profile);
int plan_profile_number(const char *name);

/* A plan is a pure function of N, gap, block and seek_bytes. */
struct hd_plan {
    int sampled;                /* 0: full MD5 */
    off_t n;
    off_t block;
    off_t k;                    /* number of samples */
};

void plan_make(off_t n, off_t gap, off_t block, off_t seek_bytes, struct hd_plan *p);

/* Offsets of the samples in increasing order, with an integer Bresenham accumulator. */
struct hd_plan_iter {
    off_t i;
    off_t k;
    off_t off;
    off_t q;
    off_t r;
    off_t acc;
};

void plan_iter_init(struct hd_plan_iter *it, const struct hd_plan *p);
int plan_iter_next(struct hd_plan_iter *it, off_t *off);

/* Bytes the plan reads, saturated at HD_OFF_MAX (for the cost estimates of -j). */
off_t plan_read_bytes(const struct hd_plan *p);

#endif
