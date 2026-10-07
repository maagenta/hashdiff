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
#include "util.h"
#include "walk.h"

#define RESULTS_NAME "results.hashdiff"

/* Every file that a run may leave in results.hashdiff (section 3 of the specification). */
static const char *const result_files[] = {
    "tree-origin.txt", "tree-destination.txt", "tree-diff.txt", "hashes-origin.txt",
    "hashes-destination.txt", "diff-files.txt", "rsync-files.lst", "rsync-command.txt",
    "tree-changes.txt"
};

#define N_RESULT_FILES (sizeof(result_files) / sizeof(result_files[0]))

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

struct side {
    const char *name;           /* "origin" or "destination" */
    const char *resume_source;  /* resume: journal of the interrupted run, or NULL */
    unsigned long changes;      /* resume: differences with the saved tree */
    const char *root;           /* as given, trailing slashes removed */
    char *abs_root;
    struct os_stat st;
    long pid;
    int to_child;               /* parent's end of the parent -> child pipe */
    int from_child;             /* parent's end of the child -> parent pipe */
    struct hd_stats tree;
    struct hd_stats hashes;
};

static struct side sides[2];

/* Absolute form of a path: getcwd + concatenation, without resolving symlinks. */
static char *absolute_path(const char *p)
{
    char *cwd, *abs;

    if (p[0] == '/')
        return xstrdup(p);
    cwd = os_getcwd();
    if (cwd == NULL)
        hd_die("cannot get the current directory: %s", strerror(errno));
    if (strcmp(p, ".") == 0)
        return cwd;
    abs = hd_path_join(cwd, p);
    free(cwd);
    return abs;
}

static void stat_dir(const char *what, const char *path, struct os_stat *st)
{
    if (os_stat(path, st) != 0)
        hd_die("cannot access %s '%s': %s", what, path, strerror(errno));
    if (st->kind != OS_DIR)
        hd_die("%s '%s' is not a directory", what, path);
}

