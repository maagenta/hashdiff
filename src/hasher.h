/* hasher.h - full and fast modes, plan execution, workers and output writing. */
#ifndef HD_HASHER_H
#define HD_HASHER_H

#include <stdio.h>
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

/* hashes-SIDE.txt, written as NAME.tmp and renamed when complete. */
struct hd_journal {
    FILE *f;
    char *path;
    char *tmp;
    struct hd_buf line;
};

void journal_open(struct hd_journal *j, const char *dir, const char *name,
                  const char *abs_root, const char *mode_line);
void journal_write(struct hd_journal *j, const char *path, const struct hd_result *r);
void journal_commit(struct hd_journal *j);

#endif
