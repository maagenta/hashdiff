/* opts.c - command line and SIZE parser (no getopt_long). */
#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "opts.h"
#include "util.h"

enum opt_id {
    OPT_OUTPUT, OPT_RESUME, OPT_FORCE, OPT_FAST, OPT_GAP, OPT_BLOCK, OPT_PROFILE, OPT_JOBS,
    OPT_SERIAL, OPT_ONE_FS, OPT_NDEST, OPT_QUIET, OPT_HELP, OPT_VERSION
};

struct optdef {
    char shortname;      /* 0: long option only */
    const char *longname;
    int has_arg;
    enum opt_id id;
};

static const struct optdef optdefs[] = {
    { 'o', "output", 1, OPT_OUTPUT },
    { 0, "resume", 0, OPT_RESUME },
    { 0, "force", 0, OPT_FORCE },
    { 'f', "fast", 0, OPT_FAST },
    { 'g', "gap", 1, OPT_GAP },
    { 'b', "block", 1, OPT_BLOCK },
    { 0, "profile", 1, OPT_PROFILE },
    { 'j', "jobs", 1, OPT_JOBS },
    { 0, "serial", 0, OPT_SERIAL },
    { 'x', "one-file-system", 0, OPT_ONE_FS },
    { 0, "number-of-destinations", 1, OPT_NDEST },
    { 'q', "quiet", 0, OPT_QUIET },
    { 'h', "help", 0, OPT_HELP },
    { 'V', "version", 0, OPT_VERSION }
};

#define N_OPTDEFS (sizeof(optdefs) / sizeof(optdefs[0]))

static const char *const help_lines[] = {
    "Usage: hashdiff ORIGIN DESTINATION [DESTINATION ...] [OPTIONS]",
    "",
    "Compare the content of one directory tree with one or more copies of it and list the",
    "files to re-sync with rsync. Every destination is compared with ORIGIN, never with",
    "another destination. Results are written to DIR/results.hashdiff/.",
    "",
    "Options (they may appear before, between or after the paths):",
    "  -o, --output DIR        Existing directory where DIR/results.hashdiff/ is created",
    "                          (default: .)",
    "      --resume            If results.hashdiff exists, resume the interrupted run",
    "                          without asking",
    "      --force             If results.hashdiff exists, discard it and start over",
    "                          without asking",
    "  -f, --fast              Sampled fast mode. Without it: full MD5 of everything",
    "  -g, --gap SIZE          Maximum unread region between two samples (default: 64M).",
    "                          Any contiguous damage larger than SIZE is always detected",
    "  -b, --block SIZE        Bytes read per sample (default: 64K for ssd, 1M for hdd)",
    "      --profile hdd|ssd   Disk type (default: ssd). Sets the default --block, the",
    "                          cost model and the read strategy of --fast",
    "  -j, --jobs N            Hashing processes per side, 1..256 (default: 1)",
    "      --serial            Process one side at a time instead of all of them at once",
    "  -x, --one-file-system   Do not cross mount points",
    "      --number-of-destinations N",
    "                          Fail unless exactly N destinations were given",
    "  -q, --quiet             No progress on stderr",
    "  -h, --help              Show this help and exit",
    "  -V, --version           Show the version and exit",
    "",
    "SIZE is an integer with an optional K, M, G or T suffix (powers of 1024).",
    "",
    "Exit status, most severe first: 2 fatal error, 4 the tree of some destination differs",
    "(it was not hashed), 3 completed with read errors, 1 differences found, 0 no",
    "differences; 128+N interrupted by signal N."
};

#define N_HELP_LINES (sizeof(help_lines) / sizeof(help_lines[0]))

static void usage_error(const char *msg, const char *arg)
{
    if (arg != NULL)
        fprintf(stderr, "hashdiff: %s '%s'\n", msg, arg);
    else
        fprintf(stderr, "hashdiff: %s\n", msg);
    fputs("Try 'hashdiff --help' for more information.\n", stderr);
}

int opts_parse_size(const char *s, off_t *out)
{
    off_t v = 0, mult;
    const char *p = s;

    if (!isdigit((unsigned char)*p))
        return -1;
    for (; isdigit((unsigned char)*p); p++) {
        int d = *p - '0';

        if (v > (HD_OFF_MAX - d) / 10)
            return -1;
        v = v * 10 + d;
    }
    switch (tolower((unsigned char)*p)) {
    case '\0':
        mult = 1;
        break;
    case 'k':
        mult = 1024;
        break;
    case 'm':
        mult = (off_t)1024 * 1024;
        break;
    case 'g':
        mult = (off_t)1024 * 1024 * 1024;
        break;
    case 't':
        mult = (off_t)1024 * 1024 * 1024 * 1024;
        break;
    default:
        return -1;
    }
    if (*p != '\0' && p[1] != '\0')
        return -1;
    return hd_off_mul(v, mult, out);
}

