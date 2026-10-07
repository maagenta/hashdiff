/* os.c - POSIX backend. */
#include "config.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "os.h"
#include "util.h"

static void convert_stat(const struct stat *sb, struct os_stat *st)
{
    if (S_ISREG(sb->st_mode))
        st->kind = OS_REG;
    else if (S_ISDIR(sb->st_mode))
        st->kind = OS_DIR;
    else if (S_ISLNK(sb->st_mode))
        st->kind = OS_LNK;
    else
        st->kind = OS_OTHER;
    st->size = sb->st_size;
    st->mtime = (off_t)sb->st_mtime;
    st->dev = sb->st_dev;
    st->ino = sb->st_ino;
}

int os_lstat(const char *path, struct os_stat *st)
{
    struct stat sb;

    if (lstat(path, &sb) != 0)
        return -1;
    convert_stat(&sb, st);
    return 0;
}

int os_stat(const char *path, struct os_stat *st)
{
    struct stat sb;

    if (stat(path, &sb) != 0)
        return -1;
    convert_stat(&sb, st);
    return 0;
}

int os_fstat(int fd, struct os_stat *st)
{
    struct stat sb;

    if (fstat(fd, &sb) != 0)
        return -1;
    convert_stat(&sb, st);
    return 0;
}

int os_list_dir(const char *path, struct os_dirent **entries, size_t *count)
{
    DIR *dir = opendir(path);
    struct os_dirent *list = NULL;
    size_t n = 0, cap = 0;
    struct dirent *de;
    int err;

    if (dir == NULL)
        return -1;
    for (;;) {
        errno = 0;
        de = readdir(dir);
        if (de == NULL)
            break;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            list = xrealloc(list, cap * sizeof(*list));
        }
        list[n].name = xstrdup(de->d_name);
        list[n].ino = de->d_ino;
        n++;
    }
    err = errno;
    closedir(dir);
    if (err != 0) {
        os_free_dir(list, n);
        errno = err;
        return -1;
    }
    *entries = list;
    *count = n;
    return 0;
}

void os_free_dir(struct os_dirent *entries, size_t count)
{
    size_t i;

    for (i = 0; i < count; i++)
        free(entries[i].name);
    free(entries);
}

int os_open_read(const char *path)
{
    /* O_NONBLOCK: never hang if the entry was replaced by a FIFO after the traversal. */
    return open(path, O_RDONLY | O_NONBLOCK);
}

int os_close(int fd)
{
    return close(fd);
}

ssize_t os_read(int fd, void *buf, size_t n)
{
    return read(fd, buf, n);
}

ssize_t os_pread(int fd, void *buf, size_t n, off_t off)
{
    return pread(fd, buf, n, off);
}

ssize_t os_write(int fd, const void *buf, size_t n)
{
    return write(fd, buf, n);
}

ssize_t os_readlink(const char *path, char *buf, size_t n)
{
    return readlink(path, buf, n);
}

void os_advise_sequential(int fd)
{
#ifdef POSIX_FADV_SEQUENTIAL
    (void)posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#else
    (void)fd;
#endif
}

int os_mkdir(const char *path)
{
    return mkdir(path, 0777);
}

int os_rename(const char *from, const char *to)
{
    return rename(from, to);
}

int os_unlink(const char *path)
{
    return unlink(path);
}

char *os_getcwd(void)
{
    size_t size = 256;

    for (;;) {
        char *buf = xmalloc(size);

        if (getcwd(buf, size) != NULL)
            return buf;
        free(buf);
        if (errno != ERANGE)
            return NULL;
        size *= 2;
    }
}

int os_isatty(int fd)
{
    return isatty(fd);
}

int os_pipe(int fds[2])
{
    return pipe(fds);
}

long os_fork(void)
{
    return (long)fork();
}

int os_wait(long pid, int *code, int *sig)
{
    int status;

    while (waitpid((pid_t)pid, &status, 0) < 0)
        if (errno != EINTR)
            return -1;
    if (WIFEXITED(status)) {
        *code = WEXITSTATUS(status);
        *sig = 0;
    } else {
        *code = -1;
        *sig = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
    }
    return 0;
}

int os_kill(long pid, int sig)
{
    return kill((pid_t)pid, sig);
}

void os_exit_now(int status)
{
    _exit(status);
}
