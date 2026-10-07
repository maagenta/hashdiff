/* walk.h - traversal of one tree, entry list and canonical sorting. */
#ifndef HD_WALK_H
#define HD_WALK_H

#include <stddef.h>
#include <sys/types.h>

#include "util.h"

/* Entry types of the traversal: regular file, symlink, error. */
#define ENT_FILE 'F'
#define ENT_LINK 'L'
#define ENT_ERROR 'E'

struct hd_entry {
    const char *path;   /* relative to the root, '/' separators, raw bytes */
    char type;
    int err;            /* errno of an ENT_ERROR entry */
    off_t size;         /* lstat st_size */
    off_t mtime;
    ino_t ino;
};

/* A directory that must not be traversed (results.hashdiff, the other tree). */
struct hd_exclude {
    dev_t dev;
    ino_t ino;
    const char *what;   /* for the warning, e.g. "results directory" */
};

struct hd_list {
    struct hd_entry *items;
    size_t count;
    size_t cap;
    unsigned long ignored;      /* FIFOs, sockets and devices */
    const char **excluded;      /* relative paths of excluded directories */
    size_t nexcluded;
    struct hd_arena arena;
};

struct walk_opts {
    const char *side;           /* "origin" or "destination", for messages */
    int one_fs;
    const struct hd_exclude *excludes;
    size_t nexcludes;
};

/* Traverses root; an unreadable root is a fatal error. The list is not sorted. The
 * traversal stops early if a signal was caught (os_caught_signal). */
void walk_tree(const char *root, const struct walk_opts *wo, struct hd_list *list);
void list_sort(struct hd_list *list);
void list_free(struct hd_list *list);

/* Writes DIR/tree-SIDE.txt (section 3.1 of the specification) from a sorted list. */
void tree_write(const char *results, const char *side, const char *abs_root,
                const struct hd_list *list);

#endif
