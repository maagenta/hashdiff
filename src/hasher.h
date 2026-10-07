/* hasher.h - full and fast modes, plan execution, workers and output writing. */
#ifndef HD_HASHER_H
#define HD_HASHER_H

#include <sys/types.h>

#include "util.h"
#include "walk.h"

/* Result types of the hashes file. */
#define RES_FULL 'F'
#define RES_SAMPLED 'S'
#define RES_LINK 'L'
#define RES_ERROR 'E'

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

struct hd_hasher {
    unsigned char *buf;         /* 1 MiB read buffer of this process */
    size_t bufsize;
    off_t bytes_read;
};

void hasher_init(struct hd_hasher *h);
void hasher_free(struct hd_hasher *h);
/* Hashes one entry of the list; path is the full path of the entry. */
void hash_entry(struct hd_hasher *h, const char *path, const struct hd_entry *e,
                struct hd_result *r);

/* Hashes a sorted list into DIR/hashes-SIDE.txt and fills the statistics. */
void hash_list(const char *root, const struct hd_list *list, const char *results,
               const char *side, const char *abs_root, const char *mode_line,
               struct hd_stats *st);

#endif
