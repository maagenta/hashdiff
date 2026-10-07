/* os.h - every POSIX call goes through here, so another backend can be added later. */
#ifndef HD_OS_H
#define HD_OS_H

#include <stddef.h>
#include <sys/types.h>

ssize_t os_read(int fd, void *buf, size_t n);
ssize_t os_write(int fd, const void *buf, size_t n);
int os_isatty(int fd);

#endif
