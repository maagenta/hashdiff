"""Test 38: the shape of the summary (section 9 of docs/implementation.md)."""
import re


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
