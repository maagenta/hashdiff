/* testhook.c - test-only driver that exposes internal functions to the Python tests. */
#include "config.h"

#include <stdio.h>

#include "util.h"

static void usage(void)
{
    fputs("usage: hashdiff-testhook md5 [CHUNK]\n"
          "       hashdiff-testhook plan N GAP BLOCK PROFILE\n", stderr);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }
    fprintf(stderr, "hashdiff-testhook: unknown command '%s'\n", argv[1]);
    usage();
    return 2;
}