static int file_exists(const char *dir, const char *name)
{
    char *p = hd_path_join(dir, name);
    struct os_stat st;
    int r = os_lstat(p, &st) == 0;

    free(p);
    return r;
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

static void clean_results(const char *results)
{
    size_t i;

    for (i = 0; i < N_RESULT_FILES; i++) {
        remove_file(results, result_files[i], "");
        remove_file(results, result_files[i], ".tmp");
    }
    remove_file(results, "hashes-origin.txt", ".new");
    remove_file(results, "hashes-destination.txt", ".new");
    remove_file(results, "tree-changes.txt", ".origin.part");
    remove_file(results, "tree-changes.txt", ".destination.part");
}

static int has_hashes(const char *results)
{
    return file_exists(results, "hashes-origin.txt")
           || file_exists(results, "hashes-origin.txt.tmp")
           || file_exists(results, "hashes-destination.txt")
           || file_exists(results, "hashes-destination.txt.tmp");
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
    if (!has_hashes(results) || o->force) {
        clean_results(results);
        return 0;
    }
    if (o->resume)
        return 1;
    if (os_isatty(0) && os_isatty(2)) {
        if (ask_existing(o->output) == 'r')
            return 1;
        clean_results(results);
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
    static char sources[2][2][64];
    char tree[64];
    int i, j;

    for (i = 0; i < 2; i++) {
        sprintf(sources[i][0], "hashes-%s.txt.tmp", sides[i].name);
        sprintf(sources[i][1], "hashes-%s.txt", sides[i].name);
        sprintf(tree, "tree-%s.txt", sides[i].name);
        remove_file(results, sources[i][1], ".new");
        resume_check_tree(results, tree, sides[i].abs_root);
        sides[i].resume_source = NULL;
        for (j = 0; j < 2 && sides[i].resume_source == NULL; j++)
            if (resume_check_hashes(results, sources[i][j], sides[i].abs_root, mode_line) == 0)
                sides[i].resume_source = sources[i][j];
    }
    remove_file(results, "diff-files.txt", "");
    remove_file(results, "diff-files.txt", ".tmp");
    remove_file(results, "rsync-files.lst", "");
    remove_file(results, "rsync-files.lst", ".tmp");
    remove_file(results, "rsync-command.txt", "");
    remove_file(results, "rsync-command.txt", ".tmp");
    remove_file(results, "tree-changes.txt", "");
    remove_file(results, "tree-changes.txt", ".tmp");
    remove_file(results, "tree-changes.txt", ".origin.part");
    remove_file(results, "tree-changes.txt", ".destination.part");
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
        char tree[64], part[64];

        sprintf(tree, "tree-%s.txt", s->name);
        sprintf(part, "tree-changes.txt.%s.part", s->name);
        m.changes = tree_changes(results, tree, s->abs_root, &list, part);
        if (m.changes == 0)
            remove_file(results, part, "");
    } else {
        tree_write(results, s->name, s->abs_root, &list);
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
    sig = hash_side(s->root, &list, results, s->name, s->abs_root, &hashopts,
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

    for (i = 0; i < 2; i++) {
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

    for (i = 0; i < 2; i++)
        if (sides[i].pid > 0)
            (void)os_kill(sides[i].pid, sig);
    for (i = 0; i < 2; i++) {
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
        kill_children();
        if (code < 0)
            hd_die("the %s process was killed by signal %d", s->name, sig);
        hd_exit(2);
    }
}

static void start_child(int idx, const char *results, const struct hd_opts *o,
                        const struct os_stat *sres)
{
    struct side *s = &sides[idx];
    const struct side *other = &sides[1 - idx];
    struct hd_exclude excl[2];
    int p2c[2], c2p[2], i;
    long pid;

    excl[0].dev = sres->dev;
    excl[0].ino = sres->ino;
    excl[0].what = "results directory";
    excl[1].dev = other->st.dev;
    excl[1].ino = other->st.ino;
    excl[1].what = idx == 0 ? "DESTINATION tree" : "ORIGIN tree";
    if (os_pipe(p2c) != 0 || os_pipe(c2p) != 0)
        hd_die("cannot create a pipe: %s", strerror(errno));
    fflush(NULL);
    pid = os_fork();
    if (pid < 0)
        hd_die("cannot fork: %s", strerror(errno));
    if (pid == 0) {
        hd_is_child = 1;
        for (i = 0; i < 2; i++) {
            if (sides[i].pid > 0) {
                os_close(sides[i].to_child);
                os_close(sides[i].from_child);
            }
        }
        os_close(p2c[1]);
        os_close(c2p[0]);
        run_child(s, p2c[0], c2p[1], results, o, excl, 2);
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

/* Section 6.3: samples, sample bytes and average coverage of the S files. */
static void print_fast_metrics(void)
{
    char a[HD_OFF_DEC_LEN + 8], b[HD_OFF_DEC_LEN + 8], num[HD_OFF_DEC_LEN];
    unsigned long files = sides[0].hashes.sampled + sides[1].hashes.sampled;
    off_t samples = sides[0].hashes.samples + sides[1].hashes.samples;
    off_t bytes = sides[0].hashes.sample_bytes + sides[1].hashes.sample_bytes;
    off_t total = sides[0].hashes.sampled_total + sides[1].hashes.sampled_total;
    off_t basis;

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

static void print_counts(const struct hd_counts *c, int tree)
{
    static const int tree_order[] = { ST_MISSING, ST_EXTRA, ST_SIZE, ST_TYPE, ST_ERR_SRC,
                                      ST_ERR_DST };
    static const int hash_order[] = { ST_HASH, ST_MISSING, ST_EXTRA, ST_SIZE, ST_TYPE,
                                      ST_ERR_SRC, ST_ERR_DST };
    const int *order = tree ? tree_order : hash_order;
    int n = tree ? 6 : 7, i;

    printf("%s:", tree ? "tree differences" : "differences");
    for (i = 0; i < n; i++)
        printf(" %lu %s%s", c->n[order[i]], status_name(order[i]), i + 1 < n ? "," : "\n");
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
    buf_append_str(&b, "# origin: ");
    hd_escape(&b, sides[0].abs_root);
    buf_append_str(&b, "\n# destination: ");
    hd_escape(&b, sides[1].abs_root);
    buf_append_char(&b, '\n');
    outfile_write(&out, b.data, b.len);
    buf_free(&b);
    for (i = 0; i < 2; i++) {
        char *path;
        FILE *f;
        size_t n;

        sprintf(part, "tree-changes.txt.%s.part", sides[i].name);
        path = hd_path_join(results, part);
        outfile_write(&out, i == 0 ? "## origin\n" : "## destination\n", i == 0 ? 10 : 15);
        f = fopen(path, "rb");
        if (f == NULL)
            hd_die("cannot read '%s': %s", path, strerror(errno));
        while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
            outfile_write(&out, chunk, n);
        fclose(f);
        (void)os_unlink(path);
        free(path);
    }
    outfile_commit(&out);
    fprintf(stderr, "hashdiff: the trees changed since the interrupted run; see "
            "tree-changes.txt.\nRestore them to resume, or use --force to start over.\n");
    return 2;
}

/* Summary of a tree stage that found differences; returns the exit status (4). */
static int finish_tree_stage(const struct hd_counts *c, const char *abs_results,
                             const struct hd_opts *o, char **excluded, size_t nexcluded)
{
    struct hd_buf cmd;
    unsigned long errors = c->n[ST_ERR_SRC] + c->n[ST_ERR_DST];
    int i;

    printf("results: %s\n", abs_results);
    for (i = 0; i < 2; i++)
        printf("%s: %lu files, %lu ignored\n", sides[i].name, sides[i].tree.entries,
               sides[i].tree.ignored);
    print_counts(c, 1);
    buf_init(&cmd);
    suggest_command(&cmd, c, sides[0].abs_root, sides[1].abs_root, o->one_fs, excluded,
                    nexcluded);
    if (cmd.len > 0) {
        printf("suggested command (review it first%s):\n%s\n",
               c->n[ST_EXTRA] > 0 ? "; --delete-after deletes the files that exist only "
                                    "in DESTINATION" : "", cmd.data);
    }
    buf_free(&cmd);
    fflush(stdout);
    fputs("hashdiff: ORIGIN and DESTINATION trees differ; nothing was hashed. See "
          "tree-diff.txt,\nfix the differences (for example with the suggested rsync "
          "command) and run hashdiff\nagain.\n", stderr);
    if (errors > 0)
        fprintf(stderr, "hashdiff: %lu paths could not be read (see ERR-SRC / ERR-DST in "
                "tree-diff.txt);\nfix their permissions and run hashdiff again.\n", errors);
    if (c->n[ST_EXTRA] > 0)
        fprintf(stderr, "hashdiff: warning: %lu files exist only in DESTINATION; the "
                "suggested command uses\n--delete-after and will delete them. Review "
                "tree-diff.txt before running it.\n", c->n[ST_EXTRA]);
    return 4;
}

static int finish_hash_stage(const struct hd_counts *c, const char *abs_results,
                             const struct hd_buf *cmd, time_t started, int fast)
{
    char a[HD_OFF_DEC_LEN + 8];
    off_t total = sides[0].hashes.bytes_read + sides[1].hashes.bytes_read;
    long elapsed = (long)(time(NULL) - started);
    unsigned long errors = sides[0].hashes.errors + sides[1].hashes.errors;
    int i;

    printf("results: %s\n", abs_results);
    for (i = 0; i < 2; i++)
        printf("%s: %lu files, %s read, %lu ignored\n", sides[i].name,
               sides[i].hashes.entries, hd_human_bytes(sides[i].hashes.bytes_read, a),
               sides[i].tree.ignored);
    if (resuming) {
        static const char *const checks[] = { "no kept entries", "last kept entry verified",
                                              "last kept entry re-hashed" };

        for (i = 0; i < 2; i++)
            printf("resume %s: %lu entries reused, %s\n", sides[i].name,
                   sides[i].hashes.reused, checks[sides[i].hashes.last_check]);
    }
    if (elapsed > 0) {
        off_t tenths = total / ((off_t)elapsed * 100000);

        printf("elapsed: %ld s, %ld.%d MB/s\n", elapsed, (long)(tenths / 10),
               (int)(tenths % 10));
    } else {
        printf("elapsed: 0 s\n");
    }
    if (fast)
        print_fast_metrics();
    if (counts_total(c) == 0)
        printf("no differences: ORIGIN and DESTINATION match\n");
    else
        print_counts(c, 0);
    if (cmd->len > 0)
        printf("%s\n", cmd->data);
    if (c->n[ST_EXTRA] > 0)
        hd_warn("%lu files exist only in DESTINATION: a tree changed during the run",
                c->n[ST_EXTRA]);
    if (errors > 0 || c->n[ST_ERR_SRC] > 0 || c->n[ST_ERR_DST] > 0)
        return 3;
    return counts_total(c) > 0 ? 1 : 0;
}

int main(int argc, char **argv)
{
    struct hd_opts o;
    struct os_stat sout, sres;
    struct hd_counts counts;
    struct hd_buf cmd, mode;
    char **excluded = NULL;
    size_t nexcluded = 0, k;
    char *results, *abs_results;
    time_t started = time(NULL);
    int r = opts_parse(argc, argv, &o), i, status;

    if (r == OPTS_DONE)
        return fflush(stdout) == 0 ? 0 : 2;
    if (r == OPTS_ERROR)
        return 2;
    sides[0].name = "origin";
    sides[0].root = o.origin;
    sides[1].name = "destination";
    sides[1].root = o.destination;
    stat_dir("ORIGIN", o.origin, &sides[0].st);
    stat_dir("DESTINATION", o.destination, &sides[1].st);
    if (sides[0].st.dev == sides[1].st.dev && sides[0].st.ino == sides[1].st.ino)
        hd_die("ORIGIN and DESTINATION are the same directory");
    stat_dir("output directory", o.output, &sout);
    setup_hashopts(&o, &mode);
    if (o.fast && o.profile == HD_PROFILE_HDD && o.jobs > 1)
        hd_warn("--jobs %d with --profile hdd: several readers on the same disk usually "
                "reduce throughput", o.jobs);
    os_install_signal_handlers();
    results = hd_path_join(o.output, RESULTS_NAME);
    resuming = prepare_results(results, &o);
    stat_dir("results directory", results, &sres);
    abs_results = absolute_path(results);
    for (i = 0; i < 2; i++) {
        sides[i].abs_root = absolute_path(sides[i].root);
        sides[i].pid = 0;
    }
    if (resuming)
        prepare_resume(results, mode.data);

    /* Tree stage: with --serial, one tree at a time. */
    for (i = 0; i < 2; i++) {
        start_child(i, results, &o, &sres);
        if (o.serial)
            receive(&sides[i], MSG_TREE, &sides[i].tree);
    }
    if (!o.serial)
        for (i = 0; i < 2; i++)
            receive(&sides[i], MSG_TREE, &sides[i].tree);
    memset(&counts, 0, sizeof(counts));
    if (!resuming)
        diff_trees(results, sides[0].abs_root, sides[1].abs_root, &counts, &excluded,
                   &nexcluded);
    check_interrupted();

    if (resuming && sides[0].changes + sides[1].changes > 0) {
        for (i = 0; i < 2; i++) {
            command(&sides[i], CMD_EXIT);
            reap_child(&sides[i]);
            close_pipes(&sides[i]);
        }
        status = finish_tree_changes(results);
    } else if (counts_total(&counts) > 0) {
        for (i = 0; i < 2; i++) {
            command(&sides[i], CMD_EXIT);
            reap_child(&sides[i]);
            close_pipes(&sides[i]);
        }
        status = finish_tree_stage(&counts, abs_results, &o, excluded, nexcluded);
    } else {
        /* Hash stage. */
        for (i = 0; i < 2; i++) {
            command(&sides[i], CMD_HASH);
            if (o.serial) {
                receive(&sides[i], MSG_HASHES, &sides[i].hashes);
                reap_child(&sides[i]);
            }
        }
        for (i = 0; i < 2; i++) {
            if (!o.serial) {
                receive(&sides[i], MSG_HASHES, &sides[i].hashes);
                reap_child(&sides[i]);
            }
            close_pipes(&sides[i]);
        }
        buf_init(&cmd);
        diff_hashes(results, abs_results, sides[0].abs_root, sides[1].abs_root, &counts,
                    &cmd);
        check_interrupted();
        status = finish_hash_stage(&counts, abs_results, &cmd, started, o.fast);
        buf_free(&cmd);
    }

    for (k = 0; k < nexcluded; k++)
        free(excluded[k]);
    free(excluded);
    for (i = 0; i < 2; i++)
        free(sides[i].abs_root);
    free(abs_results);
    free(results);
    buf_free(&mode);
    opts_free(&o);
    if (fflush(stdout) != 0)
        return 2;
    return status;
}
