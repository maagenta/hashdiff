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
                          const struct hd_role *roles, int nroles, const char *recheck)
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
    if (recheck != NULL) {
        /* What makes an interrupted recheck resumable: the archive the sets came from (3.5). */
        buf_append_str(&b, "# recheck: ");
        buf_append_str(&b, recheck);
        buf_append_char(&b, '\n');
    }
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
            } else if (strncmp(rd.line.data, "# recheck: ", 11) == 0) {
                strncpy(out->recheck, rd.line.data + 11, sizeof(out->recheck) - 1);
                out->recheck[sizeof(out->recheck) - 1] = '\0';
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
    out->recheck[0] = '\0';
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

/* ---- the archive and the recheck sets (section 3.5) ---- */

char *runstate_archive(const char *results, const char *started)
{
    struct os_dirent *entries;
    size_t count, i, k = 0;
    char digits[16], base[32], name[48], *dir;
    int n = 1;

    /*
     * The previous run's start time, so the archive identifies the scan it holds. It arrives as
     * dd/mm/yyyy HH:MM, and the name has to sort, so it is written YYYYMMDDHHMM.
     */
    for (i = 0; started[i] != '\0' && k < 12; i++)
        if (started[i] >= '0' && started[i] <= '9')
            digits[k++] = started[i];
    if (k == 12)
        sprintf(base, "scan-%.4s%.2s%.2s%.4s", digits + 4, digits + 2, digits, digits + 8);
    else
        strcpy(base, "scan-unknown");
    strcpy(name, base);
    for (;;) {
        dir = hd_path_join(results, name);
        if (os_mkdir(dir) == 0)
            break;
        if (errno != EEXIST)
            hd_die("cannot create '%s': %s", dir, strerror(errno));
        free(dir);
        if (++n > 99)
            hd_die("cannot create an archive in '%s': too many with the same stamp", results);
        sprintf(name, "%s-%d", base, n);
    }

    list_results(results, &entries, &count);
    for (i = 0; i < count; i++) {
        struct os_stat st;
        char *from, *to;

        /* Only what the cleaning would remove: history.txt, lock and the other archives stay. */
        if (!is_result_file(entries[i].name))
            continue;
        from = hd_path_join(results, entries[i].name);
        to = hd_path_join(dir, entries[i].name);
        if (os_lstat(from, &st) == 0 && st.kind == OS_REG && os_rename(from, to) != 0)
            hd_die("cannot move '%s' to '%s': %s", from, to, strerror(errno));
        free(from);
        free(to);
    }
    os_free_dir(entries, count);
    free(dir);
    return xstrdup(name);
}

unsigned long runstate_recheck_sets(const char *results, const char *archive,
                                    const struct hd_role *roles, int nroles,
                                    const char *mode_line, struct hd_set *sets)
{
    char *dir = hd_path_join(results, archive);
    int i;

    for (i = 0; i < nroles; i++)
        set_init(&sets[i]);
    for (i = 1; i < nroles; i++) {
        struct hd_reader rd;
        char *path = hd_path_join(dir, roles[i].names->diff_files);
        struct hd_buf unescaped;
        int lineno = 0;

        if (reader_open(&rd, path) != 0) {
            free(path);
            continue;                       /* that destination was never hashed */
        }
        buf_init(&unescaped);
        while (reader_next(&rd) == 1 && rd.complete) {
            const char *sp;
            int st;

            lineno++;
            if (lineno == 1 && strcmp(rd.line.data, "# hashdiff-diff: 1") != 0)
                hd_die("invalid file '%s': unknown format", path);
            if (rd.line.len > 0 && rd.line.data[0] == '#') {
                if (strncmp(rd.line.data, "# mode: ", 8) == 0
                    && strcmp(rd.line.data + 8, mode_line) != 0)
                    hd_die("'%s' was made with other parameters (its '# mode:' line differs); "
                           "use --force to start over", path);
                continue;
            }
            /* EXTRA is skipped: that path is not in ORIGIN (section 3.5). */
            for (st = 0; st < ST_COUNT; st++) {
                size_t len = strlen(status_name(st));

                if (strncmp(rd.line.data, status_name(st), len) != 0
                    || rd.line.data[len] != ' ')
                    continue;
                if (st == ST_EXTRA)
                    break;
                /* STATUS ORIGIN-HASH DESTINATION-HASH PATH: the path is the fourth field. */
                sp = strchr(rd.line.data + len + 1, ' ');
                if (sp != NULL)
                    sp = strchr(sp + 1, ' ');
                if (sp == NULL)
                    hd_die("invalid file '%s': malformed line", path);
                buf_clear(&unescaped);
                if (hd_unescape(&unescaped, sp + 1,
                                rd.line.len - (size_t)(sp + 1 - rd.line.data)) != 0
                    || unescaped.data == NULL)
                    hd_die("invalid file '%s': malformed line", path);
                set_add(&sets[i], unescaped.data);
                set_add(&sets[0], unescaped.data);
                break;
            }
        }
        buf_free(&unescaped);
        reader_close(&rd);
        free(path);
        set_sort(&sets[i]);
    }
    set_sort(&sets[0]);
    free(dir);
    return (unsigned long)sets[0].count;
}
