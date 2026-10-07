/* main.c - orchestration, fork/wait and summary. */
#include "config.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "diff.h"
#include "hasher.h"
#include "opts.h"
#include "os.h"
#include "plan.h"
#include "runstate.h"
#include "util.h"
#include "walk.h"

#define RESULTS_NAME "results.hashdiff"

/* Messages from a child to the parent; each is written with a single write(). */
#define MSG_TREE 1      /* tree stage done; stats has entries and ignored */
#define MSG_HASHES 2    /* hash stage done */

struct msg {
    int kind;
    unsigned long changes;      /* MSG_TREE when resuming: differences with the saved tree */
    struct hd_stats stats;
};

static int resuming;

/* Commands from the parent to a child, one byte. */
#define CMD_HASH 'H'
#define CMD_EXIT 'X'

/* One ORIGIN and 1 to 64 destinations (section 2.2). */
#define HD_MAX_SIDES (1 + HD_MAX_DESTINATIONS)

struct side {
    const char *name;           /* "origin", "destination" or "destination-N" */
    char label[24];             /* backs name when it carries a number */
    char role[24];              /* "ORIGIN", "DESTINATION-2", for the messages */
    char what[32];              /* "ORIGIN tree", for the exclusion warning */
    struct hd_names names;      /* the files of this side (section 3) */
    const char *resume_source;  /* resume: journal of the interrupted run, or NULL */
    unsigned long changes;      /* resume: differences with the saved tree */
    const char *root;           /* cleaned absolute path (section 2.1) */
    struct os_stat st;
    long pid;
    int to_child;               /* parent's end of the parent -> child pipe */
    int from_child;             /* parent's end of the child -> parent pipe */
    struct hd_stats tree;
    struct hd_stats hashes;
    int hashed;                 /* destinations: its tree matched, so it was hashed */
    int failed;                 /* destinations: its process failed, so it is not compared */
    struct hd_counts counts;    /* destinations: tree or content differences */
    struct hd_buf cmd;          /* destinations: the command to print, empty if none */
};

static struct side sides[HD_MAX_SIDES];
static int nsides;              /* 1 + ndest */
static int ndest;
static int side_failed;         /* a destination process failed: exit 2 (section 9) */

/*
 * Side names and file names (sections 2.2 and 3). With one destination every name is the one
 * version 1.1 used, so a single-destination run writes exactly the files it wrote before.
 */
static void setup_sides(const struct hd_opts *o)
{
    int i;

    ndest = o->ndest;
    nsides = 1 + ndest;
    for (i = 0; i < nsides; i++) {
        struct side *s = &sides[i];

        if (i == 0) {
            s->name = "origin";
            s->root = o->origin;
            strcpy(s->role, "ORIGIN");
            strcpy(s->what, "ORIGIN tree");
        } else {
            if (ndest == 1) {
                s->name = "destination";
                strcpy(s->role, "DESTINATION");
                strcpy(s->what, "DESTINATION tree");
            } else {
                sprintf(s->label, "destination-%d", i);
                s->name = s->label;
                sprintf(s->role, "DESTINATION-%d", i);
                sprintf(s->what, "%s tree", s->label);
            }
            s->root = o->dest[i - 1];
        }
        sprintf(s->names.tree, "tree-%s.txt", s->name);
        sprintf(s->names.hashes, "hashes-%s.txt", s->name);
        if (i == 0)
            continue;
        if (ndest == 1) {
            strcpy(s->names.tree_diff, "tree-diff.txt");
            strcpy(s->names.diff_files, "diff-files.txt");
            strcpy(s->names.rsync_list, "rsync-files.lst");
            strcpy(s->names.rsync_command, "rsync-command.txt");
        } else {
            sprintf(s->names.tree_diff, "tree-diff-%s.txt", s->name);
            sprintf(s->names.diff_files, "diff-files-%s.txt", s->name);
            sprintf(s->names.rsync_list, "rsync-files-%s.lst", s->name);
            sprintf(s->names.rsync_command, "rsync-command-%s.txt", s->name);
        }
    }
}

