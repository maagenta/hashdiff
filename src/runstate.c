/* runstate.c - the state of a results.hashdiff directory. */
#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "os.h"
#include "runstate.h"
#include "util.h"

/*
 * Every file a run writes begins with one of these, so one prefix test per name covers the
 * published file, its .tmp, .new and .part forms and every destination number at once.
 */
static const char *const result_prefixes[] = {
    "paths.txt", "tree-changes.txt", "tree-origin.txt", "hashes-origin.txt",
    "tree-destination", "hashes-destination", "tree-diff", "diff-files", "rsync-files",
    "rsync-command"
};

#define N_RESULT_PREFIXES (sizeof(result_prefixes) / sizeof(result_prefixes[0]))

static int has_prefix(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static int has_suffix(const char *s, const char *suffix)
{
    size_t sl = strlen(s), fl = strlen(suffix);

    return sl >= fl && strcmp(s + sl - fl, suffix) == 0;
}

static int is_result_file(const char *name)
{
    size_t i;

    for (i = 0; i < N_RESULT_PREFIXES; i++) {
        if (has_prefix(name, result_prefixes[i]))
            return 1;
    }
    return 0;
}

static void list_results(const char *results, struct os_dirent **entries, size_t *count)
{
    if (os_list_dir(results, entries, count) != 0)
        hd_die("cannot read '%s': %s", results, strerror(errno));
}

void runstate_clean(const char *results)
{
    struct os_dirent *entries;
    size_t count, i;

    list_results(results, &entries, &count);
    for (i = 0; i < count; i++) {
        struct os_stat st;
        char *path;

        if (!is_result_file(entries[i].name))
            continue;
        /* Only regular files: a directory whose name looks like a result file is left. */
        path = hd_path_join(results, entries[i].name);
        if (os_lstat(path, &st) == 0 && st.kind == OS_REG && os_unlink(path) != 0
            && errno != ENOENT)
            hd_die("cannot remove '%s': %s", path, strerror(errno));
        free(path);
    }
    os_free_dir(entries, count);
}

int runstate_has_hashes(const char *results)
{
    struct os_dirent *entries;
    size_t count, i;
    int found = 0;

    list_results(results, &entries, &count);
    for (i = 0; i < count && !found; i++) {
        struct os_stat st;
        char *path;

        if (!has_prefix(entries[i].name, "hashes-")
            || (!has_suffix(entries[i].name, ".txt")
                && !has_suffix(entries[i].name, ".txt.tmp")))
            continue;
        path = hd_path_join(results, entries[i].name);
        found = os_lstat(path, &st) == 0 && st.kind == OS_REG;
        free(path);
    }
    os_free_dir(entries, count);
    return found;
}
