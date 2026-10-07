# Release notes

One section per version, newest first. The **Changes** list of a version is the text that
goes into the `<!-- Write the summary of this release here -->` placeholder of
`.github/release-notes.md` when the draft release for its tag is reviewed and published, so
the two never say different things.

Below the versions, **Design notes** keeps the reasoning behind what shipped, and **Backlog**
what has not been designed yet. For anything implemented, `docs/implementation.md` and the code
are the truth; the notes are the record of what was decided and why.

## v2.0 (unreleased)

Until the tag exists, use the v1.0 release:
<https://github.com/maagenta/hashdiff/releases/tag/v1.0>

This version was called 1.1 while it was being written, and the commits up to and including
"Phase 16: the v1.1 changes list and the version decision" name it that way, as do the phase
descriptions of section 12 of `docs/implementation.md`. It was never released under that
number. It became 2.0 because a `results.hashdiff` written by 1.0 cannot be read any more: a
minor version must not invalidate a run already under way, and reserving the major for it
leaves the number saying so to anyone who upgrades without reading these notes. The command
line itself is compatible: no flag of 1.0 changed meaning, and with one destination every file
name and every line of the summary is what it was.

### Changes

**The result files are not compatible with v1.0 and v1.0 cannot read these.** The hashes files
are `# hashdiff-format: 3`, `diff-files.txt` has two more fields, and a v1.0 results directory
is refused with a message that says so instead of being misread. Finish or discard a v1.0 run
before upgrading.

New:

- **Several destinations.** `hashdiff ORIGIN DEST1 DEST2 ...` compares 1 to 64 copies of
  ORIGIN in one run. Each is compared with ORIGIN and never with another, and ORIGIN is
  traversed and hashed once for all of them, so one more destination costs one more read of
  that destination only. Each gets its own result files, its own summary line and its own rsync
  command, and a destination whose tree differs no longer stops the others: it is reported and
  skipped while the rest are hashed. `--number-of-destinations N` fails before anything is read
  when the count is not N, for scripts where a mistyped path would otherwise become one more
  destination in silence. With one destination every file name and every line of the summary is
  what it was, so nothing changes for the common case.
- **`--file`** compares regular files instead of directories: one disk image verified against
  its copies. The files may have different names, so they are compared by position; the command
  names both files and no `rsync-files.lst` is written, because a list holding ORIGIN's name
  would copy it into the destination's directory.
- **`--recheck`** archives a finished run that found differences into
  `results.hashdiff/scan-YYYYMMDDHHMM/` and reads only the paths that differed, which is what
  you want after running the rsync command. Exit 0 then means those paths match now, not that
  the trees match, and the summary says so.
- **One run at a time.** Every run takes an advisory lock on `results.hashdiff/lock`, so two
  runs can no longer append to the same journal and leave a file that a later `--resume` would
  trust. The kernel releases it on every exit path, so nothing stale is left behind.
  `--ignore-lock` is the way past it.
- **`paths.txt`** records the roots of the run and **`history.txt`** keeps one block per run for
  the life of the directory, oldest first; it is the one file nothing ever deletes, so it says
  what the directory has been used for.
- The result files now record when they were written: `# started:`, one `# resumed:` per resume
  and `# finished:` in the hashes files, and a footer in `diff-files.txt` repeating that
  destination's summary. Two runs over the same trees still produce the same files once those
  lines are removed.
- `diff-files.txt` carries the two hashes of every differing path, before the path, so finding
  out how two files differ no longer means grepping both hashes files by hand.

Changed:

- **Paths are cleaned** before anything else: a relative path is taken from the current
  directory and `.`, `..` and repeated slashes are removed. The default `--output .` used to
  reach the summary as `/dir/./results.hashdiff`, and a run started as `./o` and resumed as `o`
  used to be refused with "belongs to another root", losing everything it had hashed. Any
  spelling of the same directories is now one run.
- **An existing `results.hashdiff` is read, not just noticed.** hashdiff says whether the
  previous run was interrupted, finished clean or finished with differences that were never
  copied, and each question offers only what that state allows; `--resume` over a finished run
  is now an error instead of re-hashing its last file. A previous run over other directories is
  never a question: it stops and names the role that changed.
