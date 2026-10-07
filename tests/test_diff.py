"""Tests 4 and 5: identical trees, existing results, content differences."""
import os

from conftest import Symlink, printed_command, read_bytes

RESULT_FILES = [b"tree-origin.txt", b"tree-destination.txt", b"tree-diff.txt",
                b"hashes-origin.txt", b"hashes-destination.txt", b"diff-files.txt",
                b"rsync-files.lst", b"rsync-command.txt"]

SPEC = {"a.txt": b"alpha", "dir/b.bin": os.urandom(5000), "dir/sub/c": b"",
        "link": Symlink("a.txt")}


def test_identical_trees(make_tree, compare, parsers):
    origin = make_tree("origin", SPEC)
    destination = make_tree("destination", SPEC)
    code, out, err, results = compare(origin, destination)
    assert code == 0, err
    assert sorted(os.listdir(results)) == sorted(RESULT_FILES)
    header, sections = parsers.report(os.path.join(results, b"tree-diff.txt"))
    assert sections == {None: [], b"origin": [], b"destination": []}
    header, sections = parsers.report(os.path.join(results, b"diff-files.txt"))
    assert sections == {None: []}
    assert header[b"origin"] == origin and header[b"destination"] == destination
    assert read_bytes(os.path.join(results, b"rsync-files.lst")) == b""
    assert read_bytes(os.path.join(results, b"rsync-command.txt")) == b""
    assert printed_command(out) is None


def test_existing_results(make_tree, compare):
    origin = make_tree("origin", SPEC)
    destination = make_tree("destination", SPEC)
    assert compare(origin, destination)[0] == 0
    code, out, err, results = compare(origin, destination)
    assert code == 2
    assert b"--resume" in err and b"--force" in err
    assert compare(origin, destination, "--force")[0] == 0


def test_options_after_paths_and_trailing_slashes(make_tree, run_hashdiff, tmp_path):
    origin = make_tree("origin", SPEC)
    destination = make_tree("destination", SPEC)
    outputs = []
    for i, args in enumerate([("--output", None, origin, destination),
                              (origin + b"/", destination + b"//", "--output", None)]):
        out = os.path.join(os.fsencode(tmp_path), b"out%d" % i)
        os.mkdir(out)
        args = [out if a is None else a for a in args]
        code, stdout, stderr = run_hashdiff(*args)
        assert code == 0, stderr
        outputs.append({name: read_bytes(os.path.join(out, b"results.hashdiff", name))
                        for name in RESULT_FILES})
    assert outputs[0] == outputs[1]


def test_content_differences(make_tree, compare, parsers):
    spec = dict(SPEC)
    origin = make_tree("origin", spec)
    data = bytearray(spec["dir/b.bin"])
    data[2500] ^= 0x01
    spec["dir/b.bin"] = bytes(data)
    spec["link"] = Symlink("b.txt")           # same length as "a.txt"
    destination = make_tree("destination", spec)
    code, out, err, results = compare(origin, destination)
    assert code == 1, err
    header, sections = parsers.report(os.path.join(results, b"diff-files.txt"))
    assert header[b"mode"] == b"full"
    assert sections[None] == [(b"HASH", b"dir/b.bin"), (b"HASH", b"link")]
    assert parsers.rsync_list(os.path.join(results, b"rsync-files.lst")) == [b"dir/b.bin",
                                                                              b"link"]
    command = printed_command(out)
    assert command.startswith(b"rsync -a -I --from0 --files-from='")
    assert read_bytes(os.path.join(results, b"rsync-command.txt")) == command + b"\n"
