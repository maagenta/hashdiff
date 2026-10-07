"""Tests 36 to 38: timestamps, the diff-files format and the shape of the summary.

Sections 8 and 9 of docs/implementation.md.
"""
import datetime
import os
import re

from conftest import read_bytes, skip_if_root, strip_time


def parse_stamp(value):
    return datetime.datetime.strptime(value.decode(), "%d/%m/%Y %H:%M")


def header_lines(path, prefix):
    return [line for line in read_bytes(path).split(b"\n") if line.startswith(prefix)]


def test_counts_use_the_singular(compare, make_tree):
    origin = make_tree("origin", {"only": b"x"})
    destination = make_tree("destination", {"only": b"x"})
    code, out, err, _ = compare(origin, destination)
    assert code == 0, err
    assert b"origin: 1 file, " in out
    assert b"destination: 1 file, " in out
    assert b"1 files" not in out


def test_counts_use_the_plural_for_anything_else(compare, make_tree):
    spec = {"a": b"a", "b": b"bb"}
    code, out, err, _ = compare(make_tree("origin", spec), make_tree("destination", spec))
    assert code == 0, err
    assert b"origin: 2 files, " in out


def test_elapsed_always_has_the_same_shape(compare, make_tree):
    spec = {"a": b"a"}
    code, out, err, _ = compare(make_tree("origin", spec), make_tree("destination", spec))
    assert code == 0, err
    line = re.search(rb"^elapsed: (\d+) s, (-|\d+\.\d) MB/s$", out, re.M)
    assert line, out
    if line.group(1) == b"0":
        assert line.group(2) == b"-", line.group(0)


def test_the_note_follows_the_command(compare, make_tree):
    origin = make_tree("origin", {"a": b"aaa"})
    destination = make_tree("destination", {"a": b"bbb"})
    code, out, err, _ = compare(origin, destination)
    assert code == 1, err
    lines = out.split(b"\n")
    i = next(n for n, line in enumerate(lines) if line.startswith(b"rsync "))
    assert lines[i + 1] == (b"NOTE: the command above copies with rsync only the files whose"
                            b" content did not match.")
    assert lines[i + 2] == (b"      There is a copy of it in"
                            b" results.hashdiff/rsync-command.txt")


def test_no_note_when_there_is_nothing_to_copy(compare, make_tree):
    spec = {"a": b"a"}
    code, out, err, _ = compare(make_tree("origin", spec), make_tree("destination", spec))
    assert code == 0, err
    assert b"NOTE:" not in out


# ---- Test 36: the timestamps of section 8 ----

def test_a_published_journal_is_started_and_finished(compare, make_tree, parsers):
    spec = {"a": b"a", "b": b"bb"}
    code, out, err, results = compare(make_tree("origin", spec), make_tree("d", spec))
    assert code == 0, err
    for side in (b"hashes-origin.txt", b"hashes-destination.txt"):
        path = os.path.join(results, side)
        header, entries = parsers.hashes(path)
        assert header[b"hashdiff-format"] == b"3"
        parse_stamp(header[b"started"])
        assert len(entries) == 2
        finished = header_lines(path, b"# finished: ")
        assert len(finished) == 1
        assert read_bytes(path).endswith(finished[0] + b"\n")
        parse_stamp(finished[0][len(b"# finished: "):])
    # Every side records the same start, because the parent formats it once.
    starts = {parsers.hashes(os.path.join(results, s))[0][b"started"]
              for s in (b"hashes-origin.txt", b"hashes-destination.txt")}
    assert len(starts) == 1


def test_an_interrupted_journal_has_no_finished_line(run_hashdiff, make_tree, tmp_path):
    spec = {"a": b"a", "b": b"bb"}
    origin = make_tree("origin", spec)
    destination = make_tree("destination", spec)
    out = os.path.join(os.fsencode(tmp_path), b"out")
    os.makedirs(out, exist_ok=True)
    results = os.path.join(out, b"results.hashdiff")
    assert run_hashdiff(origin, destination, "--output", out)[0] == 0

    journals = []
    for side in (b"origin", b"destination"):
        final = os.path.join(results, b"hashes-%s.txt" % side)
        data = read_bytes(final)
        os.remove(final)
        with open(final + b".tmp", "wb") as f:                 # cut before the footer
            f.write(data[:data.index(b"# finished:")])
        journals.append(final + b".tmp")
    for journal in journals:
        assert b"# finished:" not in read_bytes(journal)

    code, stdout, err = run_hashdiff(origin, destination, "--output", out, "--resume")
    assert code == 0, err
    for side in (b"origin", b"destination"):
        path = os.path.join(results, b"hashes-%s.txt" % side)
        assert len(header_lines(path, b"# started: ")) == 1
        assert len(header_lines(path, b"# resumed: ")) == 1
        assert len(header_lines(path, b"# finished: ")) == 1