- **Exit codes have a documented precedence**, 2 > 4 > 3 > 1 > 0, because a run can now end
  with its destinations in different states. With one destination nothing changes.
- The summary: `no differences: ORIGIN and DESTINATION match` when every hash matches, which
  appears exactly when hashdiff exits 0; the rsync command is followed by a NOTE saying what it
  copies and where the copy of it is; counts next to a noun use the singular at 1, verb
  included; and `elapsed:` always carries a rate field, `-` under a second, so the line has one
  shape.
- CI: a `linux-arm64` job (`ubuntu-24.04-arm`, gcc) runs the suite on aarch64. The release
  workflow already built the `linux-arm64` archive; now that target is tested too.

### Release checklist

1. The version appears in three places and all of them must agree: `HD_VERSION` in
   `src/config.h`, the `--version` line in section 2 of `docs/implementation.md`, and
   `test_version` in `tests/test_cli.py`. The release workflow refuses to publish a tag
   whose name does not match `HD_VERSION`.
2. Write the Changes list from `git log vPREV..HEAD`: anything that touches the CLI, the
   output files, the README or the set of tested platforms belongs in it.
3. Push the tag, let the workflow build the five archives and `SHA256SUMS`, review the
   draft and publish it with `gh release edit vX.Y --draft=false`.

## Design notes

All ten items below shipped in v2.0, as phases 9 to 15 of section 12 of
`docs/implementation.md`. **The specification and the code are the truth now**; these notes are
kept for the intent behind each item, for the obstacles that were found before writing any of
it, and for the record of what every **Decide** was resolved to. Each one says which section of
the specification holds it.

### 1. Relative paths in ORIGIN, DESTINATION and --output

*Specified in section 2.1 of `docs/implementation.md`; phase 9.*

`absolute_path()` (`src/main.c:62`) is `getcwd()` plus the path exactly as it was typed, so
every `.` or `..` survives into each place an absolute path is printed or stored. The
default is affected, not only an explicit `--output .`:

    $ hashdiff o d                        # --output defaults to "."
    results: /home/user/./results.hashdiff
    $ hashdiff o d --output ..
    results: /home/user/x/../results.hashdiff

The same text goes into the `# root:` line of `tree-*.txt` and `hashes-*.txt`, into the
suggested rsync commands and `rsync-command.txt`, into the
`results.hashdiff already exists in .:` prompt and into the
`'./results.hashdiff' already exists` error.

It is not only cosmetic. The resume check compares `# root:` as text, so one run started
and resumed with two spellings of the same directory is rejected and everything it hashed
is lost:

    $ hashdiff ./data /mnt/backup          # interrupted after hours
    $ hashdiff data /mnt/backup --resume
    hashdiff: cannot resume: 'results.hashdiff/tree-origin.txt' belongs to another root;
    use --force to start over

Fix: normalize ORIGIN, every destination and `--output` once, right after parsing, and use
the normalized form everywhere. Normalization is lexical: collapse repeated slashes, drop
`.` components, pop a component for `..`, and strip trailing slashes from `--output` as
well, which today is only done for the two tree paths. `realpath()` is not an option:
sections 1 and 8 of the specification build the absolute path without resolving symlinks,
and resolving them now would make every existing results directory unresumable.

- **Decide:** popping `..` lexically is what `cd` does without `-P`; it names a different
  directory than the kernel reaches when the previous component is a symlink to a
  directory. Recommendation: normalize it anyway and say so in the README, since the
  alternative is to keep `..` in the stored root and keep the resume mismatch. The
  conservative variant is to normalize only `//` and `.`, which is always exact and already
  fixes the default case.
- Tests: the same tree compared as `o`, `./o`, `o/`, `./././o` and an absolute path must
  give byte-identical result files; an interrupted run resumed with another spelling of
  both paths must resume instead of failing.

### 2. Explain the rsync command at the end of a run

*Specified in section 9 (the summary, item 9); phase 9.*

