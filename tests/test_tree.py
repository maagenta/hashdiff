"""Tests 23-26: the tree stage."""
import os
import shutil
import stat

import pytest

from conftest import (TREE_STAGE_FILES, Symlink, printed_command, run_command_line,
                      skip_if_root)


def test_tree_differences(make_tree, compare, parsers):
    origin = make_tree("origin", {"same": b"s", "missing": b"m", "size": b"12", "type": b"t"})
    destination = make_tree("destination", {"same": b"s", "size": b"1", "type": Symlink("x"),
                                            "extra": b"e"})
    code, out, err, results = compare(origin, destination)
    assert code == 4, err
    _, sections = parsers.report(os.path.join(results, b"tree-diff.txt"))
    assert sections[b"origin"] == [(b"MISSING", b"missing"), (b"SIZE", b"size"),
                                   (b"TYPE", b"type")]
    assert sections[b"destination"] == [(b"EXTRA", b"extra")]
    assert sorted(os.listdir(results)) == sorted(TREE_STAGE_FILES)
    assert b"trees differ; nothing was hashed" in err
    assert b"1 file exists only in DESTINATION" in err
    assert b"will delete it." in err
    assert b"--delete-after" in printed_command(out)


def test_suggested_command_without_extra(make_tree, compare):
    origin = make_tree("origin", {"a": b"a", "b": b"b"})
    destination = make_tree("destination", {"a": b"a"})
    code, out, err, results = compare(origin, destination)
    assert code == 4
    command = printed_command(out)
    assert command.startswith(b"rsync -a '") and b"--delete-after" not in command
    assert b"exist only in DESTINATION" not in err


def test_tree_lines_match_lstat(make_tree, compare, parsers):
    spec = {"f": b"12345", "d/g": b"", "l": Symlink("target-of-link")}
    origin = make_tree("origin", spec)
    destination = make_tree("destination", spec)
    code, out, err, results = compare(origin, destination)
    assert code == 0, err
    header, entries = parsers.tree(os.path.join(results, b"tree-origin.txt"))
    assert header[b"hashdiff-tree"] == b"1" and header[b"root"] == origin
    assert [e[3] for e in entries] == [b"d/g", b"f", b"l"]
    for kind, size, mtime, rel in entries:
        st = os.lstat(os.path.join(origin, rel))
        assert kind == (b"L" if stat.S_ISLNK(st.st_mode) else b"F")
        assert int(size) == st.st_size
        assert int(mtime) == int(st.st_mtime)


def test_unreadable_directory(make_tree, compare, parsers):
    skip_if_root()
    origin = make_tree("origin", {"locked/a": b"a", "b": b"b"})
    destination = make_tree("destination", {"locked/a": b"a", "b": b"b"})
    locked = os.path.join(origin, b"locked")
    os.chmod(locked, 0)
    try:
        code, out, err, results = compare(origin, destination)
    finally:
        os.chmod(locked, 0o755)
    assert code == 4, err
    _, sections = parsers.report(os.path.join(results, b"tree-diff.txt"))
    assert (b"ERR-SRC", b"locked") in sections[b"origin"]
    assert b"could not be read" in err and b"fix their permissions" in err


def test_only_errors_suggest_no_command(make_tree, compare):
    skip_if_root()
    origin = make_tree("origin", {"locked/a": b"a"})
    destination = make_tree("destination", {"locked/a": b"a"})
    locked = [os.path.join(root, b"locked") for root in (origin, destination)]
    for path in locked:
        os.chmod(path, 0)
    try:
        code, out, err, results = compare(origin, destination)
    finally:
        for path in locked:
            os.chmod(path, 0o755)
    assert code == 4
    assert printed_command(out) is None
    assert b"fix their permissions" in err


@pytest.mark.skipif(shutil.which("rsync") is None, reason="rsync is not in PATH")
def test_fix_with_suggested_command(make_tree, run_hashdiff):
    origin = make_tree("origin", {"keep": b"k", "new/file": b"n"})
    destination = make_tree("destination", {"keep": b"k", "old": b"o"})
    code, out, err = run_hashdiff(origin, destination, "--output", destination)
    assert code == 4, err
    command = printed_command(out)
    assert b"--delete-after" in command and b"--exclude='/results.hashdiff/'" in command
    proc = run_command_line(command)
    assert proc.returncode == 0, proc.stderr
    assert os.path.isdir(os.path.join(destination, b"results.hashdiff"))
    assert not os.path.exists(os.path.join(destination, b"old"))
    code, out, err = run_hashdiff(origin, destination, "--output", destination)
    assert code == 0, err
