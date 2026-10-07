"""Tests 7, 8 and 9: special entries, unreadable files and results inside ORIGIN."""
import os
import re

from conftest import Dir, Fifo, skip_if_root


def test_empty_file_dir_and_fifo(make_tree, compare, parsers):
    origin = make_tree("origin", {"empty": b"", "emptydir": Dir(), "fifo": Fifo(), "f": b"1"})
    destination = make_tree("destination", {"empty": b"", "f": b"1"})
    code, out, err, results = compare(origin, destination)
    assert code == 0, err
    _, entries = parsers.hashes(os.path.join(results, b"hashes-origin.txt"))
    assert [(e[0], e[2], e[3]) for e in entries] == [(b"F", b"0", b"empty"), (b"F", b"1", b"f")]
    assert re.search(rb"^origin: .*\b1 ignored$", out, re.M), out
    assert re.search(rb"^destination: .*\b0 ignored$", out, re.M), out


def test_unreadable_files(make_tree, compare, parsers):
    skip_if_root()
    spec = {"secret": b"data", "ok": b"ok"}
    for side, status in ((0, b"ERR-SRC"), (1, b"ERR-DST")):
        origin = make_tree("origin%d" % side, spec)
        destination = make_tree("destination%d" % side, spec)
        target = os.path.join((origin, destination)[side], b"secret")
        os.chmod(target, 0)
        try:
            code, out, err, results = compare(origin, destination, "--force")
        finally:
            os.chmod(target, 0o644)
        assert code == 3, err
        _, sections = parsers.report(os.path.join(results, b"diff-files.txt"))
        assert sections[None] == [(status, b"secret")]
        listed = parsers.rsync_list(os.path.join(results, b"rsync-files.lst"))
        assert listed == ([b"secret"] if status == b"ERR-DST" else [])


def test_results_inside_origin(make_tree, run_hashdiff, parsers):
    origin = make_tree("origin", {"a": b"a"})
    destination = make_tree("destination", {"a": b"a"})
    code, out, err = run_hashdiff(origin, destination, "--output", origin)
    assert code == 0, err
    assert b"skipping 'results.hashdiff' (results directory)" in err
    results = os.path.join(origin, b"results.hashdiff")
    _, tree = parsers.tree(os.path.join(results, b"tree-origin.txt"))
    _, hashes = parsers.hashes(os.path.join(results, b"hashes-origin.txt"))
    assert [e[3] for e in tree] == [b"a"] and [e[3] for e in hashes] == [b"a"]