When the hash stage found something to copy, the command is printed and then, right after
it:

    NOTE: the command above copies with rsync only the files whose content did not match.
          There is a copy of it in results.hashdiff/rsync-command.txt

- Printed only when there is something to copy, which is exactly when the command line
  itself is printed.
- With several destinations (item 7) it names
  `results.hashdiff/rsync-command-destination-N.txt`.
- **Decide:** stdout, right below the command, or stderr with the other advisory messages.
  Recommendation: stdout, because the command it refers to is on stdout; the tree stage
  already prints prose there (`suggested command (review it first...)`).

### 3. The two hashes of a file that did not match

*Specified in section 9 (the diff-files format); phase 11.*

`diff-files.txt` holds `STATUS escaped-path` and nothing else, so finding out *how* two
files differ means grepping both `hashes-*.txt` by hand. The diff has both records in hand
when it writes the line, so the hashes are free.

- The hashes go *before* the path, never after it. Section 8 makes the path the last field
  precisely because it may contain spaces, so anything appended after it would make the
  line unparseable:

      HASH d41d8cd98f00b204e9800998ecf8427e 900150983cd24fb0d6963f7d28e17f72 docs/report.pdf

- **Decide:** what goes in the two fields for the statuses that are not HASH.
  Recommendation: ORIGIN hash then DESTINATION hash, `-` in the field of a side that has no
  entry (MISSING, EXTRA) and the decimal errno for ERR-SRC and ERR-DST, which is what
  `hashes-*.txt` already does for an E entry.
- The `# mode:` header of `diff-files.txt` already says whether these are full MD5s (`F`)
  or sampled hashes (`S`); an `S` hash still cannot be compared with `md5sum`.
- This changes the format of `diff-files.txt`, so it goes with the bump of item 4 and the
  parser in `tests/conftest.py` has to follow.

### 4. Start and finish times in the output files

*Specified in sections 8, 7 and 9; phase 11.*

`hashes-origin.txt` and `hashes-destination.txt` get a start line, plus a finish line when
that side completes, and `diff-files.txt` gets a finish block with the summary of the run.
Three things have to be settled first, because the shape asked for breaks the tool:

- **The lines must start with `#`.** Sections 8 and 9 define these files as `#` header
  lines followed by records, and every reader treats anything else as a record. A
  `---- Hash Scan Finished at ... ----` line in a hashes file makes `diff.c` die with
  `malformed line`, so the run fails at the diff after hashing everything; the resume reader
  stops at the first line that is not a valid record, so it would quietly treat a complete
  journal as an interrupted one and re-hash its last file. Use `# started:` and
  `# finished:` instead of the `-----` decoration.
- **Three readers have to be taught about them.** `resume_check_hashes` (`src/diff.c:555`)
  reads the first three lines positionally; `load_kept` (`src/hasher.c:441`) skips exactly
  three header lines and parses the fourth as a record, so one extra header line silently
  reduces the resumable prefix to nothing; `stream_open` (`src/diff.c:216`) dies with
  `unknown header line` on any `#` header it does not know, and hits `malformed line` on a
  `#` line that comes after the records. Keep `# hashdiff-format:`, `# root:` and `# mode:`
  as the first three lines, make the readers skip every leading `#` line and accept
  trailing ones, and raise `# hashdiff-format: 2` to `3` so a directory written by 1.0 is
  rejected with a clear message instead of being misparsed. The literal
  `"# hashdiff-format: 2"` appears in `src/diff.c` twice.
- **A timestamp breaks the byte-for-byte guarantee.** Section 7 requires the result files
  to be identical with any `-j`, with `--serial`, and between a resumed and an uninterrupted
  run; every "identical" assertion in `tests/test_resume.py` and test 14 compares them byte
  for byte. The timestamp lines have to be excluded from that comparison, in the
  specification and in the test helpers.
- A footer is **not** how a finished run is recognised (item 6): `hashes-SIDE.txt` is
  published with `rename()` only when that side finished, and the existence of
  `hashes-SIDE.txt.tmp` is what says it did not. The footer is for the person reading the
  file. For the same reason the finish block of `diff-files.txt` needs no "if the process
  finished successfully" condition: that file is only published at the end of a successful
  run.

