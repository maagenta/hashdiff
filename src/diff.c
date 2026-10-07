/* diff.c - merge-join of two lists, tree-diff.txt, diff-files.txt and the rsync outputs. */
#include "config.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "diff.h"
#include "os.h"

static const char *const status_names[ST_COUNT] = {
    "MISSING", "EXTRA", "SIZE", "TYPE", "HASH", "ERR-SRC", "ERR-DST"
};

const char *status_name(int status)
{
    return status_names[status];
}

unsigned long counts_total(const struct hd_counts *c)
{
    unsigned long t = 0;
    int i;

    for (i = 0; i < ST_COUNT; i++)
        t += c->n[i];
    return t;
}

void record_init(struct hd_record *r)
{
    buf_init(&r->path);
    buf_append(&r->path, "", 0);
}

void record_free(struct hd_record *r)
{
    buf_free(&r->path);
}

/* Splits the first three space-separated fields; the rest of the line is the path. */
static int split_fields(const char *line, size_t len, const char *f[4], size_t flen[4])
{
    size_t i, start = 0;
    int k = 0;

    for (i = 0; i < len && k < 3; i++) {
        if (line[i] == ' ') {
            f[k] = line + start;
            flen[k] = i - start;
            k++;
            start = i + 1;
        }
    }
    if (k < 3 || start >= len)
        return -1;
    f[3] = line + start;
    flen[3] = len - start;
    return 0;
}

static int parse_nonneg(const char *s, size_t n, off_t *out)
{
    if (n == 0 || s[0] == '-')
        return -1;
    return hd_dec_to_off(s, n, out);
}

int record_parse(int fmt, const char *line, size_t len, struct hd_record *r)
{
    const char *f[4];
    size_t flen[4];

    if (split_fields(line, len, f, flen) != 0 || flen[0] != 1)
        return -1;
    r->type = f[0][0];
    r->mtime = 0;
    r->hash[0] = '\0';
    if (r->type == 'E') {
        if (parse_nonneg(f[1], flen[1], &r->num) != 0 || flen[2] != 1 || f[2][0] != '-')
            return -1;
    } else if (fmt == FMT_TREE) {
        if ((r->type != 'F' && r->type != 'L') || parse_nonneg(f[1], flen[1], &r->num) != 0
            || hd_dec_to_off(f[2], flen[2], &r->mtime) != 0)
            return -1;
    } else {
        size_t i;

        if (r->type != 'F' && r->type != 'S' && r->type != 'L')
            return -1;
        if (flen[1] != 32)
            return -1;
        for (i = 0; i < 32; i++)
            if (!((f[1][i] >= '0' && f[1][i] <= '9') || (f[1][i] >= 'a' && f[1][i] <= 'f')))
                return -1;
        memcpy(r->hash, f[1], 32);
        r->hash[32] = '\0';
        if (parse_nonneg(f[2], flen[2], &r->num) != 0)
            return -1;
    }
    buf_clear(&r->path);
    return hd_unescape(&r->path, f[3], flen[3]);
}

/* A tree or hashes file read as a stream of validated records; any error is fatal. */
struct stream {
    struct hd_reader rd;
    char *path;
    int fmt;
    int has;                    /* cur holds a record */
    struct hd_record cur;
    struct hd_buf prev;         /* path of the previous record, for the order check */
    struct hd_buf mode;         /* "# mode:" value of a hashes file */
    char **excluded;            /* "# excluded:" values of a tree file */
    size_t nexcluded;
};

static void stream_fail(struct stream *s, const char *why)
{
    hd_die("invalid file '%s': %s", s->path, why);
}

static void stream_read_line(struct stream *s, int *got)
{
    int r = reader_next(&s->rd);

    if (r < 0)
        hd_die("cannot read '%s': %s", s->path, strerror(errno));
    *got = r;
    if (r == 1 && !s->rd.complete)
        stream_fail(s, "last line is not terminated");
}

static void stream_take_record(struct stream *s)
{
    if (record_parse(s->fmt, s->rd.line.data, s->rd.line.len, &s->cur) != 0)
        stream_fail(s, "malformed line");
    if (s->prev.len > 0 && strcmp(s->prev.data, s->cur.path.data) >= 0)
        stream_fail(s, "paths are not in strictly increasing order");
    buf_clear(&s->prev);
    buf_append(&s->prev, s->cur.path.data, s->cur.path.len);
    s->has = 1;
}

