/* hasher.c - full and fast modes, plan execution, workers and output writing. */
#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hasher.h"
#include "md5.h"
#include "os.h"
#include "plan.h"

#define READ_BUFSIZE (1024 * 1024)

void hasher_init(struct hd_hasher *h, const struct hd_hashopts *opts, struct hd_stats *st)
{
    h->opts = opts;
    h->bufsize = READ_BUFSIZE;
    h->buf = xmalloc(h->bufsize);
    h->stats = st;
}

void hasher_free(struct hd_hasher *h)
{
    free(h->buf);
    h->buf = NULL;
}

static void set_error(struct hd_result *r, int err)
{
    r->type = RES_ERROR;
    r->err = err;
    r->size = 0;
}

/* Full MD5 with a sequential read loop; the recorded size is the number of bytes read. */
static void hash_full(struct hd_hasher *h, int fd, struct hd_result *r)
{
    struct md5_ctx ctx;
    off_t total = 0;

    os_advise_sequential(fd);
    md5_init(&ctx);
    for (;;) {
        ssize_t n = os_read(fd, h->buf, h->bufsize);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            set_error(r, errno);
            return;
        }
        if (n == 0)
            break;
        md5_update(&ctx, h->buf, (size_t)n);
        total += n;
        h->stats->bytes_read += n;
    }
    md5_final(&ctx, r->md5);
    r->type = RES_FULL;
    r->err = 0;
    r->size = total;
}

/*
 * Reads len bytes at off in chunks of at most the buffer size, feeding them to ctx (or
 * discarding them when ctx is NULL). Returns 0, or an errno; a file that ends early (it was
 * truncated during the read) is reported as EIO.
 */
static int read_span(struct hd_hasher *h, int fd, off_t off, off_t len, struct md5_ctx *ctx)
{
    while (len > 0) {
        size_t want = len < (off_t)h->bufsize ? (size_t)len : h->bufsize;
        ssize_t n = os_pread(fd, h->buf, want, off);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return errno;
        }
        if (n == 0)
            return EIO;
        if (ctx != NULL)
            md5_update(ctx, h->buf, (size_t)n);
        off += n;
        len -= n;
        h->stats->bytes_read += n;
    }
    return 0;
}

/*
 * Sampled hash: MD5(N as 8 little-endian bytes, then the B bytes of every sample in offset
 * order). Samples closer than merge_gap are read through, discarding the bytes in between;
 * the read order never changes the digest.
 */
static void hash_sampled(struct hd_hasher *h, int fd, const struct hd_plan *plan,
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
        if (err != 0) {
            set_error(r, err);
            return;
        }
        pos = off + plan->block;
    }
    md5_final(&ctx, r->md5);
    r->type = RES_SAMPLED;
    r->err = 0;
    r->size = plan->n;
    h->stats->samples += plan->k;
    h->stats->sample_bytes += plan->k * plan->block;
    h->stats->sampled_total += plan->n;
}

static void hash_regular(struct hd_hasher *h, const char *path, struct hd_result *r)
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

void hash_entry(struct hd_hasher *h, const char *path, const struct hd_entry *e,
                struct hd_result *r)
{
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
}

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
}

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

void hash_list(const char *root, const struct hd_list *l, const char *results,
               const char *side, const char *abs_root, const struct hd_hashopts *opts,
               struct hd_stats *st)
{
    struct hd_outfile out;
    struct hd_hasher h;
    struct hd_buf line;
    char name[64];
    size_t i;

    sprintf(name, "hashes-%s.txt", side);
    outfile_open(&out, results, name);
    buf_init(&line);
    buf_append_str(&line, "# hashdiff-format: 2\n# root: ");
    hd_escape(&line, abs_root);
    buf_append_str(&line, "\n# mode: ");
    buf_append_str(&line, opts->mode_line);
    buf_append_char(&line, '\n');
    outfile_write(&out, line.data, line.len);
    hasher_init(&h, opts, st);
    for (i = 0; i < l->count; i++) {
        const struct hd_entry *e = &l->items[i];
        char *path = hd_path_join(root, e->path);
        struct hd_result r;

        hash_entry(&h, path, e, &r);
        free(path);
        count_result(st, &r);
        format_line(&line, e->path, &r);
        outfile_write(&out, line.data, line.len);
    }
    st->ignored = l->ignored;
    hasher_free(&h);
    buf_free(&line);
    outfile_commit(&out);
}
