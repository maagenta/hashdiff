/* os.h - every POSIX call goes through here, so another backend can be added later. */
#ifndef HD_OS_H
#define HD_OS_H

#include <stddef.h>
#include <sys/types.h>

/* File kinds of struct os_stat. */
#define OS_REG 1
#define OS_DIR 2
#define OS_LNK 3
#define OS_OTHER 4      /* FIFO, socket, device */

struct os_stat {
    int kind;
    off_t size;
    off_t mtime;        /* seconds */
    dev_t dev;
    ino_t ino;
};

struct os_dirent {
    char *name;         /* owned by the array returned by os_list_dir */
    ino_t ino;
};

/* All functions returning int return 0 on success, or -1 with errno set. */
int os_lstat(const char *path, struct os_stat *st);
int os_stat(const char *path, struct os_stat *st);
int os_fstat(int fd, struct os_stat *st);

/* Reads every entry of a directory except "." and "..", then closes it. */
int os_list_dir(const char *path, struct os_dirent **entries, size_t *count);
void os_free_dir(struct os_dirent *entries, size_t count);

int os_open_read(const char *path);
int os_close(int fd);
ssize_t os_read(int fd, void *buf, size_t n);
ssize_t os_pread(int fd, void *buf, size_t n, off_t off);
ssize_t os_write(int fd, const void *buf, size_t n);
ssize_t os_readlink(const char *path, char *buf, size_t n);

void os_advise_sequential(int fd);

int os_mkdir(const char *path);
int os_rename(const char *from, const char *to);
int os_unlink(const char *path);
char *os_getcwd(void);   /* malloc'd; NULL with errno on failure */
int os_isatty(int fd);

#endif