static void stream_next(struct stream *s)
{
    int got;

    stream_read_line(s, &got);
    if (!got) {
        s->has = 0;
        return;
    }
    stream_take_record(s);
}

/* Header value of a "# key: value" line, or NULL. */
static const char *header_value(const struct hd_buf *line, const char *key)
{
    size_t klen = strlen(key);

    if (line->len >= klen + 4 && strncmp(line->data, "# ", 2) == 0
        && strncmp(line->data + 2, key, klen) == 0
        && strncmp(line->data + 2 + klen, ": ", 2) == 0)
        return line->data + 4 + klen;
    return NULL;
}

static void stream_open(struct stream *s, const char *dir, const char *name, int fmt,
                        const char *abs_root)
{
    const char *magic = fmt == FMT_TREE ? "# hashdiff-tree: 1" : "# hashdiff-format: 2";
    struct hd_buf root;
    const char *v;
    int got, lineno = 0, seen_root = 0;

    s->path = hd_path_join(dir, name);
    s->fmt = fmt;
    s->has = 0;
    s->excluded = NULL;
    s->nexcluded = 0;
    record_init(&s->cur);
    buf_init(&s->prev);
    buf_init(&s->mode);
    buf_append(&s->mode, "", 0);
    if (reader_open(&s->rd, s->path) != 0)
        hd_die("cannot open '%s': %s", s->path, strerror(errno));
    for (;;) {
        stream_read_line(s, &got);
        if (!got)
            break;
        lineno++;
        if (lineno == 1 && strcmp(s->rd.line.data, magic) != 0)
            stream_fail(s, "unknown format");
        if (s->rd.line.len == 0 || s->rd.line.data[0] != '#') {
            stream_take_record(s);
            break;
        }
        if ((v = header_value(&s->rd.line, "root")) != NULL) {
            buf_init(&root);
            if (hd_unescape(&root, v, strlen(v)) != 0 || root.data == NULL
                || strcmp(root.data, abs_root) != 0)
                stream_fail(s, "it belongs to another root");
            buf_free(&root);
            seen_root = 1;
        } else if (fmt == FMT_HASHES && (v = header_value(&s->rd.line, "mode")) != NULL) {
            buf_append_str(&s->mode, v);
        } else if (fmt == FMT_TREE && (v = header_value(&s->rd.line, "excluded")) != NULL) {
            buf_init(&root);
            if (hd_unescape(&root, v, strlen(v)) != 0 || root.data == NULL)
                stream_fail(s, "malformed header");
            s->excluded = xrealloc(s->excluded, (s->nexcluded + 1) * sizeof(*s->excluded));
            s->excluded[s->nexcluded++] = root.data;
        } else if (lineno > 1) {
            stream_fail(s, "unknown header line");
        }
    }
    if (lineno == 0)
        stream_fail(s, "empty file");
    if (!seen_root)
        stream_fail(s, "missing '# root:' line");
}

static void stream_close(struct stream *s)
{
    size_t i;

    reader_close(&s->rd);
    record_free(&s->cur);
    buf_free(&s->prev);
    buf_free(&s->mode);
    for (i = 0; i < s->nexcluded; i++)
        free(s->excluded[i]);
    free(s->excluded);
    free(s->path);
}

/* Status of a path, following the rules of the specification; -1 if both sides match. */
static int classify(const struct hd_record *o, const struct hd_record *d)
{
    if (o != NULL && o->type == 'E')
        return ST_ERR_SRC;
    if (d == NULL)
        return ST_MISSING;
    if (o == NULL)
        return ST_EXTRA;
    if (d->type == 'E')
        return ST_ERR_DST;
    if ((o->type == 'L') != (d->type == 'L'))
        return ST_TYPE;
    if (o->num != d->num)
        return ST_SIZE;
    if (strcmp(o->hash, d->hash) != 0)
        return ST_HASH;
    return -1;
}

typedef void (*merge_fn)(void *ctx, int status, const char *path);