static void stat_dir(const char *what, const char *path, struct os_stat *st)
{
    if (os_stat(path, st) != 0)
        hd_die("cannot access %s '%s': %s", what, path, strerror(errno));
    if (st->kind != OS_DIR)
        hd_die("%s '%s' is not a directory", what, path);
}

static void remove_file(const char *dir, const char *name, const char *suffix)
{
    char *p = hd_path_join(dir, name);
    char *q = xmalloc(strlen(p) + strlen(suffix) + 1);

    strcpy(q, p);
    strcat(q, suffix);
    if (os_unlink(q) != 0 && errno != ENOENT)
        hd_die("cannot remove '%s': %s", q, strerror(errno));
    free(q);
    free(p);
}

/* Asks on the terminal what to do with an existing results.hashdiff: 'r' or 'o'. */
static char ask_existing(const char *output)
{
    for (;;) {
        char line[64], c;
        size_t n = 0;
        ssize_t r;

        fprintf(stderr, "results.hashdiff already exists in %s: [r]esume, [o]verwrite or "
                "[a]bort? ", output);
        fflush(stderr);
        while ((r = os_read(0, &c, 1)) == 1 && c != '\n')
            if (n + 1 < sizeof(line))
                line[n++] = c;
        if (r < 0 && errno == EINTR && os_caught_signal())
            hd_exit(128 + os_caught_signal());
        if (r <= 0 && n == 0) {
            fputc('\n', stderr);
            hd_exit(2);
        }
        while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\r'))
            n--;
        line[n] = '\0';
        if (strcmp(line, "r") == 0 || strcmp(line, "R") == 0 || strcmp(line, "resume") == 0)
            return 'r';
        if (strcmp(line, "o") == 0 || strcmp(line, "O") == 0
            || strcmp(line, "overwrite") == 0)
            return 'o';
        if (strcmp(line, "a") == 0 || strcmp(line, "A") == 0 || strcmp(line, "abort") == 0)
            hd_exit(2);
        if (r <= 0)
            hd_exit(2);
    }
}

/* Creates DIR/results.hashdiff, or decides what to do with an existing one; returns 1 if
 * the run resumes. */
static int prepare_results(const char *results, const struct hd_opts *o)
{
    struct os_stat st;

    if (os_mkdir(results) == 0)
        return 0;
    if (errno != EEXIST)
        hd_die("cannot create '%s': %s", results, strerror(errno));
    stat_dir("results directory", results, &st);
    if (!runstate_has_hashes(results) || o->force) {
        runstate_clean(results);
        return 0;
    }
    if (o->resume)
        return 1;
    if (os_isatty(0) && os_isatty(2)) {
        if (ask_existing(o->output) == 'r')
            return 1;
        runstate_clean(results);
        return 0;
    }
    hd_die("'%s' already exists; use --resume to continue the previous run or --force to "
           "start over", results);
    return 0;
}

/* Steps 1-5 of section 3.2, before forking. */
static void prepare_resume(const char *results, const char *mode_line)
{
    /* Candidate sources of each side, in order of preference. */
    static char sources[HD_MAX_SIDES][2][64];
    char part[64];
    int i, j;

    for (i = 0; i < nsides; i++) {
        sprintf(sources[i][0], "hashes-%s.txt.tmp", sides[i].name);
        sprintf(sources[i][1], "hashes-%s.txt", sides[i].name);
        remove_file(results, sources[i][1], ".new");
        resume_check_tree(results, sides[i].names.tree, sides[i].root);
        sides[i].resume_source = NULL;
        for (j = 0; j < 2 && sides[i].resume_source == NULL; j++)
            if (resume_check_hashes(results, sources[i][j], sides[i].root, mode_line) == 0)
                sides[i].resume_source = sources[i][j];
        sprintf(part, "tree-changes.txt.%s.part", sides[i].name);
        remove_file(results, part, "");
    }
    for (i = 1; i < nsides; i++) {
        remove_file(results, sides[i].names.diff_files, "");
        remove_file(results, sides[i].names.diff_files, ".tmp");
        remove_file(results, sides[i].names.rsync_list, "");
        remove_file(results, sides[i].names.rsync_list, ".tmp");
        remove_file(results, sides[i].names.rsync_command, "");
        remove_file(results, sides[i].names.rsync_command, ".tmp");
    }
    remove_file(results, "tree-changes.txt", "");
    remove_file(results, "tree-changes.txt", ".tmp");
}

