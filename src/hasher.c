/* hasher.c - full and fast modes, plan execution, workers and output writing. */
#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diff.h"
#include "hasher.h"
#include "md5.h"
#include "os.h"
#include "plan.h"

#define READ_BUFSIZE (1024 * 1024)
#define TICK_SECONDS 2
#define CHUNKS_PER_JOB 64

void stats_init(struct hd_stats *st)
{
    st->entries = 0;
    st->full = 0;
    st->sampled = 0;
    st->links = 0;
    st->errors = 0;
    st->ignored = 0;
    st->bytes_read = 0;
    st->samples = 0;
    st->sample_bytes = 0;
    st->sampled_total = 0;
    st->reused = 0;
    st->last_check = 0;
}

/* ---- hashing of one entry ---- */

struct hasher {
    const struct hd_hashopts *opts;
    unsigned char *buf;         /* 1 MiB read buffer of this process */
    size_t bufsize;
    off_t bytes_read;           /* bytes read by this process */
    off_t samples;              /* of the last S result */
    void (*tick)(void *ctx);    /* called after every read, for progress */
    void *tick_ctx;
};

static void set_error(struct hd_result *r, int err)
{
    r->type = RES_ERROR;
    r->err = err;
    r->size = 0;
}

/* Reads at most n bytes; EINTR is retried unless a signal was caught (then -1/EINTR). */
static ssize_t read_some(int fd, void *buf, size_t n, off_t off, int positional)
{
    for (;;) {
        ssize_t r = positional ? os_pread(fd, buf, n, off) : os_read(fd, buf, n);

        if (r >= 0 || errno != EINTR || os_caught_signal())
            return r;
    }
}

static void after_read(struct hasher *h, ssize_t n)
{
    h->bytes_read += n;
    if (h->tick != NULL)
        h->tick(h->tick_ctx);
}

/* Full MD5 with a sequential read loop; the recorded size is the number of bytes read. */
static void hash_full(struct hasher *h, int fd, struct hd_result *r)
{
    struct md5_ctx ctx;
    off_t total = 0;

    os_advise_sequential(fd);
    md5_init(&ctx);
    for (;;) {
        ssize_t n;

        if (os_caught_signal()) {
            r->type = RES_NONE;
            return;
        }
        n = read_some(fd, h->buf, h->bufsize, 0, 0);
        if (n < 0) {
            if (os_caught_signal())
                r->type = RES_NONE;
            else
                set_error(r, errno);
            return;
        }
        if (n == 0)
            break;
        md5_update(&ctx, h->buf, (size_t)n);
        total += n;
        after_read(h, n);
    }
    md5_final(&ctx, r->md5);
    r->type = RES_FULL;
    r->err = 0;
    r->size = total;
}

/*
 * Reads len bytes at off in chunks of at most the buffer size, feeding them to ctx (or
 * discarding them when ctx is NULL). Returns 0, -1 if a signal was caught, or an errno; a
 * file that ends early (it was truncated during the read) is reported as EIO.
 */
static int read_span(struct hasher *h, int fd, off_t off, off_t len, struct md5_ctx *ctx)
{
    while (len > 0) {
        size_t want = len < (off_t)h->bufsize ? (size_t)len : h->bufsize;
        ssize_t n;

        if (os_caught_signal())
            return -1;
        n = read_some(fd, h->buf, want, off, 1);
        if (n < 0)
            return os_caught_signal() ? -1 : errno;
        if (n == 0)
            return EIO;
        if (ctx != NULL)
            md5_update(ctx, h->buf, (size_t)n);
        off += n;
        len -= n;
        after_read(h, n);
    }
    return 0;
}

/*
 * Sampled hash: MD5(N as 8 little-endian bytes, then the B bytes of every sample in offset
 * order). Samples closer than merge_gap are read through, discarding the bytes in between;
 * the read order never changes the digest.
 */
static void hash_sampled(struct hasher *h, int fd, const struct hd_plan *plan,
                         struct hd_result *r)
{
    const struct hd_hashopts *o = h->opts;
    struct hd_plan_iter it;
    struct md5_ctx ctx;
    unsigned char le[8];
    off_t off, pos, start;
    int i, err;

