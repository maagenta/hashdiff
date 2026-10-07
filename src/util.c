/* util.c - memory, dynamic buffers, escaping, off_t arithmetic and EINTR-safe I/O. */
#include "config.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "os.h"
#include "util.h"

int hd_is_child = 0;

void hd_exit(int status)
{
    if (hd_is_child)
        _exit(status);
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
