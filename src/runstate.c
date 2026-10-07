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

/* ---- the state of an existing results.hashdiff (section 3.3) ---- */

static int file_in(const char *results, const char *name)
{
    char *path = hd_path_join(results, name);
    struct os_stat st;
    int r = os_lstat(path, &st) == 0 && st.kind == OS_REG;

    free(path);
    return r;
}

static void mismatch(const char *output, const char *what, const char *detail)
{
    if (detail != NULL)
        hd_die("the previous run in %s %s ('%s'); use --force to start over", output, what,
               detail);
    hd_die("the previous run in %s %s; use --force to start over", output, what);
}

/* Compares paths.txt with the run that is starting; fills out->started. */
static void check_paths(const char *results, const char *output,
                        const struct hd_role *roles, int nroles, int file_mode,
                        struct hd_runstate *out)
{
    char *path = hd_path_join(results, "paths.txt");
    struct hd_reader rd;
    struct hd_buf value;
    int lineno = 0, seen = 0, was_file = 0;

    if (reader_open(&rd, path) != 0)
        hd_die("'%s' was written by an older hashdiff (it has no paths.txt); use --force to "
               "start over", results);
    out->started[0] = '\0';
    buf_init(&value);
    while (reader_next(&rd) == 1 && rd.complete) {
        const char *sep;
        size_t label;

        lineno++;
        if (lineno == 1 && strcmp(rd.line.data, "# hashdiff-paths: 1") != 0)
            hd_die("invalid file '%s': unknown format", path);
        if (rd.line.len > 0 && rd.line.data[0] == '#') {
            if (strncmp(rd.line.data, "# started: ", 11) == 0) {
                strncpy(out->started, rd.line.data + 11, HD_TIME_LEN - 1);
                out->started[HD_TIME_LEN - 1] = '\0';
            } else if (strcmp(rd.line.data, "# file: 1") == 0) {
                was_file = 1;
            }
            continue;
        }
        sep = strchr(rd.line.data, ' ');
        if (sep == NULL)
            hd_die("invalid file '%s': malformed line", path);
        if (seen >= nroles)
            mismatch(output, "compared more destinations than this one", NULL);
        label = (size_t)(sep - rd.line.data);
        if (label != strlen(roles[seen].name)
            || strncmp(rd.line.data, roles[seen].name, label) != 0)
            mismatch(output, "compared a different number of destinations", NULL);
        buf_clear(&value);
        if (hd_unescape(&value, sep + 1, rd.line.len - (size_t)(sep + 1 - rd.line.data)) != 0)
            hd_die("invalid file '%s': malformed line", path);
        if (value.data == NULL || strcmp(value.data, roles[seen].root) != 0)
            mismatch(output, roles[seen].name[0] == 'o' ? "used a different ORIGIN"
                     : "used a different destination", value.data);
        seen++;
    }
    reader_close(&rd);
    buf_free(&value);
    free(path);
    if (seen != nroles)
        mismatch(output, "compared a different number of destinations", NULL);
    if (was_file != (file_mode != 0))
        mismatch(output, was_file ? "compared files, not directories"
                 : "compared directories, not files", NULL);
}

int runstate_inspect(const char *results, const char *output, const struct hd_role *roles,
                     int nroles, int file_mode, struct hd_runstate *out)
{
    char name[80];
    int i, interrupted = 0, differences = 0;

    out->state = RUNSTATE_FRESH;
    out->started[0] = '\0';
    out->tree_differed = 0;
    if (!runstate_has_hashes(results))
        return RUNSTATE_FRESH;
    check_paths(results, output, roles, nroles, file_mode, out);

    /*
     * A destination was hashed when its tree-diff file holds no record: that is the decision
     * of the tree stage, still on disk, and what tells which sides should have a journal.
     */
    for (i = 1; i < nroles; i++) {
        struct hd_counts c;

        if (!file_in(results, roles[i].names->tree_diff)
            || report_counts(results, roles[i].names->tree_diff, &c) > 0) {
            out->tree_differed++;
            continue;
        }
        if (report_counts(results, roles[i].names->diff_files, &c) > 0)
            differences = 1;
    }
    for (i = 0; i < nroles; i++) {
        struct hd_counts c;

        if (i > 0 && (!file_in(results, roles[i].names->tree_diff)
                      || report_counts(results, roles[i].names->tree_diff, &c) > 0))
            continue;                   /* this destination was never hashed */
        sprintf(name, "%s.tmp", roles[i].names->hashes);
        if (file_in(results, name) || !file_in(results, roles[i].names->hashes))
            interrupted = 1;
    }
    out->state = interrupted ? RUNSTATE_INTERRUPTED
                 : differences ? RUNSTATE_FINISHED_DIFF : RUNSTATE_FINISHED_CLEAN;
    return out->state;
}