**Decide:** one timestamp format everywhere. `dd/mm/yy` in the text contradicts `12/12/2026`
in the example, and `12/12/2026 at 12:30PM` and `16/12/2026 - 12:30PM` are two separators
for the same thing. Recommendation: `# started: 16/12/2026 12:30` and
`# finished: 17/12/2026 04:30`, local time, 24-hour, written with
`strftime("%d/%m/%Y %H:%M", localtime(&t))` (both are C89, so section 1 only gains two
allowed calls). `%I` with `%p` is avoided on purpose: `%p` is locale-dependent and the
standard allows it to be empty.

**Decide:** what a resumed run records. Recommendation: keep the `# started:` of the
interrupted run and add one `# resumed:` line per resume, so the file shows how long the
whole comparison really took.

The finish block of `diff-files.txt` is the summary that is printed on stdout, with every
line prefixed by `# `, here for a `--fast --profile hdd` run:

    # finished: 12/12/2026 12:30
    # origin: 6669 files, 33.5 GiB read, 0 ignored
    # destination: 6669 files, 33.5 GiB read, 0 ignored
    # elapsed: 590 s, 121.9 MB/s
    # sampled files: 13312, 66560 samples, 65.0 GiB read of 3.3 TiB (coverage 1.91 %)
    # differences: 1 HASH, 0 MISSING, 0 EXTRA, 0 SIZE, 0 TYPE, 0 ERR-SRC, 0 ERR-DST

With several destinations the block goes into each `diff-files-destination-N.txt` and holds
only that destination's own counts.

### 5. paths.txt

*Specified in section 3.2; phase 11.*

Written in the tree stage, before anything is hashed, so an interrupted run leaves it
behind:

    # hashdiff-paths: 1
    origin /abs/path/origin
    destination-1 /abs/path/destination-1
    destination-2 /abs/path/destination-2

