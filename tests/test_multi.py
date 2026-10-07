"""Tests 30 to 33: one origin against several destinations (section 2.2).

Every destination is compared with ORIGIN and never with another destination, and with one
destination every name and every label is the one of version 1.0.
"""
import os

import pytest

from conftest import RESULT_FILES


def summary_lines(stdout, prefix):
    return [line for line in stdout.split(b"\n") if line.startswith(prefix)]


def test_three_destinations_in_three_states(run_hashdiff, make_tree, tmp_path):
    """Test 30: identical, a content difference, and a tree that differs."""
    spec = {"a": b"aa", "b": b"bb"}
    origin = make_tree("origin", spec)
    first = make_tree("d1", spec)
    second = make_tree("d2", {"a": b"aa", "b": b"XX"})
    third = make_tree("d3", dict(spec, c=b"cc"))
    out = os.path.join(os.fsencode(tmp_path), b"out")
    os.makedirs(out, exist_ok=True)
    results = os.path.join(out, b"results.hashdiff")

    code, stdout, err = run_hashdiff(origin, first, second, third, "--output", out)
    assert code == 4, err                       # the tree that differs wins the precedence

    names = sorted(os.listdir(results))
    for side in (b"destination-1", b"destination-2", b"destination-3"):
        assert b"tree-" + side + b".txt" in names
        assert b"tree-diff-" + side + b".txt" in names
    assert b"hashes-destination-1.txt" in names
    assert b"hashes-destination-2.txt" in names
    assert b"hashes-destination-3.txt" not in names          # it was never hashed
    assert b"diff-files-destination-3.txt" not in names
    assert names.count(b"hashes-origin.txt") == 1
    for unnumbered in (b"tree-destination.txt", b"tree-diff.txt", b"diff-files.txt",
                       b"rsync-files.lst", b"rsync-command.txt"):
        assert unnumbered not in names

    sides = summary_lines(stdout, b"origin:") + summary_lines(stdout, b"destination-")
    assert [line.split(b":")[0] for line in sides] == [b"origin", b"destination-1",
                                                       b"destination-2", b"destination-3"]
    assert b"no differences with destination-1\n" in stdout
    assert b"differences with destination-2: 1 HASH," in stdout
    assert b"tree differences with destination-3: 0 MISSING, 1 EXTRA," in stdout
    assert b"command for destination-2:\n" in stdout
    assert b"suggested command for destination-3 (review it first;" in stdout
    assert b"destination-3 was not hashed" in err


def test_one_destination_keeps_the_names_of_1_0(compare, make_tree):
    """Test 30: a single-destination run is unchanged."""
    origin = make_tree("origin", {"a": b"aa"})
    destination = make_tree("destination", {"a": b"XX"})
    code, stdout, err, results = compare(origin, destination)
    assert code == 1, err
    assert sorted(os.listdir(results)) == sorted(RESULT_FILES)
    assert b"destination: 1 file," in stdout
    assert b"differences: 1 HASH," in stdout
    assert b"command for" not in stdout


def test_same_result_with_jobs_and_serial(run_hashdiff, make_tree, tmp_path, snapshot):
    """Test 31: -j 1, -j 4 and --serial give the same files over three destinations."""
    spec = {"a": b"aa", "sub/b": b"bb", "sub/c": b"cc"}
    roots = [make_tree("origin", spec), make_tree("d1", spec),
             make_tree("d2", {"a": b"aa", "sub/b": b"XX", "sub/c": b"cc"}),
             make_tree("d3", spec)]
    out = os.path.join(os.fsencode(tmp_path), b"out")
    os.makedirs(out, exist_ok=True)
    results = os.path.join(out, b"results.hashdiff")
    first = None

    for extra in (("-j", "1"), ("-j", "4"), ("--serial",)):
        code, _, err = run_hashdiff(*roots, "--output", out, "--force", *extra)
        assert code == 1, (extra, err)
        current = snapshot(results)
        if first is None:
            first = current
        else:
            assert current == first, extra


