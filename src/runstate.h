/* runstate.h - the state of a results.hashdiff directory (sections 3.2, 3.3 and 3.5). */
#ifndef HD_RUNSTATE_H
#define HD_RUNSTATE_H

/*
 * Removes every file a run may have left in results.hashdiff, and nothing else. The names
 * depend on the number of destinations, so a fixed list cannot do it and they are matched by
 * prefix (section 3.3): history.txt, lock and the scan-* archives match nothing here and are
 * never removed.
 */
void runstate_clean(const char *results);

/* One side of the run, for paths.txt. */
struct hd_role {
    const char *name;   /* side name (section 2.2) */
    const char *root;   /* cleaned absolute path */
};

/*
 * Writes paths.txt and appends the same block to history.txt, which records one block per run
 * for the life of the directory and is the one file nothing ever removes (section 3.2).
 */
void runstate_write_paths(const char *results, const char *started, int file_mode,
                          const struct hd_role *roles, int nroles);

/* 1 when the directory holds a published hashes file or a journal, so there may be something
 * to resume (section 3.3). */
int runstate_has_hashes(const char *results);

#endif
