/* opts.h - command line and SIZE parser. */
#ifndef HD_OPTS_H
#define HD_OPTS_H

#include <sys/types.h>

#define HD_PROFILE_SSD 0
#define HD_PROFILE_HDD 1

/* Section 2.2: one ORIGIN and 1 to 64 destinations. */
#define HD_MAX_DESTINATIONS 64

struct hd_opts {
    char *origin;        /* cleaned absolute path (section 2.1) */
    char **dest;         /* ndest cleaned absolute paths, in command-line order */
    int ndest;
    int ndest_check;     /* --number-of-destinations, or 0 if it was not given */
    char *output;        /* cleaned absolute directory where results.hashdiff is created */
    int resume;
    int force;
    int ignore_lock;
    int fast;
    off_t gap;
    off_t block;         /* 0: default of the profile */
    int profile;
    int jobs;
    int serial;
    int one_fs;
    int quiet;
};

/* Return values of opts_parse. */
#define OPTS_RUN 0
#define OPTS_DONE 1      /* --help or --version was printed: exit 0 */
#define OPTS_ERROR 2     /* message printed: exit 2 */

int opts_parse(int argc, char **argv, struct hd_opts *o);
void opts_free(struct hd_opts *o);
int opts_parse_size(const char *s, off_t *out);

#endif
