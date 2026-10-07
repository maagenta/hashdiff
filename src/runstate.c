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

void runstate_write_paths(const char *results, const char *started, int file_mode,
                          const struct hd_role *roles, int nroles)
{
    static const char *const paths_magic = "# hashdiff-paths: 1\n";
    static const char *const history_magic = "# hashdiff-history: 1\n";
    struct hd_outfile out;
    struct os_stat st;
    struct hd_buf b;
    char *path;
    FILE *f;
    int i, fresh;

    buf_init(&b);
    buf_append_str(&b, "# started: ");
    buf_append_str(&b, started);
    buf_append_char(&b, '\n');
    if (file_mode)
        buf_append_str(&b, "# file: 1\n");
    for (i = 0; i < nroles; i++) {
        buf_append_str(&b, roles[i].name);
        buf_append_char(&b, ' ');
        hd_escape(&b, roles[i].root);
        buf_append_char(&b, '\n');
    }
    outfile_open(&out, results, "paths.txt");
    outfile_write(&out, paths_magic, strlen(paths_magic));
    outfile_write(&out, b.data, b.len);
    outfile_commit(&out);

    path = hd_path_join(results, "history.txt");
    fresh = os_lstat(path, &st) != 0;
    f = fopen(path, "ab");
    if (f == NULL)
        hd_die("cannot write '%s': %s", path, strerror(errno));
    /* Appended, oldest first: "newest on top" would mean rewriting the whole file every run. */
    if ((fresh && fwrite(history_magic, 1, strlen(history_magic), f) != strlen(history_magic))
        || fwrite(b.data, 1, b.len, f) != b.len || fputc('\n', f) == EOF || fclose(f) != 0)
        hd_die("cannot write '%s': %s", path, strerror(errno));
    free(path);
    buf_free(&b);
}