/* Merge-join of both streams in canonical order; calls fn for every differing path. */
static void merge(struct stream *o, struct stream *d, merge_fn fn, void *ctx)
{
    /* A caught signal stops the merge; the caller discards its outputs. */
    while ((o->has || d->has) && !os_caught_signal()) {
        int c = !o->has ? 1 : !d->has ? -1 : strcmp(o->cur.path.data, d->cur.path.data);
        int st;

        if (c < 0) {
            st = classify(&o->cur, NULL);
            fn(ctx, st, o->cur.path.data);
            stream_next(o);
        } else if (c > 0) {
            st = classify(NULL, &d->cur);
            fn(ctx, st, d->cur.path.data);
            stream_next(d);
        } else {
            st = classify(&o->cur, &d->cur);
            if (st >= 0)
                fn(ctx, st, o->cur.path.data);
            stream_next(o);
            stream_next(d);
        }
    }
}

static void write_status_line(struct hd_outfile *out, struct hd_buf *b, int status,
                              const char *path)
{
    buf_clear(b);
    buf_append_str(b, status_names[status]);
    buf_append_char(b, ' ');
    hd_escape(b, path);
    buf_append_char(b, '\n');
    outfile_write(out, b->data, b->len);
}

static void write_roots_header(struct hd_outfile *out, const char *abs_origin,
                               const char *abs_destination)
{
    struct hd_buf b;

    buf_init(&b);
    buf_append_str(&b, "# origin: ");
    hd_escape(&b, abs_origin);
    buf_append_str(&b, "\n# destination: ");
    hd_escape(&b, abs_destination);
    buf_append_char(&b, '\n');
    outfile_write(out, b.data, b.len);
    buf_free(&b);
}

/* tree-diff.txt is written in two passes: the origin section, then the destination one. */
struct tree_pass {
    struct hd_outfile *out;
    struct hd_buf line;
    struct hd_counts *c;
    int destination;            /* 0: origin section, 1: destination section */
};

static void tree_pass_fn(void *ctx, int status, const char *path)
{
    struct tree_pass *p = ctx;
    int in_destination = status == ST_EXTRA || status == ST_ERR_DST;

    if (in_destination != p->destination)
        return;
    p->c->n[status]++;
    write_status_line(p->out, &p->line, status, path);
}

void diff_trees(const char *results, const char *abs_origin, const char *abs_destination,
                struct hd_counts *c, char ***excluded, size_t *nexcluded)
{
    struct hd_outfile out;
    struct tree_pass pass;
    int i;

    memset(c, 0, sizeof(*c));
    outfile_open(&out, results, "tree-diff.txt");
    write_roots_header(&out, abs_origin, abs_destination);
    pass.out = &out;
    pass.c = c;
    buf_init(&pass.line);
    for (i = 0; i < 2; i++) {
        struct stream o, d;
        size_t k;

        stream_open(&o, results, "tree-origin.txt", FMT_TREE, abs_origin);
        stream_open(&d, results, "tree-destination.txt", FMT_TREE, abs_destination);
        if (i == 0) {
            for (k = 0; k < o.nexcluded + d.nexcluded; k++) {
                const char *x = k < o.nexcluded ? o.excluded[k] : d.excluded[k - o.nexcluded];

                *excluded = xrealloc(*excluded, (*nexcluded + 1) * sizeof(**excluded));
                (*excluded)[(*nexcluded)++] = xstrdup(x);
            }
        }
        outfile_write(&out, i == 0 ? "## origin\n" : "## destination\n",
                      i == 0 ? 10 : 15);
        pass.destination = i;
        merge(&o, &d, tree_pass_fn, &pass);
        stream_close(&o);
        stream_close(&d);
        if (os_caught_signal())
            break;
    }
    buf_free(&pass.line);
    if (os_caught_signal())
        outfile_discard(&out);
    else
        outfile_commit(&out);
}

/* Root of a transfer with exactly one trailing '/', quoted for the shell. */
static void quote_dir(struct hd_buf *out, const char *abs)
{
    char *p = hd_path_join(abs, "");

    hd_shell_quote(out, p);
    free(p);
}

