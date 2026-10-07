/* hasher.c - full and fast modes, plan execution, workers and output writing. */
#include "config.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "hasher.h"
#include "md5.h"
#include "os.h"

#define READ_BUFSIZE (1024 * 1024)

void hasher_init(struct hd_hasher *h)
{
    h->bufsize = READ_BUFSIZE;
    h->buf = xmalloc(h->bufsize);
    h->bytes_read = 0;
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
static void hash_full(struct hd_hasher *h, const char *path, struct hd_result *r)
{
    struct md5_ctx ctx;
    struct os_stat st;
    off_t total = 0;
    int fd = os_open_read(path);

    if (fd < 0) {
        set_error(r, errno);
        return;
    }
    if (os_fstat(fd, &st) != 0 || st.kind != OS_REG) {
        /* Replaced by something that is not a regular file since the traversal. */
        set_error(r, st.kind != OS_REG ? EINVAL : errno);
        os_close(fd);
        return;
    }
    os_advise_sequential(fd);
    md5_init(&ctx);
    for (;;) {
        ssize_t n = os_read(fd, h->buf, h->bufsize);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            set_error(r, errno);
            os_close(fd);
            return;
        }
        if (n == 0)
            break;
        md5_update(&ctx, h->buf, (size_t)n);
        total += n;
        h->bytes_read += n;
    }
    os_close(fd);
    md5_final(&ctx, r->md5);
    r->type = RES_FULL;
    r->err = 0;
    r->size = total;
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
        hash_full(h, path, r);
        break;
    case ENT_LINK:
        hash_link(path, e, r);
        break;
    default:
        set_error(r, e->err);
        break;
    }
}

static void journal_fail(struct hd_journal *j)
{
    hd_die("cannot write '%s': %s", j->tmp, strerror(errno));
}

static void journal_put(struct hd_journal *j, const struct hd_buf *b)
{
    if (b->len > 0 && fwrite(b->data, 1, b->len, j->f) != b->len)
        journal_fail(j);
}

void journal_open(struct hd_journal *j, const char *dir, const char *name,
                  const char *abs_root, const char *mode_line)
{
    j->path = hd_path_join(dir, name);
    j->tmp = xmalloc(strlen(j->path) + 5);
    strcpy(j->tmp, j->path);
    strcat(j->tmp, ".tmp");
    buf_init(&j->line);
    j->f = fopen(j->tmp, "wb");
    if (j->f == NULL)
        journal_fail(j);
    buf_append_str(&j->line, "# hashdiff-format: 2\n# root: ");
    hd_escape(&j->line, abs_root);
    buf_append_str(&j->line, "\n# mode: ");
    buf_append_str(&j->line, mode_line);
    buf_append_char(&j->line, '\n');
    journal_put(j, &j->line);
}

void journal_write(struct hd_journal *j, const char *path, const struct hd_result *r)
{
    char num[HD_OFF_DEC_LEN];

    buf_clear(&j->line);
    buf_append_char(&j->line, r->type);
    buf_append_char(&j->line, ' ');
    if (r->type == RES_ERROR) {
        buf_append_str(&j->line, hd_off_to_dec((off_t)r->err, num));
        buf_append_str(&j->line, " -");
    } else {
        char hex[33];

        md5_hex(r->md5, hex);
        buf_append(&j->line, hex, 32);
        buf_append_char(&j->line, ' ');
        buf_append_str(&j->line, hd_off_to_dec(r->size, num));
    }
    buf_append_char(&j->line, ' ');
    hd_escape(&j->line, path);
    buf_append_char(&j->line, '\n');
    journal_put(j, &j->line);
}

void journal_commit(struct hd_journal *j)
{
    if (fflush(j->f) != 0)
        journal_fail(j);
    if (fclose(j->f) != 0) {
        j->f = NULL;
        journal_fail(j);
    }
    j->f = NULL;
    if (os_rename(j->tmp, j->path) != 0)
        hd_die("cannot rename '%s' to '%s': %s", j->tmp, j->path, strerror(errno));
    free(j->path);
    free(j->tmp);
    buf_free(&j->line);
}