    /* Hints: no readahead for the file, then every span that will be read. */
    os_advise_random(fd);
    plan_iter_init(&it, plan);
    start = pos = -1;
    while (plan_iter_next(&it, &off)) {
        if (pos >= 0 && off - pos >= o->merge_gap) {
            os_advise_willneed(fd, start, pos - start);
            start = off;
        }
        if (start < 0)
            start = off;
        pos = off + plan->block;
    }
    os_advise_willneed(fd, start, pos - start);

    for (i = 0; i < 8; i++)
        le[i] = (unsigned char)((plan->n >> (8 * i)) & 0xFF);
    md5_init(&ctx);
    md5_update(&ctx, le, 8);
    plan_iter_init(&it, plan);
    pos = -1;
    while (plan_iter_next(&it, &off)) {
        err = 0;
        if (pos >= 0 && off - pos < o->merge_gap)
            err = read_span(h, fd, pos, off - pos, NULL);
        if (err == 0)
            err = read_span(h, fd, off, plan->block, &ctx);
        if (err < 0) {
            r->type = RES_NONE;
            return;
        }
        if (err > 0) {
            set_error(r, err);
            return;
        }
        pos = off + plan->block;
    }
    md5_final(&ctx, r->md5);
    r->type = RES_SAMPLED;
    r->err = 0;
    r->size = plan->n;
    h->samples = plan->k;
}

static void hash_regular(struct hasher *h, const char *path, struct hd_result *r)
{
    struct os_stat st;
    struct hd_plan plan;
    int fd = os_open_read(path);

    if (fd < 0) {
        set_error(r, errno);
        return;
    }
    if (os_fstat(fd, &st) != 0) {
        set_error(r, errno);
    } else if (st.kind != OS_REG) {
        /* Replaced by something that is not a regular file since the traversal. */
        set_error(r, EINVAL);
    } else {
        plan.sampled = 0;
        if (h->opts->fast)
            plan_make(st.size, h->opts->gap, h->opts->block, h->opts->seek_bytes, &plan);
        if (plan.sampled)
            hash_sampled(h, fd, &plan, r);
        else
            hash_full(h, fd, r);
    }
    os_close(fd);
}

/* MD5 of the link target returned by readlink; the size is the length of the target. */
static void hash_link(const char *path, const struct hd_entry *e, struct hd_result *r)
{
    size_t cap = e->size > 0 && e->size < 65536 ? (size_t)e->size + 1 : 256;
    struct md5_ctx ctx;

    for (;;) {
        char *target = xmalloc(cap);
        ssize_t n = os_readlink(path, target, cap);

        if (n < 0) {
            set_error(r, errno);
            free(target);
            return;
        }
        if ((size_t)n < cap) {
            md5_init(&ctx);
            md5_update(&ctx, (const unsigned char *)target, (size_t)n);
            md5_final(&ctx, r->md5);
            r->type = RES_LINK;
            r->err = 0;
            r->size = (off_t)n;
            free(target);
            return;
        }
        free(target);   /* possibly truncated: retry with a larger buffer */
        if (cap > (size_t)-1 / 2)
            hd_die("symlink target too long: %s", path);
        cap *= 2;
    }
}

static void hash_entry(struct hasher *h, const char *root, const struct hd_entry *e,
                       struct hd_result *r)
{
    char *path = hd_path_join(root, e->path);

    h->samples = 0;
    switch (e->type) {
    case ENT_FILE:
        hash_regular(h, path, r);
        break;
    case ENT_LINK:
        hash_link(path, e, r);
        break;
    default:
        set_error(r, e->err);
        break;
    }
    free(path);
}

/* Estimated cost of an entry: bytes the plan will read plus one seek; 0 if reused. */
static off_t entry_cost(const struct hd_entry *e, const struct hd_result *res,
                        const struct hd_hashopts *o)
{
    struct hd_plan plan;
    off_t bytes = 0, cost;

    if (res->type != RES_NONE)
        return 0;
    if (e->type == ENT_FILE) {
        plan.sampled = 0;
        plan.n = e->size;
        if (o->fast)
            plan_make(e->size, o->gap, o->block, o->seek_bytes, &plan);
        bytes = plan_read_bytes(&plan);
    }
    return hd_off_add(bytes, o->seek_bytes, &cost) == 0 ? cost : HD_OFF_MAX;
}

