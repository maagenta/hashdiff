/* util.c - memory, dynamic buffers, escaping, off_t arithmetic and EINTR-safe I/O. */
#include "config.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "os.h"
#include "util.h"

int hd_is_child = 0;

void hd_exit(int status)
{
    if (hd_is_child)
        os_exit_now(status);
    exit(status);
}

static void vmessage(const char *prefix, const char *fmt, va_list ap)
{
    fputs("hashdiff: ", stderr);
    fputs(prefix, stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

void hd_die(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vmessage("", fmt, ap);
    va_end(ap);
    hd_exit(2);
}

void hd_warn(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vmessage("warning: ", fmt, ap);
    va_end(ap);
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);

    if (p == NULL)
        hd_die("out of memory");
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (p == NULL)
        hd_die("out of memory");
    return p;
}

char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);

    memcpy(p, s, n);
    return p;
}

void buf_init(struct hd_buf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_free(struct hd_buf *b)
{
    free(b->data);
    buf_init(b);
}

void buf_clear(struct hd_buf *b)
{
    b->len = 0;
    if (b->data != NULL)
        b->data[0] = '\0';
}

static void buf_reserve(struct hd_buf *b, size_t extra)
{
    size_t need = b->len + extra + 1;
    size_t cap;

    if (need < b->len)
        hd_die("out of memory");
    if (need <= b->cap)
        return;
    cap = b->cap ? b->cap : 64;
    while (cap < need) {
        if (cap > (size_t)-1 / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    b->data = xrealloc(b->data, cap);
    b->cap = cap;
}

void buf_append(struct hd_buf *b, const char *s, size_t n)
{
    buf_reserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void buf_append_str(struct hd_buf *b, const char *s)
{
    buf_append(b, s, strlen(s));
}

void buf_append_char(struct hd_buf *b, int c)
{
    char ch = (char)c;

    buf_append(b, &ch, 1);
}

struct hd_arena_chunk {
    struct hd_arena_chunk *next;
};

#define ARENA_CHUNK (256 * 1024)

void arena_init(struct hd_arena *a)
{
    a->chunks = NULL;
    a->cur = NULL;
    a->left = 0;
}

void arena_free(struct hd_arena *a)
{
    while (a->chunks != NULL) {
        struct hd_arena_chunk *next = a->chunks->next;

        free(a->chunks);
        a->chunks = next;
    }
    arena_init(a);
}

/* Copies n bytes of s and a terminating NUL. */
char *arena_strndup(struct hd_arena *a, const char *s, size_t n)
{
    /* Data starts after the chunk header, rounded up so the header stays aligned. */
    size_t header = (sizeof(struct hd_arena_chunk) + sizeof(double) - 1) / sizeof(double)
                    * sizeof(double);
    char *p;

    if (n + 1 > a->left) {
        size_t size = n + 1 > ARENA_CHUNK ? n + 1 : ARENA_CHUNK;
        struct hd_arena_chunk *c;

        if (size > (size_t)-1 - header)
            hd_die("out of memory");
        c = xmalloc(header + size);
        c->next = a->chunks;
        a->chunks = c;
        a->cur = (char *)c + header;
        a->left = size;
    }
    p = a->cur;
    memcpy(p, s, n);
    p[n] = '\0';
    a->cur += n + 1;
    a->left -= n + 1;
    return p;
}

char *hd_path_join(const char *dir, const char *name)
{
    size_t dlen = strlen(dir), nlen = strlen(name);
    int slash = dlen > 0 && dir[dlen - 1] != '/';
    char *p = xmalloc(dlen + (size_t)slash + nlen + 1);

    memcpy(p, dir, dlen);
    if (slash)
        p[dlen] = '/';
    memcpy(p + dlen + (size_t)slash, name, nlen + 1);
    return p;
}

void hd_escape(struct hd_buf *out, const char *s)
{
    for (; *s != '\0'; s++) {
        if (*s == '\\')
            buf_append(out, "\\\\", 2);
        else if (*s == '\n')
            buf_append(out, "\\n", 2);
        else if (*s == '\r')
            buf_append(out, "\\r", 2);
        else
            buf_append_char(out, *s);
    }
}

int hd_unescape(struct hd_buf *out, const char *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (s[i] == '\0' || s[i] == '\n' || s[i] == '\r')
            return -1;
        if (s[i] != '\\') {
            buf_append_char(out, s[i]);
            continue;
        }
        if (++i == n)
            return -1;
        if (s[i] == '\\')
            buf_append_char(out, '\\');
        else if (s[i] == 'n')
            buf_append_char(out, '\n');
        else if (s[i] == 'r')
            buf_append_char(out, '\r');
        else
            return -1;
    }
    return 0;
}

void hd_shell_quote(struct hd_buf *out, const char *s)
{
    buf_append_char(out, '\'');
    for (; *s != '\0'; s++) {
        if (*s == '\'')
            buf_append(out, "'\\''", 4);
        else
            buf_append_char(out, *s);
    }
    buf_append_char(out, '\'');
}

/*
 * C89 leaves the rounding of division with negative operands to the implementation, so the
 * remainder is normalized to take the digits of negative values toward zero.
 */
char *hd_off_to_dec(off_t v, char *buf)
{
    char tmp[HD_OFF_DEC_LEN];
    int neg = v < 0;
    size_t n = 0, i = 0;
    off_t q, r;

    do {
        q = v / 10;
        r = v - q * 10;
        if (neg && r > 0) {
            r -= 10;
            q += 1;
        }
        tmp[n++] = (char)('0' + (neg ? -r : r));
        v = q;
    } while (v != 0);
    if (neg)
        buf[i++] = '-';
    while (n > 0)
        buf[i++] = tmp[--n];
    buf[i] = '\0';
    return buf;
}

int hd_dec_to_off(const char *s, size_t n, off_t *out)
{
    size_t i = 0;
    int neg = 0;
    off_t v = 0;

    if (n > 0 && s[0] == '-') {
        neg = 1;
        i = 1;
    }
    if (i == n)
        return -1;
    for (; i < n; i++) {
        int d;

        if (s[i] < '0' || s[i] > '9')
            return -1;
        d = s[i] - '0';
        if (v > (HD_OFF_MAX - d) / 10)
            return -1;
        v = v * 10 + d;
    }
    *out = neg ? -v : v;
    return 0;
}

/* Both operands must be >= 0. Returns -1 on overflow. */
int hd_off_add(off_t a, off_t b, off_t *r)
{
    if (a > HD_OFF_MAX - b)
        return -1;
    *r = a + b;
    return 0;
}

/* Both operands must be >= 0. Returns -1 on overflow. */
int hd_off_mul(off_t a, off_t b, off_t *r)
{
    if (a != 0 && b > HD_OFF_MAX / a)
        return -1;
    *r = a * b;
    return 0;
}

/* Reads until n bytes or EOF; retries on EINTR. Returns bytes read, or -1 with errno. */
ssize_t hd_read_full(int fd, void *buf, size_t n)
{
    size_t done = 0;

    while (done < n) {
        ssize_t r = os_read(fd, (char *)buf + done, n - done);

        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            break;
        done += (size_t)r;
    }
    return (ssize_t)done;
}

int hd_write_all(int fd, const void *buf, size_t n)
{
    size_t done = 0;

    while (done < n) {
        ssize_t r = os_write(fd, (const char *)buf + done, n - done);

        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        done += (size_t)r;
    }
    return 0;
}

char *hd_human_bytes(off_t v, char *buf)
{
    static const char *const units[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB" };
    off_t whole = v, rest = 0;
    int u = 0;

    while (whole >= 1024 && u < 6) {
        rest = whole % 1024;
        whole /= 1024;
        u++;
    }
    hd_off_to_dec(whole, buf);
    if (u > 0) {
        size_t n = strlen(buf);

        buf[n] = '.';
        buf[n + 1] = (char)('0' + (int)(rest * 10 / 1024));
        buf[n + 2] = '\0';
    }
    strcat(buf, " ");
    strcat(buf, units[u]);
    return buf;
}

#define READER_BUFSIZE 65536

int reader_open(struct hd_reader *r, const char *path)
{
    r->f = fopen(path, "rb");
    if (r->f == NULL)
        return -1;
    r->buf = xmalloc(READER_BUFSIZE);
    r->pos = 0;
    r->end = 0;
    r->eof = 0;
    r->complete = 0;
    buf_init(&r->line);
    buf_append(&r->line, "", 0);
    return 0;
}

int reader_next(struct hd_reader *r)
{
    buf_clear(&r->line);
    for (;;) {
        const char *nl;

        if (r->pos == r->end) {
            size_t n;

            if (r->eof) {
                r->complete = 0;
                return r->line.len > 0 ? 1 : 0;
            }
            n = fread(r->buf, 1, READER_BUFSIZE, r->f);
            if (n == 0) {
                if (ferror(r->f))
                    return -1;
                r->eof = 1;
                continue;
            }
            r->pos = 0;
            r->end = n;
        }
        nl = memchr(r->buf + r->pos, '\n', r->end - r->pos);
        if (nl != NULL) {
            size_t len = (size_t)(nl - (r->buf + r->pos));

            buf_append(&r->line, r->buf + r->pos, len);
            r->pos += len + 1;
            r->complete = 1;
            return 1;
        }
        buf_append(&r->line, r->buf + r->pos, r->end - r->pos);
        r->pos = r->end;
    }
}

void reader_close(struct hd_reader *r)
{
    fclose(r->f);
    free(r->buf);
    buf_free(&r->line);
}

static void outfile_fail(struct hd_outfile *o)
{
    hd_die("cannot write '%s': %s", o->tmp, strerror(errno));
}

void outfile_open(struct hd_outfile *o, const char *dir, const char *name)
{
    o->path = hd_path_join(dir, name);
    o->tmp = xmalloc(strlen(o->path) + 5);
    strcpy(o->tmp, o->path);
    strcat(o->tmp, ".tmp");
    o->f = fopen(o->tmp, "wb");
    if (o->f == NULL)
        outfile_fail(o);
}

void outfile_write(struct hd_outfile *o, const void *data, size_t n)
{
    if (n > 0 && fwrite(data, 1, n, o->f) != n)
        outfile_fail(o);
}

void outfile_flush(struct hd_outfile *o)
{
    if (fflush(o->f) != 0)
        outfile_fail(o);
}

void outfile_commit(struct hd_outfile *o)
{
    int r;

    outfile_flush(o);
    r = fclose(o->f);
    o->f = NULL;
    if (r != 0)
        outfile_fail(o);
    if (os_rename(o->tmp, o->path) != 0)
        hd_die("cannot rename '%s' to '%s': %s", o->tmp, o->path, strerror(errno));
    free(o->path);
    free(o->tmp);
}

void outfile_discard(struct hd_outfile *o)
{
    if (o->f != NULL)
        fclose(o->f);
    o->f = NULL;
    (void)os_unlink(o->tmp);
    free(o->path);
    free(o->tmp);
}
