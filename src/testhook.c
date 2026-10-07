/* testhook.c - test-only driver that exposes internal functions to the Python tests. */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "md5.h"
#include "util.h"

#define MAX_CHUNK ((off_t)64 * 1024 * 1024)

static void usage(void)
{
    fputs("usage: hashdiff-testhook md5 [CHUNK]\n"
          "       hashdiff-testhook plan N GAP BLOCK PROFILE\n", stderr);
}

/* md5 [CHUNK]: digest of stdin, fed to md5_update in chunks of CHUNK bytes. */
static int cmd_md5(int argc, char **argv)
{
    off_t chunk = 65536;
    struct md5_ctx ctx;
    unsigned char digest[16], *buf;
    char hex[33];
    ssize_t n;

    if (argc > 3) {
        usage();
        return 2;
    }
    if (argc == 3 && (hd_dec_to_off(argv[2], strlen(argv[2]), &chunk) != 0 || chunk < 1
                      || chunk > MAX_CHUNK)) {
        fprintf(stderr, "hashdiff-testhook: invalid CHUNK '%s'\n", argv[2]);
        return 2;
    }
    buf = xmalloc((size_t)chunk);
    md5_init(&ctx);
    while ((n = hd_read_full(0, buf, (size_t)chunk)) > 0)
        md5_update(&ctx, buf, (size_t)n);
    free(buf);
    if (n < 0) {
        perror("hashdiff-testhook: read");
        return 2;
    }
    md5_final(&ctx, digest);
    md5_hex(digest, hex);
    printf("%s\n", hex);
    return fflush(stdout) == 0 ? 0 : 2;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }
    if (strcmp(argv[1], "md5") == 0)
        return cmd_md5(argc, argv);
    fprintf(stderr, "hashdiff-testhook: unknown command '%s'\n", argv[1]);
    usage();
    return 2;
}
