"""Test 35: rechecking a finished run with --recheck (section 3.5).

A recheck answers one question, "did the copy the previous run asked for work?", so it reads
only the paths that differed, each destination only its own.
"""
import os

import pytest

from conftest import read_bytes, run_on_pty


def tree(make_tree, name, changed=()):
    spec = {"f%d" % i: b"file %d\n" % i for i in range(1, 6)}
    for i in changed:
        spec["f%d" % i] = b"XXXXXX\n"
    return make_tree(name, spec)


@pytest.fixture
def run(run_hashdiff, make_tree, tmp_path):
    """A finished run with two content differences, and a way to run it again."""
    origin = tree(make_tree, "origin")
    destination = tree(make_tree, "destination", changed=(2, 4))
    out = os.path.join(os.fsencode(tmp_path), b"out")
    os.makedirs(out, exist_ok=True)
    results = os.path.join(out, b"results.hashdiff")

    def again(*extra):
        return run_hashdiff(origin, destination, "-o", out, *extra)

    assert again()[0] == 1
    return again, origin, destination, results


def archives(results):
    return sorted(n for n in os.listdir(results) if n.startswith(b"scan-"))


def test_only_the_paths_that_differed_are_read(run):
    again, origin, destination, results = run
    # Copy one of the two, as the printed command would have done.
    with open(os.path.join(destination, b"f2"), "wb") as f:
        f.write(read_bytes(os.path.join(origin, b"f2")))

    code, stdout, err = again("--recheck")
    assert code == 1, err
    assert b"recheck: only the 2 paths that differed in the run of " in stdout
    assert b"origin: 2 files, " in stdout           # not the five of the tree
    assert b"differences: 1 HASH," in stdout

    kept = archives(results)
    assert len(kept) == 1 and len(kept[0]) == len(b"scan-202612161230")
    old = os.path.join(results, kept[0])
    assert b"hashes-origin.txt" in os.listdir(old)
    assert b"history.txt" not in os.listdir(old)    # it belongs to the directory
    assert b"lock" not in os.listdir(old)
    assert b"# recheck: " + kept[0] + b"\n" in read_bytes(os.path.join(results, b"paths.txt"))

    data = read_bytes(os.path.join(results, b"diff-files.txt"))
    assert b" f4\n" in data and b" f2\n" not in data


def test_a_second_recheck_ends_with_no_differences(run):
    again, origin, destination, results = run
    for name in (b"f2", b"f4"):
        with open(os.path.join(destination, name), "wb") as f:
            f.write(read_bytes(os.path.join(origin, name)))

    code, stdout, err = again("--recheck")
    assert code == 0, err
    assert b"no differences: ORIGIN and DESTINATION match" in stdout
    assert len(archives(results)) == 1


def test_two_archives_in_the_same_minute(run):
    again, origin, destination, results = run
    assert again("--recheck")[0] == 1
    assert again("--recheck")[0] == 1
    kept = archives(results)
    assert len(kept) == 2
    assert kept[1] == kept[0] + b"-2" or kept[0] == kept[1] + b"-2", kept


def test_a_path_gone_from_every_tree_is_not_a_difference(run):
    again, origin, destination, results = run
    os.remove(os.path.join(origin, b"f2"))
    os.remove(os.path.join(destination, b"f2"))

    code, stdout, err = again("--recheck")
    assert code == 1, err
    assert b"1 rechecked path no longer exists in any tree" in err
    data = read_bytes(os.path.join(results, b"diff-files.txt"))
    assert b" f2\n" not in data


def test_other_parameters_are_refused(run):
    again, origin, destination, results = run
    code, stdout, err = again("--recheck", "--fast")
    assert code == 2
    assert b"other parameters" in err and b"--force" in err


def test_recheck_needs_a_run_with_differences(run_hashdiff, make_tree, tmp_path):
    origin = tree(make_tree, "origin")
    destination = tree(make_tree, "destination")
    out = os.path.join(os.fsencode(tmp_path), b"out")
    os.makedirs(out, exist_ok=True)
    assert run_hashdiff(origin, destination, "-o", out)[0] == 0

    code, stdout, err = run_hashdiff(origin, destination, "-o", out, "--recheck")
    assert code == 2
    assert b"no differences to check again" in err


def test_the_prompt_offers_it(run, hashdiff_bin):
    again, origin, destination, results = run
    args = [origin, destination, "-o", os.path.dirname(results)]

    code, stdout, term = run_on_pty(hashdiff_bin, args, b"c\n")
    assert code == 1, term
    assert b"[c]heck those paths again" in term
    assert b"recheck: only the 2 paths" in stdout


def test_an_interrupted_recheck_reads_the_same_paths(run):
    again, origin, destination, results = run
    assert again("--recheck")[0] == 1
    for side in (b"origin", b"destination"):
        final = os.path.join(results, b"hashes-%s.txt" % side)
        data = read_bytes(final)
        os.remove(final)
        with open(final + b".tmp", "wb") as f:
            f.write(data[:data.index(b"# finished:")])

    code, stdout, err = again("--resume")
    assert code == 1, err
    assert b"recheck: only the 2 paths that differed in the run of " in stdout
    assert b"origin: 2 files, " in stdout


def test_one_destination_does_not_drag_the_others(run_hashdiff, make_tree, tmp_path):
    """Each destination is compared only against its own paths (section 3.5, step 3)."""
    origin = tree(make_tree, "origin")
    first = tree(make_tree, "d1", changed=(1,))
    second = tree(make_tree, "d2", changed=(3,))
    out = os.path.join(os.fsencode(tmp_path), b"out")
    os.makedirs(out, exist_ok=True)
    args = [origin, first, second, "-o", out]
    assert run_hashdiff(*args)[0] == 1
    for root, name in ((first, b"f1"), (second, b"f3")):
        with open(os.path.join(root, name), "wb") as f:
            f.write(read_bytes(os.path.join(origin, name)))

    code, stdout, err = run_hashdiff(*args, "--recheck")
    assert code == 0, err
    # f3 is not in destination-1's set, so it is never reported MISSING for it.
    assert b"no differences with destination-1" in stdout
    assert b"no differences with destination-2" in stdout
    assert b"MISSING" not in stdout
    assert b"origin: 2 files, " in stdout
    assert b"destination-1: 1 file, " in stdout