/* ---- the side process: results, canonical journal, progress ---- */

struct side_state {
    const char *root;
    const char *name;
    const struct hd_list *list;
    const struct hd_hashopts *opts;
    struct hd_result *res;
    size_t next_write;          /* entries [0, next_write) are in the journal */
    size_t done;
    struct hd_outfile out;
    struct hd_buf line;
    struct hd_stats *st;
    long last_tick;
    size_t check_idx;           /* resume: entry of the last kept line, or (size_t)-1 */
    struct hd_result old_last;  /* resume: the last kept line */
};

static void format_line(struct hd_buf *b, const char *path, const struct hd_result *r)
{
    char num[HD_OFF_DEC_LEN];

    buf_clear(b);
    buf_append_char(b, r->type);
    buf_append_char(b, ' ');
    if (r->type == RES_ERROR) {
        buf_append_str(b, hd_off_to_dec((off_t)r->err, num));
        buf_append_str(b, " -");
    } else {
        char hex[33];

        md5_hex(r->md5, hex);
        buf_append(b, hex, 32);
        buf_append_char(b, ' ');
        buf_append_str(b, hd_off_to_dec(r->size, num));
    }
    buf_append_char(b, ' ');
    hd_escape(b, path);
    buf_append_char(b, '\n');
}

static void progress_line(struct side_state *s)
{
    char line[256], size[HD_OFF_DEC_LEN + 8];

    sprintf(line, "[%s] %lu/%lu files, %s\n", s->name, (unsigned long)s->done,
            (unsigned long)s->list->count, hd_human_bytes(s->st->bytes_read, size));
    (void)hd_write_all(2, line, strlen(line));
}

/* Every ~2 s: a progress line and a flush of the journal. */
static void side_tick(void *ctx)
{
    struct side_state *s = ctx;
    long now = os_time();

    if (now - s->last_tick < TICK_SECONDS)
        return;
    s->last_tick = now;
    outfile_flush(&s->out);
    if (s->opts->progress)
        progress_line(s);
}

static void count_result(struct hd_stats *st, const struct hd_result *r)
{
    st->entries++;
    switch (r->type) {
    case RES_FULL:
        st->full++;
        break;
    case RES_SAMPLED:
        st->sampled++;
        break;
    case RES_LINK:
        st->links++;
        break;
    default:
        st->errors++;
        break;
    }
}

static void write_prefix(struct side_state *s)
{
    while (s->next_write < s->list->count && s->res[s->next_write].type != RES_NONE) {
        format_line(&s->line, s->list->items[s->next_write].path, &s->res[s->next_write]);
        outfile_write(&s->out, s->line.data, s->line.len);
        s->next_write++;
    }
}

static int same_result(const struct hd_result *a, const struct hd_result *b)
{
    if (a->type != b->type)
        return 0;
    if (a->type == RES_ERROR)
        return a->err == b->err;
    return a->size == b->size && memcmp(a->md5, b->md5, 16) == 0;
}

static void count_samples(struct side_state *s, const struct hd_result *r, off_t samples)
{
    if (r->type == RES_SAMPLED) {
        s->st->samples += samples;
        s->st->sample_bytes += samples * s->opts->block;
        s->st->sampled_total += r->size;
    }
}

/* Stores a result and writes every entry of the journal's prefix that is now complete. */
static void store_result(struct side_state *s, size_t idx, const struct hd_result *r,
                         off_t samples)
{
    s->res[idx] = *r;
    s->done++;
    count_result(s->st, r);
    count_samples(s, r, samples);
    if (idx == s->check_idx) {
        if (same_result(r, &s->old_last)) {
            s->st->last_check = 1;
        } else {
            struct hd_buf b;

            s->st->last_check = 2;
            buf_init(&b);
            hd_escape(&b, s->list->items[idx].path);
            fprintf(stderr, "[%s] resume: last entry changed, re-hashed: %s\n", s->name,
                    b.data);
            buf_free(&b);
        }
    }
    write_prefix(s);
    side_tick(s);
}

/* Is a hashes line of this type possible for an entry of the traversal? */
static int compatible(const struct hd_entry *e, char type)
{
    if (type == RES_ERROR)
        return 1;
    if (e->type == ENT_FILE)
        return type == RES_FULL || type == RES_SAMPLED;
    return e->type == ENT_LINK && type == RES_LINK;
}

