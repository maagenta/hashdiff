/* plan.c - profiles, sampling plan and cost model (no I/O). */
#include "config.h"

#include <string.h>

#include "opts.h"
#include "plan.h"
#include "util.h"

static const struct hd_profile profiles[2] = {
    { "ssd", 64 * 1024, 200000 },           /* 0.1 ms x 2 GB/s */
    { "hdd", 1024 * 1024, 1440000 }         /* 8 ms x 180 MB/s */
};

const struct hd_profile *plan_profile(int profile)
{
    return &profiles[profile == HD_PROFILE_HDD ? 1 : 0];
}

int plan_profile_number(const char *name)
{
    if (strcmp(name, "ssd") == 0)
        return HD_PROFILE_SSD;
    if (strcmp(name, "hdd") == 0)
        return HD_PROFILE_HDD;
    return -1;
}

/*
 * Rules of section 6.2 of the specification. Every product and sum is checked for
 * overflow; a comparison whose operands would overflow counts as "sampling is not cheaper",
 * so the file is read in full.
 */
void plan_make(off_t n, off_t gap, off_t block, off_t seek_bytes, struct hd_plan *p)
{
    off_t twice, span, step, k, kb, per, lhs, rhs;

    p->sampled = 0;
    p->n = n;
    p->block = block;
    p->k = 0;
    /* Rule 1: N <= 2B. */
    if (hd_off_add(block, block, &twice) != 0 || n <= twice)
        return;
    /* Rule 2: k = ceil((N - B) / (G + B)) + 1; G + B beyond off_t means one step. */
    span = n - block;
    if (hd_off_add(gap, block, &step) != 0)
        k = 2;
    else
        k = span / step + (span % step != 0) + 1;
    /* Rule 3: sampling must read less than N and cost less than reading N. */
    if (hd_off_mul(k, block, &kb) != 0 || kb >= n)
        return;
    if (hd_off_add(seek_bytes, block, &per) != 0 || hd_off_mul(k, per, &lhs) != 0
        || hd_off_add(seek_bytes, n, &rhs) != 0 || lhs >= rhs)
        return;
    p->sampled = 1;
    p->k = k;
}

void plan_iter_init(struct hd_plan_iter *it, const struct hd_plan *p)
{
    off_t span = p->n - p->block;

    it->i = 0;
    it->k = p->k;
    it->off = 0;
    it->q = span / (p->k - 1);
    it->r = span % (p->k - 1);
    it->acc = 0;
}

int plan_iter_next(struct hd_plan_iter *it, off_t *off)
{
    if (it->i == it->k)
        return 0;
    if (it->i > 0) {
        it->off += it->q;
        it->acc += it->r;
        if (it->acc >= it->k - 1) {
            it->off += 1;
            it->acc -= it->k - 1;
        }
    }
    it->i++;
    *off = it->off;
    return 1;
}

off_t plan_read_bytes(const struct hd_plan *p)
{
    off_t total;

    if (!p->sampled)
        return p->n;
    if (hd_off_mul(p->k, p->block, &total) != 0)
        return HD_OFF_MAX;
    return total;
}