/* ---- child side ---- */

static void child_send(int fd, const struct msg *m)
{
    if (hd_write_all(fd, m, sizeof(*m)) != 0) {
        if (os_caught_signal())
            hd_exit(128 + os_caught_signal());
        hd_die("cannot write to the parent process: %s", strerror(errno));
    }
}

static struct hd_hashopts hashopts;

static void run_child(struct side *s, int in_fd, int out_fd, const char *results,
                      const struct hd_opts *o, const struct hd_exclude *excl, size_t nexcl)
{
    struct walk_opts wo;
    struct hd_list list;
    struct msg m;
    char cmd;
    int sig;

    wo.side = s->name;
    wo.one_fs = o->one_fs;
    wo.excludes = excl;
    wo.nexcludes = nexcl;
    walk_tree(s->root, &wo, &list);
    if (os_caught_signal())
        hd_exit(128 + os_caught_signal());
    list_sort(&list);
    memset(&m, 0, sizeof(m));
    if (resuming) {
        char part[64];

        sprintf(part, "tree-changes.txt.%s.part", s->name);
        m.changes = tree_changes(results, s->names.tree, s->root, &list, part);
        if (m.changes == 0)
            remove_file(results, part, "");
    } else {
        tree_write(results, s->name, s->root, &list);
    }
    m.kind = MSG_TREE;
    stats_init(&m.stats);
    m.stats.entries = (unsigned long)list.count;
    m.stats.ignored = list.ignored;
    child_send(out_fd, &m);

    if (hd_read_full(in_fd, &cmd, 1) != 1 || cmd != CMD_HASH) {
        list_free(&list);
        hd_exit(os_caught_signal() ? 128 + os_caught_signal() : 0);
    }
    memset(&m, 0, sizeof(m));
    m.kind = MSG_HASHES;
    stats_init(&m.stats);
    sig = hash_side(s->root, &list, results, s->name, s->root, &hashopts,
                    resuming ? s->resume_source : NULL, &m.stats);
    if (sig != 0)
        hd_exit(128 + sig);
    child_send(out_fd, &m);
    list_free(&list);
    hd_exit(m.stats.errors > 0 ? 3 : 0);
}

/* ---- parent side ---- */

static void kill_children(void)
{
    int i, code, sig;

    for (i = 0; i < nsides; i++) {
        if (sides[i].pid > 0) {
            (void)os_kill(sides[i].pid, SIGTERM);
            (void)os_wait(sides[i].pid, &code, &sig);
            sides[i].pid = 0;
        }
    }
}

/* SIGINT/SIGTERM in the parent: forward it to the children, wait for them (they keep their
 * journals for --resume) and exit with 128 + signal number. */
static void interrupted(void)
{
    int i, code, sig = os_caught_signal();

    for (i = 0; i < nsides; i++)
        if (sides[i].pid > 0)
            (void)os_kill(sides[i].pid, sig);
    for (i = 0; i < nsides; i++) {
        if (sides[i].pid > 0) {
            (void)os_wait(sides[i].pid, &code, &sig);
            sides[i].pid = 0;
        }
    }
    fputs("hashdiff: interrupted\n", stderr);
    hd_exit(128 + os_caught_signal());
}

static void check_interrupted(void)
{
    if (os_caught_signal())
        interrupted();
}

/* Waits for a child that should have exited; a failure is fatal for the whole run. */
static void reap_child(struct side *s)
{
    int code, sig;

    if (os_wait(s->pid, &code, &sig) != 0)
        hd_die("cannot wait for the %s process: %s", s->name, strerror(errno));
    s->pid = 0;
    check_interrupted();
    if (code != 0 && code != 3) {
        if (code < 0)
            hd_warn("the %s process was killed by signal %d", s->name, sig);
        else
            hd_warn("the %s process failed", s->name);
        if (s == &sides[0]) {
            /* Without ORIGIN there is nothing to compare anything with. */
            kill_children();
            hd_exit(2);
        }
        s->failed = 1;
        s->hashed = 0;
        side_failed = 1;
    }
}