/*
 * Loads the valid prefix of the journal of an interrupted run: complete, well-formed lines
 * whose paths are the entries of the list in order. Returns the number of kept lines.
 */
static size_t load_kept(struct side_state *s, const char *results, const char *source)
{
    char *path = hd_path_join(results, source);
    struct hd_reader rd;
    struct hd_record rec;
    size_t n = 0;
    int headers = 0;

    if (reader_open(&rd, path) != 0) {
        free(path);
        return 0;
    }
    record_init(&rec);
    while (reader_next(&rd) == 1 && rd.complete) {
        const struct hd_entry *e;
        struct hd_result *r;

        if (headers < 3) {
            headers++;
            continue;
        }
        if (n == s->list->count)
            break;
        e = &s->list->items[n];
        if (record_parse(FMT_HASHES, rd.line.data, rd.line.len, &rec) != 0
            || strcmp(rec.path.data, e->path) != 0 || !compatible(e, rec.type))
            break;
        r = &s->res[n];
        r->type = rec.type;
        r->err = rec.type == RES_ERROR ? (int)rec.num : 0;
        r->size = rec.type == RES_ERROR ? 0 : rec.num;
        if (rec.type != RES_ERROR)
            hex_to_md5(rec.hash, r->md5);
        n++;
    }
    record_free(&rec);
    reader_close(&rd);
    free(path);
    return n;
}

/* Number of samples of a reused S line, from its plan. */
static off_t plan_samples(const struct hd_hashopts *o, off_t n)
{
    struct hd_plan plan;

    plan_make(n, o->gap, o->block, o->seek_bytes, &plan);
    return plan.sampled ? plan.k : 0;
}

/* Reuses the kept lines except the last one, which is hashed again. */
static void reuse_kept(struct side_state *s, size_t kept)
{
    size_t i;

    if (kept == 0)
        return;
    s->check_idx = kept - 1;
    s->old_last = s->res[kept - 1];
    s->res[kept - 1].type = RES_NONE;
    for (i = 0; i + 1 < kept; i++) {
        count_result(s->st, &s->res[i]);
        count_samples(s, &s->res[i], s->res[i].type == RES_SAMPLED
                                     ? plan_samples(s->opts, s->res[i].size) : 0);
    }
    s->done = kept - 1;
    s->st->reused = (unsigned long)(kept - 1);
}

/* Consecutive chunks of about total / (64 x jobs) estimated cost; returns their count. */
static size_t make_chunks(const struct hd_list *l, const struct hd_result *res,
                          const struct hd_hashopts *o, size_t **starts)
{
    off_t total = 0, target, acc = 0, c;
    size_t i, n = 0;

    for (i = 0; i < l->count; i++) {
        c = entry_cost(&l->items[i], &res[i], o);
        total = hd_off_add(total, c, &total) == 0 ? total : HD_OFF_MAX;
    }
    target = total / ((off_t)CHUNKS_PER_JOB * o->jobs);
    if (target < 1)
        target = 1;
    *starts = xmalloc((l->count + 1) * sizeof(**starts));
    for (i = 0; i < l->count; i++) {
        if (acc == 0)
            (*starts)[n++] = i;
        c = entry_cost(&l->items[i], &res[i], o);
        if (hd_off_add(acc, c, &acc) != 0 || acc >= target)
            acc = 0;
    }
    (*starts)[n] = l->count;
    return n;
}

static const struct hd_entry *sort_base;

static int cmp_index_ino(const void *a, const void *b)
{
    size_t x = *(const size_t *)a, y = *(const size_t *)b;
    ino_t ix = sort_base[x].ino, iy = sort_base[y].ino;

    if (ix != iy)
        return ix < iy ? -1 : 1;
    return x < y ? -1 : x > y;
}

/* -j 1: the bytes of the file being read count in the progress at once. */
struct serial_ctx {
    struct side_state *s;
    struct hasher *h;
    off_t base;
};

static void serial_tick(void *ctx)
{
    struct serial_ctx *c = ctx;

    c->s->st->bytes_read = c->base + c->h->bytes_read;
    side_tick(c->s);
}

