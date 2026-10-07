"""Test 6: names that need escaping, and raw bytes in rsync-files.lst."""
import errno
import os

import pytest

NAMES = [b"with space", b"-leading-dash", b"back\\slash", b"tab\there", b"new\nline",
         b"carriage\rreturn", b"a\xffb"]


def make_or_skip(make_tree, name, spec):
    try:
        return make_tree(name, spec)
    except OSError as e:
        if e.errno in (errno.EILSEQ, errno.EINVAL):
            pytest.skip("the filesystem rejects this file name: %s" % e)
        raise


@pytest.mark.parametrize("name", NAMES)
def test_round_trip(make_tree, compare, parsers, name):
    path = b"dir/" + name
    origin = make_or_skip(make_tree, "origin", {path: b"one", b"x": b"x"})
    destination = make_or_skip(make_tree, "destination", {path: b"two", b"x": b"x"})
    code, out, err, results = compare(origin, destination)
    assert code == 1, err
    for side in (b"origin", b"destination"):
        _, tree = parsers.tree(os.path.join(results, b"tree-" + side + b".txt"))
        assert path in [e[3] for e in tree]
        _, hashes = parsers.hashes(os.path.join(results, b"hashes-" + side + b".txt"))
        assert path in [e[3] for e in hashes]
    _, sections = parsers.report(os.path.join(results, b"diff-files.txt"))
    assert sections[None] == [(b"HASH", path)]
    assert parsers.rsync_list(os.path.join(results, b"rsync-files.lst")) == [path]


@pytest.mark.parametrize("name", NAMES)
def test_round_trip_in_tree_diff(make_tree, compare, parsers, name):
    path = b"dir/" + name
    origin = make_or_skip(make_tree, "origin", {path: b"one", b"x": b"x"})
    destination = make_tree("destination", {b"x": b"x"})
    code, out, err, results = compare(origin, destination)
    assert code == 4, err
    _, sections = parsers.report(os.path.join(results, b"tree-diff.txt"))
    assert sections[b"origin"] == [(b"MISSING", path)]