static void start_child(int idx, const char *results, const struct hd_opts *o,
                        const struct os_stat *sres)
{
    struct side *s = &sides[idx];
    struct hd_exclude excl[HD_MAX_SIDES];
    size_t nexcl = 0;
    int p2c[2], c2p[2], i;
    long pid;

    excl[nexcl].dev = sres->dev;
    excl[nexcl].ino = sres->ino;
    excl[nexcl].what = "results directory";
    nexcl++;
    for (i = 0; i < nsides; i++) {
        if (i == idx)
            continue;
        excl[nexcl].dev = sides[i].st.dev;
        excl[nexcl].ino = sides[i].st.ino;
        excl[nexcl].what = sides[i].what;
        nexcl++;
    }
    if (os_pipe(p2c) != 0 || os_pipe(c2p) != 0)
        hd_die("cannot create a pipe: %s", strerror(errno));
    fflush(NULL);
    pid = os_fork();
    if (pid < 0)
        hd_die("cannot fork: %s", strerror(errno));
    if (pid == 0) {
        hd_is_child = 1;
        for (i = 0; i < nsides; i++) {
            if (sides[i].pid > 0) {
                os_close(sides[i].to_child);
                os_close(sides[i].from_child);
            }
        }
        os_close(p2c[1]);
        os_close(c2p[0]);
        run_child(s, p2c[0], c2p[1], results, o, excl, nexcl);
    }
    os_close(p2c[0]);
    os_close(c2p[1]);
    s->pid = pid;
    s->to_child = p2c[1];
    s->from_child = c2p[0];
}

/* Reads one message of the expected kind; if the child died instead, the run fails. */
static void receive(struct side *s, int kind, struct hd_stats *st)
{
    struct msg m;

    if (hd_read_full(s->from_child, &m, sizeof(m)) == (ssize_t)sizeof(m) && m.kind == kind) {
        *st = m.stats;
        if (kind == MSG_TREE)
            s->changes = m.changes;
        return;
    }
    check_interrupted();
    reap_child(s);
    kill_children();
    hd_die("the %s process ended unexpectedly", s->name);
}

static void command(struct side *s, char cmd)
{
    if (hd_write_all(s->to_child, &cmd, 1) != 0) {
        check_interrupted();
        kill_children();
        hd_die("cannot write to the %s process: %s", s->name, strerror(errno));
    }
}

static void close_pipes(struct side *s)
{
    os_close(s->to_child);
    os_close(s->from_child);
}

/* Hash stage parameters and the "# mode:" line (section 8 of the specification). */
static void setup_hashopts(const struct hd_opts *o, struct hd_buf *mode)
{
    const struct hd_profile *prof = plan_profile(o->profile);
    const char *env = getenv("HASHDIFF_MERGE_GAP");
    char num[HD_OFF_DEC_LEN];

    hashopts.fast = o->fast;
    hashopts.hdd = o->profile == HD_PROFILE_HDD;
    hashopts.jobs = o->jobs;
    hashopts.progress = !o->quiet && os_isatty(2);
    hashopts.gap = o->gap;
    hashopts.block = o->block > 0 ? o->block : prof->default_block;
    hashopts.seek_bytes = prof->seek_bytes;
    hashopts.merge_gap = prof->seek_bytes;
    if (env != NULL && opts_parse_size(env, &hashopts.merge_gap) != 0)
        hd_die("invalid HASHDIFF_MERGE_GAP '%s'", env);
    buf_init(mode);
    if (!o->fast) {
        buf_append_str(mode, "full");
    } else {
        buf_append_str(mode, "fast gap=");
        buf_append_str(mode, hd_off_to_dec(hashopts.gap, num));
        buf_append_str(mode, " block=");
        buf_append_str(mode, hd_off_to_dec(hashopts.block, num));
        buf_append_str(mode, " profile=");
        buf_append_str(mode, prof->name);
        buf_append_str(mode, " seek_bytes=");
        buf_append_str(mode, hd_off_to_dec(hashopts.seek_bytes, num));
    }
    hashopts.mode_line = mode->data;
}