- Absolute paths, escaped like every other text file (section 8: `\` to `\\`, LF to `\n`,
  CR to `\r`). A two-line `LABEL` then path layout cannot hold a path that contains a
  newline, and such names are one of the things hashdiff is built to handle.
- One line per side, label first and path last, like every other format of the project; the
  labels are the side names used in the summary and in the file names, so a reader and the
  code agree on them.
- Its job is to let item 6 say what the previous run compared and how many destinations it
  had, which is also what makes the cleanup of item 7 exact.
- It must be added to the table in section 3 of `docs/implementation.md`, to the table in
  the README and to `result_files[]` in `src/main.c`, or `--force` will leave it behind.

### 6. What to do when results.hashdiff already exists

*Specified in sections 3.3 (the three states) and 3.5 (the recheck); phases 12 and 13.*

Today: with no `hashes-*` file there is nothing to resume and the run starts over without
asking; otherwise `--resume` resumes, `--force` starts over, a terminal is asked
`results.hashdiff already exists in DIR: [r]esume, [o]verwrite or [a]bort?`, and without a
terminal the run stops with an error naming the two flags. What follows refines that prompt
using `paths.txt` and the three states a previous run can be in. Two rules first, because
the item as written breaks them:

- **Every new question needs a flag and a defined behaviour without a terminal.** A
  question with no flag makes hashdiff unusable from cron, from a script and from the test
  suite. Each new choice gets its own flag, its own fatal error when stdin or stderr is not
  a tty, and a test through a pseudo-terminal like test 21.
- **A different root can only mean "start over".** "Ask whether to continue anyway" cannot
  be offered: lists made from another tree are not comparable, which is why section 3.4
  makes a different `# root:` fatal. In that case the choice is overwrite or abort, never
  resume.

The three states are told apart by the files themselves, with no new bookkeeping: a
`hashes-SIDE.txt.tmp` means that side was interrupted, both sides published as
`hashes-SIDE.txt` mean the run finished, and a finished run had differences exactly when
`diff-files.txt` holds at least one record line.

**State 1, the previous run was interrupted** (at least one `hashes-SIDE.txt.tmp` exists;
with `--serial` the other side may already be published as `hashes-SIDE.txt`):

- Compare `paths.txt` with the current command line. A different number of destinations, or
  a different origin or destination, is a fatal error that names which one changed and
  suggests `--force`. It is not a question.
- Then ask `[r]esume, [o]verwrite or [a]bort?` as today.
- Resuming keeps the per-side rule of section 3.4: each journal is truncated at its own last
  valid line and each side continues from there. **Do not** cut the side that got further
  down to the shortest one. Section 3.2 says in so many words that a side that was ahead
  loses nothing, and truncating it would throw away hours of hashing for no gain, since the
  two sides are never compared until both lists are complete.
- There is no "a file is missing in the middle of the list" case to detect, and no repair to
  write for it. A journal is written strictly in canonical order and its written prefix only
  advances when every earlier entry is complete, so a hole cannot exist. What the item asks
  for is already there and is stricter: `load_kept` (`src/hasher.c:441`) checks every kept
  line against the entry at that position in the current list, by path and by type, and
  stops at the first that does not match; and before hashing, each side compares its whole
  tree with the saved `tree-SIDE.txt` on type, size **and** mtime and stops with
  `tree-changes.txt` if anything moved.

**State 2, the previous run finished with no differences:** say so, with the date from
item 4, and offer overwrite or abort. `[r]esume` should not be offered here: there is
nothing to resume, and resuming a finished run only re-hashes its last file and prints the
same result again. Distinguishing this from state 1 in the prompt is most of the value of
this item.

**State 3, the previous run finished with differences:** say that the differences were never
copied, that the rsync command in `rsync-command.txt` is what copies them, and offer:

- *recheck*: move the files of the previous run into `results.hashdiff/scan-YYYYMMDDHHMM/`
  and hash only the paths listed in its `diff-files.txt`;
- *overwrite*: a full run, as `--force` does today;
- *abort*.

Each needs its flag; recommendation: `--recheck` for the first one. Points to settle:

- **Move, do not copy.** The hashes files hold about 60 bytes per file, so a few million
  files make hundreds of megabytes; `rename()` inside the same directory is free and atomic,
  a copy is neither.
- The name uses the **previous** run's start time, not the current one, so that it
  identifies the scan it holds; `YYYYMMDDHHMM` sorts chronologically, which is why it is not
  the `dd/mm/yyyy` of item 4. Two runs started in the same minute need a suffix.
- The previous `diff-files.txt` is read **after** the move, from inside the archive, and its
  `# mode:` line must match the current one, exactly as resume requires: sampled hashes made
  with other parameters are not comparable.
- It also contains EXTRA (not in ORIGIN) and ERR-SRC / ERR-DST paths. **Decide** which are
  re-read; recommendation: skip EXTRA, re-read the ERR ones, since a permission that was
  fixed is the usual reason to recheck.
- A recheck is **not** a full comparison: it says only whether the paths that differed
  before match now. Nothing else in either tree is read, so MISSING and EXTRA elsewhere
  cannot be found. The summary must label it as such, the counts and the coverage describe
  only the rechecked subset, and exit 0 must not be read as "the trees match". **Decide**
  how the summary says this.
- `clean_results()` only deletes the names it knows, so `--force` leaves the `scan-*`
  archives in place. Nothing prunes them: say so in the README.
- `paths.txt` is kept per scan the way the item asks, newest first. Recommendation: one
  `history.txt` appended to by each run instead of renaming `paths.txt` to `paths-old.txt`
  and rewriting it; "append, newest on top" contradicts itself, and the archive of the
  previous run already carries that run's own `paths.txt`. If `paths-old.txt` is kept
  anyway, each block needs the run's start time so blocks can be told apart.
  (Specified as `history.txt` in section 3.2: appended, oldest first, and the one file that
  neither `--force` nor a recheck ever removes.)

### 7. One origin against several destinations

*Specified in sections 2.2, 3, 3.1, 7 and 9; phase 10, which comes first of the three big
ones because it is what makes a side name the key of every output file and label.*

    hashdiff ORIGIN DEST1 [DEST2 ...]

Each destination is compared with ORIGIN, never with another destination. ORIGIN is
traversed and hashed once and its lists are reused for every comparison, so one more
destination costs one more tree and one more hash of that destination only.

**File names.** With one destination the current names stay exactly as they are, so the
common case is unchanged. With two or more, every per-destination file is numbered in the
place of the word it replaces:

    tree-origin.txt                hashes-origin.txt
    tree-destination-1.txt         hashes-destination-1.txt
    tree-destination-2.txt         hashes-destination-2.txt
    tree-diff-destination-1.txt    diff-files-destination-1.txt
    tree-diff-destination-2.txt    diff-files-destination-2.txt
    rsync-files-destination-1.lst  rsync-command-destination-1.txt
    rsync-files-destination-2.lst  rsync-command-destination-2.txt
    paths.txt                      tree-changes.txt                 (never numbered)

`tree-diff-*` and `diff-files-*` are per destination too; the list in the first draft of
this item left them out, and `tree-changes.txt` stays one file with one `##` section per
side. Numbers are the position on the command line, from 1. **Decide:** one word for a
destination everywhere. Recommendation: `destination-1`, as above, matching the existing
`tree-destination.txt` and the summary labels; `dest1` and `destination-1` in the same
directory is the kind of thing that costs an hour later.

**Summary**, here with `--fast -g 512K --profile hdd` and two destinations:

    results: /home/user/results.hashdiff
    origin: 6669 files, 2.0 TiB read, 0 ignored
    destination-1: 6669 files, 2.0 TiB read, 0 ignored
    destination-2: 6669 files, 2.0 TiB read, 0 ignored
    elapsed: 52776 s, 125.0 MB/s
    sampled files: 1152, 6292224 samples, 6.0 TiB read of 9.0 TiB (coverage 66.67 %)
    differences with destination-1: 1 HASH, 0 MISSING, 0 EXTRA, 0 SIZE, 0 TYPE, 0 ERR-SRC, 0 ERR-DST
    differences with destination-2: 0 HASH, 0 MISSING, 0 EXTRA, 0 SIZE, 0 TYPE, 0 ERR-SRC, 0 ERR-DST
    command for destination-1:
    rsync -a -I --from0 --files-from='/home/user/results.hashdiff/rsync-files-destination-1.lst' '/origin/portable.hardisks/' '/destination-1/portable.hardisks/'
    NOTE: the command above copies with rsync only the files whose content did not match.
          There is a copy of it in results.hashdiff/rsync-command-destination-1.txt

- Counts stay on one line per destination, as `differences:` is today, so the summary can
  still be read with grep, and the rsync command stays on one line so it can be copied.
  The trade-off is width: with the label the counts line is 100 characters and wraps on an
  80-column terminal, which is what the label-then-counts form first drafted here avoided.
- The numbers above are consistent with each other on purpose: three sides at 2.0 TiB are
  6.0 TiB in 52776 s, which is the 125.0 MB/s shown; 6292224 samples of the 1 MiB block of
  `--profile hdd` are the 6.0 TiB sampled, which is 66.67 % of 9.0 TiB. The figures in the
  first draft of this item could not occur: they described two sides, and 66556 samples
  reading 4.0 TiB would be 63 MiB per sample.
- With one destination the labels stay `destination:`, `differences:` and the bare command.
- `no differences: ORIGIN and DESTINATION match` needs a plural form. Recommendation: keep
  it verbatim for one destination, and for several print one
  `no differences with destination-N` line per destination, so a per-destination grep works
  the same in both cases.
- Progress lines are tagged `[destination-1]`, `[destination-2]`.

**Decide: the exit code.** Today a tree that differs means nothing is hashed at all
(exit 4). With four backup disks, one that differs should not stop the other three.
Recommendation: hash every destination whose tree matches, report the others as
tree-differing, and exit with the most severe status under a documented precedence,
2 > 4 > 3 > 1 > 0. With one destination nothing changes; with several, exit 4 comes to mean
"at least one destination's tree differs" and the per-destination lines say which.

**Decide: `--number-of-destinations`.** It is not needed to parse the command line: the
parser knows which options take a value, so variadic paths are unambiguous even with
options between them. Its only value is as an assertion, and that is real: in a script a
mistyped or glob-expanded path would otherwise become one more destination silently.
Recommendation: keep it, defined as "fail before anything is read if the number of
destination paths is not N", or drop it. Not both meanings.

Everything else this item needs:

- All roots must be distinct: the current ORIGIN-equals-DESTINATION check becomes a pairwise
  check over all D+1 roots by `st_dev` and `st_ino`, including destination against
  destination.
- Nesting: if any root is inside another, or `results.hashdiff` is inside any root, it is
  excluded from that traversal with a warning, as today. The **union** of the excluded paths
  must go into every suggested command: a destination nested inside another is not in
  ORIGIN, so a `--delete-after` command for destination-1 would otherwise delete
  destination-2.
- Limits and processes: D from 1 to a documented maximum (**Decide**; recommendation 64).
  Hashing processes become (1 + D) x `--jobs`, which the `-j` documentation has to say.
  `--serial` means ORIGIN's tree, then each destination's tree, then ORIGIN's hashes, then
  each destination's hashes. Several destinations read at once on the same bus or USB hub
  reduce throughput the same way `-j > 1` does on one disk, and deserve the same warning.
- Cleanup cannot keep using a fixed list. `result_files[]` (`src/main.c:22`) and
  `has_hashes()` know two sides by name, so a two-destination run with `--force` in a
  directory left by a three-destination run would leave every `*-destination-3.*` file
  behind, looking like part of the new result. The cleanup has to read the directory and
  delete by known name and numbered pattern, keeping the `scan-*` archives of item 6;
  `has_hashes()` has to look for any `hashes-destination*.txt` or `.tmp`.
- The diff compares `hashes-origin.txt` with each destination's list in its own streaming
  merge-join, re-reading the origin file once per destination, so memory stays constant and
  each per-destination output is independent.
- `sides[2]`, the `[2]` dimensions and the two-element loops throughout `src/main.c` become
  D+1 sides.

### 8. Comparing single files (--file)

*Specified in section 2.3; phase 14.*

With `--file`, ORIGIN and the destinations are regular files instead of directories.

- A symlink given as an argument is followed, as the roots already are. Every path must be
  a regular file: a directory with `--file`, or a FIFO, socket or device with or without
  it, is a fatal error.
- The run keeps both stages and every output file. The root of a side is the file's parent
  directory and its list has exactly one entry, so the formats, the diff and the rsync
  command keep working with no special case.
- **Decide:** the two files may have different names (`hashdiff a.iso backup/b.iso`). With
  the names in the lists, the merge-join reports MISSING and EXTRA instead of HASH, which is
  useless. Recommendation: in `--file` mode the single entries are compared positionally,
  ignoring the names, so the only statuses are HASH, SIZE, TYPE, ERR-SRC and ERR-DST; each
  file still appears in its own list under its own name.
- Messages: when a path is a regular file and `--file` was not given,
  `hashdiff: ORIGIN 'file' is a file, not a directory; use --file to compare files`
  (today: `hashdiff: ORIGIN 'file' is not a directory`). With `--file` and a directory,
  `hashdiff: ORIGIN 'dir' is a directory, not a file; drop --file`. The `hashdiff: ` prefix
  and the lowercase style of every other message are kept, and `--file` is only suggested
  when the path really is a regular file or a symlink to one, never for a FIFO or a device.
- `--file` and several destinations together are allowed:
  `hashdiff disk.img /mnt/a/disk.img /mnt/b/disk.img`.
- Resume granularity is one file, so an interrupted `--file` run has no complete entry to
  keep and reads the file again from the start. It is exactly the case where resume would be
  worth most, since the file is typically a disk image. Say so in the README, or make
  resuming inside a single file an item of its own.

### 9. Smaller things

*Specified in section 9 (the summary); phase 9.*

- The summary says `1 files`. `%lu file%s` with `n == 1 ? "" : "s"` fixes it in the three
  places that print a count of files.
- `elapsed: 0 s` has no MB/s field, while `elapsed: 590 s, 121.9 MB/s` has one, so the line
  has two shapes. It does not matter while the summary is only read by a person, but item 4
  stores it inside `diff-files.txt`, where a parser then has to handle both. Printing
  `elapsed: 0 s, - MB/s` keeps one shape.

### 10. Refuse to touch a results.hashdiff that another run is using

*Specified in section 3.6; phase 15.*

The worst case this prevents is real and silent: two hashdiff runs on one results.hashdiff
append to the same journal, and what is left is a file that is not a canonical prefix of
anything, which the resume of section 3.4 then trusts and reuses.

A lock does it, in POSIX and nothing else:

1. `results.hashdiff/lock`, opened once at startup and held open for the whole run, with an
   advisory write lock taken through `fcntl(F_SETLK)`. The kernel releases it when the process
   dies, so a crash, a `kill -9` or a power loss leaves nothing stale behind and no "is this
   lock still valid" guesswork. The lock belongs to the parent; the children inherit the
   descriptor but hold nothing, and the parent outlives them by design (section 7).
2. When it is already held, `fcntl(F_GETLK)` fills in `l_pid`, the pid of the holder:

       hashdiff: results.hashdiff in DIR is in use by process 48213.
       continue anyway? [y]es or [a]bort?

   `y` continues, `a` exits 2, any other answer repeats the question and EOF exits 2, as in
   section 3.3. Over NFS `l_pid` is a pid on another machine and means nothing locally, so the
   message then says only that the directory is in use.
3. No process name. Translating a pid to a name is the one piece that is not POSIX, and it
   would cost three backends: `/proc/<pid>/comm` on Linux, `proc_pidpath` on macOS, and
   `libprocstat` on FreeBSD, which is a dependency outside libc that section 1 forbids.
   Calling `lsof` is not an option either: hashdiff never executes an external command, it
   does not even run the rsync command it prints. The pid is enough to find the process with
   `ps`.
4. Every run takes the lock, not only a resume or an overwrite. Two runs starting at the same
   moment on a fresh directory are as bad as one overwriting another, and one rule is simpler
   than two.
5. The flag for scripts is `--ignore-lock`, and without a terminal the run stops with a fatal
   error that names it. Not `--force`: --force means "discard these results", and the user who
   wants to discard them is precisely the one who must not do it while another run is writing
   them.
6. `lock` goes into the table of section 3, is never removed by the cleaning of section 3.3
   and is never moved into a `scan-*` archive by section 3.5, like `history.txt`.

What it does not cover, and the README should say so: only runs that take the lock are seen,
so an editor holding `diff-files.txt` open or a `cp` copying the directory is invisible; and
`fcntl` locks over NFS depend on the server's lock manager and may be ignored without saying
so, so the lock is not a guarantee on a network filesystem. The lock covers
results.hashdiff and never ORIGIN or the destinations, where the existing rule stands
unchanged (do not run rsync on the trees while hashdiff is running).

Tests: a helper process holds `fcntl.lockf` on `results.hashdiff/lock`; hashdiff prints the
message with that pid, exits 2 without a terminal and proceeds with `--ignore-lock`. The helper
killed with `SIGKILL` leaves no lock and the next run takes it without asking. Two hashdiffs
started at once on a fresh directory: the second one stops.

## Backlog

Nothing here right now: everything asked for so far is specified. An item lands here when it
is still an intent, and moves up to **Specified** once its **Decide** points are answered and
it has a section, its tests and a phase in `docs/implementation.md`.

## v1.0

First release: <https://github.com/maagenta/hashdiff/releases/tag/v1.0>

Five prebuilt archives per tag (Linux x86-64 and arm64, statically linked against musl;
macOS Intel and Apple Silicon; FreeBSD amd64) plus `SHA256SUMS`, built by the release
workflow and published by hand from a draft.
