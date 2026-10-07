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

/* posix_fadvise hints where available (not on macOS); failures are ignored. */
void os_advise_sequential(int fd);
void os_advise_random(int fd);
void os_advise_willneed(int fd, off_t off, off_t len);

int os_mkdir(const char *path);
int os_rename(const char *from, const char *to);
int os_unlink(const char *path);
char *os_getcwd(void);   /* malloc'd; NULL with errno on failure */
int os_isatty(int fd);

/*
 * Section 3.6: opens path and takes an advisory write lock on the whole file with
 * fcntl(F_SETLK). The descriptor is kept inside os.c and never closed, because POSIX drops
 * every lock a process holds on a file as soon as it closes any descriptor to it; the kernel
 * releases the lock when the process ends, through any exit path. Returns 0 when the lock is
 * held by this process, 1 when another one holds it (*pid is its pid, or 0 when the system
 * cannot say), or -1 when this filesystem does not do locking (errno is set).
 */
int os_lock(const char *path, long *pid);

int os_pipe(int fds[2]);
/* Returns the child pid in the parent, 0 in the child, -1 on failure. */
long os_fork(void);
/* Waits for a child, retrying on EINTR. On success *code is the exit status, or -1 if the
 * child was killed by a signal (then *sig is the signal number). */
int os_wait(long pid, int *code, int *sig);
int os_kill(long pid, int sig);
void os_exit_now(int status);   /* _exit() */

/* SIGINT and SIGTERM: the handler only records the signal number. No SA_RESTART, so a
 * blocking read or write returns EINTR and the caller can check os_caught_signal(). */
void os_install_signal_handlers(void);
int os_caught_signal(void);     /* 0, or the number of the last signal caught */
int os_sigint(void);
int os_sigterm(void);
long os_time(void);             /* time() in seconds */

#endif
