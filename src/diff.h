/* diff.h - merge-join of two lists, tree-diff.txt, diff-files.txt and the rsync outputs. */
#ifndef HD_DIFF_H
#define HD_DIFF_H

#include <stddef.h>
#include <sys/types.h>

#include "util.h"
#include "walk.h"

/* Line formats. */
#define FMT_TREE 1
#define FMT_HASHES 2

/* One parsed line of a tree-*.txt or hashes-*.txt file. */
struct hd_record {
    char type;
    off_t num;          /* size; errno for E */
    off_t mtime;        /* tree files only */
    char hash[33];      /* hashes files only; empty for E */
    struct hd_buf path; /* unescaped */
};

void record_init(struct hd_record *r);
void record_free(struct hd_record *r);
/* Parses one line (without its LF); returns -1 if it is not valid for the format. */
int record_parse(int fmt, const char *line, size_t len, struct hd_record *r);

/* Statuses, in the order of the counters. */
enum hd_status {
    ST_MISSING, ST_EXTRA, ST_SIZE, ST_TYPE, ST_HASH, ST_ERR_SRC, ST_ERR_DST, ST_COUNT
};

struct hd_counts {
    unsigned long n[ST_COUNT];
};

const char *status_name(int status);
unsigned long counts_total(const struct hd_counts *c);

/*
 * The result files of one side and of one destination's comparison (section 3). tree and
 * hashes are the files of that side; the four names of the comparison carry "-DEST" only
 * when the run has several destinations, so a single-destination run writes the names of
 * version 1.1.
 */
struct hd_names {
    char tree[48];              /* tree-SIDE.txt */
    char hashes[48];            /* hashes-SIDE.txt */
    char tree_diff[48];         /* tree-diff[-DEST].txt */
    char diff_files[48];        /* diff-files[-DEST].txt */
    char rsync_list[48];        /* rsync-files[-DEST].lst */
    char rsync_command[48];     /* rsync-command[-DEST].txt */
};

/* Counts the records of a tree-diff or diff-files file per status, 0 if it does not exist,
 * and returns the total. A resume reads the decision of the tree stage back this way
 * (section 3.4). */
unsigned long report_counts(const char *results, const char *name, struct hd_counts *c);

/* Tree stage: compares tree-origin.txt with one destination's tree file and writes its
 * tree-diff file. The directories excluded from either traversal are appended to *excluded
 * (malloc'd strings). */
void diff_trees(const char *results, const struct hd_names *n, const char *abs_origin,
                const char *abs_destination, struct hd_counts *c, char ***excluded,
                size_t *nexcluded);

/* Appends the "# excluded:" paths of a saved tree file to *excluded (malloc'd strings), for
 * a resumed run, which does not compare the trees again (section 3.4). */
void tree_excluded(const char *results, const char *name, const char *abs_root,
                   char ***excluded, size_t *nexcluded);

/* Builds the suggested synchronization command of the tree stage; empty if none. */
void suggest_command(struct hd_buf *out, const struct hd_counts *c, const char *abs_origin,
                     const char *abs_destination, int one_fs, char **excluded,
                     size_t nexcluded);

/* Hash stage: writes diff-files.txt, rsync-files.lst and rsync-command.txt; the rsync
 * command is also returned in *cmd (empty if there is nothing to copy). */
void diff_hashes(const char *results, const struct hd_names *n, const char *abs_origin,
                 const char *abs_destination, struct hd_counts *c, struct hd_buf *cmd);

/* Parses 32 lowercase hex digits into a digest. */
void hex_to_md5(const char *hex, unsigned char md5[16]);

/* Resume (section 3.2). Checks the header of a saved tree file: missing, unreadable or of
 * another root is fatal. */
void resume_check_tree(const char *results, const char *name, const char *abs_root);
/* Checks the header of a hashes source: returns 0 if valid, 1 if missing or cut short (no
 * source); a different root or mode is fatal. */
int resume_check_hashes(const char *results, const char *name, const char *abs_root,
                        const char *mode_line);
/* Compares a saved tree file with the current sorted list on every field; writes ADDED,
 * DELETED and CHANGED lines to DIR/part_name and returns their number. */
unsigned long tree_changes(const char *results, const char *tree_name, const char *abs_root,
                           const struct hd_list *list, const char *part_name);

#endif