@pytest.mark.parametrize("case", ["origin-twice", "destination-twice", "too-many", "count"])
def test_rejections(run_hashdiff, make_tree, tmp_path, case):
    """Test 32: the checks of section 2.2."""
    spec = {"a": b"aa"}
    origin = make_tree("origin", spec)
    first = make_tree("d1", spec)
    out = os.path.join(os.fsencode(tmp_path), b"out")
    os.makedirs(out, exist_ok=True)

    if case == "origin-twice":
        args = [origin, first, origin]
        expected = b"ORIGIN and DESTINATION-2 are the same directory"
    elif case == "destination-twice":
        args = [origin, first, first]
        expected = b"DESTINATION-1 and DESTINATION-2 are the same directory"
    elif case == "too-many":
        args = [origin] + [make_tree("x%d" % i, spec) for i in range(65)]
        expected = b"too many destinations"
    else:
        args = [origin, first, "--number-of-destinations", "3"]
        expected = b"--number-of-destinations does not match"

    code, out_bytes, err = run_hashdiff(*args, "--output", out)
    assert code == 2, err
    assert expected in err, err


def test_sixty_four_destinations_are_allowed(run_hashdiff, make_tree, tmp_path):
    """Test 32: 64 is the limit, not 63."""
    spec = {"a": b"aa"}
    roots = [make_tree("origin", spec)]
    roots += [make_tree("d%d" % i, spec) for i in range(64)]
    out = os.path.join(os.fsencode(tmp_path), b"out")
    os.makedirs(out, exist_ok=True)

    code, stdout, err = run_hashdiff(*roots, "--output", out,
                                     "--number-of-destinations", "64")
    assert code == 0, err
    assert b"destination-64: 1 file," in stdout
    assert b"more than the disks can serve" in err      # (1 + 64) * 1 jobs > 64


def test_a_nested_destination_is_excluded_from_every_command(run_hashdiff, make_tree,
                                                             tmp_path):
    """Test 32: the excluded paths of every side go into every suggested command."""
    origin = make_tree("origin", {"a": b"aa", "gone": b"g"})
    first = make_tree("d1", {"a": b"aa"})
    nested = make_tree(os.path.join("d1", "inner"), {"a": b"aa"})
    out = os.path.join(os.fsencode(tmp_path), b"out")
    os.makedirs(out, exist_ok=True)

    code, stdout, err = run_hashdiff(origin, first, nested, "--output", out)
    assert code == 4, err
    assert b"skipping 'inner' (destination-2 tree)" in err
    for line in stdout.split(b"\n"):
        if line.startswith(b"rsync "):
            assert b"--exclude='/inner/'" in line, line


def test_force_removes_the_files_of_a_wider_run(run_hashdiff, make_tree, tmp_path):
    """Test 33: the cleaning by prefix of section 3.3."""
    spec = {"a": b"aa"}
    roots = [make_tree("origin", spec), make_tree("d1", spec), make_tree("d2", spec),
             make_tree("d3", spec)]
    out = os.path.join(os.fsencode(tmp_path), b"out")
    os.makedirs(out, exist_ok=True)
    results = os.path.join(out, b"results.hashdiff")

    code, _, err = run_hashdiff(*roots, "--output", out)
    assert code == 0, err
    assert any(b"destination-3" in n for n in os.listdir(results))
    archive = os.path.join(results, b"scan-202612161230")
    os.makedirs(archive, exist_ok=True)
    with open(os.path.join(archive, b"kept"), "wb") as f:
        f.write(b"x")

    code, _, err = run_hashdiff(*roots[:3], "--output", out, "--force")
    assert code == 0, err
    names = os.listdir(results)
    assert not [n for n in names if b"destination-3" in n], names
    assert b"scan-202612161230" in names
    assert os.path.exists(os.path.join(archive, b"kept"))
