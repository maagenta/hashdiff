/* walk.c - traversal of one tree, entry list and canonical sorting. */
#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "os.h"
#include "walk.h"

static void list_init(struct hd_list *l)
{
    l->items = NULL;
    l->count = 0;
    l->cap = 0;
    l->ignored = 0;
    l->excluded = NULL;
    l->nexcluded = 0;
    arena_init(&l->arena);
}

static struct hd_entry *list_push(struct hd_list *l, const char *path, char type)
{
    struct hd_entry *e;

    if (l->count == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 1024;
        l->items = xrealloc(l->items, l->cap * sizeof(*l->items));
    }
    e = &l->items[l->count++];
    e->path = path;
    e->type = type;
    e->err = 0;
    e->size = 0;
    e->mtime = 0;
    e->ino = 0;
    return e;
}

static void add_error(struct hd_list *l, const char *path, int err)
{
    list_push(l, path, ENT_ERROR)->err = err;
}

static int cmp_dirent_ino(const void *a, const void *b)
{
    ino_t x = ((const struct os_dirent *)a)->ino, y = ((const struct os_dirent *)b)->ino;

    return x < y ? -1 : x > y;
}

static const struct hd_exclude *find_exclude(const struct walk_opts *wo,
                                             const struct os_stat *st)
{
    size_t i;

    for (i = 0; i < wo->nexcludes; i++)
        if (wo->excludes[i].dev == st->dev && wo->excludes[i].ino == st->ino)
            return &wo->excludes[i];
    return NULL;
}

/* Relative path of a child: "name" at the root, "dir/name" below it; stored in the arena. */
static const char *child_path(struct hd_list *l, const char *dir, const char *name)
{
    size_t dlen = strlen(dir), nlen = strlen(name);
    struct hd_buf b;
    const char *p;

    if (dlen == 0)
        return arena_strndup(&l->arena, name, nlen);
    buf_init(&b);
    buf_append(&b, dir, dlen);
    buf_append_char(&b, '/');
    buf_append(&b, name, nlen);
    p = arena_strndup(&l->arena, b.data, b.len);
    buf_free(&b);
    return p;
}

/* Reads one directory; its subdirectories are pushed on the stack of pending ones. */
static void walk_dir(const char *root, const char *rel, const struct walk_opts *wo,
                     dev_t root_dev, struct hd_list *l, const char ***stack, size_t *depth,
                     size_t *stack_cap)
{
    char *dirpath = rel[0] != '\0' ? hd_path_join(root, rel) : xstrdup(root);
    struct os_dirent *ents;
    size_t n, i;

    if (os_list_dir(dirpath, &ents, &n) != 0) {
        int err = errno;

        if (rel[0] == '\0')
            hd_die("cannot read %s directory '%s': %s", wo->side, root, strerror(err));
        add_error(l, rel, err);
        free(dirpath);
        return;
    }
    /* readdir order may be hash order (ext4 dir_index); inode order keeps the lstat
     * calls close together in the inode table. */
    if (n > 1)
        qsort(ents, n, sizeof(*ents), cmp_dirent_ino);
    for (i = 0; i < n; i++) {
        const char *crel = child_path(l, rel, ents[i].name);
        char *cpath = hd_path_join(dirpath, ents[i].name);
        const struct hd_exclude *ex;
        struct os_stat st;
        struct hd_entry *e;

        if (os_lstat(cpath, &st) != 0) {
            add_error(l, crel, errno);
            free(cpath);
            continue;
        }
        free(cpath);
        switch (st.kind) {
        case OS_REG:
        case OS_LNK:
            e = list_push(l, crel, st.kind == OS_REG ? ENT_FILE : ENT_LINK);
            e->size = st.size;
            e->mtime = st.mtime;
            e->ino = st.ino;
            break;
        case OS_DIR:
            ex = find_exclude(wo, &st);
            if (ex != NULL) {
                hd_warn("%s: skipping '%s' (%s)", wo->side, crel, ex->what);
                l->excluded = xrealloc(l->excluded,
                                       (l->nexcluded + 1) * sizeof(*l->excluded));
                l->excluded[l->nexcluded++] = crel;
            } else if (!wo->one_fs || st.dev == root_dev) {
                if (*depth == *stack_cap) {
                    *stack_cap = *stack_cap ? *stack_cap * 2 : 64;
                    *stack = xrealloc(*stack, *stack_cap * sizeof(**stack));
                }
                (*stack)[(*depth)++] = crel;
            }
            break;
        default:
            l->ignored++;
            break;
        }
    }
    os_free_dir(ents, n);
    free(dirpath);
}

