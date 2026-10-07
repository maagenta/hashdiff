# hashdiff

[![CI](https://github.com/maagenta/hashdiff/actions/workflows/ci.yml/badge.svg)](https://github.com/maagenta/hashdiff/actions/workflows/ci.yml)

`hashdiff` compares the **content** of two directory trees, ORIGIN and DESTINATION, and
tells you exactly which files must be copied again with rsync. It is meant for backups and
mirrors where corruption may keep a file's size and modification time, which rsync's quick
check cannot see.

- Both trees are hashed in parallel (one process per tree, optionally several per tree).
- A fast sampled mode reads a small, deterministic part of every large file and still
  guarantees that any contiguous damage larger than a chosen size is detected.
- Long runs can be interrupted and resumed.
- Strict C89, no dependencies beyond libc and POSIX. Linux (glibc and musl), macOS and
  FreeBSD, including 32-bit systems with files over 4 GiB.

## Build and install

A C89 compiler and **GNU make** are required (on FreeBSD, run `gmake`).

    make                         # builds build/hashdiff
    make install                 # installs to /usr/local/bin (PREFIX=/usr/local)
    make install PREFIX=$HOME/.local

`CC` may include flags (`make CC="gcc -m32"`) and `CFLAGS_EXTRA` adds flags after the
warning flags (`make CFLAGS_EXTRA=-Werror`).

## Usage

    hashdiff ORIGIN DESTINATION [OPTIONS]

Options may appear before, between or after the two paths.

    -o, --output DIR        Existing directory where DIR/results.hashdiff/ is created (default: .)
        --resume            If results.hashdiff exists, resume the interrupted run without asking
        --force             If results.hashdiff exists, discard it and start over without asking
    -f, --fast              Sampled fast mode. Without it: full MD5 of everything
    -g, --gap SIZE          Maximum unread region between two samples (default: 64M)
    -b, --block SIZE        Bytes read per sample (default: 64K for ssd, 1M for hdd)
        --profile hdd|ssd   Disk type (default: ssd), for --fast
    -j, --jobs N            Hashing processes per tree, 1..256 (default: 1)
        --serial            Process ORIGIN and then DESTINATION instead of in parallel
    -x, --one-file-system   Do not cross mount points
    -q, --quiet             No progress on stderr
    -h, --help
    -V, --version

SIZE is an integer with an optional K, M, G or T suffix (powers of 1024).

Examples:

    # Full comparison; results in ./results.hashdiff
    hashdiff /data/photos /mnt/backup/photos

    # Results somewhere else, fast mode on a hard disk, 2 GiB gap
    hashdiff /data/photos/ /mnt/backup/photos/ --output /tmp/reports --fast --profile hdd -g 2G

    # Continue a run that was interrupted (Ctrl-C, reboot...)
    hashdiff /data/photos /mnt/backup/photos --resume

## How a run works

A run has two stages.

**1. Tree stage.** Both trees are listed (names, types, sizes and modification times, no
file content is read) into `tree-origin.txt` and `tree-destination.txt`, and compared:

- If they differ (files missing in DESTINATION, files that exist only in DESTINATION,
  different sizes or types, unreadable directories), hashdiff writes `tree-diff.txt`, prints
  a suggested rsync command and **stops with exit code 4: nothing is hashed**. Fix the
  differences yourself and run hashdiff again; since nothing was hashed, it starts over
  without asking.
- The suggested command is `rsync -a 'ORIGIN/' 'DESTINATION/'`, or
  `rsync -a --delete-after ...` when DESTINATION has files that ORIGIN does not have.
  **`--delete-after` deletes those files.** Review `tree-diff.txt` before running it. The
  command excludes results.hashdiff and a nested tree, so they are never deleted.
- Paths that could not be read are reported, and you are asked to fix their permissions.

**2. Hash stage.** Only when both trees have exactly the same files, types and sizes, every
file is hashed and the lists are compared. Files whose content differs are listed in
`diff-files.txt` and `rsync-files.lst`, and hashdiff prints the command that copies exactly
those files:

    rsync -a -I --from0 --files-from='/abs/results.hashdiff/rsync-files.lst' '/abs/origin/' '/abs/destination/'

`-I` is essential: without it rsync's quick check (size + mtime) would skip corrupted files
that kept their size and modification time.

**hashdiff never runs rsync**: you review the commands and run them yourself. Do not run any
rsync command on these trees while hashdiff is running.

## Output files

All files are in `DIR/results.hashdiff/`; their names are fixed.

| File | Stage | Content |
|---|---|---|
| `tree-origin.txt`, `tree-destination.txt` | tree | `TYPE SIZE MTIME PATH` per file and symlink |
| `tree-diff.txt` | tree | ORIGIN vs DESTINATION, in an `## origin` and a `## destination` section |
| `hashes-origin.txt`, `hashes-destination.txt` | hash | `TYPE HASH SIZE PATH` per file and symlink |
| `diff-files.txt` | hash | `STATUS PATH` for every path whose content differs |
| `rsync-files.lst` | hash | paths to copy, each terminated by `\0` |
| `rsync-command.txt` | hash | the rsync command above (empty if there is nothing to copy) |
| `tree-changes.txt` | resume | what changed in the trees since the interrupted run |

Paths are relative to their root and escaped in text files (`\` → `\\`, newline → `\n`,
carriage return → `\r`); `rsync-files.lst` holds the raw bytes. Hash types: `F` full MD5,
`S` sampled hash, `L` symlink (MD5 of its target), `E` error (the hash field is the errno).
Statuses: `MISSING`, `EXTRA`, `SIZE`, `TYPE`, `HASH`, `ERR-SRC`, `ERR-DST`.

FIFOs, sockets and devices are ignored (and counted in the summary). Directories have no
entries of their own, so empty directories are not compared.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | no differences |
| 1 | differences found |
| 2 | fatal error (also: "abort" at the prompt, trees changed when resuming) |
| 3 | completed, but some files could not be read (with or without differences) |
| 4 | the trees differ: nothing was hashed |
| 128+N | interrupted by signal N (130 for Ctrl-C) |

## Fast mode

With `--fast`, a large file is not read completely. hashdiff reads samples of `--block`
bytes at evenly spaced offsets, always including the first and the last block, so that the
unread region between two samples is never larger than `--gap`.

**Guarantee: any contiguous damage larger than `--gap` is always detected.** Smaller damage
is detected only if it touches a sample. A smaller gap reads more and detects more.

The plan depends only on the file size and on `--gap`, `--block` and `--profile`, so both
trees make exactly the same decisions on any machine. Files of at most two blocks, and files
for which sampling would not be cheaper than a full read, are read completely (type `F`).

`--profile` describes the disk: `ssd` (default) or `hdd`. It sets the default block (64K or
1M) and the cost of a seek, which decides when sampling pays off and when samples that are
close together are read through. With `--profile hdd` and `-j 1`, files are read in inode
order to reduce head movement; several readers on the same hard disk usually reduce
throughput, so `-j > 1` prints a warning. Without `--fast`, `--gap`, `--block` and
`--profile` have no effect (a warning says so).

**An `S` hash is not the MD5 of the file** and cannot be compared with `md5sum`: it is the
MD5 of the file size (8 little-endian bytes) followed by the sampled blocks. Lists made with
different parameters cannot be compared; the `# mode:` line of the hashes files records
them.

## Resume

The hashes files are written progressively, in order, as `hashes-*.txt.tmp`. If a run is
interrupted (Ctrl-C, `kill`, crash, power loss), run the same command again. When
`results.hashdiff` exists:

- with `--resume`, hashdiff continues the interrupted run;
- with `--force`, it starts over;
- with neither, on a terminal it asks `[r]esume, [o]verwrite or [a]bort?`; in a script it
  stops with an error that suggests `--resume` or `--force`.

When resuming, hashdiff lists both trees again and compares them with the saved
`tree-*.txt`, including modification times. If anything changed, it writes
`tree-changes.txt` and stops (exit 2): restore the trees, or use `--force`. Otherwise each
side continues from its last valid line: an incomplete or damaged tail is discarded, and the
last kept file is hashed again to verify it.

**Limitation:** resume reuses the results of files whose type, size and modification time
(in seconds) did not change. A file modified within the same second while keeping its size,
or whose modification time was restored, is not read again. For a result that reflects the
current content of every file, use `--force`.

## Running the tests

The tests are written in Python (3.10 or newer) with pytest and hypothesis. Create a
virtual environment in `tests/venv` (ignored by git), install the dependencies and run them:

    python3 -m venv tests/venv
    make test-deps PYTHON=tests/venv/bin/python
    make test PYTHON=tests/venv/bin/python

`make test` builds `build/hashdiff` and the test-only driver `build/hashdiff-testhook`, then runs
`python -m pytest tests`. Slow tests (a full read of a sparse file over 4 GiB) run only with
`HASHDIFF_SLOW_TESTS=1`:

    HASHDIFF_SLOW_TESTS=1 make test PYTHON=tests/venv/bin/python

To run the suite under AddressSanitizer and UndefinedBehaviorSanitizer:

    make asan
    make test PYTHON=tests/venv/bin/python

Tests that need something unavailable are skipped with a reason: rsync not in PATH,
running as root (permission tests), no sparse files, or a filesystem that rejects
non-UTF-8 file names (macOS APFS).

Continuous integration runs on Linux (gcc, clang, 32-bit, arm64, musl, sanitizers), macOS
and FreeBSD.

## License

GNU General Public License v3.0; see [LICENSE](LICENSE).