/* -j 1: the side process hashes every chunk itself. */
static void hash_serial(struct side_state *s, const size_t *starts, size_t nchunks)
{
    const struct hd_list *l = s->list;
    int inode_order = s->opts->fast && s->opts->hdd;
    struct hasher h;
    struct serial_ctx sc;
    size_t *order = xmalloc((l->count + 1) * sizeof(*order));
    size_t c, i, n;

    h.opts = s->opts;
    h.bufsize = READ_BUFSIZE;
    h.buf = xmalloc(h.bufsize);
    h.bytes_read = 0;
    sc.s = s;
    sc.h = &h;
    sc.base = s->st->bytes_read;
    h.tick = serial_tick;
    h.tick_ctx = &sc;
    for (c = 0; c < nchunks && !os_caught_signal(); c++) {
        n = starts[c + 1] - starts[c];
        for (i = 0; i < n; i++)
            order[i] = starts[c] + i;
        if (inode_order) {
            sort_base = l->items;
            qsort(order, n, sizeof(*order), cmp_index_ino);
        }
        for (i = 0; i < n && !os_caught_signal(); i++) {
            struct hd_result r;

            if (s->res[order[i]].type != RES_NONE)
                continue;       /* reused from the interrupted run */
            hash_entry(&h, s->root, &l->items[order[i]], &r);
            s->st->bytes_read = sc.base + h.bytes_read;
            if (r.type == RES_NONE)
                break;
            store_result(s, order[i], &r, h.samples);
        }
    }
    free(h.buf);
    free(order);
}

/* Records from the workers; each is written with a single write() smaller than PIPE_BUF. */
#define REC_RESULT 1
#define REC_PROGRESS 2

struct worker_rec {
    int kind;
    unsigned long index;
    struct hd_result r;
    off_t bytes;                /* bytes read since the previous record of this worker */
    off_t samples;
};

struct worker_state {
    int fd;
    struct hasher h;
    off_t reported;             /* bytes already sent in records */
    long last_tick;
};

static void worker_send(struct worker_state *w, struct worker_rec *rec)
{
    rec->bytes = w->h.bytes_read - w->reported;
    w->reported = w->h.bytes_read;
    if (hd_write_all(w->fd, rec, sizeof(*rec)) != 0)
        os_exit_now(os_caught_signal() ? 128 + os_caught_signal() : 2);
}

static void worker_tick(void *ctx)
{
    struct worker_state *w = ctx;
    struct worker_rec rec;
    long now = os_time();

    if (now - w->last_tick < TICK_SECONDS)
        return;
    w->last_tick = now;
    memset(&rec, 0, sizeof(rec));
    rec.kind = REC_PROGRESS;
    worker_send(w, &rec);
}

static void run_worker(const struct side_state *s, const size_t *starts, size_t nchunks,
                       int k, int fd)
{
    const struct hd_list *l = s->list;
    struct worker_state w;
    size_t c, i;

    w.fd = fd;
    w.reported = 0;
    w.last_tick = os_time();
    w.h.opts = s->opts;
    w.h.bufsize = READ_BUFSIZE;
    w.h.buf = xmalloc(w.h.bufsize);
    w.h.bytes_read = 0;
    w.h.tick = worker_tick;
    w.h.tick_ctx = &w;
    for (c = (size_t)k; c < nchunks; c += (size_t)s->opts->jobs) {
        for (i = starts[c]; i < starts[c + 1]; i++) {
            struct worker_rec rec;

            if (os_caught_signal())
                os_exit_now(128 + os_caught_signal());
            if (s->res[i].type != RES_NONE)
                continue;       /* reused from the interrupted run */
            memset(&rec, 0, sizeof(rec));
            hash_entry(&w.h, s->root, &l->items[i], &rec.r);
            if (rec.r.type == RES_NONE)
                os_exit_now(128 + os_caught_signal());
            rec.kind = REC_RESULT;
            rec.index = (unsigned long)i;
            rec.samples = w.h.samples;
            worker_send(&w, &rec);
        }
    }
    os_exit_now(0);
}