/* Section 2: the exit status of a run is the most severe of every destination's. */
static int status_rank(int status)
{
    switch (status) {
    case 2:
        return 4;
    case 4:
        return 3;
    case 3:
        return 2;
    case 1:
        return 1;
    default:
        return 0;
    }
}

static int worse(int a, int b)
{
    return status_rank(b) > status_rank(a) ? b : a;
}

/* Section 6.3: samples, sample bytes and average coverage of the S files of every side that
 * was hashed. */
static void print_fast_metrics(int any_hashed)
{
    char a[HD_OFF_DEC_LEN + 8], b[HD_OFF_DEC_LEN + 8], num[HD_OFF_DEC_LEN];
    unsigned long files = 0;
    off_t samples = 0, bytes = 0, total = 0, basis;
    int i;

    for (i = 0; i < nsides; i++) {
        if (!(i == 0 ? any_hashed : sides[i].hashed))
            continue;
        files += sides[i].hashes.sampled;
        samples += sides[i].hashes.samples;
        bytes += sides[i].hashes.sample_bytes;
        total += sides[i].hashes.sampled_total;
    }
    if (files == 0) {
        printf("sampled files: 0\n");
        return;
    }
    /* Coverage in hundredths of a percent, without overflowing bytes * 10000. */
    basis = total >= 10000 ? bytes / (total / 10000) : bytes * 10000 / total;
    printf("sampled files: %lu, %s samples, %s read of %s (coverage %ld.%02ld %%)\n", files,
           hd_off_to_dec(samples, num), hd_human_bytes(bytes, a), hd_human_bytes(total, b),
           (long)(basis / 100), (long)(basis % 100));
}

/*
 * The counts line of one destination (section 9). With one destination the labels are the ones
 * of version 1.1; with several, every line names its destination.
 */
static void print_counts(const struct side *s, int tree)
{
    static const int tree_order[] = { ST_MISSING, ST_EXTRA, ST_SIZE, ST_TYPE, ST_ERR_SRC,
                                      ST_ERR_DST };
    static const int hash_order[] = { ST_HASH, ST_MISSING, ST_EXTRA, ST_SIZE, ST_TYPE,
                                      ST_ERR_SRC, ST_ERR_DST };
    const int *order = tree ? tree_order : hash_order;
    int n = tree ? 6 : 7, i;

    if (!tree && counts_total(&s->counts) == 0) {
        if (ndest == 1)
            printf("no differences: ORIGIN and DESTINATION match\n");
        else
            printf("no differences with %s\n", s->name);
        return;
    }
    fputs(tree ? "tree differences" : "differences", stdout);
    if (ndest > 1)
        printf(" with %s", s->name);
    fputc(':', stdout);
    for (i = 0; i < n; i++)
        printf(" %lu %s%s", s->counts.n[order[i]], status_name(order[i]),
               i + 1 < n ? "," : "\n");
}

/* The stderr messages for a destination whose tree differs from ORIGIN (section 3.1). */
static void tree_stage_messages(const struct side *s)
{
    unsigned long extra = s->counts.n[ST_EXTRA];
    unsigned long errors = s->counts.n[ST_ERR_SRC] + s->counts.n[ST_ERR_DST];
    const char *who = ndest == 1 ? "DESTINATION" : s->name;
    const char *file = s->names.tree_diff;

    if (ndest == 1)
        fputs("hashdiff: ORIGIN and DESTINATION trees differ; nothing was hashed. See "
              "tree-diff.txt,\nfix the differences (for example with the suggested rsync "
              "command) and run hashdiff\nagain.\n", stderr);
    else
        fprintf(stderr, "hashdiff: the trees of ORIGIN and %s differ; %s was not hashed. "
                "See %s,\nfix the differences (for example with the suggested rsync "
                "command) and run hashdiff\nagain.\n", s->name, s->name, file);
    if (errors > 0)
        fprintf(stderr, "hashdiff: %lu path%s could not be read (see ERR-SRC / ERR-DST in "
                "%s);\nfix their permissions and run hashdiff again.\n", errors,
                hd_plural(errors), file);
    if (extra > 0)
        fprintf(stderr, "hashdiff: warning: %lu file%s exist%s only in %s; the suggested "
                "command uses\n--delete-after and will delete %s. Review %s before running "
                "it.\n", extra, hd_plural(extra), extra == 1 ? "s" : "", who,
                extra == 1 ? "it" : "them", file);
}

