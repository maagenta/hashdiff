# hashdiff: implementation specification

Implement `hashdiff`, a CLI that computes, in parallel, the content hash of two directory trees,
compares them, and generates the list of files to re-sync with rsync. Work in phases
(section 12). If anything in this specification is ambiguous or contradictory, ask before
implementing. Do not add functionality not described here.

## 1. Language and platform

- Strict ISO C89/C90. Must compile with zero warnings under both GCC and Clang using:
  `-std=c89 -pedantic-errors -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes`
- Zero third-party dependencies. The only external interface is the system libc (C89) +
  POSIX/SUSv3: opendir/readdir/closedir, lstat/fstat, readlink, open/read/pread/write/close,
  mkdir, rename, unlink, pipe, fork/waitpid/_exit, kill, sigaction, getcwd, isatty, time,
  posix_fadvise (under #ifdef).
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

    hashdiff ORIGIN DESTINATION [OPTIONS]

    -o, --output DIR        Existing directory where DIR/results.hashdiff/ is created (default: .)
        --resume            If results.hashdiff exists, resume the interrupted run without
                            asking (section 3.2)
        --force             If results.hashdiff exists, discard it and start over without asking
    -f, --fast              Sampled fast mode (section 6). Without it: full MD5 of everything
    -g, --gap SIZE          Maximum unread region between two samples (default: 64M).
                            Any contiguous damage larger than SIZE is always detected
    -b, --block SIZE        Bytes read per sample (default: depends on --profile)
        --profile hdd|ssd   Disk type (default: ssd). Sets the default --block, the cost
                            model and the read strategy (section 6.1)
    -j, --jobs N            Hashing processes per tree, 1..256 (default: 1)
        --serial            Process ORIGIN and then DESTINATION instead of in parallel
    -x, --one-file-system   Do not cross mount points (st_dev different from the root's)
    -q, --quiet             No progress on stderr
    -h, --help
    -V, --version           Print "hashdiff 1.0" and exit 0

- SIZE: integer with optional K, M, G or T suffix (base 1024, case-insensitive), with
  overflow detection. --gap >= 1; --block >= 512. Accepted forms: `--opt VALUE`,
  `--opt=VALUE`, `-g 8M`, `-g8M`, and `--` as end of options. Custom parser.
- Options may appear before, between or after the two paths, e.g.
  `hashdiff /data/origin/ /data/destination/ --output /data/reports`. Every argument after
  `--` is a path. Exactly two paths are required.
- Trailing slashes of ORIGIN and DESTINATION are removed (except for the root `/`), so
  `/data/origin/` and `/data/origin` are the same run.
- --resume and --force together: fatal error.
- --gap, --block and --profile without --fast: warning on stderr that they have no effect.
  Full mode always uses the ssd constants for its cost estimates and never uses the hdd
  read order (section 7).
- ORIGIN and DESTINATION must be directories; a symlink is followed only for the roots
  themselves. Fatal error if both are the same directory (same st_dev and st_ino).
- Exit codes: 0 no differences; 1 differences found; 2 fatal error (including "abort" at the
  prompt of section 3 and trees changed on resume, section 3.2); 3 completed, but some side
  had read errors (E entries); 4 the trees differ, nothing was hashed (section 3.1);
  128 + signal number when interrupted by SIGINT or SIGTERM. Errors and differences
  together → 3.
- Test-only environment variable, not documented in --help: HASHDIFF_MERGE_GAP=SIZE
  overrides the profile's merge_gap (section 6.2).

## 3. Results directory

`DIR/results.hashdiff/` is created with `mkdir(path, 0777)` (umask applies). File names are
fixed; --output only chooses DIR. A run has two stages: the tree stage (section 3.1) always
runs; the hash stage (sections 5-9) runs only when both trees match.

    file                     written by                content
    tree-origin.txt          tree stage                list of ORIGIN (section 3.1)
    tree-destination.txt     tree stage                list of DESTINATION
    tree-diff.txt            tree stage                ORIGIN vs DESTINATION (section 3.1)
    hashes-origin.txt        hash stage                hash list of ORIGIN (section 8)
    hashes-destination.txt   hash stage                hash list of DESTINATION
    diff-files.txt           hash stage                content differences (section 9)
    rsync-files.lst          hash stage                paths to copy, each ended by `\0`
    rsync-command.txt        hash stage                rsync command that copies them
    tree-changes.txt         resume, only on change    saved trees vs current (section 3.2)

- The tree stage writes no command files; it only prints a suggested rsync command
  (section 3.1).
- `rsync-command.txt`: the single-line rsync command of section 9 that copies exactly the
  paths in rsync-files.lst; empty if there is nothing to copy. The paths are not embedded in
  the command: a list of thousands of paths would exceed the system's argument length limit
  (ARG_MAX), and names with newlines, spaces or non-UTF-8 bytes are only safe in the
  `\0`-terminated list.
- hashdiff never runs any rsync command; the user reviews and runs it.

Each file is written as `NAME.tmp` and published with `rename()` when complete, so a partial
result never looks complete. Check the return value of fwrite, fflush and fclose: deferred
write errors (ENOSPC, EIO, NFS) can surface on close. `hashes-*.txt.tmp` are journals that
are written progressively and kept on interruption so the run can be resumed (section 3.2);
every other `.tmp` file is deleted on interruption.

If `DIR/results.hashdiff` already exists:

- if it contains no `hashes-*.txt` and no `hashes-*.txt.tmp` (for example, the previous run
  stopped at the tree stage), there is nothing to resume: delete the files of the table, and
  their `.tmp`, `.new` and `.part` files, and start over without asking, even with --resume;
- otherwise, with --force: delete those files and start over;
- with --resume: resume (section 3.2);
- with neither, if stdin and stderr are both terminals (isatty), ask on stderr (also with -q):

      results.hashdiff already exists in DIR: [r]esume, [o]verwrite or [a]bort?

  and read one line from stdin with read() on fd 0. `r` → resume; `o` → as --force;
  `a` → exit 2 without touching anything; any other answer repeats the question; EOF →
  exit 2;
- with neither and no terminal (scripts, CI, tests): fatal error (exit 2) whose message
  suggests --resume or --force.

If results.hashdiff lies inside ORIGIN or DESTINATION, or one tree is nested inside the
other, those directories are excluded from the traversal by comparing st_dev/st_ino, with a
warning on stderr.

### 3.1 Tree stage

Before anything is hashed, each child traverses its tree (section 4), sorts it, writes
`tree-SIDE.txt` (SIDE is `origin` or `destination`) and waits for the parent (section 7).
No file content is read in this stage.

    # hashdiff-tree: 1
    # root: /abs/path/origin
    # excluded: backups/results.hashdiff
    F 1048576 1728212345 docs/report.pdf
    L 11 1728212345 lib/libfoo.so
    E 13 - private

- Header: `# hashdiff-tree: 1`, `# root:` (escaped, absolute) and one `# excluded:` line
  (escaped path relative to the root) per directory excluded from the traversal (section 3),
  used for the `--exclude` options of the suggested command.
- Fields separated by a single space: type (F = regular file, L = symlink, E = error);
  size (lstat st_size; for L, the length of the link target; for E, the errno in decimal);
  mtime (st_mtime in decimal seconds, possibly negative, converted through off_t with the
  util.c functions; `-` for E); and the path, escaped as in section 8. Canonical order.
- The parent merge-joins both tree files (streaming, constant memory). First matching rule
  wins:
  1. E in ORIGIN → ERR-SRC
  2. Only in ORIGIN → MISSING
  3. Only in DESTINATION → EXTRA
  4. E in DESTINATION → ERR-DST
  5. F vs L → TYPE
  6. Different size → SIZE
  7. Otherwise the entries match. mtime is not compared: a copy may not keep it.
- `tree-diff.txt` is always written; it has no entries when the trees match:

      # origin: /abs/path/origin
      # destination: /abs/path/destination
      ## origin
      MISSING escaped-path
      SIZE escaped-path
      ## destination
      EXTRA escaped-path

  The `## origin` section holds MISSING, SIZE, TYPE and ERR-SRC (what ORIGIN has that
  DESTINATION lacks or has differently, and read errors in ORIGIN); the `## destination`
  section holds EXTRA and ERR-DST. Each section is in canonical order (two streaming passes).
- No differences: the parent tells both children to start the hash stage.
- Differences: the parent tells both children to exit and prints the summary of this stage
  on stdout: count per status and a suggested general synchronization command, chosen as
  follows:
  - EXTRA present (with or without other statuses):
    `rsync -a --delete-after EXCLUDES '<ORIGIN abs>/' '<DESTINATION abs>/'`
  - otherwise, MISSING, SIZE or TYPE present:
    `rsync -a EXCLUDES '<ORIGIN abs>/' '<DESTINATION abs>/'`
  - only ERR-SRC / ERR-DST: no command.

  If there is any ERR-SRC or ERR-DST (alone or with other statuses), also print on stderr:

      hashdiff: N paths could not be read (see ERR-SRC / ERR-DST in tree-diff.txt);
      fix their permissions and run hashdiff again.

  The command is printed, not written to a file. `-x` is added if hashdiff ran with -x.
  EXCLUDES is one `--exclude='/<relative path>/'` for every directory excluded from either
  traversal (results.hashdiff and a nested tree, section 3), so `--delete-after` can never
  delete results.hashdiff or a tree nested in DESTINATION. In the pattern, `*`, `?`, `[` and
  `\` are escaped with `\`. Everything is quoted with POSIX quoting (`'` → `'\''`). On stderr:

      hashdiff: ORIGIN and DESTINATION trees differ; nothing was hashed. See tree-diff.txt,
      fix the differences (for example with the suggested rsync command) and run hashdiff
      again.

  plus, if there are EXTRA paths:

      hashdiff: warning: N files exist only in DESTINATION; the suggested command uses
      --delete-after and will delete them. Review tree-diff.txt before running it.

  Exit 4. Unlike the command of section 9, the suggested one has no `-I`: rsync's quick
  check is enough to copy missing files and files of a different size, and `-I` would
  rewrite the whole tree.

### 3.2 Resuming an interrupted run

While hashing, each side writes `hashes-SIDE.txt.tmp` progressively: the header first and
then complete lines in canonical order (section 7), flushed at least every ~2 s and before
exiting. After an interruption (signal, crash, power loss) it holds a canonical prefix of the
result, possibly followed by an incomplete or damaged tail.

The parent prepares the resume before forking:

1. Source of each side: `hashes-SIDE.txt.tmp` if it exists, otherwise `hashes-SIDE.txt` (the
   side had finished), otherwise none. Stale `hashes-SIDE.txt.new` files are deleted.
2. `tree-origin.txt` and `tree-destination.txt` must exist with a valid header and the
   current roots; otherwise fatal error (exit 2) suggesting --force.
3. Header of each source: the first three lines must be exactly `# hashdiff-format: 2`,
   `# root:` with the current (escaped, absolute) root of that side, and `# mode:` with the
   current mode line. A different root or mode is a fatal error (exit 2): lists from another
   tree or made with other parameters are not comparable. The message suggests --force. A
   header that is missing or cut short counts as no source.
4. Valid prefix: read the lines after the header and stop at the first one that is not
   valid: no terminating LF (cut by the interruption); not exactly four fields; type not one
   of F/S/L/E; hash not 32 lowercase hex digits (decimal errno for E); size not decimal (`-`
   for E); a path whose escaping is invalid (`\` followed by anything other than `\`, `n`
   or `r`); or a path that is not strictly greater than the previous one. That line and
   everything after it are discarded. Each side is cut independently at its own last valid
   line, so a side that was further ahead (for example, the finished ORIGIN of a --serial
   run) loses nothing.
5. The previous outputs (diff-files.txt, rsync-files.lst, rsync-command.txt,
   tree-changes.txt and their `.tmp` files) are deleted; they are produced again at the end.

Tree check: each child traverses and sorts its tree again, keeps the list in memory (it does
not rewrite tree-SIDE.txt) and compares it with the saved tree-SIDE.txt on every field,
including mtime. It writes its differences to `tree-changes.txt.SIDE.part`: ADDED (only in the
current tree), DELETED (only in the saved one) or CHANGED (type, size or mtime differ),
in canonical order, and tells the parent whether its tree matches (section 7). If either
tree changed, the parent tells both children to exit, builds `tree-changes.txt`:

    # origin: /abs/path/origin
    # destination: /abs/path/destination
    ## origin
    DELETED escaped-path
    CHANGED escaped-path
    ## destination
    ADDED escaped-path

deletes the `.part` files, prints on stderr

    hashdiff: the trees changed since the interrupted run; see tree-changes.txt.
    Restore them to resume, or use --force to start over.

and exits 2 without touching the journals or the tree files.

If both trees match, the hash stage continues on each side:

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
  `# mode:` line is a fatal error (section 3.2), and the diff also aborts with a fatal error
  if the `# mode:` lines of both lists differ (internal consistency check).
- For type S files, the summary reports the total number of samples, bytes read and average
  coverage (sample bytes / total bytes).

## 7. Parallelism (fork, no threads)

- The main process validates, handles an existing results.hashdiff (sections 3 and 3.2),
  creates results.hashdiff, calls `fflush(NULL)` and uses `fork()` to launch one child per
  tree: ORIGIN → tree-origin.txt and hashes-origin.txt, DESTINATION → tree-destination.txt
  and hashes-destination.txt. Each child gets two `pipe()`s: child → parent (tree stage
  result and final statistics) and parent → child (one byte: continue to the hash stage, or
  exit). The parent waits for the children with `waitpid`, retrying on EINTR.
- Sequence: both children finish the tree stage (section 3.1) or, when resuming, the tree
  check (section 3.2) and report it; the parent compares and tells both children to
  continue or to exit; then both hash. With --serial, one side at a time in each stage:
  ORIGIN's tree, DESTINATION's tree, then ORIGIN's hashes, then DESTINATION's hashes.
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
  change.
- SIGINT and SIGTERM via `sigaction`: the handler only writes a `volatile sig_atomic_t`.
  Processes check it between files and between reads. Workers exit. The child forwards the
  signal to its workers with kill, reads the remaining records until EOF, writes the
  completed prefix, flushes and closes its journal (it is kept for --resume) and exits with
  128 + signal number. The parent forwards the signal to its children with kill, deletes its
  own `.tmp` files and exits with 128 + signal number.
- Progress on stderr if isatty(2) and no -q: one line per side every ~2 s (measured with
  time()), emitted by the child with a single write() call so lines do not interleave. With
  -j N the child adds up its workers' records: `[origin] 1532/90211 files, 12.4 GiB`.
  Sizes are formatted with integer arithmetic.

## 8. Format of hashes-origin.txt and hashes-destination.txt

    # hashdiff-format: 2
    # root: /abs/path/origin
    # mode: full
    F d41d8cd98f00b204e9800998ecf8427e 0 docs/empty.txt
    S <32 hex> 10737418240 video/big.mkv
    L <32 hex> 11 lib/libfoo.so
    E 13 - private

- Header: `#` lines only at the beginning. In fast mode:
  `# mode: fast gap=67108864 block=65536 profile=ssd seek_bytes=200000`
- Fields separated by a single space: type, hash (32 lowercase hex; for E, the errno in
  decimal), size (decimal; for L, length of the link target; for E, `-`) and path.
- Types: F = regular file with full MD5; S = regular file with sampled hash; L = symlink,
  with hash = MD5 of the target returned by readlink; E = error.
- The path is the last field and may contain spaces. Escaping: `\` → `\\`, LF → `\n`,
  CR → `\r`; all other bytes are written as-is, including non-UTF-8. `# root:` is escaped
  the same way and is absolute (getcwd + concatenation, without resolving symlinks).
- Lines follow the canonical order of section 4, computed on the unescaped path.

## 9. Diff: diff-files.txt, rsync-files.lst and rsync-command.txt

- If either side exits with 2: exit 2 without diffing. If both exit with 0 or 3, the parent
  streams both files with its own line reader (arbitrary length), checks that the `# mode:`
  lines match and that paths are in strictly increasing order (otherwise fatal error),
  unescapes, and performs a merge-join with strcmp on the raw paths. Constant memory with
  respect to the number of files.
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
- diff-files.txt: header (`# origin:`, `# destination:`, `# mode:`) and one line
  `STATUS escaped-path` per difference, in canonical order. Identical paths do not appear.
- rsync-files.lst: raw path terminated by `\0` for MISSING, SIZE, HASH, TYPE and ERR-DST
  (EXTRA and ERR-SRC are not transferred). Always created, empty if there is nothing to
  transfer. EXTRA paths (a file created in DESTINATION during the hash stage) only appear in
  diff-files.txt, with a warning on stderr that the tree changed during the run.
- Summary on stdout: path of results.hashdiff, files and bytes read per side, ignored
  entries, elapsed time, MB/s, count per status and, in fast mode, the metrics from
  section 6.3. When resuming, also the number of reused entries per side and whether each
  side's last kept entry was verified or re-hashed. If there is anything to transfer, print
  on a single line:

      rsync -a -I --from0 --files-from='<abs>/results.hashdiff/rsync-files.lst' '<ORIGIN abs>/' '<DESTINATION abs>/'

  and write the same line followed by LF to rsync-command.txt. rsync-command.txt is always
  created, empty if there is nothing to transfer. Paths are quoted with POSIX quoting
  (`'` → `'\''`). `-I` is mandatory: without it, rsync's quick check
  (size + mtime) would skip corrupted files that keep their size and mtime. `--files-from`
  implies -R and cancels the -r implied by -a, which is correct because the list contains
  only files.

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
    src/diff.c/.h     merge-join, diff-files.txt, rsync-files.lst, rsync-command.txt
    src/util.c/.h     xmalloc, dynamic buffers, escape/unescape, off_t <-> decimal,
                      off_t arithmetic with overflow detection, EINTR-safe I/O
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
  HASHDIFF_TESTHOOK, defaulting to ./hashdiff and ./build/hashdiff-testhook), a function
  that runs hashdiff and returns (exit code, stdout, stderr), a tree builder on top of
  pytest's tmp_path, and parsers for hashes-*.txt, diff-files.txt and rsync-files.lst.
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
   the same length; exit 1. rsync-command.txt contains exactly the command printed on
   stdout plus LF.

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
    in the right section; hashes-*.txt, diff-files.txt, rsync-files.lst and
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
10. --gap guarantee with `--fast -g 64K -b 4K --profile ssd` and a 2 MiB + 123 byte file:
    a damaged region of G + 1 bytes swept across every position (4K step) is always
    detected; one damaged byte in the middle of a gap (offset computed from the testhook
    plan) is NOT detected; one byte within the last B bytes is detected.
11. Same hashes with HASHDIFF_MERGE_GAP=0 and HASHDIFF_MERGE_GAP=1T.
13. Sparse file > 4 GiB created with os.truncate (skip if st_blocks shows it is not sparse
    or the FS rejects it), one byte changed at an offset > 4 GiB inside a sample: detected
    in fast mode. The same in full mode is marked slow.

test_parallel.py:
14. Byte-for-byte identical result files with -j 1, -j 4, --serial and --profile hdd.

test_rsync.py:
15. Skip if rsync is not in PATH. Corrupt a DESTINATION file while keeping size and mtime
    (os.utime with the original times), run the printed rsync command (parse it with
    shlex.split and run without a shell), re-run hashdiff → exit 0.

test_resume.py:
In every case "identical" means all the result files are byte-for-byte equal to those of an
uninterrupted run with --force on the same trees.
12. A completed run in full mode, then --resume with --fast → exit 2 (different `# mode:`).
    --resume with a different ORIGIN in the same results.hashdiff → exit 2. --resume
    together with --force → exit 2.
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
21. Prompt through a pseudo-terminal (os.openpty for stdin and stderr): `a` → exit 2 and
    nothing is touched; `o` → starts over; `r` → resumes; an invalid answer repeats the
    question.
22. Real interruption, with -j 1 and -j 4: SIGINT once the journal has some lines (tree of
    ~2000 files of 64 KiB) → exit 130, `hashes-*.txt.tmp` kept; then --resume → identical.
    Skip with a reason if the run finishes before the signal is delivered.

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
7. Resume (section 3.2) with the tree check and tree-changes.txt, --resume and the prompt;
   tests 12 and 17-22.
8. README: what hashdiff does, build and install (GNU make), usage with examples, the two
   stages and every output file, exit codes, that hashdiff never runs rsync, that the
   suggested `--delete-after` command deletes files that exist only in DESTINATION, that no
   rsync command must be run while hashdiff is running, fast mode and its
   --gap guarantee, why S hashes are not comparable with md5sum, resume and its limitation,
   how to run the tests (make test-deps, make test, HASHDIFF_SLOW_TESTS=1) and the CI
   badge. Test 16.

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
    freebsd         ubuntu-latest + vmactions/freebsd-vm; pkg install gmake python3 rsync;
                    build with gmake CC=cc CFLAGS_EXTRA=-Werror; test deps in a venv.

- Python via actions/setup-python on Ubuntu and macOS (3.12), from packages on Alpine and
  FreeBSD. The test suite must also work on the oldest supported version (3.10).
- Install rsync on every job so test 15 runs instead of being skipped.
- Use the latest major versions of actions/checkout, actions/setup-python and
  vmactions/freebsd-vm at implementation time, pinned to a major tag (e.g. @v4).
- Triggers: push to main and every pull request.
- Add a CI status badge for the workflow at the top of README.md.