/* A positive integer of at most max, for --jobs and --number-of-destinations. */
static int parse_count(const char *s, int max, int *out)
{
    int v = 0;

    if (*s == '\0')
        return -1;
    for (; *s != '\0'; s++) {
        if (!isdigit((unsigned char)*s))
            return -1;
        v = v * 10 + (*s - '0');
        if (v > max)
            return -1;
    }
    if (v < 1)
        return -1;
    *out = v;
    return 0;
}

/* Flags of options given explicitly, for the "no effect without --fast" warnings. */
struct seen {
    int gap;
    int block;
    int profile;
};

static int apply(const struct optdef *d, const char *val, struct hd_opts *o, struct seen *seen)
{
    size_t i;

    switch (d->id) {
    case OPT_OUTPUT:
        free(o->output);
        o->output = xstrdup(val);
        break;
    case OPT_RESUME:
        o->resume = 1;
        break;
    case OPT_FORCE:
        o->force = 1;
        break;
    case OPT_FAST:
        o->fast = 1;
        break;
    case OPT_GAP:
        if (opts_parse_size(val, &o->gap) != 0 || o->gap < 1) {
            usage_error("invalid --gap (expected a SIZE >= 1):", val);
            return OPTS_ERROR;
        }
        seen->gap = 1;
        break;
    case OPT_BLOCK:
        if (opts_parse_size(val, &o->block) != 0 || o->block < 512) {
            usage_error("invalid --block (expected a SIZE >= 512):", val);
            return OPTS_ERROR;
        }
        seen->block = 1;
        break;
    case OPT_PROFILE:
        if (strcmp(val, "ssd") == 0)
            o->profile = HD_PROFILE_SSD;
        else if (strcmp(val, "hdd") == 0)
            o->profile = HD_PROFILE_HDD;
        else {
            usage_error("invalid --profile (expected hdd or ssd):", val);
            return OPTS_ERROR;
        }
        seen->profile = 1;
        break;
    case OPT_JOBS:
        if (parse_count(val, 256, &o->jobs) != 0) {
            usage_error("invalid --jobs (expected an integer from 1 to 256):", val);
            return OPTS_ERROR;
        }
        break;
    case OPT_NDEST:
        if (parse_count(val, HD_MAX_DESTINATIONS, &o->ndest_check) != 0) {
            usage_error("invalid --number-of-destinations (expected an integer from 1 to "
                        "64):", val);
            return OPTS_ERROR;
        }
        break;
    case OPT_SERIAL:
        o->serial = 1;
        break;
    case OPT_ONE_FS:
        o->one_fs = 1;
        break;
    case OPT_QUIET:
        o->quiet = 1;
        break;
    case OPT_HELP:
        for (i = 0; i < N_HELP_LINES; i++)
            printf("%s\n", help_lines[i]);
        return OPTS_DONE;
    case OPT_VERSION:
        printf("hashdiff %s\n", HD_VERSION);
        return OPTS_DONE;
    }
    return OPTS_RUN;
}

static const struct optdef *find_long(const char *name, size_t len)
{
    size_t i;

    for (i = 0; i < N_OPTDEFS; i++)
        if (strlen(optdefs[i].longname) == len && strncmp(optdefs[i].longname, name, len) == 0)
            return &optdefs[i];
    return NULL;
}

static const struct optdef *find_short(char c)
{
    size_t i;

    for (i = 0; i < N_OPTDEFS; i++)
        if (optdefs[i].shortname != 0 && optdefs[i].shortname == c)
            return &optdefs[i];
    return NULL;
}

/* Parses one "--name[=value]" argument; *i is advanced when the value is the next one. */
static int parse_long(int argc, char **argv, int *i, struct hd_opts *o, struct seen *seen)
{
    const char *arg = argv[*i];
    const char *name = arg + 2;
    const char *eq = strchr(name, '=');
    size_t len = eq != NULL ? (size_t)(eq - name) : strlen(name);
    const struct optdef *d = find_long(name, len);
    const char *val = NULL;

    if (d == NULL) {
        usage_error("unknown option", arg);
        return OPTS_ERROR;
    }
    if (d->has_arg) {
        if (eq != NULL)
            val = eq + 1;
        else if (*i + 1 < argc)
            val = argv[++*i];
        else {
            usage_error("option requires an argument:", arg);
            return OPTS_ERROR;
        }
    } else if (eq != NULL) {
        usage_error("option does not take an argument:", arg);
        return OPTS_ERROR;
    }
    return apply(d, val, o, seen);
}

