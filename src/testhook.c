/* testhook.c - test-only driver that exposes internal functions to the Python tests. */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "md5.h"
#include "plan.h"
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

static int parse_size_arg(const char *s, off_t min, off_t *out)
{
    return hd_dec_to_off(s, strlen(s), out) == 0 && *out >= min ? 0 : -1;
}

/* plan N GAP BLOCK PROFILE: "full", or "sampled K" and one offset per line. */
static int cmd_plan(int argc, char **argv)
{
    struct hd_plan plan;
    struct hd_plan_iter it;
    off_t n, gap, block, off;
    int profile;
    char num[HD_OFF_DEC_LEN];

    if (argc != 6 || parse_size_arg(argv[2], 0, &n) != 0 || parse_size_arg(argv[3], 1, &gap) != 0
        || parse_size_arg(argv[4], 512, &block) != 0
        || (profile = plan_profile_number(argv[5])) < 0) {
        fputs("hashdiff-testhook: invalid plan arguments\n", stderr);
        usage();
        return 2;
    }
    plan_make(n, gap, block, plan_profile(profile)->seek_bytes, &plan);
    if (!plan.sampled) {
        printf("full\n");
        return fflush(stdout) == 0 ? 0 : 2;
    }
    printf("sampled %s\n", hd_off_to_dec(plan.k, num));
    plan_iter_init(&it, &plan);
    while (plan_iter_next(&it, &off))
        printf("%s\n", hd_off_to_dec(off, num));
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
    if (strcmp(argv[1], "plan") == 0)
        return cmd_plan(argc, argv);
    fprintf(stderr, "hashdiff-testhook: unknown command '%s'\n", argv[1]);
    usage();
    return 2;
}
