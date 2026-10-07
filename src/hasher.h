/* hasher.h - full and fast modes, plan execution, workers and output writing. */
#ifndef HD_HASHER_H
#define HD_HASHER_H

#include <sys/types.h>

#include "util.h"
#include "walk.h"

/* Result types of the hashes file; RES_NONE: not hashed (interrupted). */
#define RES_FULL 'F'
#define RES_SAMPLED 'S'
#define RES_LINK 'L'
#define RES_ERROR 'E'
#define RES_NONE 0

struct hd_result {
    char type;
    int err;                    /* errno of a RES_ERROR result */
    off_t size;
    unsigned char md5[16];
};

/* Statistics of one side, sent to the parent as one fixed-size record. */
struct hd_stats {
    unsigned long entries;
    unsigned long full;
    unsigned long sampled;
    unsigned long links;
    unsigned long errors;
    unsigned long ignored;
    off_t bytes_read;
    off_t samples;              /* S files: number of samples, sample bytes, total size */
    off_t sample_bytes;
    off_t sampled_total;
};

void stats_init(struct hd_stats *st);

/* Parameters of the hash stage, fixed for the whole run. */
struct hd_hashopts {
    int fast;
    int hdd;                    /* --profile hdd: inode read order with -j 1 in fast mode */
    off_t gap;
    off_t block;
    off_t seek_bytes;
    off_t merge_gap;
    int jobs;
    int progress;               /* print progress lines on stderr */
    const char *mode_line;      /* value of the "# mode:" header */
};

/*
 * Hashes a sorted list into DIR/hashes-SIDE.txt (written as a journal in canonical order and
 * renamed when complete) and fills the statistics. Returns 0, or the number of the signal
 * that interrupted it; the journal is then kept as hashes-SIDE.txt.tmp.
 */
int hash_side(const char *root, const struct hd_list *list, const char *results,
              const char *side, const char *abs_root, const struct hd_hashopts *opts,
              struct hd_stats *st);

#endif