/* Parses a cluster of short options such as "-fq" or "-g8M". */
static int parse_short(int argc, char **argv, int *i, struct hd_opts *o, struct seen *seen)
{
    const char *arg = argv[*i];
    size_t j;

    for (j = 1; arg[j] != '\0'; j++) {
        const struct optdef *d = find_short(arg[j]);
        char optname[3];
        int r;

        optname[0] = '-';
        optname[1] = arg[j];
        optname[2] = '\0';
        if (d == NULL) {
            usage_error("unknown option", optname);
            return OPTS_ERROR;
        }
        if (d->has_arg) {
            const char *val;

            if (arg[j + 1] != '\0')
                val = arg + j + 1;
            else if (*i + 1 < argc)
                val = argv[++*i];
            else {
                usage_error("option requires an argument:", optname);
                return OPTS_ERROR;
            }
            return apply(d, val, o, seen);
        }
        r = apply(d, NULL, o, seen);
        if (r != OPTS_RUN)
            return r;
    }
    return OPTS_RUN;
}

static int parse_args(int argc, char **argv, struct hd_opts *o)
{
    struct seen seen;
    const char *paths[1 + HD_MAX_DESTINATIONS];
    int npaths = 0, end_of_options = 0, i, r;

    seen.gap = seen.block = seen.profile = 0;
    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (end_of_options || arg[0] != '-' || arg[1] == '\0') {
            if (npaths == 1 + HD_MAX_DESTINATIONS) {
                usage_error("too many destinations (the maximum is 64):", arg);
                return OPTS_ERROR;
            }
            paths[npaths++] = arg;
            continue;
        }
        if (strcmp(arg, "--") == 0) {
            end_of_options = 1;
            continue;
        }
        if (arg[1] == '-')
            r = parse_long(argc, argv, &i, o, &seen);
        else
            r = parse_short(argc, argv, &i, o, &seen);
        if (r != OPTS_RUN)
            return r;
    }
    if (npaths < 2) {
        usage_error("ORIGIN and at least one DESTINATION are required", NULL);
        return OPTS_ERROR;
    }
    for (i = 0; i < npaths; i++) {
        if (paths[i][0] == '\0') {
            usage_error("ORIGIN and the destinations must not be empty", NULL);
            return OPTS_ERROR;
        }
    }
    if (o->ndest_check != 0 && o->ndest_check != npaths - 1) {
        char given[32];

        sprintf(given, "%d", npaths - 1);
        usage_error("--number-of-destinations does not match the number of destinations "
                    "given:", given);
        return OPTS_ERROR;
    }
    if (o->output != NULL && o->output[0] == '\0') {
        usage_error("--output must not be empty", NULL);
        return OPTS_ERROR;
    }
    if (o->resume && o->force) {
        usage_error("--resume and --force cannot be used together", NULL);
        return OPTS_ERROR;
    }
    if (!o->fast) {
        if (seen.gap)
            hd_warn("--gap has no effect without --fast");
        if (seen.block)
            hd_warn("--block has no effect without --fast");
        if (seen.profile)
            hd_warn("--profile has no effect without --fast");
    }
    /* Section 2.1: every path is cleaned once, here, and only the cleaned form is used. */
    o->origin = hd_clean_abs(paths[0]);
    o->ndest = npaths - 1;
    o->dest = xmalloc((size_t)o->ndest * sizeof(*o->dest));
    for (i = 0; i < o->ndest; i++)
        o->dest[i] = hd_clean_abs(paths[i + 1]);
    {
        char *given = o->output;

        o->output = hd_clean_abs(given == NULL ? "." : given);
        free(given);
    }
    return OPTS_RUN;
}

int opts_parse(int argc, char **argv, struct hd_opts *o)
{
    int r;

    o->origin = NULL;
    o->dest = NULL;
    o->ndest = 0;
    o->ndest_check = 0;
    o->output = NULL;
    o->resume = 0;
    o->force = 0;
    o->fast = 0;
    o->gap = (off_t)64 * 1024 * 1024;
    o->block = 0;
    o->profile = HD_PROFILE_SSD;
    o->jobs = 1;
    o->serial = 0;
    o->one_fs = 0;
    o->quiet = 0;
    r = parse_args(argc, argv, o);
    if (r != OPTS_RUN)
        opts_free(o);
    return r;
}

void opts_free(struct hd_opts *o)
{
    int i;

    for (i = 0; i < o->ndest; i++)
        free(o->dest[i]);
    free(o->dest);
    free(o->origin);
    free(o->output);
    o->dest = NULL;
    o->ndest = 0;
    o->origin = NULL;
    o->output = NULL;
}
