/* main.c - orchestration, fork/wait and summary. */
#include "config.h"

#include <stdio.h>

#include "opts.h"
#include "util.h"

int main(int argc, char **argv)
{
    struct hd_opts o;
    int r = opts_parse(argc, argv, &o);

    if (r == OPTS_DONE)
        return fflush(stdout) == 0 ? 0 : 2;
    if (r == OPTS_ERROR)
        return 2;
    hd_warn("comparison is not implemented yet");
    opts_free(&o);
    return 2;
}