/* Resume with changed trees: tree-changes.txt from the children's parts, then exit 2. */
static int finish_tree_changes(const char *results)
{
    struct hd_outfile out;
    struct hd_buf b;
    char part[64], chunk[65536];
    int i;

    outfile_open(&out, results, "tree-changes.txt");
    buf_init(&b);
    for (i = 0; i < nsides; i++) {
        buf_append_str(&b, "# ");
        buf_append_str(&b, sides[i].name);
        buf_append_str(&b, ": ");
        hd_escape(&b, sides[i].root);
        buf_append_char(&b, '\n');
    }
    outfile_write(&out, b.data, b.len);
    for (i = 0; i < nsides; i++) {
        char *path;
        FILE *f;
        size_t n;

        buf_clear(&b);
        buf_append_str(&b, "## ");
        buf_append_str(&b, sides[i].name);
        buf_append_char(&b, '\n');
        outfile_write(&out, b.data, b.len);
        sprintf(part, "tree-changes.txt.%s.part", sides[i].name);
        path = hd_path_join(results, part);
        f = fopen(path, "rb");
        /* A side that did not change has no part file: its section is empty. */
        if (f == NULL && errno != ENOENT)
            hd_die("cannot read '%s': %s", path, strerror(errno));
        if (f != NULL) {
            while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
                outfile_write(&out, chunk, n);
            fclose(f);
            (void)os_unlink(path);
        }
        free(path);
    }
    buf_free(&b);
    outfile_commit(&out);
    fprintf(stderr, "hashdiff: the trees changed since the interrupted run; see "
            "tree-changes.txt.\nRestore them to resume, or use --force to start over.\n");
    return 2;
}

/*
 * The summary of section 9 and the exit status. It is printed for every run, including one
 * where every destination stopped in the tree stage and nothing was hashed; the lines that
 * describe reading are then absent.
 */
