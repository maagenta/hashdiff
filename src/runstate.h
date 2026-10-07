/* runstate.h - the state of a results.hashdiff directory (sections 3.2, 3.3 and 3.5). */
#ifndef HD_RUNSTATE_H
#define HD_RUNSTATE_H

#include "diff.h"
#include "util.h"

/*
 * Removes every file a run may have left in results.hashdiff, and nothing else. The names
 * depend on the number of destinations, so a fixed list cannot do it and they are matched by
 * prefix (section 3.3): history.txt, lock and the scan-* archives match nothing here and are
 * never removed.
 */
void runstate_clean(const char *results);

/* One side of the run, for paths.txt and for the state of section 3.3. */
struct hd_role {
    const char *name;                   /* side name (section 2.2) */
    const char *root;                   /* cleaned absolute path */
    const struct hd_names *names;       /* the files of this side (section 3) */
};

/* The state of an existing results.hashdiff (section 3.3). */
#define RUNSTATE_FRESH 0            /* nothing was hashed: start over without asking */
#define RUNSTATE_INTERRUPTED 1
#define RUNSTATE_FINISHED_CLEAN 2
#define RUNSTATE_FINISHED_DIFF 3

struct hd_runstate {
    int state;
    char started[HD_TIME_LEN];          /* "# started:" of the previous run */
    char recheck[64];                   /* "# recheck:" of paths.txt, or empty */
    int tree_differed;                  /* destinations that stopped in the tree stage */
};

/*
 * Reads the directory and its paths.txt and returns the state. A paths.txt that is missing, or
 * that does not describe this run (another number of destinations, another root, another
 * --file), is a fatal error: those lists are not comparable, so resuming is not one of the
 * possible answers (section 3.3). roles[0] is ORIGIN.
 */
int runstate_inspect(const char *results, const char *output, const struct hd_role *roles,
                     int nroles, int file_mode, struct hd_runstate *out);

/*
 * Section 3.5: moves every file runstate_clean would remove into a new
 * results.hashdiff/scan-YYYYMMDDHHMM/, named after the previous run's start time, with a "-2",
 * "-3", ... suffix when that name is taken. history.txt and lock stay where they are. Returns
 * the archive's name (malloc'd).
 */
char *runstate_archive(const char *results, const char *started);

/*
 * Reads the diff-files files of an archive and fills the recheck sets: sets[i] for destination
 * i, and sets[0] with the union, because ORIGIN is hashed once for all of them. A diff file
 * made with other parameters is a fatal error. Returns the size of the union.
 */
unsigned long runstate_recheck_sets(const char *results, const char *archive,
                                    const struct hd_role *roles, int nroles,
                                    const char *mode_line, struct hd_set *sets);

/*
 * Writes paths.txt and appends the same block to history.txt, which records one block per run
 * for the life of the directory and is the one file nothing ever removes (section 3.2).
 */
void runstate_write_paths(const char *results, const char *started, int file_mode,
                          const struct hd_role *roles, int nroles, const char *recheck);

/* 1 when the directory holds a published hashes file or a journal, so there may be something
 * to resume (section 3.3). */
int runstate_has_hashes(const char *results);

#endif