def test_each_resume_adds_one_line(run_hashdiff, make_tree, tmp_path):
    spec = {"a": b"a", "b": b"bb", "c": b"ccc"}
    origin = make_tree("origin", spec)
    destination = make_tree("destination", spec)
    out = os.path.join(os.fsencode(tmp_path), b"out")
    os.makedirs(out, exist_ok=True)
    results = os.path.join(out, b"results.hashdiff")
    assert run_hashdiff(origin, destination, "--output", out)[0] == 0

    for expected in (1, 2):
        for side in (b"origin", b"destination"):
            final = os.path.join(results, b"hashes-%s.txt" % side)
            data = read_bytes(final)
            os.remove(final)
            with open(final + b".tmp", "wb") as f:
                f.write(data[:data.index(b"# finished:")])
        code, _, err = run_hashdiff(origin, destination, "--output", out, "--resume")
        assert code == 0, err
        path = os.path.join(results, b"hashes-origin.txt")
        assert len(header_lines(path, b"# resumed: ")) == expected
        assert len(header_lines(path, b"# started: ")) == 1


def test_a_directory_of_an_older_version_is_refused(run_hashdiff, make_tree, tmp_path):
    spec = {"a": b"a", "b": b"bb"}
    origin = make_tree("origin", spec)
    destination = make_tree("destination", spec)
    out = os.path.join(os.fsencode(tmp_path), b"out")
    os.makedirs(out, exist_ok=True)
    results = os.path.join(out, b"results.hashdiff")
    assert run_hashdiff(origin, destination, "--output", out)[0] == 0

    for side in (b"origin", b"destination"):
        final = os.path.join(results, b"hashes-%s.txt" % side)
        data = read_bytes(final).replace(b"# hashdiff-format: 3", b"# hashdiff-format: 2", 1)
        os.remove(final)
        with open(final + b".tmp", "wb") as f:
            f.write(data)

    code, _, err = run_hashdiff(origin, destination, "--output", out, "--resume")
    assert code == 2
    assert b"written by an older hashdiff" in err and b"--force" in err


def test_two_runs_differ_only_in_the_timestamps(run_hashdiff, make_tree, tmp_path, snapshot):
    spec = {"a": b"a", "b": b"bb"}
    origin = make_tree("origin", spec)
    destination = make_tree("destination", {"a": b"a", "b": b"XX"})
    out = os.path.join(os.fsencode(tmp_path), b"out")
    os.makedirs(out, exist_ok=True)
    results = os.path.join(out, b"results.hashdiff")

    assert run_hashdiff(origin, destination, "--output", out)[0] == 1
    first = snapshot(results)
    raw_first = read_bytes(os.path.join(results, b"hashes-origin.txt"))
    assert run_hashdiff(origin, destination, "--output", out, "--force")[0] == 1
    assert snapshot(results) == first
    assert strip_time(raw_first) != raw_first       # there was something to strip


# ---- Test 37: the diff-files format of section 9 ----

def test_diff_files_carries_both_hashes(compare, make_tree, parsers):
    """MISSING and EXTRA need a tree to change during the run, which no test provokes."""
    origin = make_tree("origin", {"same": b"ss", "changed": b"aa", "with space": b"cc",
                                  "odd\nname": b"dd"})
    destination = make_tree("destination", {"same": b"ss", "changed": b"bb",
                                            "with space": b"CC", "odd\nname": b"DD"})
    code, out, err, results = compare(origin, destination)
    assert code == 1, err

    header, records, footer = parsers.diff(os.path.join(results, b"diff-files.txt"))
    assert header[b"hashdiff-diff"] == b"1"
    def digests(name):
        return {path: digest
                for _, digest, _, path in parsers.hashes(os.path.join(results, name))[1]}

    origin_hashes, dest_hashes = digests(b"hashes-origin.txt"), \
        digests(b"hashes-destination.txt")
    assert len(records) == 3
    for status, ohash, dhash, path in records:
        assert status == b"HASH"
        assert ohash == origin_hashes[path] and dhash == dest_hashes[path]
        assert ohash != dhash
    assert {r[3] for r in records} == {b"changed", b"with space", b"odd\nname"}


def test_diff_files_carries_the_errno_of_an_unreadable_file(compare, make_tree, parsers):
    skip_if_root()
    origin = make_tree("origin", {"a": b"aa", "locked": b"bb"})
    destination = make_tree("destination", {"a": b"aa", "locked": b"bb"})
    os.chmod(os.path.join(destination, b"locked"), 0)
    try:
        code, out, err, results = compare(origin, destination)
        assert code == 3, err
        _, records, _ = parsers.diff(os.path.join(results, b"diff-files.txt"))
        assert len(records) == 1
        status, ohash, dhash, path = records[0]
        assert (status, path) == (b"ERR-DST", b"locked")
        assert len(ohash) == 32 and dhash == b"13"          # EACCES in decimal
    finally:
        os.chmod(os.path.join(destination, b"locked"), 0o644)


def test_the_footer_repeats_the_summary(compare, make_tree, parsers):
    origin = make_tree("origin", {"a": b"aa"})
    destination = make_tree("destination", {"a": b"bb"})
    code, out, err, results = compare(origin, destination)
    assert code == 1, err

    _, records, footer = parsers.diff(os.path.join(results, b"diff-files.txt"))
    assert footer[0].startswith(b"# finished: ")
    parse_stamp(footer[0][len(b"# finished: "):])
    body = [line[2:] for line in footer[1:]]
    stdout = out.split(b"\n")
    for line in body:
        assert line in stdout, line
    assert [line.split(b":")[0] for line in body] == [b"origin", b"destination", b"elapsed",
                                                      b"differences"]
    assert not any(line.startswith(b"results:") for line in body)
    assert not any(line.startswith(b"rsync ") for line in body)
