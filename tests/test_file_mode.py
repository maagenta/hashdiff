"""Test 29: comparing single files with --file (section 2.3).

A side's root is the file's parent directory and its list holds one entry, so every format and
every stage works unchanged; the two entries are compared by position, because the names may
differ.
"""
import os

import pytest

from conftest import RESULT_FILES, Symlink, printed_command, read_bytes


@pytest.fixture
def files(tmp_path):
    """(origin file, destination file, output dir, results dir) with equal content."""
    base = os.fsencode(tmp_path)
    out = os.path.join(base, b"out")
    os.makedirs(os.path.join(base, b"sub"), exist_ok=True)
    os.makedirs(out, exist_ok=True)
    origin = os.path.join(base, b"disk.img")
    destination = os.path.join(base, b"sub", b"copy-of-disk.img")
    for path in (origin, destination):
        with open(path, "wb") as f:
            f.write(b"image" * 2000)
    return origin, destination, out, os.path.join(out, b"results.hashdiff")


def test_identical_files(run_hashdiff, files):
    origin, destination, out, results = files
    code, stdout, err = run_hashdiff("--file", origin, destination, "-o", out)
    assert code == 0, err
    assert b"origin: 1 file, 9.7 KiB read" in stdout        # 10000 bytes
    assert b"no differences: ORIGIN and DESTINATION match" in stdout
    # The root of a side is the file's parent, and its list is the file's own name.
    for side, path in ((b"origin", origin), (b"destination", destination)):
        data = read_bytes(os.path.join(results, b"hashes-%s.txt" % side))
        assert b"# root: " + os.path.dirname(path) + b"\n" in data
        assert data.count(b"\nF ") == 1
        assert b" " + os.path.basename(path) + b"\n" in data


def test_different_names_are_compared_by_position(run_hashdiff, files, parsers):
    origin, destination, out, results = files
    with open(destination, "r+b") as f:
        f.seek(5000)
        f.write(b"X")

    code, stdout, err = run_hashdiff("--file", origin, destination, "-o", out)
    assert code == 1, err
    _, records, _ = parsers.diff(os.path.join(results, b"diff-files.txt"))
    assert len(records) == 1
    status, ohash, dhash, path = records[0]
    assert status == b"HASH"                    # never MISSING or EXTRA
    assert path == os.path.basename(origin)     # the ORIGIN entry's name
    assert len(ohash) == 32 and len(dhash) == 32 and ohash != dhash


def test_the_command_names_the_two_files(run_hashdiff, files):
    origin, destination, out, results = files
    with open(destination, "r+b") as f:
        f.seek(0)
        f.write(b"X")

    code, stdout, err = run_hashdiff("--file", origin, destination, "-o", out)
    assert code == 1, err
    expected = b"rsync -a -I '" + origin + b"' '" + destination + b"'"
    assert printed_command(stdout) == expected
    assert read_bytes(os.path.join(results, b"rsync-command.txt")) == expected + b"\n"
    # A list would copy ORIGIN's name into the destination's directory (section 2.3).
    assert b"rsync-files.lst" not in os.listdir(results)
    assert sorted(os.listdir(results)) == sorted(n for n in RESULT_FILES
                                                 if n != b"rsync-files.lst")


def test_a_difference_in_size_stops_before_reading(run_hashdiff, files):
    origin, destination, out, results = files
    with open(destination, "ab") as f:
        f.write(b"more")

    code, stdout, err = run_hashdiff("--file", origin, destination, "-o", out)
    assert code == 4, err
    assert b"differ in size or type" in err
    assert printed_command(stdout) == b"rsync -a '" + origin + b"' '" + destination + b"'"
    assert b"hashes-origin.txt" not in os.listdir(results)


def test_both_files_in_one_directory(run_hashdiff, tmp_path):
    base = os.fsencode(tmp_path)
    out = os.path.join(base, b"out")
    os.makedirs(out, exist_ok=True)
    first, second = os.path.join(base, b"a.iso"), os.path.join(base, b"b.iso")
    for path in (first, second):
        with open(path, "wb") as f:
            f.write(b"same")

    code, stdout, err = run_hashdiff("--file", first, second, "-o", out)
    assert code == 0, err                       # the parents may be the same directory
    code, stdout, err = run_hashdiff("--file", first, first, "-o", out, "--force")
    assert code == 2 and b"are the same file" in err


def test_a_symlink_argument_is_followed(run_hashdiff, files):
    origin, destination, out, results = files
    link = origin + b".link"
    os.symlink(origin, link)

    code, stdout, err = run_hashdiff("--file", link, destination, "-o", out)
    assert code == 0, err
    data = read_bytes(os.path.join(results, b"hashes-origin.txt"))
    assert b"\nL " not in data                  # the link was followed, so no L entry
    assert b"\nF " in data


@pytest.mark.parametrize("case", ["file-without-flag", "dir-with-flag", "fifo"])
def test_the_messages_of_section_2_3(run_hashdiff, files, tmp_path, case):
    origin, destination, out, results = files
    if case == "file-without-flag":
        args, expected = [origin, destination], b"is a file, not a directory; use --file"
    elif case == "dir-with-flag":
        args = ["--file", os.path.dirname(destination), destination]
        expected = b"is a directory, not a file; drop --file"
    else:
        fifo = os.path.join(os.fsencode(tmp_path), b"fifo")
        os.mkfifo(fifo)
        args, expected = [fifo, destination], b"is not a directory"

    code, stdout, err = run_hashdiff(*args, "-o", out)
    assert code == 2
    assert expected in err
    if case == "fifo":
        assert b"--file" not in err             # --file would not help a FIFO


def test_file_mode_with_two_destinations(run_hashdiff, files, tmp_path):
    origin, destination, out, results = files
    third = os.path.join(os.fsencode(tmp_path), b"third.img")
    with open(third, "wb") as f:
        f.write(b"image" * 2000)

    code, stdout, err = run_hashdiff("--file", origin, destination, third, "-o", out)
    assert code == 0, err
    assert b"no differences with destination-1" in stdout
    assert b"no differences with destination-2" in stdout
    assert b"# file: 1" in read_bytes(os.path.join(results, b"paths.txt"))


def test_a_run_started_with_file_is_not_the_same_run(run_hashdiff, files):
    origin, destination, out, results = files
    assert run_hashdiff("--file", origin, destination, "-o", out)[0] == 0
    code, stdout, err = run_hashdiff(os.path.dirname(origin), os.path.dirname(destination),
                                     "-o", out)
    assert code == 2
    assert b"compared files, not directories" in err
