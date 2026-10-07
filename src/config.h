/* config.h - build configuration checks; included first in every .c file. */
#ifndef HD_CONFIG_H
#define HD_CONFIG_H

#if !defined(_FILE_OFFSET_BITS) || _FILE_OFFSET_BITS != 64
#error "hashdiff must be built with -D_FILE_OFFSET_BITS=64 (use the Makefile)"
#endif

#include <sys/types.h>

/* C89 static assert: sizes and offsets of files > 4 GiB must fit in off_t. */
typedef char hd_off_t_is_64bit[sizeof(off_t) >= 8 ? 1 : -1];

#define HD_VERSION "2.0"

#endif
