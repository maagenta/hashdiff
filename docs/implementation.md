# hashdiff: implementation specification

Implement `hashdiff`, a CLI that computes, in parallel, the content hash of two directory trees,
compares them, and generates the list of files to re-sync with rsync. Work in phases
(section 12). If anything in this specification is ambiguous or contradictory, ask before
implementing. Do not add functionality not described here.

## 1. Language and platform

- Strict ISO C89/C90. Must compile with zero warnings under both GCC and Clang using:
  `-std=c89 -pedantic-errors -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes`
- Zero third-party dependencies. The only external interface is the system libc (C89) +
  POSIX/SUSv3: opendir/readdir/closedir, lstat/stat/fstat, readlink,
  open/read/pread/write/close, mkdir, rename, unlink, pipe, fork/waitpid/_exit, kill,
  sigaction, fcntl, getcwd, isatty, time, localtime, strftime, posix_fadvise (under #ifdef).
  `stat` only for the roots, which are followed (sections 2.2 and 2.3); `fcntl` only for the
  lock of section 3.6, with F_SETLK and F_GETLK; `localtime` and `strftime`, both C89, only
  for the timestamp lines of section 8.
- Feature-test macros only in the Makefile, identical across all translation units:
  `-D_XOPEN_SOURCE=600 -D_FILE_OFFSET_BITS=64`. `src/config.h` is included first in every
  .c file, emits `#error` if `_FILE_OFFSET_BITS != 64`, and contains a C89 static assert:
  `typedef char hd_off_t_is_64bit[sizeof(off_t) >= 8 ? 1 : -1];`
- Targets: Linux (glibc and musl), macOS and FreeBSD, including 32-bit platforms with files
  > 4 GiB. Windows is out of scope; encapsulate OS calls in `src/os.c` so a Win32 backend can
  be added later.
- Forbidden (not C89 or triggers a diagnostic under -pedantic): `//` comments; declarations
  after statements; `for (int i ...)`; `long long`, `%lld`, `%zu`; `<stdint.h>`,
  `<stdbool.h>`, `<inttypes.h>`; `inline`, `restrict`; `snprintf`/`vsnprintf`; VLAs;
  designated initializers and compound literals; non-constant initializers in automatic
  structs or arrays; trailing comma in `enum`; variadic macros; `__func__`; string literals
  longer than 509 characters (the `--help` text goes in an array of lines); `strdup`,
  `getline` and `getopt_long` (implement your own). `main` ends with an explicit `return`.
- Sizes, offsets and I/O costs always in `off_t`, never in `long` or floating point (the
  sampling plan decisions must be bit-for-bit reproducible on any machine). off_t <-> decimal
  conversion with your own functions. Print `size_t` with `%lu` and a cast to
  `unsigned long`.
- Code, identifiers, comments and CLI messages in English.

## 2. CLI

    hashdiff ORIGIN DESTINATION [DESTINATION ...] [OPTIONS]

    -o, --output DIR        Existing directory where DIR/results.hashdiff/ is created (default: .)
        --file              ORIGIN and the destinations are files, not directories (2.3)
        --resume            If results.hashdiff holds an interrupted run, resume it without
                            asking (section 3.3)
        --force             If results.hashdiff exists, discard it and start over without asking
        --recheck           If results.hashdiff holds a finished run with differences, archive
                            it and read only the paths that differed (section 3.5)
        --ignore-lock       Do not refuse when another run holds the lock (section 3.6)
    -f, --fast              Sampled fast mode (section 6). Without it: full MD5 of everything
    -g, --gap SIZE          Maximum unread region between two samples (default: 64M).
                            Any contiguous damage larger than SIZE is always detected
    -b, --block SIZE        Bytes read per sample (default: depends on --profile)
        --profile hdd|ssd   Disk type (default: ssd). Sets the default --block, the cost
                            model and the read strategy (section 6.1)
    -j, --jobs N            Hashing processes per side, 1..256 (default: 1)
        --serial            Process one side at a time instead of all of them at once
    -x, --one-file-system   Do not cross mount points (st_dev different from the root's)
        --number-of-destinations N
                            Fail unless exactly N destinations were given (section 2.2)
    -q, --quiet             No progress on stderr
    -h, --help
    -V, --version           Print "hashdiff 1.1" and exit 0

- SIZE: integer with optional K, M, G or T suffix (base 1024, case-insensitive), with
  overflow detection. --gap >= 1; --block >= 512. Accepted forms: `--opt VALUE`,
  `--opt=VALUE`, `-g 8M`, `-g8M`, and `--` as end of options. Custom parser.
- Options may appear before, between or after the paths, e.g.
  `hashdiff /data/origin/ /data/destination/ --output /data/reports`. Every argument after
  `--` is a path. At least two paths are required: ORIGIN and one destination.
- --resume, --force and --recheck exclude each other: any two of them together is a fatal
  error.
- --gap, --block and --profile without --fast: warning on stderr that they have no effect.
  Full mode always uses the ssd constants for its cost estimates and never uses the hdd
  read order (section 7).
- -x with --file, and --jobs > 1 with --file: warning on stderr that they have no effect
  (nothing is traversed, and a side has a single entry).
- Exit codes, in decreasing order of precedence: 2 fatal error (including "abort" at the
  prompts of sections 3.3 and 3.6 and trees changed on resume, section 3.4); 4 the tree of at
  least one destination differs from ORIGIN, so that destination was not hashed (section 3.1);
  3 completed, but some side had read errors (E entries); 1 differences found; 0 no
  differences. A run reports the most severe status over every destination, so errors and
  differences together are 3, and one destination whose tree differs while another has hash
  differences is 4; the summary says which destination is in which state. With one
  destination, 4 still means that nothing was hashed. An interruption by SIGINT or SIGTERM
  exits 128 + signal number and takes precedence over all of them.
- Test-only environment variable, not documented in --help: HASHDIFF_MERGE_GAP=SIZE
  overrides the profile's merge_gap (section 6.2).

### 2.1 Paths

Every path that reaches an output file, a message or a comparison is first *cleaned*, so
that the same directory is always written the same way whatever the user typed:

1. if it is not absolute, `getcwd()` and a `/` are prepended;
2. runs of `/` are collapsed into one;
3. `.` components are removed;
4. a `..` component removes the component before it, and a `..` directly under the root is
   dropped (`/..` is `/`);
5. a trailing `/` is removed unless the result is the root `/`.

The result has no `.` or `..` component, no `//` and no trailing slash. ORIGIN, every
destination and the --output directory are cleaned once, right after parsing, and only the
cleaned form is used afterwards: to open and traverse them, in `# root:`, `# origin:` and
`# destination:` headers, in paths.txt (section 3.2), in the `results:` line of the summary,
in every suggested rsync command and in every message that shows a path. What is recorded is
therefore exactly what was read, and `hashdiff o d`, `hashdiff ./o d/` and
`hashdiff "$PWD/o" d` are one run: byte-identical result files, and either can resume the
other.

- `..` is removed lexically, as a shell does without `-P`. When the component before it is a
  symlink to another directory, the cleaned path names a different directory than the one the
  kernel would reach from what was typed, and hashdiff uses the cleaned one. Document it in
  the README: pass an absolute path if there is any doubt.
- `realpath()` is not used and symlinks are never resolved, so a root reached through a
  symlink keeps the name the user gave it.

### 2.2 Destinations

- One ORIGIN and 1 to 64 destinations. Each destination is compared with ORIGIN and never
  with another destination. ORIGIN is traversed and hashed once and its lists are reused for
  every comparison, so one more destination costs one more traversal and one more hash of
  that destination only.
- A *side* is ORIGIN or one destination. Side names are the keys of every output file name,
  summary label, progress tag and paths.txt line:

      destinations    side names
      1               origin, destination
      2 or more       origin, destination-1, destination-2, ... in command-line order

  With one destination every name is the one version 1.1 used, so a single-destination run
  writes exactly the files it wrote before.
- ORIGIN and every destination must be directories (regular files with --file, section 2.3);
  a symlink is followed only for the roots themselves. Every two roots must be different
  directories (st_dev and st_ino): ORIGIN against each destination, and each destination
  against the others. Otherwise a fatal error that names the two roles.
- If a root is inside another, or results.hashdiff is inside a root, that directory is
  excluded from the traversal that contains it, with a warning on stderr (section 3). The
  excluded paths of every side are collected together and all of them go into every suggested
  rsync command, so a `--delete-after` command for one destination can never delete another
  destination nested inside it.
- --number-of-destinations N is only an assertion. It does not change how the command line is
  parsed, because the parser knows which options take a value and the paths are unambiguous
  without it; it is a fatal error, before anything is read, when the number of destinations
  given is not exactly N. It is there for scripts, where a mistyped or glob-expanded path
  would otherwise become one more destination in silence.
- --serial processes one side at a time, in command-line order, inside each stage
  (section 7). Without it every side runs at once: 1 + D traversals and 1 + D hashing
  processes, each times --jobs. Warning on stderr when (1 + D) * --jobs is greater than 64,
  for the same reason --jobs > 1 on one disk warns: more readers than the disks can serve
  reduce throughput.

### 2.3 Comparing single files (--file)

With --file, ORIGIN and every destination is a regular file instead of a directory: one
large file, a disk image or an archive, verified against its copies.

- A path given as a symlink is followed, as the roots already are, so the file that is read
  is the one reached: `stat`, not `lstat`. Any path that is not a regular file is a fatal
  error, a directory included.
- Nothing is traversed. A side's root is the cleaned absolute path of the file's parent
  directory and its list holds exactly one entry, the file's name, so every format, the
  resume and the diff work with no special case. The entry's type is F or S, never L because
  the symlink was followed, or E when the file cannot be opened or read.
- The two files may have different names, so the single entries are compared by position and
  not by name. Only HASH, SIZE, ERR-SRC and ERR-DST can appear: TYPE cannot, both sides are
  regular files, and MISSING and EXTRA cannot, each side has exactly one entry.
  diff-files.txt records the ORIGIN entry's name, and each hashes file its own.
- rsync-files.lst is not written: a list holding ORIGIN's name would copy it into the
  destination's directory, which is wrong as soon as the names differ. The command of
  section 9 is the direct form instead, with the two cleaned paths:

      rsync -a -I '/data/disk.img' '/mnt/backup/disk-copy.img'

  The suggested command of the tree stage is the same line without `-I`.
- Every two files must be different files (st_dev and st_ino); their parent directories may
  be the same one, so `hashdiff --file a.iso b.iso` is a valid run.
- Nothing is ever excluded, because nothing is traversed, and results.hashdiff may sit in the
  same directory as the files.
- Resume has one entry of granularity and the last kept entry is always hashed again, so an
  interrupted --file run reads the whole file again. Document it in the README.
- Messages, replacing `hashdiff: ORIGIN 'X' is not a directory`:

      hashdiff: ORIGIN 'X' is a file, not a directory; use --file to compare files
      hashdiff: ORIGIN 'X' is a directory, not a file; drop --file

  The first appears only when the path is a regular file or a symlink to one; a FIFO, a
  socket or a device keeps `is not a directory`, because --file would not help. The role in
  the message is ORIGIN, or DESTINATION with one destination and DESTINATION-N with several.

## 3. Results directory

`DIR/results.hashdiff/` is created with `mkdir(path, 0777)` (umask applies); EEXIST is not an
error, it is the case of section 3.3. File names are fixed; --output only chooses DIR. A run
has two stages: the tree stage (section 3.1) always runs; the hash stage (sections 5-9) runs
for every destination whose tree matches ORIGIN.

The order of what the parent does to this directory matters and is fixed: create it or find
it, take the lock (section 3.6), decide what to do with what is already there (sections 3.3
and 3.5), write paths.txt and history.txt (section 3.2), and only then fork (section 7). The
lock comes before the decision because the decision itself writes: two runs starting at the
same moment on one directory would otherwise both conclude that there is nothing to resume and
both clean it.

SIDE below is a side name of section 2.2 (`origin`, `destination` or `destination-N`) and
DEST is the name of a destination. The `-DEST` part is present only when there are several
destinations: with one, the four files of the comparison are `tree-diff.txt`,
`diff-files.txt`, `rsync-files.lst` and `rsync-command.txt`.

    file                       written by                content
    paths.txt                  tree stage                roots of the run (section 3.2)
    history.txt                tree stage                one block per run, ever (section 3.2)
    lock                       start of the run          empty; the lock of section 3.6
    tree-SIDE.txt              tree stage                list of that side (section 3.1)
    tree-diff-DEST.txt         tree stage                ORIGIN vs DEST (section 3.1)
    hashes-SIDE.txt            hash stage                hash list of that side (section 8)
    diff-files-DEST.txt        hash stage                content differences (section 9)
    rsync-files-DEST.lst       hash stage                paths to copy, each ended by `\0`
    rsync-command-DEST.txt     hash stage                rsync command that copies them
    tree-changes.txt           resume, only on change    saved trees vs current (section 3.4)
    scan-YYYYMMDDHHMM/         recheck                   the archived run (section 3.5)

- The tree stage writes no command files; it only prints one suggested rsync command per
  destination whose tree differs (section 3.1).
- `rsync-command-DEST.txt`: the single-line rsync command of section 9 that copies exactly
  the paths in that destination's rsync-files list; empty if there is nothing to copy. The
  paths are not embedded in the command: a list of thousands of paths would exceed the
  system's argument length limit (ARG_MAX), and names with newlines, spaces or non-UTF-8
  bytes are only safe in the `\0`-terminated list. With --file there is no list and the
  command is the direct form of section 2.3.
- hashdiff never runs any rsync command; the user reviews and runs it.

Each file is written as `NAME.tmp` and published with `rename()` when complete, so a partial
result never looks complete. Check the return value of fwrite, fflush and fclose: deferred
write errors (ENOSPC, EIO, NFS) can surface on close. `hashes-*.txt.tmp` are journals that
are written progressively and kept on interruption so the run can be resumed (section 3.4);
every other `.tmp` file is deleted on interruption.

What happens when `DIR/results.hashdiff` already exists is section 3.3.

If results.hashdiff lies inside a root, or one root is inside another, those directories are
excluded from the traversal by comparing st_dev/st_ino, with a warning on stderr
(section 2.2).

### 3.1 Tree stage

Before anything is hashed, each child traverses its tree (section 4), sorts it, writes
`tree-SIDE.txt` (SIDE is its side name, section 2.2) and waits for the parent (section 7).
No file content is read in this stage. With --file nothing is traversed and the list is the
single entry of section 2.3, but the file is written and compared in the same way. paths.txt
(section 3.2) is already there: the parent writes it before forking (section 7).

    # hashdiff-tree: 1
    # root: /abs/path/origin
    # excluded: backups/results.hashdiff
    F 1048576 1728212345 docs/report.pdf
    L 11 1728212345 lib/libfoo.so
    E 13 - private

- Header: `# hashdiff-tree: 1`, `# root:` (escaped, cleaned, absolute) and one
  `# excluded:` line (escaped path relative to the root) per directory excluded from the
  traversal (section 2.2), used for the `--exclude` options of the suggested command.
- Fields separated by a single space: type (F = regular file, L = symlink, E = error);
  size (st_size from lstat, or from stat with --file; for L, the length of the link target;
  for E, the errno in decimal);
  mtime (st_mtime in decimal seconds, possibly negative, converted through off_t with the
  util.c functions; `-` for E); and the path, escaped as in section 8. Canonical order.
- The parent merge-joins `tree-origin.txt` with the tree file of each destination in turn,
  re-opening `tree-origin.txt` for every one of them: one independent comparison per
  destination, streaming, constant memory. With --file the single entry of each side is
  compared by position instead of by path (section 2.3). First matching rule wins:
  1. E in ORIGIN → ERR-SRC
  2. Only in ORIGIN → MISSING
  3. Only in DESTINATION → EXTRA
  4. E in DESTINATION → ERR-DST
  5. F vs L → TYPE
  6. Different size → SIZE
  7. Otherwise the entries match. mtime is not compared: a copy may not keep it.
- `tree-diff-DEST.txt` is written for every destination, always; it has no entries when that
  destination's tree matches ORIGIN:

      # origin: /abs/path/origin
      # destination: /abs/path/destination-2
      ## origin
      MISSING escaped-path
      SIZE escaped-path
      ## destination
      EXTRA escaped-path

  The `## origin` section holds MISSING, SIZE, TYPE and ERR-SRC (what ORIGIN has that the
  destination lacks or has differently, and read errors in ORIGIN); the `## destination`
  section holds EXTRA and ERR-DST. The section names never carry the destination's number:
  the file name already says which destination it is. Each section is in canonical order
  (two streaming passes).
- Every destination whose tree-diff has no entries goes on to the hash stage: the parent
  tells those children to continue and tells every other destination to exit. ORIGIN
  continues when at least one destination is left and exits when none is.
- For every destination whose tree differs, the parent prints on stdout its count per status
  and a suggested general synchronization command. They take their place in the summary of
  section 9, which is printed whether anything was hashed or not. The command is chosen as
  follows:
  - EXTRA present (with or without other statuses):
    `rsync -a --delete-after EXCLUDES '<ORIGIN abs>/' '<DESTINATION abs>/'`
  - otherwise, MISSING, SIZE or TYPE present:
    `rsync -a EXCLUDES '<ORIGIN abs>/' '<DESTINATION abs>/'`
  - only ERR-SRC / ERR-DST: no command.

  With one destination the lines on stdout are the ones of version 1.1:

      tree differences: 1 MISSING, 0 EXTRA, 0 SIZE, 0 TYPE, 0 ERR-SRC, 0 ERR-DST
      suggested command (review it first):
      rsync -a '/abs/path/origin/' '/abs/path/destination/'

  With several, every line names its destination, the counts of all of them come first and
  the commands after, both in command-line order:

      tree differences with destination-2: 1 MISSING, 0 EXTRA, 0 SIZE, 0 TYPE, 0 ERR-SRC, 0 ERR-DST
      suggested command for destination-2 (review it first):
      rsync -a '/abs/path/origin/' '/abs/path/destination-2/'

  `(review it first...)` gains `; --delete-after deletes the files that exist only in
  DESTINATION` when the command carries `--delete-after`. With --file the command is the
  direct form of section 2.3.

  The commands are printed, not written to a file. `-x` is added if hashdiff ran with -x.
  EXCLUDES is one `--exclude='/<relative path>/'` for every directory excluded from any
  traversal of the run (results.hashdiff and any nested root, section 2.2), so
  `--delete-after` can never delete results.hashdiff, a nested destination or a nested
  ORIGIN. In the pattern, `*`, `?`, `[` and `\` are escaped with `\`. Everything is quoted
  with POSIX quoting (`'` → `'\''`).

  On stderr, with one destination, exactly the messages of version 1.1:

      hashdiff: ORIGIN and DESTINATION trees differ; nothing was hashed. See tree-diff.txt,
      fix the differences (for example with the suggested rsync command) and run hashdiff
      again.
      hashdiff: N paths could not be read (see ERR-SRC / ERR-DST in tree-diff.txt);
      fix their permissions and run hashdiff again.
      hashdiff: warning: N files exist only in DESTINATION; the suggested command uses
      --delete-after and will delete them. Review tree-diff.txt before running it.

  The second appears when that destination has any ERR-SRC or ERR-DST, alone or with other
  statuses, and the third when it has EXTRA paths. A count of 1 puts the whole sentence in the
  singular, verb and pronoun included: `1 file exists only in DESTINATION; the suggested
  command uses --delete-after and will delete it.`, and `1 path could not be read`. With several destinations each message is
  printed once per destination concerned, naming it and its own tree-diff file, and the
  first becomes `hashdiff: the trees of ORIGIN and destination-2 differ; destination-2 was
  not hashed. See tree-diff-destination-2.txt, fix the differences (for example with the
  suggested rsync command) and run hashdiff again.`

  Exit 4; with several destinations it means that at least one of them was not hashed
  (section 2). Unlike the command of section 9, the suggested one has no `-I`: rsync's quick
  check is enough to copy missing files and files of a different size, and `-I` would
  rewrite the whole tree.

### 3.2 paths.txt

The parent writes it before forking the children (section 7), so that a run interrupted at any
point leaves it behind:

    # hashdiff-paths: 1
    # started: 16/12/2026 12:30
    origin /abs/path/origin
    destination-1 /abs/path/destination-1
    destination-2 /abs/path/destination-2

- `# started:` is the start time of the run, in the format of section 8. It is the one place
  that always holds it: a run that stops in the tree stage has no hashes file. Section 3.3
  reads it and section 3.5 names its archive after it.
- With --file, one more header line, `# file: 1`, so a run started with --file and one
  started without it are never taken for the same run.
- With --recheck, one more header line, `# recheck: scan-YYYYMMDDHHMM`, naming the archive
  whose differences this run rechecks (section 3.5).
- One line per side: its side name (section 2.2), one space, and its cleaned absolute root,
  escaped as in section 8. ORIGIN first, then the destinations in command-line order. Label
  first and path last, as in every other format of the project: a path may contain a newline,
  which a layout of one line per label and one per path could not hold.
- It is written once per run and never rewritten; it records what this run compares. A resume
  keeps the paths.txt of the run it continues, and the `# resumed:` lines go in the hashes
  files (section 8).

`history.txt` is the same thing kept for good. Immediately before writing paths.txt, the
parent appends to it the block it is about to write, headers included, followed by one empty
line:

    # hashdiff-history: 1
    # started: 16/12/2026 12:30
    origin /abs/path/origin
    destination-1 /abs/path/destination-1

    # started: 02/01/2027 08:15
    origin /abs/path/another-origin
    destination-1 /abs/path/another-destination

- The first line is written only when the file is created. Blocks are appended, so the file is
  in chronological order, oldest first, which is the order a plain `fopen(path, "ab")` gives
  and the only one that needs no rewriting: a "newest first" file would have to be read and
  written whole on every run, and this one grows for the life of the results directory.
- It is the only file that nothing ever deletes: section 3.3 does not clean it and section 3.5
  does not move it into the archive, because it belongs to the directory and not to one run.
  Every other record of a previous run is either replaced or archived; this is what is left
  after an --force, and what answers "what has this directory been used for".
- A run that fails before the tree stage (bad arguments, a root that cannot be read, a refused
  prompt) appends nothing: the block is written when the run is about to start reading.

### 3.3 An existing results.hashdiff

`mkdir` returning EEXIST starts this section. What hashdiff does depends on the state of the
previous run, which is read from the directory itself, with no extra bookkeeping. A
destination *was hashed* when its `tree-diff` file exists and holds no record line; that is
also how a resume knows which destinations to continue (section 3.4).

- No `hashes-*.txt` and no `hashes-*.txt.tmp`, so nothing was hashed and there is nothing to
  decide: clean the directory (below) and start over without asking, even with --resume,
  --force or --recheck. This is the case after an exit 4.
- Otherwise `paths.txt` must exist. A directory without it was written by an older hashdiff:
  fatal error (exit 2) whose message suggests --force.
- paths.txt must describe this run: the same number of destinations, the same cleaned root
  for every side and the same `# file:` marker. Any difference is a fatal error (exit 2) that
  names the first role that differs and suggests --force. It is never a question: lists made
  from another tree are not comparable, so resuming is not one of the possible answers.
- Then the state is one of three:

      state  recognised by                                      choices
      1      some `hashes-SIDE.txt.tmp` exists, or a side that   resume, overwrite, abort
             was to be hashed has no journal at all
      2      every such side published and no diff file holds    overwrite, abort
             a record
      3      every such side published and some diff file        recheck, overwrite, abort
             holds a record

  "Every such side" is ORIGIN and every destination that was hashed. A journal is published
  as `hashes-SIDE.txt` only when that side finished (section 3), which is what tells state 1
  from the others; the `# finished:` line of section 8 is for the reader, never the test.

Flags, which never ask anything:

- --resume: state 1 resumes (section 3.4). In states 2 and 3 it is a fatal error, because the
  previous run finished and there is nothing to resume; the message suggests --force, and
  --recheck as well in state 3.
- --force: any state. Clean the directory and start over.
- --recheck: state 3 (section 3.5). In states 1 and 2 it is a fatal error suggesting --resume
  or --force.

Without any of them, if stdin and stderr are both terminals (isatty), ask on stderr (also
with -q). DD/MM/YYYY HH:MM is the `# started:` of paths.txt:

    results.hashdiff in DIR holds an interrupted run started on DD/MM/YYYY HH:MM.
    [r]esume, [o]verwrite or [a]bort?

    results.hashdiff in DIR holds a finished run started on DD/MM/YYYY HH:MM with no
    content differences.
    [o]verwrite or [a]bort?

    results.hashdiff in DIR holds a finished run started on DD/MM/YYYY HH:MM whose
    differences were never copied (see rsync-command.txt).
    [c]heck those paths again, [o]verwrite or [a]bort?

In state 2, when some destination had stopped at the tree stage, the first line ends with
`with no content differences and N destinations whose trees differ`; there is nothing to
recheck for those, since nothing was hashed for them.

Read one line from stdin with read() on fd 0. `r` resumes, `o` is --force, `c` is --recheck,
`a` exits 2 without touching anything; the long forms `resume`, `overwrite`, `recheck` and
`abort` are accepted, in any case; a letter this state does not offer, or any other answer,
repeats the question; EOF exits 2.

Without any of them and without a terminal (scripts, CI, tests): fatal error (exit 2) whose
message names the flags this state accepts.

Cleaning the directory removes every file a run may have left and nothing else. The names
depend on the number of destinations, so a fixed list cannot do it: read the directory with
opendir/readdir and unlink every entry that is a regular file whose name is `paths.txt`,
`tree-changes.txt`, `tree-origin.txt` or `hashes-origin.txt`, or begins with
`tree-destination`, `hashes-destination`, `tree-diff`, `diff-files`, `rsync-files` or
`rsync-command`, in every case with or without a `.tmp`, `.new` or `.part` suffix. The
matching is strncmp and strcmp on prefixes and suffixes, not a glob library. Everything else
is left untouched, which includes `history.txt` (section 3.2) and `lock` (section 3.6), which
are never deleted, and the `scan-*` archives of section 3.5: --force never deletes them and
nothing in hashdiff ever prunes them (say so in the README). A directory left by a run with
more destinations than this one therefore keeps none of its result files, which is
the point: a stale `hashes-destination-3.txt` would look like part of the new result.

### 3.4 Resuming an interrupted run

While hashing, each side writes `hashes-SIDE.txt.tmp` progressively: the header first and
then complete lines in canonical order (section 7), flushed at least every ~2 s and before
exiting. After an interruption (signal, crash, power loss) it holds a canonical prefix of the
result, possibly followed by an incomplete or damaged tail.

The parent prepares the resume before forking:

1. Source of each side: `hashes-SIDE.txt.tmp` if it exists, otherwise `hashes-SIDE.txt` (the
   side had finished), otherwise none. Stale `hashes-SIDE.txt.new` files are deleted.
2. The tree file of every side must exist with a valid header and the current root;
   otherwise fatal error (exit 2) suggesting --force.
3. Header of each source: the first three lines must be exactly `# hashdiff-format: 3`,
   `# root:` with the current (escaped, cleaned, absolute) root of that side, and `# mode:`
   with the current mode line. A different root or mode is a fatal error (exit 2): lists from
   another tree or made with other parameters are not comparable. The message suggests
   --force. A header that is missing or cut short counts as no source. The `#` lines that may
   follow those three (`# started:`, `# resumed:`, section 8) are read and kept: they are
   copied into the new journal, so one file records the whole history of the run.
4. Valid prefix: read the lines after the header and stop at the first one that is not
   valid: no terminating LF (cut by the interruption); not exactly four fields; type not one
   of F/S/L/E; hash not 32 lowercase hex digits (decimal errno for E); size not decimal (`-`
   for E); a path whose escaping is invalid (`\` followed by anything other than `\`, `n`
   or `r`); or a path that is not strictly greater than the previous one. That line and
   everything after it are discarded. A line starting with `#` is one of those invalid lines
   and stops the reading in the same way, which is how the `# finished:` footer of a complete
   journal (section 8) is handled: it is the normal end of the records, not damage, and
   nothing is reported about it. Each side is cut independently at its own last valid line, so
   a side that was further ahead (for example, the finished ORIGIN of a --serial run) loses
   nothing: the sides are never compared until every list is complete, so cutting one side
   down to the length of another would throw away work for nothing.
5. The previous outputs (every `diff-files`, `rsync-files` and `rsync-command` file,
   `tree-changes.txt` and their `.tmp` files) are deleted; they are produced again at the end.
   paths.txt and the tree files are kept.
6. Which destinations continue: the decision of the tree stage is read back from the
   `tree-diff` files, which are still valid because every tree is verified unchanged below,
   and they are not rewritten. A destination whose tree-diff holds records stops again and its
   lines of section 3.1 are printed again in the summary. The origin-vs-destination comparison
   is not run again.
7. When paths.txt holds a `# recheck:` line, the archived diff files it names are read again
   and the recheck sets of section 3.5 are rebuilt, so a resumed recheck reads exactly the
   same paths as the run it continues.

Tree check: each child traverses and sorts its tree again, keeps the list in memory (it does
not rewrite tree-SIDE.txt) and compares it with the saved tree-SIDE.txt on every field,
including mtime. It writes its differences to `tree-changes.txt.SIDE.part`: ADDED (only in the
current tree), DELETED (only in the saved one) or CHANGED (type, size or mtime differ),
in canonical order, and tells the parent whether its tree matches (section 7). If the tree of
any side changed, the parent tells every child to exit, builds `tree-changes.txt` with one
`#` line and one `##` section per side, named by its side name and in command-line order:

    # origin: /abs/path/origin
    # destination-1: /abs/path/destination-1
    # destination-2: /abs/path/destination-2
    ## origin
    DELETED escaped-path
    CHANGED escaped-path
    ## destination-1
    ADDED escaped-path
    ## destination-2

A changed tree stops the whole resumed run, not only that side: a resume is the continuation
of one run, and restoring the tree or --force are the two ways out. With one destination the
names are `# destination:` and `## destination`, as in version 1.1. The parent then
deletes the `.part` files, prints on stderr

    hashdiff: the trees changed since the interrupted run; see tree-changes.txt.
    Restore them to resume, or use --force to start over.

and exits 2 without touching the journals or the tree files.

If every tree matches, the hash stage continues on each side that has work left:

- The last kept line is always hashed again. If the new line is identical to the old one,
  the run continues from there. Otherwise the new line replaces the old one and a warning is
  printed: `[origin] resume: last entry changed, re-hashed: PATH`.
- Every other kept line is reused without reading the file.
- The remaining entries are hashed normally.

The child writes the new journal to `hashes-SIDE.txt.new`: the header and the reused lines,
flushed, then renamed over `hashes-SIDE.txt.tmp` (unlinking `hashes-SIDE.txt` if that was the
source); it then continues appending through the same stream. So there is always a complete
journal to resume from, even if the resumed run is itself interrupted.

Limitation (document it in the README): resume reuses the lines of files whose type, size
and mtime (in seconds) did not change. A file modified within the same second while keeping
its size, or whose mtime was restored, is not read again. For a result that reflects the
current content of every file, use --force.

### 3.5 Rechecking a finished run (--recheck)

Only in state 3 of section 3.3: the previous run finished and found content differences. A
recheck answers one question, "did the copy that the previous run asked for work?", and the
copy that was asked for is the rsync command of each destination, which copied that
destination's own paths and nothing else. That is why every destination is compared only
against its own paths, in both comparisons: a path that was in another destination's command
was never part of this destination's question. The other question, "is there anything else
wrong in this destination", is a normal run. Reading both trees in full again to verify a
handful of files is waste.

1. Archive. Create `results.hashdiff/scan-YYYYMMDDHHMM/`, the stamp being the `# started:` of
   the previous run's paths.txt formatted as `%Y%m%d%H%M`, which sorts chronologically, unlike
   the dd/mm/yyyy of section 8. If that name exists, append `-2`, `-3`, ... until one is free.
   Move into it, with `rename()`, every file that section 3.3 would clean, which leaves
   `history.txt` and `lock` where they are (sections 3.2 and 3.6); `rename()` inside
   one directory is atomic and free, while a copy is neither: a hashes file holds about sixty
   bytes per entry, so a few million files make hundreds of megabytes. Existing `scan-*`
   directories are never moved.
2. Recheck set, built by the parent before forking, so the children inherit it. Read every
   `diff-files` file of the archive. Its first line and its `# mode:`
   line must be the current ones; a different mode line is a fatal error (exit 2) suggesting
   --force, because hashes made with other parameters are not comparable. The recheck set of a
   destination is the paths of its records whose status is HASH, SIZE, TYPE, MISSING, ERR-SRC
   or ERR-DST. EXTRA is skipped: that path is not in ORIGIN. ORIGIN's set is the union of all
   of them.
3. The run then proceeds exactly as a normal one, with one difference: after a side has
   traversed and sorted its tree (section 4), every entry whose path is not in that side's
   recheck set is dropped from the list. The traversal itself is complete, so a path that
   disappeared is still seen; nothing outside the set is read, hashed, counted or compared.
   ORIGIN is hashed for the union, so one pass serves every destination, but both comparisons
   of a destination, the one of section 3.1 and the diff of section 9, consider only the paths
   of that destination's own set: a path rechecked for one destination and not for another
   would otherwise be reported as MISSING.
4. A path of a recheck set that is no longer in any tree is counted and reported once on
   stderr: `hashdiff: warning: N rechecked paths no longer exist in any tree`. It is not a
   difference: it was one between two trees and neither holds it now.
5. paths.txt is written again for the new run, with the same roots, a new `# started:` and a
   `# recheck: scan-YYYYMMDDHHMM` line naming the archive the sets came from. That line is
   what makes an interrupted recheck resumable (section 3.4).
6. A recheck is not a comparison of the trees. Only the paths that already differed are read,
   so a file that became different anywhere else is not seen and MISSING or EXTRA elsewhere
   cannot be found. The summary says so on the line after `results:`:

       recheck: only the N paths that differed in the run of 16/12/2026 12:30 were read

   and the same line, prefixed with `# `, goes into the footer of every diff file
   (section 9). Exit 0 means that those paths match now and never that the trees match; say
   it in the README next to the exit codes.
7. The archives are never pruned and --force keeps them (section 3.3).

### 3.6 One run at a time in one results.hashdiff

Two runs writing one results.hashdiff append to the same journal, and what is left is a file
that is not a canonical prefix of anything and that the resume of section 3.4 then trusts and
reuses. An advisory lock prevents it, in POSIX and nothing else.

- `DIR/results.hashdiff/lock`, opened once with `open(O_RDWR | O_CREAT, 0666)` right after the
  directory exists and before anything reads or writes in it (section 3), and kept open until
  the process ends.
- The lock is taken with `fcntl(F_SETLK)` and a `struct flock` of `l_type = F_WRLCK`,
  `l_whence = SEEK_SET`, `l_start = 0`, `l_len = 0`, so it covers the whole file now and
  however it grows. Retry on EINTR.
- The kernel releases it when the process ends, through any exit path: a normal return, a
  `hd_die`, a signal, a `kill -9` or a power loss. There is nothing to clean up and no stale
  lock to reason about, which is the whole reason for using `fcntl` instead of the presence of
  a file.
- The file stays empty. Writing the pid into it would be a second source of truth that goes
  stale exactly when it matters, while `l_pid` below cannot.
- `lock` is never unlinked, never renamed and never moved: not by the cleaning of section 3.3,
  not by --force, not into a `scan-*` archive by section 3.5. Unlinking it would not release
  anything, but the next run would create a new file, and two runs each holding a lock on a
  different inode exclude nobody.
- EACCES or EAGAIN means another run holds it. `fcntl(F_GETLK)` with the same structure fills
  it in with `l_pid`, the pid of the holder. If F_GETLK comes back with `l_type == F_UNLCK`
  the holder released it in between: retry F_SETLK exactly once, and carry on if it succeeds.
  Otherwise, if stdin and stderr are both terminals (isatty), ask on stderr, also with -q:

      results.hashdiff in DIR is in use by process 48213.
      continue anyway? [y]es or [a]bort?

  Read one line from stdin with read() on fd 0, as in section 3.3: `y` or `yes` continues, `a`
  or `abort` exits 2, any other answer repeats the question, EOF exits 2. Over NFS `l_pid` is
  a pid on another machine and means nothing locally, so when the lock file is not on a local
  filesystem the first line is `results.hashdiff in DIR is in use by another run.` without a
  number.
- Without a terminal: fatal error (exit 2) whose message names --ignore-lock.
- --ignore-lock only suppresses that refusal. The run still takes the lock when it is free, so
  a third run still sees one, and the flag is deliberately not --force: --force means "discard
  these results", and the user who wants to discard them is precisely the one who must not do
  it while another run is writing them.
- Any other error from F_SETLK, ENOLCK and EINVAL among them, means that this filesystem does
  not do locking, not that the directory is busy. Warning on stderr and the run continues:
  `hashdiff: warning: cannot lock '<path>': <strerror>; another run on the same
  results.hashdiff would not be noticed.`
- The lock belongs to the parent. The children inherit the descriptor but hold nothing, so a
  child closing it releases nothing, and the parent outlives every child by design. The parent
  must never close that descriptor and must never open the lock file a second time: POSIX
  drops every lock a process holds on a file as soon as that process closes any descriptor to
  it.
- What the lock does not cover, for the README: only runs that take it are seen, so an editor
  holding `diff-files.txt` open, a `cp` copying the directory or a hashdiff older than this
  version is invisible; and `fcntl` locks over NFS depend on the server's lock manager and may
  be ignored without saying so, so on a network filesystem the lock is not a guarantee. It
  covers results.hashdiff and never ORIGIN or the destinations, where the rule of the README
  stands unchanged: do not run rsync on the trees while hashdiff is running.
- A run that aborts at this prompt may leave behind the empty directory and the empty lock
  file it created a moment earlier. Both are empty and the next run reuses them.

## 4. Traversal

- DFS. To avoid exhausting file descriptors, read all names of a directory into memory, call
  `closedir()`, then process and descend: at most one `DIR *` open per process.
- Before the lstat calls for a directory, sort its entries by `d_ino` (on ext4 with dir_index,
  readdir returns hash order and the lstat calls would jump around the inode table).
- `lstat` on every entry; symlinks are never followed. Regular → F/S; symlink → L;
  directory → descend (unless excluded, or with -x and a different st_dev); FIFO, socket and
  devices → ignored and counted. Directories do not produce entries of their own.
- Path relative to the root with `/` as separator, no leading `./`. Do not rely on PATH_MAX:
  dynamic buffers.
- A failed opendir, lstat or readlink produces an E entry with its errno.
- Canonical order: `qsort` + `strcmp` on the relative path as raw bytes (not strcoll; locale
  independent). It is the order of every output file and of the diff merge. A DFS with each
  directory sorted does NOT produce this order (`a-b` < `a/x` because '-' = 0x2D <
  '/' = 0x2F), so the full list is sorted.
- With --file nothing is traversed: the list is the single entry of section 2.3, built with
  `stat` because the root is followed.
- With --recheck the sorted list is then filtered to that side's recheck set (section 3.5).
  The traversal itself is never shortened, so a path that no longer exists is still seen.

## 5. MD5 and full mode

- Own implementation from the text of RFC 1321; do not copy the RSA reference code (its
  license requires attribution). API: `md5_init`, `md5_update(ctx, const unsigned char *,
  size_t)`, `md5_final(ctx, unsigned char out[16])`.
- 32-bit words in `unsigned long`, masked with `0xFFFFFFFFUL` after every addition and in
  the rotation (on LP64, unsigned long is 64 bits). Constants with the UL suffix. Cast to
  unsigned long before shifting a byte (unsigned char promotes to int).
- Little-endian load and store byte by byte; never cast `unsigned char *` to word pointers
  (alignment, strict aliasing, endianness).
- 64-bit length counter as two 32-bit words with carry.
- Test 1 (tests/test_md5.py through the testhook, section 11): RFC 1321 A.5 test vectors,
  plus the same digest when feeding the data in chunks of 1, 63, 64 and 65 bytes:

      ""                                  d41d8cd98f00b204e9800998ecf8427e
      "a"                                 0cc175b9c0f1b6a831c399e269772661
      "abc"                               900150983cd24fb0d6963f7d28e17f72
      "message digest"                    f96b697d7cb7938d525a2f31aaf161d0
      "abcdefghijklmnopqrstuvwxyz"        c3fcd3d76192e4007dfb496cca67e13b
      A-Z, a-z and 0-9 concatenated       d174ab98d277d9f5a5611c2c9f419d9f
      "1234567890" repeated 8 times       57edf4a22be3c955ac49da2e2107b67a

- Full mode (default): `open(O_RDONLY)`, `fstat`, and a `read` loop with a 1 MiB buffer per
  process, retrying on EINTR and short reads. `posix_fadvise(POSIX_FADV_SEQUENTIAL)` under
  #ifdef (macOS does not have it). The recorded size is the number of bytes actually read.
- Any open or read error produces an E entry with its errno and processing continues with
  the next file; it never aborts the traversal.

## 6. Fast mode (--fast)

### 6.1 Profiles

Fixed constants per profile (not measured, so both sides decide identically):

    profile  default block   seek_bytes (= t_seek × throughput)
    ssd      64K             200000      (0.1 ms × 2 GB/s)
    hdd      1M              1440000     (8 ms × 180 MB/s)

seek_bytes expresses the cost of a seek as "equivalent sequential read bytes", so the whole
cost model is integer arithmetic in off_t.

### 6.2 Sampling plan (pure function of N, gap, block and seek_bytes)

For each regular file, with N = size from fstat after opening, G = gap, B = block,
SB = seek_bytes:

1. If N <= 2*B: full MD5. Type F.
2. k = ceil((N - B) / (G + B)) + 1   (k >= 2; includes the first sample at 0 and the last
   at N - B).
3. If k*B >= N, or k*(SB + B) >= SB + N (sampling is not cheaper than reading the whole
   file): full MD5. Type F. Evaluate the multiplications with overflow checking; if they
   would overflow, the condition is true.
4. Otherwise: k samples of B bytes at evenly spaced offsets:
   offset_0 = 0, offset_{k-1} = N - B, and in between a stride of (N - B) / (k - 1),
   distributing the remainder with a Bresenham-style integer accumulator (q = (N-B)/(k-1),
   r = (N-B)%(k-1); off += q; acc += r; if acc >= k-1 { off += 1; acc -= k-1 }). No i*x
   multiplications, so there is no overflow. By construction, the gap between consecutive
   samples is <= G. Type S.
5. Type S digest = MD5(N as 8 little-endian bytes, followed by the B bytes of each sample in
   offset order).

Read execution (does not change the hash):

- merge_gap = SB (or HASHDIFF_MERGE_GAP if set). Walk the samples in order; if the gap to the
  next one is < merge_gap, read through continuously and discard the intermediate bytes;
  otherwise, pread at the new offset. Reads in chunks of at most 1 MiB using the process
  buffer.
- Before reading, `posix_fadvise(fd, off, len, POSIX_FADV_WILLNEED)` on each span to be read
  (under #ifdef), so the kernel and NCQ can reorder the requests. The rest of the file with
  `POSIX_FADV_RANDOM` to avoid wasting readahead.
- If pread returns EOF earlier than expected (file truncated during the read) → E entry.
- An S hash is not the MD5 of the content and is not comparable with md5sum; document this
  in the README.

### 6.3 Determinism

- The plan depends only on N and on (gap, block, profile), which are fixed for the whole run
  and written to the `# mode:` header. merge_gap and read order do not affect the hash.
- Lists generated with different parameters are not comparable: resuming with a different
  `# mode:` line is a fatal error (section 3.4), and the diff also aborts with a fatal error
  if the `# mode:` lines of both lists differ (internal consistency check).
- For type S files, the summary reports the total number of samples, bytes read and average
  coverage (sample bytes / total bytes).

## 7. Parallelism (fork, no threads)

- The main process validates, creates or finds results.hashdiff, takes its lock
  (section 3.6), handles what is already there (sections 3.3 and 3.5), writes paths.txt and
  history.txt (section 3.2), calls `fflush(NULL)` and uses `fork()` to launch one child per
  side (section 2.2), so 1 + D children: each one writes
  `tree-SIDE.txt` and `hashes-SIDE.txt` and nothing else. Each child gets two `pipe()`s:
  child → parent (tree stage result and final statistics) and parent → child (one byte:
  continue to the hash stage, or exit). The parent waits for the children with `waitpid`,
  retrying on EINTR.
- Sequence: every child finishes the tree stage (section 3.1) or, when resuming, the tree
  check (section 3.4) and reports it; the parent compares ORIGIN with each destination and
  tells each child to continue or to exit, destination by destination (section 3.1), ORIGIN
  continuing if any destination does; then they hash. With --serial, one side at a time inside
  each stage, in command-line order: ORIGIN's tree, each destination's tree, then ORIGIN's
  hashes, then each destination's hashes.
- In the hash stage each child uses the list it already has in memory, reuses the kept
  lines when resuming, hashes, writes its journal and renames it to `hashes-SIDE.txt`. Before exiting it sends its statistics to the
  parent through the pipe as one fixed-size record: entries per type, bytes read, ignored
  entries, and samples, sample bytes and total bytes of S files. These are needed for the
  summary of section 9 and do not appear in the hashes file. It then closes its FILE *
  streams and terminates with `_exit()`, never `exit()` (it would re-flush inherited buffers
  and run atexit handlers). Status: 0 success, 3 with E entries, 2 fatal.
- The child is the only writer of its journal and always writes it in canonical order. A
  result computed out of order is stored at the entry's position in the canonical list and
  written as soon as every previous entry is done (the written prefix advances).
- Chunks: the canonical list is cut into consecutive chunks whose estimated cost (bytes the
  plan will read + SB per file; reused entries cost 0) is about total / (64 × jobs), with at
  least one entry each. Chunks bound the work lost on interruption: results beyond the
  written prefix are lost, and that is at most the chunks in progress.
- Read order: in fast mode with --profile hdd and -j 1, the files of each chunk are read in
  st_ino order (approximates physical on-disk order); chunks are processed in canonical
  order. Otherwise files are read in canonical order.
- -j N > 1: after sorting, the child launches N workers, which inherit the list via
  copy-on-write. Worker k processes chunks k, k + N, k + 2N, ... For each finished entry it
  sends the child a fixed-size result record (entry index, type, errno, size, MD5, bytes
  read, samples) with a single write() on a shared pipe. Records are smaller than PIPE_BUF,
  so writes from different workers never interleave, and paths are never sent because the
  child already has the list. Workers also send a progress record at least every ~2 s while
  reading a long file. The child reads records until EOF (all workers have exited), then
  waits for them. A worker that terminates abnormally is a fatal error for that side.
- With --profile hdd (fast mode), -j > 1 emits a warning (multiple readers on the same disk
  degrade throughput) but is honored.
- Output must be byte-for-byte identical with any -j, with or without --serial, with any
  merge_gap, and between a resumed run and an uninterrupted one when the trees did not
  change. The exceptions are the lines that record wall-clock time, which cannot be identical:
  `# started:`, `# resumed:` and `# finished:` (section 8), the `# started:` of paths.txt and
  the footer of the diff files (section 9). Comparisons, in the specification and in the
  tests, are made with those lines removed, and conftest.py provides the helper that removes
  them.
- SIGINT and SIGTERM via `sigaction`: the handler only writes a `volatile sig_atomic_t`.
  Processes check it between files and between reads. Workers exit. The child forwards the
  signal to its workers with kill, reads the remaining records until EOF, writes the
  completed prefix, flushes and closes its journal (it is kept for --resume) and exits with
  128 + signal number. The parent forwards the signal to its children with kill, deletes its
  own `.tmp` files and exits with 128 + signal number.
- Progress on stderr if isatty(2) and no -q: one line per side every ~2 s (measured with
  time()), emitted by the child with a single write() call so lines do not interleave. The tag
  is the side name of section 2.2. With -j N the child adds up its workers' records:
  `[destination-2] 1532/90211 files, 12.4 GiB`. Sizes are formatted with integer arithmetic.

## 8. Format of the hashes files

    # hashdiff-format: 3
    # root: /abs/path/origin
    # mode: full
    # started: 16/12/2026 12:30
    # resumed: 17/12/2026 09:05
    F d41d8cd98f00b204e9800998ecf8427e 0 docs/empty.txt
    S <32 hex> 10737418240 video/big.mkv
    L <32 hex> 11 lib/libfoo.so
    E 13 - private
    # finished: 17/12/2026 04:30

- Timestamps. Every timestamp hashdiff writes is local time in the form `dd/mm/yyyy HH:MM`,
  from `strftime("%d/%m/%Y %H:%M", localtime(&t))` into a 32-byte buffer. 24-hour on purpose:
  `%I` needs `%p`, which is locale-dependent and which the standard allows to be empty. The
  only other form is the `%Y%m%d%H%M` of the archive names of section 3.5, which has to sort.
- The first three lines are exactly `# hashdiff-format:`, `# root:` and `# mode:`, in that
  order: the resume reads them by position (section 3.4). In fast mode the third is
  `# mode: fast gap=67108864 block=65536 profile=ssd seek_bytes=200000`.
- After them, any number of `#` lines: `# started:`, the moment the parent began the run,
  identical on every side, and one `# resumed:` per resume, in order, carried over from the
  journal being continued (section 3.4).
- `# finished:` is written when the side has hashed everything, just before the journal is
  published with `rename()`, so a published `hashes-SIDE.txt` always carries it and a `.tmp`
  left by an interruption never does. It is there for whoever reads the file: what tells a
  finished side from an interrupted one is which of the two names exists (sections 3 and 3.3).
- Every reader of these files skips the `#` lines before the first record and accepts the ones
  after the last record: the diff (section 9), the resume (section 3.4) and the test helpers.
  A reader that rejects an unknown `#` header, or that counts header lines instead of testing
  them, breaks on the lines above. `# hashdiff-format:` is 3 and not 2 for that reason: a
  results directory written by version 1.1 is refused with a clear message instead of being
  misread.
- Fields separated by a single space: type, hash (32 lowercase hex; for E, the errno in
  decimal), size (decimal; for L, length of the link target; for E, `-`) and path.
- Types: F = regular file with full MD5; S = regular file with sampled hash; L = symlink,
  with hash = MD5 of the target returned by readlink; E = error.
- The path is the last field and may contain spaces. Escaping: `\` → `\\`, LF → `\n`,
  CR → `\r`; all other bytes are written as-is, including non-UTF-8. `# root:` is escaped
  the same way and is the cleaned absolute path of section 2.1.
- Lines follow the canonical order of section 4, computed on the unescaped path.

## 9. Diff: the diff-files, rsync-files and rsync-command files

One comparison per destination that reached the hash stage, in command-line order, each one
writing its own three files (section 3).

- A side that exits with 2 is a fatal error for the run (exit 2 by precedence, section 2), but
  every destination that finished is still compared and reported, so its work is not lost. If
  ORIGIN exits with 2 nothing can be compared and the run stops at once.
- For each destination that exited with 0 or 3, the parent streams `hashes-origin.txt` and
  that destination's list with its own line reader (arbitrary length), re-opening
  `hashes-origin.txt` for every destination, checks that the `# mode:` lines match and that
  paths are in strictly increasing order (otherwise fatal error), unescapes, and performs a
  merge-join with strcmp on the raw paths. Constant memory with respect to the number of
  files. With --file the single entries are compared by position (section 2.3); with --recheck
  only the paths of that destination's recheck set are considered (section 3.5).
- Status per path, first matching rule wins. After a matching tree stage only HASH and the
  ERR statuses are expected; the others appear only if a tree changed during the hash
  stage (for example, a file grew after the tree stage: its fstat size differs):
  1. E in ORIGIN → ERR-SRC
  2. Only in ORIGIN → MISSING
  3. Only in DESTINATION → EXTRA
  4. E in DESTINATION → ERR-DST
  5. Regular (F/S) vs symlink (L) → TYPE
  6. Different size → SIZE
  7. Different hash → HASH
  8. Identical → nothing is written
- The diff-files file of a destination:

      # hashdiff-diff: 1
      # origin: /abs/path/origin
      # destination: /abs/path/destination-2
      # mode: full
      HASH d41d8cd98f00b204e9800998ecf8427e 900150983cd24fb0d6963f7d28e17f72 docs/report.pdf
      MISSING 0cc175b9c0f1b6a831c399e269772661 - docs/new.pdf
      EXTRA - f96b697d7cb7938d525a2f31aaf161d0 tmp/left-over
      ERR-DST c3fcd3d76192e4007dfb496cca67e13b 13 private/key
      # finished: 12/12/2026 12:30
      # origin: 6669 files, 33.5 GiB read, 0 ignored
      # destination: 6669 files, 33.5 GiB read, 0 ignored
      # elapsed: 590 s, 121.9 MB/s
      # sampled files: 13312, 66560 samples, 65.0 GiB read of 3.3 TiB (coverage 1.91 %)
      # differences: 1 HASH, 0 MISSING, 0 EXTRA, 0 SIZE, 0 TYPE, 0 ERR-SRC, 0 ERR-DST

  - First line `# hashdiff-diff: 1`: --recheck reads this file back (section 3.5), so it
    carries its own version. Then `# origin:` and `# destination:`, the cleaned escaped roots,
    and `# mode:`.
  - One record per difference, in canonical order:
    `STATUS ORIGIN-HASH DESTINATION-HASH escaped-path`. The two hashes come before the path
    because the path is the last field and may contain spaces (section 8); appending them
    after it would make the line unparseable. Each hash is the 32 hex digits of that side's
    entry, the decimal errno when that entry is an E, or `-` when that side has no entry at
    all (the destination's for MISSING, the origin's for EXTRA). Identical paths do not appear.
  - The footer begins at `# finished:` and every `#` line from there on belongs to it: that
    marker, the `# recheck:` line when there is one (section 3.5), and then the lines of the
    summary below that concern this destination, in the same order and with the same text: the
    ORIGIN line, this destination's line, `elapsed:`, the fast-mode metrics and this
    destination's counts, never the `results:` line and never a command. `# origin:` and
    `# destination:` therefore appear twice in the file, as a root in the header and as a
    count in the footer; `# finished:` is what tells the two apart, which works even when
    there are no records.
- The rsync-files file: raw path terminated by `\0` for MISSING, SIZE, HASH, TYPE and ERR-DST
  (EXTRA and ERR-SRC are not transferred). Always created, empty if there is nothing to
  transfer, and not created at all with --file (section 2.3). EXTRA paths (a file created in
  the destination during the hash stage) appear only in the diff-files file, with a warning on
  stderr that the tree changed during the run.
- Summary on stdout, printed for every run, including one where every destination stopped in
  the tree stage and nothing was hashed; items 4, 5 and 6 are then absent. In this order:
  1. `results:` and the cleaned absolute path of results.hashdiff;
  2. with --recheck, the `recheck:` line of section 3.5;
  3. one line per side, ORIGIN first and then the destinations in command-line order: a side
     that was hashed prints `SIDE: N files, X read, M ignored`, and a destination that stopped
     at the tree stage prints `SIDE: N files, M ignored`, without a `read` figure;
  4. when resuming, `resume SIDE: N entries reused, <last kept entry verified | re-hashed |
     no kept entries>` per side that was hashed;
  5. if anything was hashed, `elapsed: T s, R MB/s`, where R is `-` when T is 0, so the line
     always has the same shape, which matters because it also goes into the footer above;
  6. in fast mode, the metrics of section 6.3, added up over every side that was hashed;
  7. one line per destination, in command-line order: its count per status as
     `differences with DEST: ...`, or `no differences with DEST` when it has none, or
     `tree differences with DEST: ...` when it stopped at the tree stage (section 3.1);
  8. one command block per destination that has a command, in command-line order:
     `command for DEST:` and then, on a single line,

         rsync -a -I --from0 --files-from='<abs>/results.hashdiff/rsync-files-destination-2.lst' '<ORIGIN abs>/' '<DESTINATION abs>/'

     or, for a destination that stopped at the tree stage, the `suggested command for DEST`
     block of section 3.1;
  9. when at least one command of the hash stage was printed, once:

         NOTE: the commands above copy with rsync only the files whose content did not match.
               There is a copy of each one in its results.hashdiff/rsync-command-destination-N.txt

  With one destination every label is the one of version 1.1: `destination:` in 3 and 4,
  `differences:` or `no differences: ORIGIN and DESTINATION match` or `tree differences:` in
  7, the command alone with no `command for` line in 8, and in 9

         NOTE: the command above copies with rsync only the files whose content did not match.
               There is a copy of it in results.hashdiff/rsync-command.txt

  Every count printed next to a noun uses the singular when it is 1: `1 file`, `1 path`,
  `0 files`.
- The command of a destination is written, followed by LF, to its rsync-command file, which is
  always created and is empty when there is nothing to transfer. Paths are quoted with POSIX
  quoting (`'` → `'\''`). `-I` is mandatory: without it, rsync's quick check (size + mtime)
  would skip corrupted files that keep their size and mtime. `--files-from` implies -R and
  cancels the -r implied by -a, which is correct because the list contains only files. With
  --file the command is the direct form of section 2.3 and there is no `--files-from`.

## 10. Repository layout

    Makefile          all, testhook, test, test-deps, asan (-fsanitize=address,undefined -g),
                      clean, install (PREFIX=/usr/local); CC=clang must work
    README.md
    src/config.h      feature-test checks, static assert
    src/main.c        orchestration, fork/wait, summary
    src/opts.c/.h     argument and size parser
    src/md5.c/.h
    src/walk.c/.h     traversal, list, sorting
    src/plan.c/.h     profiles, sampling plan, cost model (no I/O)
    src/hasher.c/.h   full and fast modes, plan execution, workers, output writing
    src/diff.c/.h     merge-join, the diff-files, rsync-files and rsync-command files
    src/runstate.c/.h paths.txt, the state of an existing results.hashdiff and its cleaning,
                      the scan-* archive and the recheck sets (sections 3.2, 3.3 and 3.5)
    src/util.c/.h     xmalloc, dynamic buffers, escape/unescape, off_t <-> decimal,
                      off_t arithmetic with overflow detection, EINTR-safe I/O, path cleaning
                      (section 2.1), timestamp formatting (section 8)
    src/os.c/.h       encapsulated POSIX calls
    src/testhook.c    test-only driver (see below); never installed
    tests/requirements.txt
    tests/conftest.py
    tests/test_*.py

### Test-only driver: build/hashdiff-testhook

Built by `make testhook` from src/testhook.c + src/md5.c + src/plan.c + src/util.c, with the
same C89 flags as the main binary. It is not installed and contains no assertions; it only
exposes internal functions to the Python tests:

    hashdiff-testhook md5 [CHUNK]
        Reads stdin, feeds md5_update in chunks of CHUNK bytes (default 65536), prints the
        32-hex digest and a newline.
    hashdiff-testhook plan N GAP BLOCK PROFILE
        Prints "full" if the plan is a full MD5; otherwise "sampled K" on the first line and
        then one offset per line in decimal. Sizes in plain decimal bytes. Exit 2 on invalid
        arguments, including values that do not fit in off_t.

### Python tests

- All tests live in ./tests and are written in Python 3 (>= 3.10), using pytest.
- tests/requirements.txt lists every third-party package the tests use, pinned with ==
  to the latest release at implementation time: pytest and hypothesis. Nothing else; for
  everything else use the standard library (hashlib, subprocess, os, pathlib, shutil, stat).
  The tests must never import anything that is not in requirements.txt or the standard
  library.
- `make test-deps` runs `$(PYTHON) -m pip install -r tests/requirements.txt` (PYTHON defaults
  to python3). For local development the virtual environment lives in `tests/venv` (not
  hidden; ignored by .gitignore): `python3 -m venv tests/venv`, then
  `make test-deps test PYTHON=tests/venv/bin/python`. `make test` builds hashdiff and hashdiff-testhook and runs
  `$(PYTHON) -m pytest tests`; it does not install anything.
- tests/conftest.py provides fixtures: paths to both binaries (from env HASHDIFF_BIN and
  HASHDIFF_TESTHOOK, defaulting to ./build/hashdiff and ./build/hashdiff-testhook), a function
  that runs hashdiff and returns (exit code, stdout, stderr), a tree builder on top of
  pytest's tmp_path, and parsers for the hashes, diff-files, rsync-files and paths.txt
  formats. Two more helpers for the work of sections 2.2 and 7: one that returns the name of a
  side's files for a given number of destinations, and one that removes the time-dependent
  lines of a result file so that two runs can be compared byte for byte.
- Paths with arbitrary bytes are handled as bytes (os.fsencode / bytes paths) everywhere;
  output files are read in binary mode and parsed as bytes, never decoded as UTF-8.
- Cross-check MD5 with hashlib.md5, not with external md5sum/md5 tools.
- Tests needing something unavailable are skipped with an explicit reason, never failed:
  rsync not in PATH, running as root (permission tests), FS without sparse-file support.
- Slow tests are marked `@pytest.mark.slow` and skipped unless HASHDIFF_SLOW_TESTS=1.
  Register the marker in conftest.py.

## 11. Tests

test_md5.py (via testhook):
1. RFC 1321 A.5 vectors:

      ""                                  d41d8cd98f00b204e9800998ecf8427e
      "a"                                 0cc175b9c0f1b6a831c399e269772661
      "abc"                               900150983cd24fb0d6963f7d28e17f72
      "message digest"                    f96b697d7cb7938d525a2f31aaf161d0
      "abcdefghijklmnopqrstuvwxyz"        c3fcd3d76192e4007dfb496cca67e13b
      A-Z, a-z and 0-9 concatenated       d174ab98d277d9f5a5611c2c9f419d9f
      "1234567890" repeated 8 times       57edf4a22be3c955ac49da2e2107b67a

   Same digest with chunk sizes 1, 63, 64 and 65. Hypothesis: random byte strings
   (0..10000 bytes) and random chunk sizes match hashlib.md5.

test_plan.py (via testhook):
2. For a fixed battery of (N, G, B, profile), including N = 2B, 2B+1, values around each
   threshold of rule 3 and N close to 2^62: strictly increasing offsets, first 0, last
   N - B, no gap > G, k equal to the formula. Control case: N = 10 GiB, G = 64M, B = 1M,
   hdd → sampled 159. Hypothesis property test over random N, G, B and profile with the same
   invariants, plus "full" exactly when rule 1 or rule 3 says so (reimplement the formula in
   Python with integers as the oracle).

test_full_mode.py:
3. Every F line matches hashlib.md5 of the file.

test_diff.py:
4. Identical trees → exit 0, tree-diff.txt and diff-files.txt with no entries, empty
   rsync-files.lst and rsync-command.txt. Re-run without --force or --resume and with stdin not a
   terminal (stdin=DEVNULL) → exit 2; with --force → exit 0. Options after the paths
   (`ORIGIN DESTINATION --output DIR`) and trailing slashes on the paths give the same result.
5. HASH with 1 byte changed and same size, and HASH on a symlink with a different target of
   the same length; exit 1. rsync-command.txt contains exactly the rsync line printed on
   stdout plus LF, and that line is followed on stdout by the NOTE of section 9.

test_names.py:
6. Names with a space, leading `-`, `\`, tab, newline and a non-UTF-8 byte (b'a\xffb'):
   correct round-trip through escaping in tree-*.txt, hashes-*.txt, tree-diff.txt and
   diff-files.txt, and exact raw bytes in rsync-files.lst.

test_walk.py:
7. Empty file; empty directory (ignored); FIFO via os.mkfifo (ignored and counted).
8. chmod 000 on a file on each side (lstat still works, so the trees match) → ERR-SRC /
   ERR-DST in diff-files.txt and exit 3 (skip as root).
9. results.hashdiff inside ORIGIN → does not appear in tree-origin.txt or
   hashes-origin.txt.

test_tree.py:
23. MISSING, EXTRA, SIZE and TYPE (file vs symlink) → exit 4; tree-diff.txt has each entry
    in the right section; paths.txt exists; hashes-*.txt, diff-files.txt, rsync-files.lst and
    rsync-command.txt do not exist; stderr has the "trees differ" message and the warning
    about files only in DESTINATION. Suggested command: with EXTRA it has --delete-after;
    with only MISSING/SIZE/TYPE it has not; with only ERR there is none and stderr has the
    message about fixing permissions. With results.hashdiff
    inside DESTINATION the command excludes it.
24. Every line of tree-*.txt matches os.lstat (type, st_size, int(st_mtime)).
25. chmod 000 on a directory (skip as root) → ERR-SRC or ERR-DST in tree-diff.txt, exit 4.
26. After an exit 4 with MISSING and EXTRA, and results.hashdiff inside DESTINATION: run the
    suggested command (shlex.split, no shell; skip if rsync is not in PATH), check that
    results.hashdiff still exists, then hashdiff again with stdin not a terminal and without
    --force or --resume → it starts over without asking; exit 0.

test_fast_mode.py:
10. --gap guarantee with `--fast -g 256K -b 4K --profile ssd` and a 2 MiB + 123 byte file
    (with -g 64K, rule 3 reads this file in full: 32 samples cost more than the file):
    a damaged region of G + 1 bytes swept across every position (4K step) is always
    detected; one damaged byte in the middle of a gap (offset computed from the testhook
    plan) is NOT detected; one byte within the last B bytes is detected.
11. Same hashes with HASHDIFF_MERGE_GAP=0 and HASHDIFF_MERGE_GAP=1T.
13. Sparse file > 4 GiB created with os.truncate (skip if st_blocks shows it is not sparse
    or the FS rejects it), one byte changed at an offset > 4 GiB inside a sample: detected
    in fast mode. The same in full mode is marked slow.

test_parallel.py:
14. Byte-for-byte identical result files with -j 1, -j 4, --serial and --profile hdd, with the
    time-dependent lines of section 7 removed.

test_rsync.py:
15. Skip if rsync is not in PATH. Corrupt a DESTINATION file while keeping size and mtime
    (os.utime with the original times), run the printed rsync command (parse it with
    shlex.split and run without a shell), re-run hashdiff → exit 0.

test_resume.py:
In every case "identical" means all the result files are byte-for-byte equal to those of an
uninterrupted run with --force on the same trees, once the time-dependent lines of section 7
have been removed with the conftest helper.
12. A completed run, then --resume → exit 2, because the run finished and there is nothing to
    resume (section 3.3), with a message that suggests --force. An interrupted run with
    `--resume --fast` over a full-mode journal → exit 2 (different `# mode:`). --resume with a
    different ORIGIN in the same results.hashdiff → exit 2. --resume together with --force,
    and either of them with --recheck → exit 2.
17. Simulated interruption: after a completed run, rename both hashes files to `.tmp`, cut
    them at different points (the longer one in the middle of a line) and delete the diff
    outputs; --resume → identical, and the longer side keeps all its valid lines (each side
    is cut independently). Repeat leaving one side as a published hashes-SIDE.txt.
18. The last kept line has a well-formed but wrong hash → re-hashed, warning on stderr,
    identical.
19. An invalid line in the middle (bad hex, unsorted path, invalid escape, wrong field
    count) → it and everything after it are discarded; identical.
20. Trees modified after the interruption (a file deleted, a file created, a file resized,
    a file with only its mtime changed with os.utime) → exit 2, tree-changes.txt with each
    entry as DELETED, ADDED or CHANGED in the right section, journals and tree files
    untouched. Restoring the trees (and the mtime) and running --resume again → identical.
21. Prompt through a pseudo-terminal (os.openpty for stdin and stderr) on an interrupted run,
    state 1 of section 3.3: `a` → exit 2 and nothing is touched; `o` → starts over; `r` →
    resumes; an invalid answer repeats the question. The other two states are test 34.
22. Real interruption, with -j 1 and -j 4: SIGINT once the journal has some lines → exit
    130, `hashes-*.txt.tmp` kept; then --resume → identical. The tree is 50 small files and
    then a 2 GiB sparse file that takes seconds to read (small files alone are read from the
    page cache too fast to interrupt reliably). Skip with a reason if the filesystem has no
    sparse files or the run finishes before the signal is delivered.

test_paths.py:
27. Path cleaning (section 2.1): the same trees compared as `o`, `./o`, `o/`, `./././o`,
    `../<dir>/o` and an absolute path give identical result files, and every `# root:`,
    `# origin:`, `# destination:`, `results:` line and suggested command shows a path with no
    `.`, no `..`, no `//` and no trailing slash. `--output .`, `--output ./`, `--output .//.`
    and no --output at all print the same `results:` path. A run interrupted as `./o ./d` and
    resumed as `o d` resumes instead of failing.
28. paths.txt and history.txt (section 3.2): one line per side in command-line order; labels
    `origin` and
    `destination` with one destination and `origin`, `destination-1`, ... with several; cleaned
    absolute escaped paths, including a root whose name holds a newline; a `# started:` that
    parses as `%d/%m/%Y %H:%M`; `# file: 1` only with --file; and the file present after a run
    that stopped at the tree stage (exit 4). history.txt: one block per run in chronological
    order, its first line written once, surviving --force and a recheck, and not appended to by
    a run that fails before the tree stage.

test_file_mode.py:
29. --file (section 2.3): two identical files → exit 0; one byte changed with the same size →
    exit 1, one HASH record, and `rsync -a -I 'ORIGIN' 'DESTINATION'` both on stdout and in
    rsync-command.txt; two files with different names → HASH and never MISSING or EXTRA;
    different sizes → exit 4 in the tree stage with the same command without `-I`;
    rsync-files.lst does not exist; both files in one directory is a valid run; the same file
    given twice → exit 2; a symlink to a file is followed and no L entry ever appears; a
    directory with --file and a regular file without it produce the two messages of
    section 2.3 and exit 2, while a FIFO keeps `is not a directory`; `-x` and `-j 4` warn;
    --file with two destinations.

test_multi.py:
30. Three destinations (section 2.2), one identical to ORIGIN, one with a HASH difference and
    one whose tree differs: exit 4 by precedence; the numbered files of section 3 exist and
    the unnumbered ones do not; the per-destination lines of the summary are in command-line
    order and name every destination; the destination whose tree differs has no hashes file
    while the other two do; hashes-origin.txt is written once and its entries are read once.
    The same trees with one destination give exactly the file names and the summary labels of
    version 1.1.
31. Identical result files with -j 1, -j 4 and --serial over three destinations.
32. Rejections and warnings: a destination equal to ORIGIN, two equal destinations, 65
    destinations and `--number-of-destinations 2` with three destinations → exit 2 each, the
    message naming the roles. A destination nested inside another is excluded with a warning
    and every suggested command carries its `--exclude`. `(1 + D) * --jobs` over 64 warns.
33. Cleaning (section 3.3): a three-destination run, then a two-destination run with --force in
    the same directory → no file whose name holds `destination-3` is left behind, and a
    `scan-*` directory created by hand survives.

test_existing.py:
34. The three states of section 3.3 through a pseudo-terminal: state 1 offers r/o/a, state 2
    o/a and state 3 c/o/a; every answer does what it says and a letter the state does not offer
    repeats the question. Without a terminal each state exits 2 with a message naming the flags
    that state accepts. --recheck in states 1 and 2 → exit 2. A results.hashdiff with no
    paths.txt → exit 2 suggesting --force. A changed root, and a different number of
    destinations, → exit 2 without asking, even on a terminal.

test_recheck.py:
35. --recheck (section 3.5): a run with two differing files; copy one of them with the printed
    command; --recheck → the previous files are in `scan-YYYYMMDDHHMM/`, named after the
    previous run's `# started:`; only those two paths were read (the `recheck:` line and the
    per-side counts of the summary); the copied one is gone from the new diff file and the
    other is still there; exit 1. Then copy the second and --recheck again → exit 0 and a
    second archive. Two archives in the same minute → the `-2` suffix. A rechecked path deleted
    from every tree → the warning of step 4 and no difference. --recheck with a different
    `# mode:` → exit 2. An interrupted recheck resumed reads the same paths. With three
    destinations, a path rechecked for one of them only is not reported as MISSING for the
    others.

test_formats.py:
36. Timestamps (section 8): `# started:` and `# finished:` in every published hashes file,
    `# finished:` absent from a `.tmp` journal, one `# resumed:` line per resume and in order,
    `# hashdiff-format: 3`, and a results directory whose hashes files say `2` → exit 2 with a
    message that says it was written by an older hashdiff. Two runs over the same trees are
    identical once the time-dependent lines are removed, and differ only in those lines.
37. The diff-files format (section 9): the `# hashdiff-diff: 1` line; the two hash columns
    equal to the hashes files for HASH and SIZE; `-` for the side without an entry in MISSING
    and EXTRA; the decimal errno for ERR-SRC and ERR-DST; a path with spaces and a newline
    still parsed, because the path is the last field; and a footer that starts at
    `# finished:` whose lines repeat the summary of that destination.
38. The summary (section 9): `1 file` and not `1 files` for a tree of one file;
    `elapsed: 0 s, - MB/s`; the NOTE after the command, and no NOTE when there is nothing to
    copy.

test_lock.py:
39. The lock (section 3.6): a helper process opens `results.hashdiff/lock` and holds a write
    lock on it with `fcntl.lockf`; hashdiff then prints `is in use by process <pid>` with that
    pid, exits 2 without a terminal with a message that names --ignore-lock, exits 2 on `a` and
    continues on `y` through a pseudo-terminal, and proceeds without asking with
    --ignore-lock. The helper killed with SIGKILL leaves no lock and the next run takes it
    without asking and without a warning. Two hashdiffs started at once on a fresh directory:
    one of them finishes and the other stops. `lock` survives --force and a recheck and is
    never moved into a `scan-*` directory. Skip the cases that need the helper, with a reason,
    on a filesystem that does not support locking.

Sanitizers:
16. `make asan` followed by `make test` passes (the binaries under test are the ASan builds).

## 12. Phases

At the end of each phase: compile locally with the available compiler (Apple clang on macOS
is enough) using the flags from section 1 (any warning counts as a failure); run
`make test` and commit. GCC, musl, 32-bit and FreeBSD are covered by CI (section 13). Do not
move on with warnings or failing tests. Tests for features of later phases may exist
earlier only if marked xfail with a reason.

1. Skeleton, Makefile (including test and test-deps targets), config.h, util, option
   parser, --help and --version; tests/requirements.txt and conftest.py; CI workflow.
2. MD5 and the testhook md5 command; test 1.
3. Traversal, sorting and full mode in a single process; writing hashes-*.txt as a
   canonical-order journal; test 3.
4. results.hashdiff (including --force and the fatal error when it exists and there is no
   terminal), fork of both sides with both pipes, tree stage (tree-*.txt, tree-diff.txt,
   exit 4, suggested command), diff, rsync-files.lst, rsync-command.txt and summary;
   tests 4-9, 15 and 23-26.
5. plan.c and the testhook plan command (test 2); fast mode with plan execution, merge_gap
   and fadvise; tests 10, 11 and 13.
6. -j with chunks and result records, --serial, -x, inode read order per chunk on hdd,
   progress and signals; test 14.
7. Resume (section 3.4) with the tree check and tree-changes.txt, --resume and the prompt;
   tests 12 and 17-22.
8. README: what hashdiff does, build and install (GNU make), usage with examples, the two
   stages and every output file, exit codes, that hashdiff never runs rsync, that the
   suggested `--delete-after` command deletes files that exist only in DESTINATION, that no
   rsync command must be run while hashdiff is running, fast mode and its
   --gap guarantee, why S hashes are not comparable with md5sum, resume and its limitation,
   how to run the tests (make test-deps, make test, HASHDIFF_SLOW_TESTS=1) and the CI
   badge. Test 16.

Phases 1 to 8 are version 1.1. Phases 9 to 16 are the work of sections 2.1, 2.2, 2.3, 3.2,
3.3, 3.5, 3.6 and the parts of 3, 3.1, 7, 8 and 9 that depend on them. Their order is not the
order in which they were asked for: phase 10 renames every per-destination file and is what
makes a side name the key of every output, so doing it before the formats and the prompts
writes them once instead of twice.

9. Path cleaning in util and its use for ORIGIN, the destinations, --output and every root,
   message and command (section 2.1); the singular of the counts and the fixed shape of
   `elapsed:` (section 9); the NOTE after a command (section 9). Tests 27 and 38. README: the
   examples and the note about `..`.
10. Several destinations (sections 2.2, 3, 3.1, 7 and 9): side names, 1 + D children, the
    per-destination decision of the tree stage, the numbered files, one comparison and one
    command block per destination, the exit-code precedence of section 2,
    --number-of-destinations and the warning about the number of readers. runstate.c with the
    cleaning of the directory by prefix and suffix instead of a fixed list (section 3.3),
    which belongs here and not later: as soon as the names carry a number, a --force over a
    directory left by a run with more destinations would leave the extra ones behind.
    Tests 30 to 33. README: usage, the table of output files and the exit codes.
11. paths.txt and history.txt in runstate.c (section 3.2). Timestamps,
    `# hashdiff-format: 3` and readers that skip the `#` lines before the records and accept
    the ones after (sections 7 and 8); the two hash columns and the footer of the diff files,
    with `# hashdiff-diff: 1` (section 9). Tests 28, 36 and 37.
12. The three states of an existing results.hashdiff, their flags and their prompts
    (section 3.3). Test 34. README: what hashdiff asks and which flag answers it.
13. --recheck: the archive, the recheck sets, the filtered lists and the `recheck:` line
    (section 3.5). Test 35. README: that exit 0 after a recheck does not mean that the trees
    match, and that the archives are never pruned.
14. --file (section 2.3). Test 29. README: usage and the limitation of resume.
15. The lock of section 3.6: `lock`, `fcntl(F_SETLK)` taken before anything else touches the
    directory, the `F_GETLK` message with the holder's pid, --ignore-lock, and the warning
    that lets a filesystem without locking through. Test 39. README: that one run at a time
    writes a results.hashdiff, what the flag does, and the two things the lock does not see.
16. README and RELEASE_NOTES.md sweep. The on-disk formats of this work do not read on version
    1.1 and 1.1 does not read theirs, so the release that carries it is 1.2: bump `HD_VERSION`
    in src/config.h, the `--version` line of section 2 and `test_version` in
    tests/test_cli.py together, as the checklist in RELEASE_NOTES.md says.

## 13. Continuous integration (GitHub Actions)

Create `.github/workflows/ci.yml` in phase 1, so every later phase is validated in CI from the
first commit. CI must be green before a phase is considered complete.

### Makefile requirements

- `CC` may include flags (e.g. `CC="gcc -m32"`) and must be used for every compile and link
  step, including the testhook and ASan builds.
- `CFLAGS_EXTRA` is appended after the C89 warning flags from section 1 without replacing
  them. CI passes `CFLAGS_EXTRA=-Werror`; the default user build does not use -Werror.
- `PYTHON` is overridable (default python3), as described in section 10.
- The Makefile must work with GNU make. Either keep it POSIX make compatible or document in
  the README that GNU make is required (FreeBSD invokes it as gmake).

### Jobs

All jobs: checkout, build `all testhook`, install tests/requirements.txt, run `make test`.
`fail-fast: false` in matrices so one failing platform does not hide the others.

    linux-gcc       ubuntu-latest, CC=gcc, CFLAGS_EXTRA=-Werror
    linux-clang     ubuntu-latest, CC=clang, CFLAGS_EXTRA=-Werror
    linux-m32       ubuntu-latest, CC="gcc -m32" (install gcc-multilib),
                    CFLAGS_EXTRA=-Werror, HASHDIFF_SLOW_TESTS=1. This is the job that
                    verifies offsets > 4 GiB with a 32-bit long. The 64-bit Python drives the
                    32-bit binaries via subprocess; Python itself is never built for 32 bits.
    linux-asan      ubuntu-latest, `make asan`, with
                    ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 and
                    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
    linux-musl      container alpine:latest; apk add build-base python3 py3-pip rsync;
                    test deps installed in a venv and passed via PYTHON=. Runs as root, so
                    permission tests are skipped (expected).
    macos           macos-latest, CC=clang, CFLAGS_EXTRA=-Werror
    freebsd         ubuntu-latest + vmactions/freebsd-vm; pkg install gmake python3 rsync rust
                    (hypothesis has no FreeBSD wheel and builds with Rust);
                    build with gmake CC=cc CFLAGS_EXTRA=-Werror; test deps in a venv.

- Python via actions/setup-python on Ubuntu and macOS (3.12), from packages on Alpine and
  FreeBSD. The test suite must also work on the oldest supported version (3.10).
- Install rsync on every job so test 15 runs instead of being skipped.
- Use the latest major versions of actions/checkout, actions/setup-python and
  vmactions/freebsd-vm at implementation time, pinned to a major tag (e.g. @v4).
- Triggers: push to main and every pull request.
- Add a CI status badge for the workflow at the top of README.md.