static int finish_run(const char *results, const struct hd_opts *o, time_t started,
                      int any_hashed)
{
    char a[HD_OFF_DEC_LEN + 8];
    off_t total = 0;
    long elapsed = (long)(time(NULL) - started);
    int status = 0, commands = 0, i;

    printf("results: %s\n", results);
    for (i = 0; i < nsides; i++) {
        const struct side *s = &sides[i];

        if (i == 0 ? any_hashed : s->hashed) {
            printf("%s: %lu file%s, %s read, %lu ignored\n", s->name, s->hashes.entries,
                   hd_plural(s->hashes.entries), hd_human_bytes(s->hashes.bytes_read, a),
                   s->tree.ignored);
            total += s->hashes.bytes_read;
        } else {
            printf("%s: %lu file%s, %lu ignored\n", s->name, s->tree.entries,
                   hd_plural(s->tree.entries), s->tree.ignored);
        }
    }
    if (resuming) {
        static const char *const checks[] = { "no kept entries", "last kept entry verified",
                                              "last kept entry re-hashed" };

        for (i = 0; i < nsides; i++) {
            if (!(i == 0 ? any_hashed : sides[i].hashed))
                continue;
            printf("resume %s: %lu %s reused, %s\n", sides[i].name, sides[i].hashes.reused,
                   sides[i].hashes.reused == 1 ? "entry" : "entries",
                   checks[sides[i].hashes.last_check]);
        }
    }
    if (any_hashed) {
        if (elapsed > 0) {
            off_t tenths = total / ((off_t)elapsed * 100000);

            printf("elapsed: %ld s, %ld.%d MB/s\n", elapsed, (long)(tenths / 10),
                   (int)(tenths % 10));
        } else {
            printf("elapsed: 0 s, - MB/s\n");
        }
        if (o->fast)
            print_fast_metrics(any_hashed);
    }
    for (i = 1; i < nsides; i++) {
        if (!sides[i].failed)
            print_counts(&sides[i], !sides[i].hashed);
    }
    /* Every command block after every counts line, in command-line order (section 9). */
    for (i = 1; i < nsides; i++) {
        const struct side *s = &sides[i];

        if (s->cmd.len == 0)
            continue;
        if (s->hashed) {
            if (ndest > 1)
                printf("command for %s:\n", s->name);
            commands++;
        } else {
            if (ndest > 1)
                printf("suggested command for %s", s->name);
            else
                fputs("suggested command", stdout);
            if (s->counts.n[ST_EXTRA] > 0)
                printf(" (review it first; --delete-after deletes the files that exist only "
                       "in %s):\n", ndest == 1 ? "DESTINATION" : s->name);
            else
                fputs(" (review it first):\n", stdout);
        }
        printf("%s\n", s->cmd.data);
    }
    if (commands > 0) {
        if (ndest == 1)
            printf("NOTE: the command above copies with rsync only the files whose content "
                   "did not match.\n      There is a copy of it in "
                   "results.hashdiff/rsync-command.txt\n");
        else
            printf("NOTE: the commands above copy with rsync only the files whose content "
                   "did not match.\n      There is a copy of each one in its "
                   "results.hashdiff/rsync-command-destination-N.txt\n");
    }
    fflush(stdout);

    for (i = 1; i < nsides; i++) {
        struct side *s = &sides[i];
        unsigned long errors;

        if (s->failed) {
            status = worse(status, 2);
            continue;
        }
        if (!s->hashed) {
            tree_stage_messages(s);
            status = worse(status, 4);
            continue;
        }
        if (s->counts.n[ST_EXTRA] > 0)
            hd_warn("%lu file%s exist%s only in %s: a tree changed during the run",
                    s->counts.n[ST_EXTRA], hd_plural(s->counts.n[ST_EXTRA]),
                    s->counts.n[ST_EXTRA] == 1 ? "s" : "",
                    ndest == 1 ? "DESTINATION" : s->name);
        errors = sides[0].hashes.errors + s->hashes.errors + s->counts.n[ST_ERR_SRC]
                 + s->counts.n[ST_ERR_DST];
        if (errors > 0)
            status = worse(status, 3);
        else if (counts_total(&s->counts) > 0)
            status = worse(status, 1);
    }
    return side_failed ? worse(status, 2) : status;
}