void walk_tree(const char *root, const struct walk_opts *wo, struct hd_list *l)
{
    const char **stack = NULL;
    size_t depth = 0, stack_cap = 0;
    struct os_stat st;

    list_init(l);
    if (os_stat(root, &st) != 0)
        hd_die("cannot access %s '%s': %s", wo->side, root, strerror(errno));
    walk_dir(root, "", wo, st.dev, l, &stack, &depth, &stack_cap);
    /* A caught signal stops the traversal between directories; the caller checks it. */
    while (depth > 0 && !os_caught_signal()) {
        const char *rel = stack[--depth];

        walk_dir(root, rel, wo, st.dev, l, &stack, &depth, &stack_cap);
    }
    free(stack);
}

static int cmp_entry_path(const void *a, const void *b)
{
    return strcmp(((const struct hd_entry *)a)->path, ((const struct hd_entry *)b)->path);
}

/* Canonical order: strcmp on the raw bytes (strcmp compares as unsigned char). */
void list_filter(struct hd_list *l, const struct hd_set *keep)
{
    size_t i, n = 0;

    for (i = 0; i < l->count; i++)
        if (set_has(keep, l->items[i].path))
            l->items[n++] = l->items[i];
    l->count = n;
}

void walk_file(const char *root, const char *name, struct hd_list *list)
{
    struct hd_entry *e;
    struct os_stat st;
    char *full;

    list_init(list);
    full = hd_path_join(root, name);
    e = list_push(list, arena_strndup(&list->arena, name, strlen(name)), ENT_FILE);
    if (os_stat(full, &st) != 0) {
        e->type = ENT_ERROR;
        e->err = errno;
    } else {
        e->size = st.size;
        e->mtime = st.mtime;
        e->ino = st.ino;
    }
    free(full);
}

void list_sort(struct hd_list *l)
{
    if (l->count > 1)
        qsort(l->items, l->count, sizeof(*l->items), cmp_entry_path);
}

void list_free(struct hd_list *l)
{
    free(l->items);
    free(l->excluded);
    arena_free(&l->arena);
    list_init(l);
}

void tree_write(const char *results, const char *side, const char *abs_root,
                const struct hd_list *l)
{
    struct hd_outfile out;
    struct hd_buf b;
    char name[64], num[HD_OFF_DEC_LEN];
    size_t i;

    sprintf(name, "tree-%s.txt", side);
    outfile_open(&out, results, name);
    buf_init(&b);
    buf_append_str(&b, "# hashdiff-tree: 1\n# root: ");
    hd_escape(&b, abs_root);
    buf_append_char(&b, '\n');
    for (i = 0; i < l->nexcluded; i++) {
        buf_append_str(&b, "# excluded: ");
        hd_escape(&b, l->excluded[i]);
        buf_append_char(&b, '\n');
    }
    outfile_write(&out, b.data, b.len);
    for (i = 0; i < l->count; i++) {
        const struct hd_entry *e = &l->items[i];

        buf_clear(&b);
        buf_append_char(&b, e->type);
        buf_append_char(&b, ' ');
        if (e->type == ENT_ERROR) {
            buf_append_str(&b, hd_off_to_dec((off_t)e->err, num));
            buf_append_str(&b, " -");
        } else {
            buf_append_str(&b, hd_off_to_dec(e->size, num));
            buf_append_char(&b, ' ');
            buf_append_str(&b, hd_off_to_dec(e->mtime, num));
        }
        buf_append_char(&b, ' ');
        hd_escape(&b, e->path);
        buf_append_char(&b, '\n');
        outfile_write(&out, b.data, b.len);
    }
    buf_free(&b);
    outfile_commit(&out);
}