/* -j N: N workers hash chunks k, k + N, ...; the side process stores their records. */
static void hash_parallel(struct side_state *s, const size_t *starts, size_t nchunks)
{
    int jobs = s->opts->jobs, k, fds[2], code, sig, failed = 0, forwarded = 0;
    long *pids = xmalloc((size_t)jobs * sizeof(*pids));
    struct worker_rec rec;

    if (os_pipe(fds) != 0)
        hd_die("cannot create a pipe: %s", strerror(errno));
    outfile_flush(&s->out);
    fflush(NULL);
    for (k = 0; k < jobs; k++) {
        pids[k] = os_fork();
        if (pids[k] < 0)
            hd_die("cannot fork: %s", strerror(errno));
        if (pids[k] == 0) {
            os_close(fds[0]);
            run_worker(s, starts, nchunks, k, fds[1]);
        }
    }
    os_close(fds[1]);
    for (;;) {
        ssize_t n = hd_read_full(fds[0], &rec, sizeof(rec));

        if (n < 0 && errno == EINTR) {
            /* Interrupted: forward the signal and keep reading until every worker exits. */
            if (!forwarded)
                for (k = 0; k < jobs; k++)
                    (void)os_kill(pids[k], os_caught_signal());
            forwarded = 1;
            continue;
        }
        if (n < 0)
            hd_die("cannot read from the workers: %s", strerror(errno));
        if (n == 0)
            break;
        if (n != (ssize_t)sizeof(rec))
            hd_die("truncated record from a worker");
        s->st->bytes_read += rec.bytes;
        if (rec.kind == REC_RESULT && rec.index < s->list->count)
            store_result(s, (size_t)rec.index, &rec.r, rec.samples);
        else
            side_tick(s);
    }
    os_close(fds[0]);
    for (k = 0; k < jobs; k++) {
        if (os_wait(pids[k], &code, &sig) != 0 || (code != 0 && !os_caught_signal()))
            failed = 1;
    }
    free(pids);
    if (failed)
        hd_die("a %s worker process failed", s->name);
}

int hash_side(const char *root, const struct hd_list *l, const char *results,
              const char *side, const char *abs_root, const struct hd_hashopts *opts,
              const char *resume_source, struct hd_stats *st)
{
    struct side_state s;
    size_t *starts, nchunks, i;
    char name[64];
    int sig;

    s.root = root;
    s.name = side;
    s.list = l;
    s.opts = opts;
    s.res = xmalloc((l->count + 1) * sizeof(*s.res));
    for (i = 0; i < l->count; i++)
        s.res[i].type = RES_NONE;
    s.next_write = 0;
    s.done = 0;
    s.st = st;
    s.last_tick = 0;
    s.check_idx = (size_t)-1;
    st->ignored = l->ignored;

    sprintf(name, "hashes-%s.txt", side);
    if (resume_source != NULL) {
        reuse_kept(&s, load_kept(&s, results, resume_source));
        outfile_open_suffix(&s.out, results, name, ".new");
    } else {
        outfile_open(&s.out, results, name);
    }
    buf_init(&s.line);
    buf_append_str(&s.line, "# hashdiff-format: 2\n# root: ");
    hd_escape(&s.line, abs_root);
    buf_append_str(&s.line, "\n# mode: ");
    buf_append_str(&s.line, opts->mode_line);
    buf_append_char(&s.line, '\n');
    outfile_write(&s.out, s.line.data, s.line.len);
    if (resume_source != NULL) {
        /* The new journal holds everything the old one had: it replaces it. */
        write_prefix(&s);
        outfile_flush(&s.out);
        outfile_rename_tmp(&s.out, ".tmp");
        if (strcmp(resume_source, name) == 0 && os_unlink(s.out.path) != 0)
            hd_die("cannot remove '%s': %s", s.out.path, strerror(errno));
    }

    nchunks = make_chunks(l, s.res, opts, &starts);
    if (opts->jobs > 1 && l->count > 0)
        hash_parallel(&s, starts, nchunks);
    else
        hash_serial(&s, starts, nchunks);
    free(starts);

    sig = os_caught_signal();
    if (opts->progress && sig == 0)
        progress_line(&s);
    buf_free(&s.line);
    free(s.res);
    if (sig != 0) {
        /* Keep the journal (complete lines in canonical order) for --resume. */
        outfile_flush(&s.out);
        fclose(s.out.f);
        free(s.out.path);
        free(s.out.tmp);
        return sig;
    }
    outfile_commit(&s.out);
    return 0;
}