int main(int argc, char **argv)
{
    struct hd_opts o;
    struct os_stat sout, sres;
    struct hd_buf mode;
    char **excluded = NULL;
    size_t nexcluded = 0, k;
    char *results;
    time_t started = time(NULL);
    int r = opts_parse(argc, argv, &o), i, j, status, changed = 0, nhash = 0;

    if (r == OPTS_DONE)
        return fflush(stdout) == 0 ? 0 : 2;
    if (r == OPTS_ERROR)
        return 2;
    setup_sides(&o);
    for (i = 0; i < nsides; i++)
        stat_dir(sides[i].role, sides[i].root, &sides[i].st);
    /* Every two roots must be a different directory (section 2.2). */
    for (i = 0; i < nsides; i++) {
        for (j = i + 1; j < nsides; j++) {
            if (sides[i].st.dev == sides[j].st.dev && sides[i].st.ino == sides[j].st.ino)
                hd_die("%s and %s are the same directory", sides[i].role, sides[j].role);
        }
    }
    stat_dir("output directory", o.output, &sout);
    setup_hashopts(&o, &mode);
    if (o.fast && o.profile == HD_PROFILE_HDD && o.jobs > 1)
        hd_warn("--jobs %d with --profile hdd: several readers on the same disk usually "
                "reduce throughput", o.jobs);
    if (nsides * o.jobs > 64)
        hd_warn("%d sides with --jobs %d: %d readers at once, more than the disks can serve "
                "usually reduces throughput", nsides, o.jobs, nsides * o.jobs);
    os_install_signal_handlers();
    results = hd_path_join(o.output, RESULTS_NAME);
    resuming = prepare_results(results, &o);
    stat_dir("results directory", results, &sres);
    for (i = 0; i < nsides; i++)
        sides[i].pid = 0;
    if (resuming)
        prepare_resume(results, mode.data);

    /* Tree stage: with --serial, one side at a time, in command-line order. */
    for (i = 0; i < nsides; i++) {
        start_child(i, results, &o, &sres);
        if (o.serial)
            receive(&sides[i], MSG_TREE, &sides[i].tree);
    }
    if (!o.serial)
        for (i = 0; i < nsides; i++)
            receive(&sides[i], MSG_TREE, &sides[i].tree);
    for (i = 0; i < nsides; i++)
        changed += sides[i].changes > 0;
    if (!resuming) {
        for (i = 1; i < nsides; i++)
            diff_trees(results, &sides[i].names, sides[0].root, sides[i].root,
                       &sides[i].counts, &excluded, &nexcluded);
    } else if (!changed) {
        /*
         * A resumed run does not compare the trees again: every tree is verified unchanged,
         * so the decision of the tree stage still holds and is read back from the tree-diff
         * files, which are not rewritten (section 3.4). Only the excluded paths have to be
         * read again, for the suggested commands.
         */
        for (i = 0; i < nsides; i++)
            tree_excluded(results, sides[i].names.tree, sides[i].root, &excluded, &nexcluded);
        for (i = 1; i < nsides; i++)
            (void)report_counts(results, sides[i].names.tree_diff, &sides[i].counts);
    }
    check_interrupted();
    for (i = 1; i < nsides; i++) {
        sides[i].hashed = counts_total(&sides[i].counts) == 0;
        nhash += sides[i].hashed;
    }

    if (resuming && changed) {
        for (i = 0; i < nsides; i++) {
            command(&sides[i], CMD_EXIT);
            reap_child(&sides[i]);
            close_pipes(&sides[i]);
        }
        status = finish_tree_changes(results);
    } else {
        /* Hash stage: the destinations whose tree matched, and ORIGIN if any of them did. */
        for (i = 0; i < nsides; i++) {
            int go = i == 0 ? nhash > 0 : sides[i].hashed;

            command(&sides[i], go ? CMD_HASH : CMD_EXIT);
            if (!go || o.serial) {
                if (go)
                    receive(&sides[i], MSG_HASHES, &sides[i].hashes);
                reap_child(&sides[i]);
                close_pipes(&sides[i]);
            }
        }
        if (!o.serial) {
            for (i = 0; i < nsides; i++) {
                if (!(i == 0 ? nhash > 0 : sides[i].hashed))
                    continue;
                receive(&sides[i], MSG_HASHES, &sides[i].hashes);
                reap_child(&sides[i]);
                close_pipes(&sides[i]);
            }
        }
        for (i = 1; i < nsides; i++) {
            buf_init(&sides[i].cmd);
            if (sides[i].failed)
                continue;
            if (sides[i].hashed)
                diff_hashes(results, &sides[i].names, sides[0].root, sides[i].root,
                            &sides[i].counts, &sides[i].cmd);
            else
                suggest_command(&sides[i].cmd, &sides[i].counts, sides[0].root,
                                sides[i].root, o.one_fs, excluded, nexcluded);
        }
        check_interrupted();
        status = finish_run(results, &o, started, nhash > 0);
        for (i = 1; i < nsides; i++)
            buf_free(&sides[i].cmd);
    }

    for (k = 0; k < nexcluded; k++)
        free(excluded[k]);
    free(excluded);
    free(results);
    buf_free(&mode);
    opts_free(&o);
    if (fflush(stdout) != 0)
        return 2;
    return status;
}
