/* util.h - memory, dynamic buffers, escaping, off_t arithmetic and EINTR-safe I/O. */
#ifndef HD_UTIL_H
#define HD_UTIL_H

#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

/* Largest off_t value; off_t is signed and at least 64 bits (config.h). */
#define HD_OFF_MAX ((off_t)((((off_t)1 << (sizeof(off_t) * 8 - 2)) - 1) * 2 + 1))

/* Enough room for any off_t in decimal, with sign and terminating NUL. */
#define HD_OFF_DEC_LEN 48

/* Set in forked processes: hd_exit() then uses _exit(). */
extern int hd_is_child;

void hd_exit(int status);
void hd_die(const char *fmt, ...);
void hd_warn(const char *fmt, ...);

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);

/* Growable byte buffer; data is always NUL-terminated once anything was appended. */
struct hd_buf {
    char *data;
    size_t len;
    size_t cap;
};

void buf_init(struct hd_buf *b);
void buf_free(struct hd_buf *b);
void buf_clear(struct hd_buf *b);
void buf_append(struct hd_buf *b, const char *s, size_t n);
void buf_append_str(struct hd_buf *b, const char *s);
void buf_append_char(struct hd_buf *b, int c);

/* Bump allocator for many small strings, freed all at once. */
struct hd_arena_chunk;
struct hd_arena {
    struct hd_arena_chunk *chunks;
    char *cur;
    size_t left;
};

void arena_init(struct hd_arena *a);
void arena_free(struct hd_arena *a);
char *arena_strndup(struct hd_arena *a, const char *s, size_t n);

/* Joins a directory and a name with exactly one '/' (dir may be "/"); malloc'd. */
char *hd_path_join(const char *dir, const char *name);

/* Section 2.1: absolute and lexically cleaned, so the same directory is always written the
 * same way. No '.' or '..' component, no "//" and no trailing '/' (except the root "/").
 * Relative paths are taken from getcwd(); symlinks are never resolved. malloc'd; fatal if
 * the current directory cannot be read. */
char *hd_clean_abs(const char *p);

/* "" or "s", for a count printed next to a noun (section 9). */
const char *hd_plural(unsigned long n);

/* Room for a timestamp of section 8 and for the stamp of an archive name (section 3.5). */
#define HD_TIME_LEN 32

/* Local time as "dd/mm/yyyy HH:MM" (section 8). 24-hour on purpose: %I needs %p, which is
 * locale-dependent and which the standard allows to be empty. Returns buf. */
char *hd_time_text(long t, char *buf);
/* Local time as "YYYYMMDDHHMM", which sorts (the archive names of section 3.5). */
char *hd_time_stamp(long t, char *buf);

/* Path escaping of the output files: '\' -> "\\", LF -> "\n", CR -> "\r". */
void hd_escape(struct hd_buf *out, const char *s);
/* Appends the unescaped form of s[0..n); returns -1 if the escaping is invalid. */
int hd_unescape(struct hd_buf *out, const char *s, size_t n);
/* POSIX shell single quoting: ' -> '\'' */
void hd_shell_quote(struct hd_buf *out, const char *s);

char *hd_off_to_dec(off_t v, char *buf);
int hd_dec_to_off(const char *s, size_t n, off_t *out);
int hd_off_add(off_t a, off_t b, off_t *r);
int hd_off_mul(off_t a, off_t b, off_t *r);

/* "12.4 GiB": one decimal, integer arithmetic only; buf has HD_OFF_DEC_LEN + 8 bytes. */
char *hd_human_bytes(off_t v, char *buf);

/* Buffered reader of LF-terminated lines of any length. */
struct hd_reader {
    FILE *f;
    char *buf;
    size_t pos;
    size_t end;
    int eof;
    struct hd_buf line;         /* current line, without the LF */
    int complete;               /* the current line was terminated by LF */
};

int reader_open(struct hd_reader *r, const char *path);
/* 1: a line is in r->line; 0: end of file; -1: read error (errno set). */
int reader_next(struct hd_reader *r);
void reader_close(struct hd_reader *r);

/* Output file written as NAME.tmp and published with rename(); errors are fatal. */
struct hd_outfile {
    FILE *f;
    char *path;
    char *tmp;
};

void outfile_open(struct hd_outfile *o, const char *dir, const char *name);
/* Like outfile_open, but the temporary file is NAME + suffix (e.g. ".new"). */
void outfile_open_suffix(struct hd_outfile *o, const char *dir, const char *name,
                         const char *suffix);
/* Renames the temporary file to NAME + suffix while it stays open for writing. */
void outfile_rename_tmp(struct hd_outfile *o, const char *suffix);
void outfile_write(struct hd_outfile *o, const void *data, size_t n);
void outfile_flush(struct hd_outfile *o);
void outfile_commit(struct hd_outfile *o);
void outfile_discard(struct hd_outfile *o);

ssize_t hd_read_full(int fd, void *buf, size_t n);
int hd_write_all(int fd, const void *buf, size_t n);

#endif
