/* os.c - POSIX backend. */
#include "config.h"

#include <unistd.h>

#include "os.h"

ssize_t os_read(int fd, void *buf, size_t n)
{
    return read(fd, buf, n);
}

ssize_t os_write(int fd, const void *buf, size_t n)
{
    return write(fd, buf, n);
}

int os_isatty(int fd)
{
    return isatty(fd);
}