/* rsync filter patterns treat *, ? and [ as wildcards; a backslash escapes them. */
static void append_pattern(struct hd_buf *out, const char *rel)
{
    struct hd_buf pat;

    buf_init(&pat);
    buf_append_char(&pat, '/');
    for (; *rel != '\0'; rel++) {
        if (*rel == '*' || *rel == '?' || *rel == '[' || *rel == '\\')
            buf_append_char(&pat, '\\');
        buf_append_char(&pat, *rel);
    }
    buf_append_char(&pat, '/');
    buf_append_str(out, " --exclude=");
    hd_shell_quote(out, pat.data);
    buf_free(&pat);
}

void suggest_command(struct hd_buf *out, const struct hd_counts *c, const char *abs_origin,
                     const char *abs_destination, int one_fs, char **excluded,
                     size_t nexcluded)
{
    size_t i, j;

    buf_clear(out);
    if (c->n[ST_EXTRA] == 0 && c->n[ST_MISSING] == 0 && c->n[ST_SIZE] == 0
        && c->n[ST_TYPE] == 0)
        return;
    buf_append_str(out, "rsync -a");
    if (c->n[ST_EXTRA] > 0)
        buf_append_str(out, " --delete-after");
    if (one_fs)
        buf_append_str(out, " -x");
    for (i = 0; i < nexcluded; i++) {
        for (j = 0; j < i; j++)
            if (strcmp(excluded[j], excluded[i]) == 0)
                break;
        if (j == i)
            append_pattern(out, excluded[i]);
    }
    buf_append_char(out, ' ');
    quote_dir(out, abs_origin);
    buf_append_char(out, ' ');
    quote_dir(out, abs_destination);
}

struct hash_pass {
    struct hd_outfile *diff;
    struct hd_outfile *list;
    struct hd_buf line;
    struct hd_counts *c;
};

static void hash_pass_fn(void *ctx, int status, const char *path)
{
    struct hash_pass *p = ctx;

    p->c->n[status]++;
    write_status_line(p->diff, &p->line, status, path);
    if (status != ST_EXTRA && status != ST_ERR_SRC)
        outfile_write(p->list, path, strlen(path) + 1);   /* raw path and its '\0' */
}

void diff_hashes(const char *results, const char *abs_results, const char *abs_origin,
                 const char *abs_destination, struct hd_counts *c, struct hd_buf *cmd)
{
    struct hd_outfile diff, list, command;
    struct hash_pass pass;
    struct stream o, d;
    char *lst;

    memset(c, 0, sizeof(*c));
    stream_open(&o, results, "hashes-origin.txt", FMT_HASHES, abs_origin);
    stream_open(&d, results, "hashes-destination.txt", FMT_HASHES, abs_destination);
    if (strcmp(o.mode.data, d.mode.data) != 0)
        hd_die("the '# mode:' lines of the hashes files differ; the lists are not comparable");
    outfile_open(&diff, results, "diff-files.txt");
    outfile_open(&list, results, "rsync-files.lst");
    write_roots_header(&diff, abs_origin, abs_destination);
    outfile_write(&diff, "# mode: ", 8);
    outfile_write(&diff, o.mode.data, o.mode.len);
    outfile_write(&diff, "\n", 1);
    pass.diff = &diff;
    pass.list = &list;
    pass.c = c;
    buf_init(&pass.line);
    merge(&o, &d, hash_pass_fn, &pass);
    buf_free(&pass.line);
    stream_close(&o);
    stream_close(&d);
    if (os_caught_signal()) {
        outfile_discard(&diff);
        outfile_discard(&list);
        return;
    }
    outfile_commit(&diff);
    outfile_commit(&list);

    buf_clear(cmd);
    if (counts_total(c) - c->n[ST_EXTRA] - c->n[ST_ERR_SRC] > 0) {
        lst = hd_path_join(abs_results, "rsync-files.lst");
        buf_append_str(cmd, "rsync -a -I --from0 --files-from=");
        hd_shell_quote(cmd, lst);
        buf_append_char(cmd, ' ');
        quote_dir(cmd, abs_origin);
        buf_append_char(cmd, ' ');
        quote_dir(cmd, abs_destination);
        free(lst);
    }
    outfile_open(&command, results, "rsync-command.txt");
    if (cmd->len > 0) {
        outfile_write(&command, cmd->data, cmd->len);
        outfile_write(&command, "\n", 1);
    }
    outfile_commit(&command);
}
