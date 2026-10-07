/* main.c - orchestration, fork/wait and summary. */
#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hasher.h"
#include "opts.h"
#include "os.h"
#include "util.h"
#include "walk.h"

#define RESULTS_NAME "results.hashdiff"

/* Absolute form of a path: getcwd + concatenation, without resolving symlinks. */
static char *absolute_path(const char *p)
{
    char *cwd, *abs;

    if (p[0] == '/')
        return xstrdup(p);
    cwd = os_getcwd();
    if (cwd == NULL)
        hd_die("cannot get the current directory: %s", strerror(errno));
    abs = hd_path_join(cwd, p);
    free(cwd);
    return abs;
}

static void stat_dir(const char *what, const char *path, struct os_stat *st)
{
    if (os_stat(path, st) != 0)
        hd_die("cannot access %s '%s': %s", what, path, strerror(errno));
    if (st->kind != OS_DIR)
        hd_die("%s '%s' is not a directory", what, path);
}

/* Traverses, sorts and hashes one tree into DIR/hashes-SIDE.txt; returns 1 if E entries. */
static int process_side(const char *side, const char *root, const char *results,
                        const struct hd_opts *o, const struct hd_exclude *excl, size_t nexcl)
{
    struct walk_opts wo;
    struct hd_list list;
    struct hd_hasher h;
    struct hd_journal j;
    char *abs_root = absolute_path(root);
    char name[64];
    size_t i;
    int errors = 0;

    wo.side = side;
    wo.one_fs = o->one_fs;
    wo.excludes = excl;
    wo.nexcludes = nexcl;
    walk_tree(root, &wo, &list);
    list_sort(&list);
    hasher_init(&h);
    sprintf(name, "hashes-%s.txt", side);
    journal_open(&j, results, name, abs_root, "full");
    for (i = 0; i < list.count; i++) {
        const struct hd_entry *e = &list.items[i];
        char *path = hd_path_join(root, e->path);
        struct hd_result r;

        hash_entry(&h, path, e, &r);
        free(path);
        if (r.type == RES_ERROR)
            errors = 1;
        journal_write(&j, e->path, &r);
    }
    journal_commit(&j);
    hasher_free(&h);
    list_free(&list);
    free(abs_root);
    return errors;
}

int main(int argc, char **argv)
{
    struct hd_opts o;
    struct os_stat so, sd, sout, sres;
    struct hd_exclude excl[2];
    char *results;
    int r = opts_parse(argc, argv, &o), errors = 0;

    if (r == OPTS_DONE)
        return fflush(stdout) == 0 ? 0 : 2;
    if (r == OPTS_ERROR)
        return 2;
    stat_dir("ORIGIN", o.origin, &so);
    stat_dir("DESTINATION", o.destination, &sd);
    if (so.dev == sd.dev && so.ino == sd.ino)
        hd_die("ORIGIN and DESTINATION are the same directory");
    stat_dir("output directory", o.output, &sout);
    results = hd_path_join(o.output, RESULTS_NAME);
    if (os_mkdir(results) != 0 && !(errno == EEXIST && o.force))
        hd_die("cannot create '%s': %s", results, strerror(errno));
    stat_dir("results directory", results, &sres);

    excl[0].dev = sres.dev;
    excl[0].ino = sres.ino;
    excl[0].what = "results directory";
    excl[1].dev = sd.dev;
    excl[1].ino = sd.ino;
    excl[1].what = "DESTINATION tree";
    errors |= process_side("origin", o.origin, results, &o, excl, 2);
    excl[1].dev = so.dev;
    excl[1].ino = so.ino;
    excl[1].what = "ORIGIN tree";
    errors |= process_side("destination", o.destination, results, &o, excl, 2);

    free(results);
    opts_free(&o);
    return errors ? 3 : 0;
}
