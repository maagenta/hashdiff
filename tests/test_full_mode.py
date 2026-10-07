"""Test 3: full mode, every F line matches hashlib.md5 of the file."""
import hashlib
import os

from conftest import Symlink


def tree_spec():
    return {
        "empty": b"",
        "small.txt": b"hello\n",
        "block-1": os.urandom(64 * 1024 - 1),
        "mib-plus-1": os.urandom(1024 * 1024 + 1),
        "three-mib": os.urandom(3 * 1024 * 1024 + 17),
        "a-b": b"dash",
        "a/x": b"slash",
        "a/deep/er/file": b"deep",
        "link": Symlink("small.txt"),
        "dangling": Symlink("does/not/exist"),
    }


def run_identical(make_tree, run_hashdiff, tmp_path):
    spec = tree_spec()
    origin = make_tree("origin", spec)
    destination = make_tree("destination", spec)
    out = os.fsencode(tmp_path)
    code, stdout, stderr = run_hashdiff(origin, destination, "--output", out)
    return code, stderr, origin, os.path.join(out, b"results.hashdiff")


def test_f_lines_match_hashlib(make_tree, run_hashdiff, parsers, tmp_path):
    code, stderr, origin, results = run_identical(make_tree, run_hashdiff, tmp_path)
    assert code == 0, stderr
    for side in (b"origin", b"destination"):
        header, entries = parsers.hashes(os.path.join(results, b"hashes-" + side + b".txt"))
        assert header[b"hashdiff-format"] == b"3"
        assert header[b"mode"] == b"full"
        files = [e for e in entries if e[0] == b"F"]
        assert len(files) == 8
        for kind, digest, size, rel in files:
            with open(os.path.join(origin, rel), "rb") as f:
                data = f.read()
            assert digest == hashlib.md5(data).hexdigest().encode(), rel
            assert int(size) == len(data)


def test_symlink_lines(make_tree, run_hashdiff, parsers, tmp_path):
    code, stderr, origin, results = run_identical(make_tree, run_hashdiff, tmp_path)
    assert code == 0, stderr
    header, entries = parsers.hashes(os.path.join(results, b"hashes-origin.txt"))
    links = {e[3]: e for e in entries if e[0] == b"L"}
    for name, target in ((b"link", b"small.txt"), (b"dangling", b"does/not/exist")):
        kind, digest, size, rel = links[name]
        assert digest == hashlib.md5(target).hexdigest().encode()
        assert int(size) == len(target)


def test_canonical_order_and_root(make_tree, run_hashdiff, parsers, tmp_path):
    code, stderr, origin, results = run_identical(make_tree, run_hashdiff, tmp_path)
    assert code == 0, stderr
    header, entries = parsers.hashes(os.path.join(results, b"hashes-origin.txt"))
    paths = [e[3] for e in entries]
    assert paths == sorted(paths)
    # A DFS with sorted directories would put a/x before a-b; byte order puts '-' first.
    assert paths.index(b"a-b") < paths.index(b"a/deep/er/file") < paths.index(b"a/x")
    assert header[b"root"] == origin
